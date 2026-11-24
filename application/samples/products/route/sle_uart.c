/*
 * Five-hop router node (R1-R4). It receives notifications from upstream and
 * queues them briefly and forwards them to the downstream link.
 */
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "pinctrl.h"
#include "uart.h"
#include "watchdog.h"
#include "sle_low_latency.h"
#include "securec.h"
#include "sle_connection_manager.h"
#include "route_server.h"
#include "route_server_adv.h"
#include "sle_ssap_client.h"
#include "route_client.h"
#include "tcxo.h"
#include "uapi_crc.h"
#include "sle_router_config.h"
#include "../sle_uart/sle_file_transfer.h"

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define ROUTER_FORWARD_RETRY_US             100
#define ROUTER_FORWARD_MAX_LEN              1500
#define ROUTER_FORWARD_QUEUE_DEPTH          64
#define ROUTER_TX_TASK_PRIO                 26
#define ROUTER_TX_TASK_STACK_SIZE           0x2000
#define ROUTER_TX_IDLE_SLEEP_MS             1
#define ROUTER_STATS_INTERVAL_US            5000000ULL
#define ROUTER_WATCHDOG_INTERVAL_MS         500
#define ROUTER_REVERSE_RETRY_US              1000ULL
#define ROUTER_DIAG_QUEUE_DEPTH              16
#define ROUTER_STATUS_RETRY_US               5000ULL
#define ROUTER_STATUS_CREDIT_PRESSURE_DELTA  20u
#define ROUTER_FORWARD_STALL_RECOVERY_US      5000000ULL
#define ROUTER_FORWARD_RECOVERY_COOLDOWN_US  10000000ULL

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_route_mtu_negotiated;

static volatile uint32_t g_router_forwarded = 0;
static volatile uint32_t g_router_dropped = 0;
static volatile uint32_t g_router_tx_failed = 0;
static volatile uint32_t g_router_credit_wait = 0;
static volatile uint32_t g_router_queued = 0;
static volatile uint32_t g_router_queue_dropped = 0;
static volatile uint32_t g_router_queue_duplicates = 0;
static volatile uint32_t g_router_queue_max = 0;
static volatile uint32_t g_router_reverse_ok = 0;
static volatile uint32_t g_router_reverse_fail = 0;
static volatile bool g_router_reverse_pending = false;
static volatile uint32_t g_router_reverse_generation = 0;
static uint64_t g_router_reverse_last_attempt_us = 0;
static sle_ft_control_t g_router_reverse_control;
static volatile bool g_router_status_pending = false;
static volatile uint32_t g_router_status_generation = 0;
static uint64_t g_router_status_last_attempt_us = 0;
static sle_ft_status_t g_router_status;
static uint32_t g_router_status_last_credit_wait = 0;
static uint32_t g_router_status_last_tx_failed = 0;
static uint32_t g_router_status_last_dropped = 0;
static sle_diag_report_t g_diag_reverse_queue[ROUTER_DIAG_QUEUE_DEPTH];
static uint8_t g_diag_reverse_head = 0;
static uint8_t g_diag_reverse_tail = 0;
static uint8_t g_diag_reverse_count = 0;

typedef struct {
    uint16_t len;
    uint8_t data[ROUTER_FORWARD_MAX_LEN];
} router_forward_packet_t;

static router_forward_packet_t g_forward_queue[ROUTER_FORWARD_QUEUE_DEPTH];
static uint8_t g_forward_queue_head = 0;
static uint8_t g_forward_queue_tail = 0;
static uint8_t g_forward_queue_count = 0;
static bool g_forward_session_valid = false;
static uint16_t g_forward_session_id = 0;

static bool sle_seq_after(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right) > 0;
}

static bool sle_seq_acked(uint16_t ack_seq, uint16_t seq)
{
    return (int16_t)(ack_seq - seq) >= 0;
}

static uint16_t sle_sat_add_u16(uint16_t left, uint32_t right)
{
    uint32_t sum = (uint32_t)left + right;
    return (sum > UINT16_MAX) ? UINT16_MAX : (uint16_t)sum;
}

static void router_reverse_queue(const sle_ft_control_t *control)
{
    uint32_t irq_status = osal_irq_lock();

    if (!g_router_reverse_pending || g_router_reverse_control.session_id != control->session_id) {
        g_router_reverse_control = *control;
    } else {
        uint16_t merged_ack = g_router_reverse_control.ack_seq;
        uint16_t merged_nack = g_router_reverse_control.nack_seq;

        if (sle_seq_after(control->ack_seq, merged_ack)) {
            merged_ack = control->ack_seq;
        }
        if (merged_nack != SLE_FT_NO_NACK && sle_seq_acked(merged_ack, merged_nack)) {
            merged_nack = SLE_FT_NO_NACK;
        }
        if (control->nack_seq != SLE_FT_NO_NACK && !sle_seq_acked(merged_ack, control->nack_seq)) {
            uint16_t new_distance = (uint16_t)(control->nack_seq - merged_ack - 1u);
            uint16_t old_distance = (uint16_t)(merged_nack - merged_ack - 1u);
            if (merged_nack == SLE_FT_NO_NACK || new_distance < old_distance) {
                merged_nack = control->nack_seq;
            }
        }
        g_router_reverse_control.ack_seq = merged_ack;
        g_router_reverse_control.nack_seq = merged_nack;
        g_router_reverse_control.crc16 = 0;
        g_router_reverse_control.crc16 = uapi_crc16(0,
            (uint8_t *)&g_router_reverse_control,
            SLE_FT_CTRL_LEN - sizeof(g_router_reverse_control.crc16));
    }
    g_router_reverse_generation++;
    g_router_reverse_pending = true;
    osal_irq_restore(irq_status);
}

static void router_status_queue(const sle_ft_status_t *incoming)
{
    sle_ft_status_t status = *incoming;
    uint32_t credit_wait = g_router_credit_wait;
    uint32_t tx_failed = g_router_tx_failed;
    uint32_t dropped = g_router_dropped;
    uint32_t credit_delta = credit_wait - g_router_status_last_credit_wait;
    uint32_t tx_fail_delta = tx_failed - g_router_status_last_tx_failed;
    uint32_t drop_delta = dropped - g_router_status_last_dropped;
    uint8_t queue_count;
    uint8_t queue_percent;
    uint32_t irq_status;

    g_router_status_last_credit_wait = credit_wait;
    g_router_status_last_tx_failed = tx_failed;
    g_router_status_last_dropped = dropped;

    irq_status = osal_irq_lock();
    queue_count = g_forward_queue_count;
    osal_irq_restore(irq_status);
    queue_percent = (uint8_t)(((uint16_t)queue_count * 100u) / ROUTER_FORWARD_QUEUE_DEPTH);

    status.link_mask |= (uint8_t)(1u << ROUTER_NODE_ID);
    if (queue_percent > status.max_queue_percent) {
        status.max_queue_percent = queue_percent;
        status.bottleneck_node = ROUTER_NODE_ID;
    }
    status.credit_wait_delta = sle_sat_add_u16(status.credit_wait_delta, credit_delta);
    status.tx_fail_delta = sle_sat_add_u16(status.tx_fail_delta, tx_fail_delta);
    status.drop_delta = sle_sat_add_u16(status.drop_delta, drop_delta);
    if (credit_delta >= ROUTER_STATUS_CREDIT_PRESSURE_DELTA || queue_percent >= 35u) {
        status.flags |= SLE_FT_STATUS_FLAG_CREDIT_PRESSURE;
    }
    if (tx_fail_delta != 0) {
        status.flags |= SLE_FT_STATUS_FLAG_TX_FAILURE;
    }
    if (drop_delta != 0) {
        status.flags |= SLE_FT_STATUS_FLAG_DROP;
    }
    if (!route_client_write_ready() || !route_server_has_downstream() || !g_route_mtu_negotiated) {
        status.flags |= SLE_FT_STATUS_FLAG_LINK_NOT_READY;
    }
    status.crc16 = 0;
    status.crc16 = uapi_crc16(0, (uint8_t *)&status,
        SLE_FT_STATUS_LEN - sizeof(status.crc16));

    irq_status = osal_irq_lock();
    g_router_status = status;
    g_router_status_generation++;
    g_router_status_pending = true;
    osal_irq_restore(irq_status);
}

static bool router_status_try_send(void)
{
    sle_ft_status_t status;
    uint32_t generation;
    uint32_t irq_status;
    uint64_t now_us;

    irq_status = osal_irq_lock();
    if (!g_router_status_pending) {
        osal_irq_restore(irq_status);
        return false;
    }
    status = g_router_status;
    generation = g_router_status_generation;
    osal_irq_restore(irq_status);

    now_us = uapi_tcxo_get_us();
    if (g_router_status_last_attempt_us != 0 &&
        now_us - g_router_status_last_attempt_us < ROUTER_STATUS_RETRY_US) {
        return false;
    }
    g_router_status_last_attempt_us = now_us;
    if (route_client_write_upstream((uint8_t *)&status, sizeof(status)) != ERRCODE_SUCC) {
        g_router_reverse_fail++;
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_router_status_pending && g_router_status_generation == generation) {
        g_router_status_pending = false;
    }
    osal_irq_restore(irq_status);
    g_router_reverse_ok++;
    return true;
}

static bool router_reverse_try_send(void)
{
    sle_ft_control_t control;
    uint32_t generation;
    uint32_t irq_status;
    uint64_t now_us;

    irq_status = osal_irq_lock();
    if (!g_router_reverse_pending) {
        osal_irq_restore(irq_status);
        return false;
    }
    control = g_router_reverse_control;
    generation = g_router_reverse_generation;
    osal_irq_restore(irq_status);

    now_us = uapi_tcxo_get_us();
    if (g_router_reverse_last_attempt_us != 0 &&
        now_us - g_router_reverse_last_attempt_us < ROUTER_REVERSE_RETRY_US) {
        return false;
    }
    g_router_reverse_last_attempt_us = now_us;

    if (route_client_write_upstream((uint8_t *)&control, sizeof(control)) != ERRCODE_SUCC) {
        g_router_reverse_fail++;
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_router_reverse_pending && g_router_reverse_generation == generation) {
        g_router_reverse_pending = false;
    }
    osal_irq_restore(irq_status);
    g_router_reverse_ok++;
    return true;
}

static bool router_diag_queue_push(const sle_diag_report_t *report)
{
    uint32_t irq_status;

    if (report == NULL) {
        return false;
    }
    irq_status = osal_irq_lock();
    if (g_diag_reverse_count >= ROUTER_DIAG_QUEUE_DEPTH) {
        osal_irq_restore(irq_status);
        return false;
    }
    g_diag_reverse_queue[g_diag_reverse_tail] = *report;
    g_diag_reverse_tail = (uint8_t)((g_diag_reverse_tail + 1) % ROUTER_DIAG_QUEUE_DEPTH);
    g_diag_reverse_count++;
    osal_irq_restore(irq_status);
    return true;
}

static bool router_diag_try_send(void)
{
    sle_diag_report_t report;
    uint32_t irq_status;

    irq_status = osal_irq_lock();
    if (g_diag_reverse_count == 0) {
        osal_irq_restore(irq_status);
        return false;
    }
    report = g_diag_reverse_queue[g_diag_reverse_head];
    osal_irq_restore(irq_status);

    if (route_client_write_upstream((uint8_t *)&report, sizeof(report)) != ERRCODE_SUCC) {
        g_router_reverse_fail++;
        return false;
    }
    irq_status = osal_irq_lock();
    if (g_diag_reverse_count > 0) {
        g_diag_reverse_head = (uint8_t)((g_diag_reverse_head + 1) % ROUTER_DIAG_QUEUE_DEPTH);
        g_diag_reverse_count--;
    }
    osal_irq_restore(irq_status);
    g_router_reverse_ok++;
    return true;
}

static bool router_forward_queue_push(const uint8_t *data, uint16_t len)
{
    router_forward_packet_t *slot;
    sle_ft_header_t header;
    bool is_file_packet = false;
    uint32_t irq_status;

    if (data == NULL || len == 0 || len > ROUTER_FORWARD_MAX_LEN) {
        g_router_dropped++;
        return false;
    }
    if (len >= SLE_FT_HEADER_LEN &&
        memcpy_s(&header, sizeof(header), data, SLE_FT_HEADER_LEN) == EOK &&
        header.magic == SLE_FT_MAGIC &&
        (uint32_t)header.payload_len + SLE_FT_HEADER_LEN <= len) {
        is_file_packet = true;
    }

    irq_status = osal_irq_lock();
    if (is_file_packet) {
        if (g_forward_session_valid && g_forward_session_id != header.session_id) {
            g_router_dropped += g_forward_queue_count;
            g_forward_queue_head = 0;
            g_forward_queue_tail = 0;
            g_forward_queue_count = 0;
        }
        g_forward_session_valid = true;
        g_forward_session_id = header.session_id;
        for (uint8_t offset = 0; offset < g_forward_queue_count; offset++) {
            uint8_t index = (uint8_t)((g_forward_queue_head + offset) % ROUTER_FORWARD_QUEUE_DEPTH);
            router_forward_packet_t *queued = &g_forward_queue[index];
            sle_ft_header_t queued_header;

            if (queued->len >= SLE_FT_HEADER_LEN &&
                memcpy_s(&queued_header, sizeof(queued_header), queued->data,
                    SLE_FT_HEADER_LEN) == EOK &&
                queued_header.magic == SLE_FT_MAGIC &&
                queued_header.session_id == header.session_id && queued_header.seq == header.seq) {
                g_router_queue_duplicates++;
                osal_irq_restore(irq_status);
                return true;
            }
        }
    }
    if (g_forward_queue_count >= ROUTER_FORWARD_QUEUE_DEPTH) {
        osal_irq_restore(irq_status);
        g_router_queue_dropped++;
        g_router_dropped++;
        return false;
    }

    slot = &g_forward_queue[g_forward_queue_tail];
    slot->len = len;
    if (memcpy_s(slot->data, sizeof(slot->data), data, len) != EOK) {
        osal_irq_restore(irq_status);
        g_router_dropped++;
        return false;
    }

    g_forward_queue_tail = (uint8_t)((g_forward_queue_tail + 1) % ROUTER_FORWARD_QUEUE_DEPTH);
    g_forward_queue_count++;
    if (g_forward_queue_count > g_router_queue_max) {
        g_router_queue_max = g_forward_queue_count;
    }
    g_router_queued++;
    osal_irq_restore(irq_status);
    return true;
}

static bool router_forward_queue_peek(router_forward_packet_t *packet)
{
    router_forward_packet_t *slot;
    uint32_t irq_status;

    if (packet == NULL) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_forward_queue_count > 0) {
        slot = &g_forward_queue[g_forward_queue_head];
        if (memcpy_s(packet, sizeof(*packet), slot, sizeof(*slot)) != EOK) {
            osal_irq_restore(irq_status);
            return false;
        }
        osal_irq_restore(irq_status);
        return true;
    }
    osal_irq_restore(irq_status);
    return false;
}

static void router_note_forward_stall(uint64_t *stall_started_us,
    uint64_t *last_recovery_us)
{
    uint64_t now_us = uapi_tcxo_get_us();

    if (*stall_started_us == 0) {
        *stall_started_us = now_us;
        return;
    }
    if (now_us - *stall_started_us < ROUTER_FORWARD_STALL_RECOVERY_US ||
        (*last_recovery_us != 0 &&
        now_us - *last_recovery_us < ROUTER_FORWARD_RECOVERY_COOLDOWN_US)) {
        return;
    }

    *last_recovery_us = now_us;
    *stall_started_us = now_us;
    if (route_server_force_reconnect()) {
        osal_printk("[Router Recovery] R%u downstream made no progress for 5s; reconnecting.\r\n",
            ROUTER_NODE_ID);
    }
}

static void router_forward_queue_pop(void)
{
    uint32_t irq_status = osal_irq_lock();

    if (g_forward_queue_count > 0) {
        g_forward_queue_head = (uint8_t)((g_forward_queue_head + 1) % ROUTER_FORWARD_QUEUE_DEPTH);
        g_forward_queue_count--;
    }

    osal_irq_restore(irq_status);
}

static void sle_uart_router_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    bool is_upstream;

    if (route_server_is_downstream_link(conn_id)) {
        route_server_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
        return;
    }

    is_upstream = route_client_is_upstream_link(conn_id, addr);

    if (is_upstream) {
        route_client_sample_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
        route_server_set_upstream_ready(route_client_write_ready());
    } else {
        route_server_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
    }
}

static void sle_uart_router_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    if (route_client_is_upstream_link(conn_id, addr) || !route_server_is_downstream_link(conn_id)) {
        route_client_sample_pair_complete_cbk(conn_id, addr, status);
    } else {
        route_server_pair_complete_cbk(conn_id, addr, status);
    }
}

static void sle_uart_router_register_connection_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {
        .connect_state_changed_cb = sle_uart_router_connect_state_changed_cbk,
        .pair_complete_cb = sle_uart_router_pair_complete_cbk,
        .set_phy_cb = route_server_set_phy_cbk
    };
    sle_connection_register_callbacks(&conn_cbks);
}

static void sle_uart_router_print_stats(void)
{
    static uint64_t last_print_us = 0;
    static uint32_t last_forwarded = 0;
    static uint32_t last_dropped = 0;
    static uint32_t last_tx_failed = 0;
    static uint32_t last_credit_wait = 0;
    static uint32_t last_queued = 0;
    static uint32_t last_queue_dropped = 0;
    static uint32_t last_queue_duplicates = 0;
    static uint32_t last_reverse_ok = 0;
    static uint32_t last_reverse_fail = 0;
    uint64_t now = uapi_tcxo_get_us();
    uint8_t queue_count;

    if (last_print_us == 0) {
        last_print_us = now;
        return;
    }

    if (now - last_print_us < ROUTER_STATS_INTERVAL_US) {
        return;
    }

    queue_count = g_forward_queue_count;
    osal_printk("[Router Stats] fwd:%u drop:%u tx_fail:%u credit_wait:%u queued:%u q_drop:%u duplicate:%u q_in:%u q_max:%u rev:%u rev_fail:%u rev_pending:%u\r\n",
        g_router_forwarded - last_forwarded,
        g_router_dropped - last_dropped,
        g_router_tx_failed - last_tx_failed,
        g_router_credit_wait - last_credit_wait,
        g_router_queued - last_queued,
        g_router_queue_dropped - last_queue_dropped,
        g_router_queue_duplicates - last_queue_duplicates,
        queue_count,
        g_router_queue_max,
        g_router_reverse_ok - last_reverse_ok,
        g_router_reverse_fail - last_reverse_fail,
        g_router_reverse_pending ? 1 : 0);

    last_forwarded = g_router_forwarded;
    last_dropped = g_router_dropped;
    last_tx_failed = g_router_tx_failed;
    last_credit_wait = g_router_credit_wait;
    last_queued = g_router_queued;
    last_queue_dropped = g_router_queue_dropped;
    last_queue_duplicates = g_router_queue_duplicates;
    last_reverse_ok = g_router_reverse_ok;
    last_reverse_fail = g_router_reverse_fail;
    last_print_us = now;
}

/* The callback must never discard a valid upstream packet merely because the
 * downstream link is temporarily out of credits. Queue it and let the TX task
 * retry outside the SLE callback. */
void sle_uart_router_notify_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    sle_diag_probe_t probe;
    sle_diag_report_t report;
    uint32_t magic;

    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data == NULL || data->data_len == 0) {
        return;
    }

    if (data->data_len == SLE_DIAG_PROBE_LEN &&
        memcpy_s(&magic, sizeof(magic), data->data, sizeof(magic)) == EOK &&
        magic == SLE_DIAG_PROBE_MAGIC) {
        if (memcpy_s(&probe, sizeof(probe), data->data, sizeof(probe)) != EOK ||
            uapi_crc16(0, (uint8_t *)&probe,
                SLE_DIAG_PROBE_LEN - sizeof(probe.crc16)) != probe.crc16) {
            g_router_dropped++;
            return;
        }
        uint16_t expected_path = (uint16_t)((1u << (ROUTER_NODE_ID - 1u)) - 1u);
        uint16_t this_hop = (uint16_t)(1u << (ROUTER_NODE_ID - 1u));

        if (probe.reserved != expected_path) {
            osal_printk("[Router DIAG] Reject path:%04x expected:%04x at R%u.\r\n",
                probe.reserved, expected_path, ROUTER_NODE_ID);
            g_router_dropped++;
            return;
        }
        report.magic = SLE_DIAG_REPORT_MAGIC;
        report.probe_id = probe.probe_id;
        report.node_id = ROUTER_NODE_ID;
        report.flags = 0;
        if (route_client_is_connected()) {
            report.flags |= SLE_DIAG_FLAG_UPSTREAM_READY;
        }
        if (route_client_write_ready()) {
            report.flags |= SLE_DIAG_FLAG_REVERSE_READY;
        }
        if (route_server_has_downstream()) {
            report.flags |= SLE_DIAG_FLAG_DOWNSTREAM_LINK;
        }
        if (g_route_mtu_negotiated) {
            report.flags |= SLE_DIAG_FLAG_DOWNSTREAM_MTU;
        }
        report.crc16 = uapi_crc16(0, (uint8_t *)&report,
            SLE_DIAG_REPORT_LEN - sizeof(report.crc16));
        (void)router_diag_queue_push(&report);
        if (route_server_has_downstream() && g_route_mtu_negotiated) {
            probe.reserved |= this_hop;
            probe.crc16 = uapi_crc16(0, (uint8_t *)&probe,
                SLE_DIAG_PROBE_LEN - sizeof(probe.crc16));
            (void)router_forward_queue_push((uint8_t *)&probe, sizeof(probe));
        }
        return;
    }

    (void)router_forward_queue_push(data->data, data->data_len);
}

void sle_uart_router_indic_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void sle_uart_router_reverse_write_cb(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    sle_ft_control_t control;
    sle_ft_status_t path_status;
    sle_diag_report_t report;
    uint32_t magic;

    unused(server_id);
    unused(conn_id);
    if (status != ERRCODE_SUCC || write_cb_para == NULL ||
        write_cb_para->value == NULL || write_cb_para->length < sizeof(magic) ||
        memcpy_s(&magic, sizeof(magic), write_cb_para->value, sizeof(magic)) != EOK) {
        return;
    }
    if (magic == SLE_DIAG_REPORT_MAGIC) {
        if (write_cb_para->length == SLE_DIAG_REPORT_LEN &&
            memcpy_s(&report, sizeof(report), write_cb_para->value, sizeof(report)) == EOK &&
            uapi_crc16(0, (uint8_t *)&report,
                SLE_DIAG_REPORT_LEN - sizeof(report.crc16)) == report.crc16) {
            (void)router_diag_queue_push(&report);
        }
        return;
    }
    if (magic == SLE_FT_STATUS_MAGIC) {
        if (write_cb_para->length == SLE_FT_STATUS_LEN &&
            memcpy_s(&path_status, sizeof(path_status), write_cb_para->value,
                sizeof(path_status)) == EOK &&
            uapi_crc16(0, (uint8_t *)&path_status,
                SLE_FT_STATUS_LEN - sizeof(path_status.crc16)) == path_status.crc16) {
            router_status_queue(&path_status);
        }
        return;
    }
    if (magic != SLE_FT_CTRL_MAGIC || write_cb_para->length != SLE_FT_CTRL_LEN ||
        memcpy_s(&control, sizeof(control), write_cb_para->value, sizeof(control)) != EOK ||
        uapi_crc16(0, (uint8_t *)&control,
            SLE_FT_CTRL_LEN - sizeof(control.crc16)) != control.crc16) {
        return;
    }
    router_reverse_queue(&control);
}

extern errcode_t route_server_register_announce_callbacks(void);

static void *sle_uart_router_tx_task(const char *arg)
{
    uint64_t forward_stall_started_us = 0;
    uint64_t last_forward_recovery_us = 0;

    unused(arg);

    while (1) {
        router_forward_packet_t packet;
        errcode_t ret;

        if (router_reverse_try_send() || router_status_try_send() || router_diag_try_send()) {
            osal_udelay(ROUTER_FORWARD_RETRY_US);
        }

        if (!route_server_has_downstream() || !g_route_mtu_negotiated) {
            forward_stall_started_us = 0;
            osal_msleep(ROUTER_TX_IDLE_SLEEP_MS);
            continue;
        }

        if (!router_forward_queue_peek(&packet)) {
            forward_stall_started_us = 0;
            osal_msleep(ROUTER_TX_IDLE_SLEEP_MS);
            continue;
        }

        if (gle_tx_acb_data_num_get() == 0) {
            g_router_credit_wait++;
            router_note_forward_stall(&forward_stall_started_us,
                &last_forward_recovery_us);
            osal_udelay(ROUTER_FORWARD_RETRY_US);
            continue;
        }

        ret = route_server_send_report_by_handle(packet.data, packet.len);
        if (ret == ERRCODE_SUCC) {
            g_router_forwarded++;
            router_forward_queue_pop();
            forward_stall_started_us = 0;
        } else {
            g_router_tx_failed++;
            router_note_forward_stall(&forward_stall_started_us,
                &last_forward_recovery_us);
            osal_udelay(ROUTER_FORWARD_RETRY_US);
        }
    }

    return NULL;
}

static void *sle_uart_router_task(const char *arg)
{
    unused(arg);
    osal_msleep(1000);

    route_server_init(NULL, sle_uart_router_reverse_write_cb);
    route_client_init(sle_uart_router_notify_cb, sle_uart_router_indic_cb);
    sle_uart_router_register_connection_cbks();
    route_server_register_announce_callbacks();
    enable_sle();

    /* Print compiled identity so we can verify each board was flashed with the
     * intended ROUTER_NODE_ID. If two boards print the same MY_ROUTER_NAME,
     * the chain will never converge. */
    osal_printk("[SLE Router] ID=%u advertises:\"%s\" scans:\"%s\"\r\n",
        ROUTER_NODE_ID, MY_ROUTER_NAME, TARGET_UPSTREAM_NAME);
    osal_printk("[SLE Router] Buffered forward + reverse ACK relay ready.\r\n");
    while (1) {
        /* Wait for the async route_server_enable_cbk to fire — that is what registers
         * services, sets MTU info, starts adv and scan. Calling adv_init or
         * the watchdog before the stack is enabled puts the announce context
         * in a state from which the SDK never recovers. */
        if (route_server_stack_is_enabled()) {
            route_server_set_upstream_ready(route_client_write_ready());
            route_server_watchdog_tick();
            route_client_watchdog_tick();
            sle_uart_router_print_stats();
        }
        osal_msleep(ROUTER_WATCHDOG_INTERVAL_MS);
    }

    return NULL;
}

static void sle_uart_entry(void)
{
    osal_task *task_handle = NULL;

    uapi_watchdog_disable();
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_task, 0,
        "RouterTask", SLE_UART_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_tx_task, 0,
        "RouterTxTask", ROUTER_TX_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, ROUTER_TX_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
