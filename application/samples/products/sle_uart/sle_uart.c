/**
 * Endpoint speed test sample.
 * Node 1: source sender.
 * Node 6: sink receiver.
 */
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "pinctrl.h"
#include "uart.h"
#include "tcxo.h"
#include "watchdog.h"
#include "sle_low_latency.h"
#include "securec.h"
#include "sle_connection_manager.h"

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
#include "sle_ssap_client.h"
#include "sle_uart_client.h"
#endif

#define SLE_UART_TASK_PRIO           27
#define SLE_UART_TASK_STACK_SIZE     0x2000
#define THROUGHPUT_PKT_LEN           1400
#define SOURCE_TX_INTERVAL_MS        10
#define SERVER_RECOVERY_INTERVAL_MS  5000

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
static uint8_t g_test_payload[THROUGHPUT_PKT_LEN];

static uint16_t sle_wait_for_source_link(void)
{
    uint32_t wait_ms = 0;

    while (!sle_uart_client_is_connected()) {
        osal_msleep(100);
        wait_ms += 100;
        if (wait_ms >= SERVER_RECOVERY_INTERVAL_MS) {
            wait_ms = 0;
            (void)sle_uart_server_adv_init();
        }
    }

    return get_connect_id();
}

static bool sle_prepare_source_link(uint16_t conn_id)
{
    osal_printk("\r\n[Speed Test] R1 linked. Waiting for MTU negotiation...\r\n");

    while (sle_uart_client_is_connected() && (get_connect_id() == conn_id) && !g_mtu_negotiated) {
        osal_msleep(100);
    }
    if (!sle_uart_client_is_connected() || (get_connect_id() != conn_id)) {
        return false;
    }

    osal_msleep(500);
    sle_set_data_len(conn_id, 1500);
    osal_msleep(100);

    sle_set_phy_t phy_parm = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_PHY_4M,
        .rx_phy = SLE_PHY_4M,
        .tx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .rx_pilot_density = SLE_PHY_PILOT_DENSITY_16_TO_1,
        .g_feedback = 0,
        .t_feedback = 0,
    };
    sle_set_phy_param(conn_id, &phy_parm);

    osal_printk("[Speed Test] Negotiating 4M PHY & MCS10. Waiting 2s...\r\n");
    osal_msleep(2000);
    return sle_uart_client_is_connected() && g_mtu_negotiated && (get_connect_id() == conn_id);
}

static void sle_throughput_tx_task(void)
{
    uint32_t pkt_cnt = 0;

    memset(g_test_payload, 'A', sizeof(g_test_payload));
    osal_printk("[Speed Test] Node 1 (Source) ready. Waiting for R1...\r\n");

    while (1) {
        uint16_t conn_id = sle_wait_for_source_link();
        if (!sle_prepare_source_link(conn_id)) {
            continue;
        }

        osal_printk("[Speed Test] 5-hop pacing burst start.\r\n");
        while (sle_uart_client_is_connected() && g_mtu_negotiated && (get_connect_id() == conn_id)) {
            if (gle_tx_acb_data_num_get() > 0) {
                pkt_cnt++;
                g_test_payload[0] = (pkt_cnt >> 8) & 0xFF;
                g_test_payload[1] = pkt_cnt & 0xFF;
                (void)sle_uart_server_send_report_by_handle(g_test_payload, THROUGHPUT_PKT_LEN);
                osal_msleep(SOURCE_TX_INTERVAL_MS);
            } else {
                osal_yield();
            }
        }

        osal_printk("[Speed Test] R1 disconnected. Waiting for auto-reconnect...\r\n");
    }
}

static void *sle_uart_server_task(const char *arg)
{
    unused(arg);
    sle_uart_server_init(NULL, NULL);
    sle_throughput_tx_task();
    return NULL;
}

#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)

static uint32_t g_rx_bytes = 0;
static uint64_t g_last_time_us = 0;

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data_len == 0) {
        return;
    }

    g_rx_bytes += data->data_len;
    uint64_t current_time_us = uapi_tcxo_get_us();

    if (g_last_time_us == 0) {
        g_last_time_us = current_time_us;
        return;
    }

    if (current_time_us - g_last_time_us >= 1000000) {
        uint32_t bytes_per_sec = g_rx_bytes;
        uint32_t kbps = (bytes_per_sec * 8) / 1000;

        osal_printk("[5-HOP E2E LIMIT] Speed: %u Bytes/s (%u kbps)\r\n", bytes_per_sec, kbps);

        g_rx_bytes = 0;
        g_last_time_us = current_time_us;
    }
}

void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void *sle_uart_client_task(const char *arg)
{
    unused(arg);
    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);
    osal_printk("[Speed Test] Node 6 (Sink) ready. Awaiting 5-hop data...\r\n");

    while (1) {
        osal_msleep(1000);
        if (get_g_sle_uart_conn_id() == 0xFFFF) {
            g_rx_bytes = 0;
            g_last_time_us = 0;
        }
        sle_uart_client_watchdog_tick();
    }
    return NULL;
}
#endif

static void sle_uart_entry(void)
{
    osal_task *task_handle = NULL;

    uapi_watchdog_disable();
    osal_kthread_lock();
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_server_task, 0, "ThroughputTX",
        SLE_UART_TASK_STACK_SIZE);
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_client_task, 0, "ThroughputRX",
        SLE_UART_TASK_STACK_SIZE);
#endif
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
