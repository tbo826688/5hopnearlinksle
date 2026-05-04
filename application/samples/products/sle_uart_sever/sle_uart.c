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
#include "tcxo.h"

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define ROUTER_FORWARD_RETRY_COUNT          12
#define ROUTER_FORWARD_RETRY_US             100
#define ROUTER_STATS_INTERVAL_US            5000000ULL
#define ROUTER_WATCHDOG_INTERVAL_MS         500

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

static volatile uint32_t g_router_forwarded = 0;
static volatile uint32_t g_router_dropped = 0;
static volatile uint32_t g_router_tx_failed = 0;
static volatile uint32_t g_router_credit_wait = 0;

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
    static uint32_t last_forwarded = 0;
    static uint32_t last_dropped = 0;
    static uint32_t last_tx_failed = 0;
    static uint32_t last_credit_wait = 0;
    uint64_t now = uapi_tcxo_get_us();

    if (last_print_us == 0) {
        last_print_us = now;
        return;
    }

    if (now - last_print_us < ROUTER_STATS_INTERVAL_US) {
        return;
    }

    osal_printk("[Router Stats] fwd:%u drop:%u tx_fail:%u credit_wait:%u\r\n",
        g_router_forwarded - last_forwarded,
        g_router_dropped - last_dropped,
        g_router_tx_failed - last_tx_failed,
        g_router_credit_wait - last_credit_wait);

    last_forwarded = g_router_forwarded;
    last_dropped = g_router_dropped;
    last_tx_failed = g_router_tx_failed;
    last_credit_wait = g_router_credit_wait;
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

    if (!sle_uart_server_has_downstream() || !g_mtu_negotiated) {
        g_router_dropped++;
        return;
    }

    for (retry = 0; retry < ROUTER_FORWARD_RETRY_COUNT; retry++) {
        if (gle_tx_acb_data_num_get() > 0) {
            ret = sle_uart_server_send_report_by_handle(data->data, data->data_len);
            if (ret == ERRCODE_SUCC) {
                g_router_forwarded++;
            } else {
                g_router_tx_failed++;
                g_router_dropped++;
            }
            g_router_credit_wait += waited;
            return;
        }

        waited++;
        osal_udelay(ROUTER_FORWARD_RETRY_US);
    }

    g_router_credit_wait += waited;
    g_router_dropped++;
}

void sle_uart_router_indic_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

extern errcode_t sle_uart_announce_register_cbks(void);

static void *sle_uart_router_task(const char *arg)
{
    unused(arg);
    osal_msleep(1000);

    sle_uart_server_init(NULL, NULL);
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

    uapi_watchdog_disable();
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_task, 0,
        "RouterTask", SLE_UART_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
