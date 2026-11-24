#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "watchdog.h"
#include "securec.h"
#include "sle_low_latency.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "tcxo.h"
#include "uapi_crc.h"
#include "route_bridge_uart.h"
#include "sle_router_config.h"
#include "../sle_uart/sle_file_transfer.h"

#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
#include "route_client.h"
#include "sle_ssap_client.h"
#elif defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_TX)
#include "route_server.h"
#include "route_server_adv.h"
#include "sle_ssap_server.h"
#else
#error "Select one route bridge side"
#endif

#define ROUTE_BRIDGE_CONTROL_TASK_PRIO       27
#define ROUTE_BRIDGE_UART_TASK_PRIO          25
#define ROUTE_BRIDGE_SLE_TX_TASK_PRIO        26
#define ROUTE_BRIDGE_TASK_STACK_SIZE         0x2000
#define ROUTE_BRIDGE_SLE_QUEUE_DEPTH         64
#define ROUTE_BRIDGE_SLE_PRIORITY_DEPTH      8
#define ROUTE_BRIDGE_CONTROL_INTERVAL_MS     100
#define ROUTE_BRIDGE_UART_INTERVAL_MS        1
#define ROUTE_BRIDGE_SLE_TX_IDLE_MS          1
#define ROUTE_BRIDGE_SLE_RETRY_US            100
#define ROUTE_BRIDGE_STATS_INTERVAL_US       5000000ULL
#define ROUTE_BRIDGE_UPSTREAM_READY_STABLE_US 1000000ULL
#define ROUTE_BRIDGE_UPSTREAM_HOLD_US         8000000ULL

typedef struct {
    uint16_t len;
    uint8_t data[ROUTE_BRIDGE_MAX_PAYLOAD];
} route_bridge_sle_packet_t;

static route_bridge_sle_packet_t g_sle_queue[ROUTE_BRIDGE_SLE_QUEUE_DEPTH];
static route_bridge_sle_packet_t g_sle_priority_queue[ROUTE_BRIDGE_SLE_PRIORITY_DEPTH];
static uint8_t g_sle_queue_head;
static uint8_t g_sle_queue_tail;
static uint8_t g_sle_queue_count;
static uint8_t g_sle_priority_head;
static uint8_t g_sle_priority_tail;
static uint8_t g_sle_priority_count;
static uint8_t g_sle_tx_buffer[ROUTE_BRIDGE_MAX_PAYLOAD];
static volatile uint32_t g_sle_queued;
static volatile uint32_t g_sle_forwarded;
static volatile uint32_t g_sle_dropped;
static volatile uint32_t g_sle_tx_failed;
static volatile uint32_t g_sle_credit_wait;

#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
static volatile bool g_sle_stack_enabled;
#else
static bool g_upstream_ready_latched;
static uint64_t g_upstream_ready_since_us;
static uint64_t g_upstream_not_ready_since_us;
#endif

static bool route_bridge_sle_is_priority(const uint8_t *data, uint16_t len)
{
    uint32_t magic;

    if (data == NULL || len != SLE_DIAG_PROBE_LEN ||
        memcpy_s(&magic, sizeof(magic), data, sizeof(magic)) != EOK) {
        return false;
    }
    return magic == SLE_DIAG_PROBE_MAGIC || magic == SLE_DIAG_REPORT_MAGIC;
}

static bool route_bridge_sle_queue_push(const uint8_t *data, uint16_t len, bool priority)
{
    route_bridge_sle_packet_t *queue;
    route_bridge_sle_packet_t *slot;
    uint8_t *tail;
    uint8_t *count;
    uint8_t depth;
    uint32_t irq_status;

    if (data == NULL || len == 0 || len > ROUTE_BRIDGE_MAX_PAYLOAD) {
        g_sle_dropped++;
        return false;
    }

    irq_status = osal_irq_lock();
    queue = priority ? g_sle_priority_queue : g_sle_queue;
    tail = priority ? &g_sle_priority_tail : &g_sle_queue_tail;
    count = priority ? &g_sle_priority_count : &g_sle_queue_count;
    depth = priority ? ROUTE_BRIDGE_SLE_PRIORITY_DEPTH : ROUTE_BRIDGE_SLE_QUEUE_DEPTH;
    if (*count >= depth) {
        g_sle_dropped++;
        osal_irq_restore(irq_status);
        return false;
    }
    slot = &queue[*tail];
    slot->len = len;
    if (memcpy_s(slot->data, sizeof(slot->data), data, len) != EOK) {
        g_sle_dropped++;
        osal_irq_restore(irq_status);
        return false;
    }
    *tail = (uint8_t)((*tail + 1u) % depth);
    (*count)++;
    g_sle_queued++;
    osal_irq_restore(irq_status);
    return true;
}

static bool route_bridge_sle_queue_peek(uint8_t *data, uint16_t *len, bool *priority)
{
    route_bridge_sle_packet_t *queue;
    route_bridge_sle_packet_t *slot;
    bool ready = false;
    uint32_t irq_status;

    if (data == NULL || len == NULL || priority == NULL) {
        return false;
    }
    irq_status = osal_irq_lock();
    if (g_sle_priority_count > 0 || g_sle_queue_count > 0) {
        *priority = g_sle_priority_count > 0;
        queue = *priority ? g_sle_priority_queue : g_sle_queue;
        slot = &queue[*priority ? g_sle_priority_head : g_sle_queue_head];
        *len = slot->len;
        ready = memcpy_s(data, ROUTE_BRIDGE_MAX_PAYLOAD, slot->data, slot->len) == EOK;
    }
    osal_irq_restore(irq_status);
    return ready;
}

static void route_bridge_sle_queue_pop(bool priority)
{
    uint32_t irq_status = osal_irq_lock();

    if (priority && g_sle_priority_count > 0) {
        g_sle_priority_head = (uint8_t)((g_sle_priority_head + 1u) % ROUTE_BRIDGE_SLE_PRIORITY_DEPTH);
        g_sle_priority_count--;
    } else if (!priority && g_sle_queue_count > 0) {
        g_sle_queue_head = (uint8_t)((g_sle_queue_head + 1u) % ROUTE_BRIDGE_SLE_QUEUE_DEPTH);
        g_sle_queue_count--;
    }
    osal_irq_restore(irq_status);
}

#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
static void route_bridge_rx_notify_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);

    if (status != ERRCODE_SUCC || data == NULL || data->data == NULL || data->data_len == 0) {
        return;
    }
    if (!route_bridge_uart_send(ROUTE_BRIDGE_FRAME_FORWARD, data->data, data->data_len)) {
        g_sle_dropped++;
    }
}

static void route_bridge_rx_indic_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void route_bridge_rx_uart_frame(uint8_t type, const uint8_t *data, uint16_t len)
{
    if (type == ROUTE_BRIDGE_FRAME_REVERSE) {
        (void)route_bridge_sle_queue_push(data, len,
            route_bridge_sle_is_priority(data, len));
    }
}

static void route_bridge_rx_connect_state_cb(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    route_client_sample_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
}

static void route_bridge_rx_pair_complete_cb(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    route_client_sample_pair_complete_cbk(conn_id, addr, status);
}

static void route_bridge_rx_stack_enable_cb(errcode_t status)
{
    if (status == ERRCODE_SUCC) {
        route_client_enable_cbk();
        g_sle_stack_enabled = true;
    }
}

static errcode_t route_bridge_rx_register_callbacks(void)
{
    sle_connection_callbacks_t connection_callbacks = {
        .connect_state_changed_cb = route_bridge_rx_connect_state_cb,
        .pair_complete_cb = route_bridge_rx_pair_complete_cb
    };
    sle_announce_seek_callbacks_t seek_callbacks = {
        .sle_enable_cb = route_bridge_rx_stack_enable_cb,
        .seek_enable_cb = route_client_sample_seek_enable_cbk,
        .seek_result_cb = route_client_sample_seek_result_info_cbk,
        .seek_disable_cb = route_client_sample_seek_disable_cbk
    };
    errcode_t ret;

    ret = sle_connection_register_callbacks(&connection_callbacks);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    return sle_announce_seek_register_callbacks(&seek_callbacks);
}
#endif

#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_TX)
extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_route_mtu_negotiated;

static bool route_bridge_tx_downstream_ready(void)
{
    return route_server_has_downstream() && g_route_mtu_negotiated;
}

static bool route_bridge_tx_peer_ready(void)
{
    return route_bridge_uart_peer_alive() &&
        (route_bridge_uart_peer_status() & ROUTE_BRIDGE_STATUS_SLE_READY) != 0;
}

static void route_bridge_tx_update_upstream_ready(void)
{
    bool ready = route_bridge_tx_peer_ready();
    uint64_t now = uapi_tcxo_get_us();

    if (ready) {
        if (g_upstream_ready_latched) {
            if (g_upstream_not_ready_since_us != 0) {
                osal_printk("[Bridge TX] R%u upstream recovered before downstream teardown.\r\n",
                    ROUTER_NODE_ID);
            }
            g_upstream_not_ready_since_us = 0;
            return;
        }
        if (g_upstream_ready_since_us == 0) {
            g_upstream_ready_since_us = now;
            return;
        }
        if (now - g_upstream_ready_since_us < ROUTE_BRIDGE_UPSTREAM_READY_STABLE_US) {
            return;
        }
        g_upstream_ready_latched = true;
        g_upstream_ready_since_us = 0;
        g_upstream_not_ready_since_us = 0;
        route_server_set_upstream_ready(true);
        osal_printk("[Bridge TX] R%u upstream stable; downstream hop enabled.\r\n",
            ROUTER_NODE_ID);
        return;
    }
    g_upstream_ready_since_us = 0;
    if (!g_upstream_ready_latched) {
        return;
    }
    if (g_upstream_not_ready_since_us == 0) {
        g_upstream_not_ready_since_us = now;
        osal_printk("[Bridge TX] R%u upstream transient; holding downstream for 8s.\r\n",
            ROUTER_NODE_ID);
        return;
    }
    if (now - g_upstream_not_ready_since_us < ROUTE_BRIDGE_UPSTREAM_HOLD_US) {
        return;
    }

    g_upstream_ready_latched = false;
    g_upstream_not_ready_since_us = 0;
    route_server_set_upstream_ready(false);
    osal_printk("[Bridge TX] R%u upstream loss confirmed; downstream closed.\r\n",
        ROUTER_NODE_ID);
}

static void route_bridge_tx_process_forward(const uint8_t *data, uint16_t len)
{
    sle_diag_probe_t probe;
    sle_diag_report_t report;
    uint32_t magic;
    uint16_t expected_path;
    uint16_t this_hop;

    if (data == NULL || len == 0) {
        return;
    }
    if (len == SLE_DIAG_PROBE_LEN &&
        memcpy_s(&magic, sizeof(magic), data, sizeof(magic)) == EOK &&
        magic == SLE_DIAG_PROBE_MAGIC) {
        if (memcpy_s(&probe, sizeof(probe), data, sizeof(probe)) != EOK ||
            uapi_crc16(0, (uint8_t *)&probe,
                SLE_DIAG_PROBE_LEN - sizeof(probe.crc16)) != probe.crc16) {
            g_sle_dropped++;
            return;
        }
        expected_path = (uint16_t)((1u << (ROUTER_NODE_ID - 1u)) - 1u);
        this_hop = (uint16_t)(1u << (ROUTER_NODE_ID - 1u));
        if (probe.reserved != expected_path) {
            g_sle_dropped++;
            return;
        }

        report.magic = SLE_DIAG_REPORT_MAGIC;
        report.probe_id = probe.probe_id;
        report.node_id = ROUTER_NODE_ID;
        report.flags = 0;
        if (route_bridge_tx_peer_ready()) {
            report.flags |= SLE_DIAG_FLAG_UPSTREAM_READY | SLE_DIAG_FLAG_REVERSE_READY;
        }
        if (route_server_has_downstream()) {
            report.flags |= SLE_DIAG_FLAG_DOWNSTREAM_LINK;
        }
        if (g_route_mtu_negotiated) {
            report.flags |= SLE_DIAG_FLAG_DOWNSTREAM_MTU;
        }
        report.crc16 = uapi_crc16(0, (uint8_t *)&report,
            SLE_DIAG_REPORT_LEN - sizeof(report.crc16));
        if (!route_bridge_uart_send(ROUTE_BRIDGE_FRAME_REVERSE,
            (uint8_t *)&report, sizeof(report))) {
            g_sle_dropped++;
        }
        if (route_bridge_tx_downstream_ready()) {
            probe.reserved |= this_hop;
            probe.crc16 = uapi_crc16(0, (uint8_t *)&probe,
                SLE_DIAG_PROBE_LEN - sizeof(probe.crc16));
            (void)route_bridge_sle_queue_push((uint8_t *)&probe, sizeof(probe), true);
        }
        return;
    }

    if (!route_bridge_tx_downstream_ready()) {
        g_sle_dropped++;
        return;
    }
    (void)route_bridge_sle_queue_push(data, len, false);
}

static void route_bridge_tx_uart_frame(uint8_t type, const uint8_t *data, uint16_t len)
{
    if (type == ROUTE_BRIDGE_FRAME_FORWARD) {
        route_bridge_tx_process_forward(data, len);
    }
}

static void route_bridge_tx_write_cb(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);

    if (status != ERRCODE_SUCC || write_cb_para == NULL || write_cb_para->value == NULL ||
        write_cb_para->length == 0 || write_cb_para->length > ROUTE_BRIDGE_MAX_PAYLOAD) {
        return;
    }
    if (!route_bridge_uart_send(ROUTE_BRIDGE_FRAME_REVERSE,
        write_cb_para->value, write_cb_para->length)) {
        g_sle_dropped++;
    }
}

static void route_bridge_tx_connect_state_cb(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    route_server_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
}

static void route_bridge_tx_pair_complete_cb(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    route_server_pair_complete_cbk(conn_id, addr, status);
}

static errcode_t route_bridge_tx_register_callbacks(void)
{
    sle_connection_callbacks_t callbacks = {
        .connect_state_changed_cb = route_bridge_tx_connect_state_cb,
        .pair_complete_cb = route_bridge_tx_pair_complete_cb,
        .set_phy_cb = route_server_set_phy_cbk
    };

    return sle_connection_register_callbacks(&callbacks);
}
#endif

static int route_bridge_uart_task(void *arg)
{
    unused(arg);

    while (1) {
        route_bridge_uart_process();
        osal_msleep(ROUTE_BRIDGE_UART_INTERVAL_MS);
    }
    return 0;
}

static int route_bridge_sle_tx_task(void *arg)
{
    uint16_t len;
    errcode_t ret;
    bool priority;

    unused(arg);
    while (1) {
#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
        if (!route_client_write_ready()) {
            osal_msleep(ROUTE_BRIDGE_SLE_TX_IDLE_MS);
            continue;
        }
#else
        if (!route_bridge_tx_downstream_ready()) {
            osal_msleep(ROUTE_BRIDGE_SLE_TX_IDLE_MS);
            continue;
        }
        if (gle_tx_acb_data_num_get() == 0) {
            g_sle_credit_wait++;
            osal_udelay(ROUTE_BRIDGE_SLE_RETRY_US);
            continue;
        }
#endif
        if (!route_bridge_sle_queue_peek(g_sle_tx_buffer, &len, &priority)) {
            osal_msleep(ROUTE_BRIDGE_SLE_TX_IDLE_MS);
            continue;
        }
#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
        ret = route_client_write_upstream(g_sle_tx_buffer, len);
#else
        ret = route_server_send_report_by_handle(g_sle_tx_buffer, len);
#endif
        if (ret == ERRCODE_SUCC) {
            route_bridge_sle_queue_pop(priority);
            g_sle_forwarded++;
        } else {
            g_sle_tx_failed++;
            osal_udelay(ROUTE_BRIDGE_SLE_RETRY_US);
        }
    }
    return 0;
}

static void route_bridge_print_stats(void)
{
    static uint64_t last_print_us;
    route_bridge_uart_stats_t uart_stats;
    uint64_t now = uapi_tcxo_get_us();

    if (last_print_us != 0 && now - last_print_us < ROUTE_BRIDGE_STATS_INTERVAL_US) {
        return;
    }
    last_print_us = now;
    route_bridge_uart_get_stats(&uart_stats);
    osal_printk("[Bridge Stats] peer:%u status:%02x uart_tx:%u uart_rx:%u uart_drop:%u uart_ovf:%u frame_err:%u crc_err:%u sle_q:%u sle_pq:%u sle_in:%u sle_ok:%u sle_drop:%u sle_fail:%u credit:%u\r\n",
        route_bridge_uart_peer_alive() ? 1 : 0,
        route_bridge_uart_peer_status(),
        uart_stats.tx_frames,
        uart_stats.rx_frames,
        uart_stats.tx_queue_drops,
        uart_stats.rx_overflow_bytes,
        uart_stats.frame_errors,
        uart_stats.crc_errors,
        g_sle_queue_count,
        g_sle_priority_count,
        g_sle_queued,
        g_sle_forwarded,
        g_sle_dropped,
        g_sle_tx_failed,
        g_sle_credit_wait);
}

static int route_bridge_control_task(void *arg)
{
    uint8_t local_status;

    unused(arg);
    osal_msleep(1000);

#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
    route_client_init(route_bridge_rx_notify_cb, route_bridge_rx_indic_cb);
    if (route_bridge_rx_register_callbacks() != ERRCODE_SUCC) {
        osal_printk("[Bridge RX] Callback registration failed.\r\n");
        return -1;
    }
    enable_sle();
    osal_printk("[Bridge RX] R%u scans:\"%s\" and forwards to UART1 GPIO15/16.\r\n",
        ROUTER_NODE_ID, TARGET_UPSTREAM_NAME);
#else
    route_server_init(NULL, route_bridge_tx_write_cb);
    if (route_bridge_tx_register_callbacks() != ERRCODE_SUCC ||
        route_server_register_announce_callbacks() != ERRCODE_SUCC) {
        osal_printk("[Bridge TX] Callback registration failed.\r\n");
        return -1;
    }
    enable_sle();
    osal_printk("[Bridge TX] R%u receives UART1 GPIO15/16 and advertises:\"%s\".\r\n",
        ROUTER_NODE_ID, MY_ROUTER_NAME);
#endif

    while (1) {
        local_status = 0;
#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
        if (g_sle_stack_enabled) {
            route_client_watchdog_tick();
        }
        if (route_client_is_connected()) {
            local_status |= ROUTE_BRIDGE_STATUS_SLE_LINK;
        }
        if (route_client_write_ready()) {
            local_status |= ROUTE_BRIDGE_STATUS_SLE_READY;
        }
#else
        if (route_server_stack_is_enabled()) {
            route_bridge_tx_update_upstream_ready();
            route_server_watchdog_tick();
        }
        if (route_server_has_downstream()) {
            local_status |= ROUTE_BRIDGE_STATUS_SLE_LINK;
        }
        if (route_bridge_tx_downstream_ready()) {
            local_status |= ROUTE_BRIDGE_STATUS_SLE_READY;
        }
#endif
        route_bridge_uart_set_local_status(local_status);
        route_bridge_print_stats();
        osal_msleep(ROUTE_BRIDGE_CONTROL_INTERVAL_MS);
    }
    return 0;
}

static void route_bridge_create_task(osal_kthread_handler handler, const char *name, uint32_t priority)
{
    osal_task *task_handle = osal_kthread_create(handler, NULL, name, ROUTE_BRIDGE_TASK_STACK_SIZE);

    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, priority);
    }
}

static void route_bridge_entry(void)
{
    errcode_t ret;

    uapi_watchdog_disable();
#if defined(CONFIG_SAMPLE_SUPPORT_ROUTE_BRIDGE_RX)
    ret = route_bridge_uart_init(ROUTE_BRIDGE_SIDE_RX, ROUTER_NODE_ID, route_bridge_rx_uart_frame);
#else
    ret = route_bridge_uart_init(ROUTE_BRIDGE_SIDE_TX, ROUTER_NODE_ID, route_bridge_tx_uart_frame);
#endif
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Bridge] UART1 init failed:%x.\r\n", ret);
        return;
    }

    osal_kthread_lock();
    route_bridge_create_task(route_bridge_uart_task,
        "BridgeUart", ROUTE_BRIDGE_UART_TASK_PRIO);
    route_bridge_create_task(route_bridge_sle_tx_task,
        "BridgeSleTx", ROUTE_BRIDGE_SLE_TX_TASK_PRIO);
    route_bridge_create_task(route_bridge_control_task,
        "BridgeControl", ROUTE_BRIDGE_CONTROL_TASK_PRIO);
    osal_kthread_unlock();
}

app_run(route_bridge_entry);
