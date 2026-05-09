/*
 * Five-hop router node (R1-R4). It receives notifications from upstream and
 * forwards them directly to the downstream link.
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
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#include "sle_ssap_client.h"
#include "sle_uart_client.h"
#include "sle_router_config.h"
#include "tcxo.h"

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define ROUTER_FORWARD_RETRY_COUNT          16
#define ROUTER_FORWARD_RETRY_US             100
#define ROUTER_FORWARD_MAX_LEN              1500
#define ROUTER_FORWARD_QUEUE_DEPTH          48
#define ROUTER_TX_TASK_PRIO                 26
#define ROUTER_TX_TASK_STACK_SIZE           0x2000
#define ROUTER_TX_IDLE_SLEEP_MS             1
#define ROUTER_TX_FAIL_DROP_THRESHOLD       20
#define ROUTER_STATS_INTERVAL_US            5000000ULL
#define ROUTER_WATCHDOG_INTERVAL_MS         500
#define ROUTER_HOP_MARKER_OFFSET            3
#define ROUTER_STABLE_PROMOTE_US            60000000ULL
#define ROUTER_CREDIT_WAIT_HIGH             10000
#define BUSINESS_MAGIC0                     0xA5
#define BUSINESS_MAGIC1                     0x5A
#define BUSINESS_TYPE_DATA                  0x01
#define BUSINESS_TYPE_ACK                   0x02
#define BUSINESS_HEADER_LEN                 10
#define BUSINESS_SEQ_HI_OFFSET              4
#define BUSINESS_LEN_HI_OFFSET              6
#define BUSINESS_CRC_HI_OFFSET              8
#define BUSINESS_ACK_PAYLOAD_LEN            8
#define BUSINESS_ACK_PACKET_LEN             (BUSINESS_HEADER_LEN + BUSINESS_ACK_PAYLOAD_LEN)

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

static volatile uint32_t g_router_forwarded = 0;
static volatile uint32_t g_router_dropped = 0;
static volatile uint32_t g_router_tx_failed = 0;
static volatile uint32_t g_router_credit_wait = 0;
static volatile uint32_t g_router_queued = 0;
static volatile uint32_t g_router_queue_dropped = 0;
static volatile uint32_t g_router_queue_max = 0;
static volatile uint32_t g_router_ack_up = 0;
static volatile uint32_t g_router_ack_drop = 0;

typedef struct {
    uint16_t len;
    uint8_t data[ROUTER_FORWARD_MAX_LEN];
} router_forward_packet_t;

static router_forward_packet_t g_forward_queue[ROUTER_FORWARD_QUEUE_DEPTH];
static uint8_t g_forward_queue_head = 0;
static uint8_t g_forward_queue_tail = 0;
static uint8_t g_forward_queue_count = 0;
static uint8_t g_forward_tx_buf[ROUTER_FORWARD_MAX_LEN];
static uint8_t g_forward_direct_buf[ROUTER_FORWARD_MAX_LEN];
static uint8_t g_router_ack_buf[BUSINESS_ACK_PACKET_LEN];

static uint16_t business_read_u16(const uint8_t *data, uint16_t offset)
{
    return ((uint16_t)data[offset] << 8) | data[offset + 1];
}

static void business_write_u16(uint8_t *data, uint16_t offset, uint16_t value)
{
    data[offset] = (uint8_t)(value >> 8);
    data[offset + 1] = (uint8_t)value;
}

static uint16_t business_checksum(const uint8_t *data, uint16_t len)
{
    uint32_t sum = 0;

    for (uint16_t i = 0; i < len; i++) {
        if (i == BUSINESS_CRC_HI_OFFSET || i == (BUSINESS_CRC_HI_OFFSET + 1)) {
            continue;
        }
        sum += data[i];
    }

    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static bool business_validate_packet(const uint8_t *data, uint16_t len, uint8_t type)
{
    uint16_t payload_len;
    uint16_t crc;

    if (data == NULL || len < BUSINESS_HEADER_LEN || data[0] != BUSINESS_MAGIC0 ||
        data[1] != BUSINESS_MAGIC1 || data[2] != type) {
        return false;
    }

    payload_len = business_read_u16(data, BUSINESS_LEN_HI_OFFSET);
    if ((uint16_t)(payload_len + BUSINESS_HEADER_LEN) != len) {
        return false;
    }

    crc = business_read_u16(data, BUSINESS_CRC_HI_OFFSET);
    return crc == business_checksum(data, len);
}

static void business_refresh_checksum(uint8_t *data, uint16_t len)
{
    business_write_u16(data, BUSINESS_CRC_HI_OFFSET, 0);
    business_write_u16(data, BUSINESS_CRC_HI_OFFSET, business_checksum(data, len));
}

static bool router_prepare_forward_packet(uint8_t *dst, uint16_t dst_size,
    const uint8_t *src, uint16_t len)
{
    if (dst == NULL || src == NULL || len == 0 || len > dst_size || len <= ROUTER_HOP_MARKER_OFFSET ||
        !business_validate_packet(src, len, BUSINESS_TYPE_DATA)) {
        g_router_dropped++;
        return false;
    }

    if (memcpy_s(dst, dst_size, src, len) != EOK) {
        g_router_dropped++;
        return false;
    }

    dst[ROUTER_HOP_MARKER_OFFSET]++;
    business_refresh_checksum(dst, len);
    return true;
}

static bool router_forward_queue_empty(void)
{
    bool empty;
    uint32_t irq_status = osal_irq_lock();

    empty = (g_forward_queue_count == 0);
    osal_irq_restore(irq_status);
    return empty;
}

static bool router_forward_queue_push(const uint8_t *data, uint16_t len)
{
    router_forward_packet_t *slot;
    uint32_t irq_status;

    if (data == NULL || len == 0 || len > ROUTER_FORWARD_MAX_LEN) {
        g_router_dropped++;
        return false;
    }

    irq_status = osal_irq_lock();
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

static bool router_forward_queue_peek(uint8_t *data, uint16_t *len)
{
    router_forward_packet_t *slot;
    uint32_t irq_status;
    bool ok = false;

    if (data == NULL || len == NULL) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_forward_queue_count > 0) {
        slot = &g_forward_queue[g_forward_queue_head];
        *len = slot->len;
        ok = (memcpy_s(data, ROUTER_FORWARD_MAX_LEN, slot->data, slot->len) == EOK);
    }
    osal_irq_restore(irq_status);
    return ok;
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

static void router_forward_queue_drop_all(void)
{
    uint8_t dropped;
    uint32_t irq_status = osal_irq_lock();

    dropped = g_forward_queue_count;
    g_forward_queue_head = 0;
    g_forward_queue_tail = 0;
    g_forward_queue_count = 0;
    osal_irq_restore(irq_status);

    g_router_dropped += dropped;
}

static void sle_uart_router_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    bool is_upstream;

    if (sle_uart_server_is_downstream_link(conn_id)) {
        sle_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
        return;
    }

    is_upstream = sle_uart_client_is_upstream_link(conn_id, addr);

    if (is_upstream) {
        sle_uart_client_sample_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
    } else {
        sle_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
    }
}

static void sle_uart_router_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    if (sle_uart_client_is_upstream_link(conn_id, addr) || !sle_uart_server_is_downstream_link(conn_id)) {
        sle_uart_client_sample_pair_complete_cbk(conn_id, addr, status);
    } else {
        sle_pair_complete_cbk(conn_id, addr, status);
    }
}

static void sle_uart_router_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    if (sle_uart_server_is_downstream_link(conn_id)) {
        sle_server_set_phy_cbk(conn_id, status, param);
    }
}

static void sle_uart_router_register_connection_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {
        .connect_state_changed_cb = sle_uart_router_connect_state_changed_cbk,
        .pair_complete_cb = sle_uart_router_pair_complete_cbk,
        .set_phy_cb = sle_uart_router_set_phy_cbk
    };
    sle_connection_register_callbacks(&conn_cbks);
}

static void sle_uart_router_print_stats(void)
{
    static uint64_t last_print_us = 0;
    static uint64_t last_clean_us = 0;
    static uint32_t last_forwarded = 0;
    static uint32_t last_dropped = 0;
    static uint32_t last_tx_failed = 0;
    static uint32_t last_credit_wait = 0;
    static uint32_t last_queued = 0;
    static uint32_t last_queue_dropped = 0;
    static uint32_t last_ack_up = 0;
    static uint32_t last_ack_drop = 0;
    uint64_t now = uapi_tcxo_get_us();
    uint8_t queue_count;
    uint32_t delta_forwarded;
    uint32_t delta_dropped;
    uint32_t delta_tx_failed;
    uint32_t delta_credit_wait;
    uint32_t delta_queued;
    uint32_t delta_queue_dropped;
    uint32_t delta_ack_up;
    uint32_t delta_ack_drop;

    if (last_print_us == 0) {
        last_print_us = now;
        last_clean_us = now;
        return;
    }

    if (now - last_print_us < ROUTER_STATS_INTERVAL_US) {
        return;
    }

    queue_count = g_forward_queue_count;
    delta_forwarded = g_router_forwarded - last_forwarded;
    delta_dropped = g_router_dropped - last_dropped;
    delta_tx_failed = g_router_tx_failed - last_tx_failed;
    delta_credit_wait = g_router_credit_wait - last_credit_wait;
    delta_queued = g_router_queued - last_queued;
    delta_queue_dropped = g_router_queue_dropped - last_queue_dropped;
    delta_ack_up = g_router_ack_up - last_ack_up;
    delta_ack_drop = g_router_ack_drop - last_ack_drop;

    if (delta_queue_dropped > 0 || delta_tx_failed > 0 || delta_credit_wait > ROUTER_CREDIT_WAIT_HIGH) {
        last_clean_us = now;
        sle_uart_server_set_link_profile(SLE_UART_LINK_PROFILE_LONG_RANGE);
    } else if (now - last_clean_us >= ROUTER_STABLE_PROMOTE_US) {
#if SLE_OUTDOOR_LONG_RANGE
        sle_uart_server_set_link_profile(SLE_UART_LINK_PROFILE_STABLE);
#else
        sle_uart_server_set_link_profile(SLE_UART_LINK_PROFILE_HIGH_SPEED);
#endif
        last_clean_us = now;
    }

    osal_printk("[Router Stats] router_id:%u profile:%u fwd:%u drop:%u tx_fail:%u credit_wait:%u queued:%u q_drop:%u q_in:%u q_max:%u ack_up:%u ack_drop:%u\r\n",
        ROUTER_NODE_ID,
        (uint8_t)sle_uart_server_get_link_profile(),
        delta_forwarded,
        delta_dropped,
        delta_tx_failed,
        delta_credit_wait,
        delta_queued,
        delta_queue_dropped,
        queue_count,
        g_router_queue_max,
        delta_ack_up,
        delta_ack_drop);

    last_forwarded = g_router_forwarded;
    last_dropped = g_router_dropped;
    last_tx_failed = g_router_tx_failed;
    last_credit_wait = g_router_credit_wait;
    last_queued = g_router_queued;
    last_queue_dropped = g_router_queue_dropped;
    last_ack_up = g_router_ack_up;
    last_ack_drop = g_router_ack_drop;
    last_print_us = now;
}

void sle_uart_router_notify_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    uint32_t retry;
    uint32_t waited = 0;
    errcode_t ret;

    unused(client_id);
    unused(conn_id);

    if (status != ERRCODE_SUCC || data == NULL || data->data == NULL || data->data_len == 0) {
        return;
    }

    if (!sle_uart_server_has_downstream() || !g_mtu_negotiated || data->data_len > ROUTER_FORWARD_MAX_LEN) {
        g_router_dropped++;
        return;
    }

    if (!router_prepare_forward_packet(g_forward_direct_buf, sizeof(g_forward_direct_buf), data->data, data->data_len)) {
        return;
    }

    if (!router_forward_queue_empty()) {
        router_forward_queue_push(g_forward_direct_buf, data->data_len);
        return;
    }

    for (retry = 0; retry < ROUTER_FORWARD_RETRY_COUNT; retry++) {
        if (gle_tx_acb_data_num_get() > 0) {
            ret = sle_uart_server_send_report_by_handle(g_forward_direct_buf, data->data_len);
            if (ret == ERRCODE_SUCC) {
                g_router_forwarded++;
                g_router_credit_wait += waited;
                return;
            } else {
                g_router_tx_failed++;
                break;
            }
        }

        waited++;
        osal_udelay(ROUTER_FORWARD_RETRY_US);
    }

    g_router_credit_wait += waited;
    router_forward_queue_push(g_forward_direct_buf, data->data_len);
}

void sle_uart_router_indic_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void sle_uart_router_downstream_write_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);

    if (status != ERRCODE_SUCC || write_cb_para == NULL || write_cb_para->value == NULL ||
        !business_validate_packet(write_cb_para->value, write_cb_para->length, BUSINESS_TYPE_ACK)) {
        g_router_ack_drop++;
        return;
    }

    if (write_cb_para->length > sizeof(g_router_ack_buf) ||
        memcpy_s(g_router_ack_buf, sizeof(g_router_ack_buf), write_cb_para->value, write_cb_para->length) != EOK) {
        g_router_ack_drop++;
        return;
    }

    if (sle_uart_client_write_upstream_cmd(g_router_ack_buf, write_cb_para->length) == ERRCODE_SUCC) {
        g_router_ack_up++;
    } else {
        g_router_ack_drop++;
    }
}

extern errcode_t sle_uart_announce_register_cbks(void);

static void *sle_uart_router_tx_task(const char *arg)
{
    uint8_t tx_fail_streak = 0;

    unused(arg);

    while (1) {
        uint16_t len = 0;
        errcode_t ret;

        if (!sle_uart_server_has_downstream() || !g_mtu_negotiated) {
            router_forward_queue_drop_all();
            tx_fail_streak = 0;
            osal_msleep(ROUTER_TX_IDLE_SLEEP_MS);
            continue;
        }

        if (!router_forward_queue_peek(g_forward_tx_buf, &len)) {
            tx_fail_streak = 0;
            osal_msleep(ROUTER_TX_IDLE_SLEEP_MS);
            continue;
        }

        if (gle_tx_acb_data_num_get() == 0) {
            g_router_credit_wait++;
            osal_udelay(ROUTER_FORWARD_RETRY_US);
            continue;
        }

        ret = sle_uart_server_send_report_by_handle(g_forward_tx_buf, len);
        if (ret == ERRCODE_SUCC) {
            g_router_forwarded++;
            tx_fail_streak = 0;
            router_forward_queue_pop();
        } else {
            g_router_tx_failed++;
            tx_fail_streak++;
            if (tx_fail_streak >= ROUTER_TX_FAIL_DROP_THRESHOLD) {
                g_router_dropped++;
                router_forward_queue_pop();
                tx_fail_streak = 0;
            }
            osal_udelay(ROUTER_FORWARD_RETRY_US);
        }
    }

    return NULL;
}

static void *sle_uart_router_task(const char *arg)
{
    unused(arg);
    osal_msleep(1000);

    sle_uart_server_init(NULL, sle_uart_router_downstream_write_cbk);
    sle_uart_client_init(sle_uart_router_notify_cb, sle_uart_router_indic_cb);
    sle_uart_router_register_connection_cbks();
    sle_uart_announce_register_cbks();
    enable_sle();

    osal_printk("[SLE Router] Direct-forward 5-hop router ready.\r\n");
    while (1) {
        if (!sle_uart_server_has_downstream()) {
            sle_uart_server_adv_init();
        }
        sle_uart_client_watchdog_tick();
        sle_uart_router_print_stats();
        osal_msleep(ROUTER_WATCHDOG_INTERVAL_MS);
    }

    return NULL;
}

static void sle_uart_entry(void)
{
    osal_task *task_handle = NULL;
    osal_task *tx_task_handle = NULL;

    uapi_watchdog_disable();
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_task, 0,
        "RouterTask", SLE_UART_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    tx_task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_tx_task, 0,
        "RouterTx", ROUTER_TX_TASK_STACK_SIZE);
    if (tx_task_handle != NULL) {
        osal_kthread_set_priority(tx_task_handle, ROUTER_TX_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
