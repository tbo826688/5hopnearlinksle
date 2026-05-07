/*
 * Endpoint speed test:
 * Node1 (server/source) -> R1 -> R2 -> R3 -> R4 -> Node6 (client/sink)
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

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define THROUGHPUT_PKT_LEN                  1400
#define THROUGHPUT_PACING_MS                10
#define SPEED_SAMPLE_INTERVAL_MS            1000
#define US_PER_SECOND                       1000000ULL

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
static uint8_t g_test_payload[THROUGHPUT_PKT_LEN];
static uint32_t g_source_credit_stall = 0;
static uint32_t g_source_tx_fail = 0;

static void sle_source_upgrade_link(uint16_t conn_id)
{
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
}

static void sle_throughput_tx_task(void)
{
    uint32_t pkt_cnt = 0;

    memset(g_test_payload, 'A', sizeof(g_test_payload));
    osal_printk("[Speed Test] Node1 source ready. pkt:%u pacing:%ums\r\n",
        THROUGHPUT_PKT_LEN, THROUGHPUT_PACING_MS);

    while (1) {
        while (!sle_uart_client_is_connected()) {
            osal_msleep(100);
        }

        uint16_t conn_id = get_connect_id();
        osal_printk("[Speed Test] R1 linked. Waiting MTU.\r\n");
        while (sle_uart_client_is_connected() && !g_mtu_negotiated) {
            osal_msleep(100);
        }

        if (!sle_uart_client_is_connected()) {
            continue;
        }

        osal_msleep(500);
        sle_source_upgrade_link(conn_id);
        osal_printk("[Speed Test] 5-hop source traffic started.\r\n");

        while (sle_uart_client_is_connected() && g_mtu_negotiated) {
            if (gle_tx_acb_data_num_get() > 0) {
                pkt_cnt++;
                g_test_payload[0] = (pkt_cnt >> 8) & 0xFF;
                g_test_payload[1] = pkt_cnt & 0xFF;
                if (sle_uart_server_send_report_by_handle(g_test_payload, THROUGHPUT_PKT_LEN) != ERRCODE_SUCC) {
                    g_source_tx_fail++;
                }
                osal_msleep(THROUGHPUT_PACING_MS);
            } else {
                g_source_credit_stall++;
                osal_yield();
            }
        }

        osal_printk("[Speed Test] source link lost. stall:%u tx_fail:%u\r\n",
            g_source_credit_stall, g_source_tx_fail);
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

static volatile uint32_t g_rx_bytes = 0;
static volatile uint32_t g_rx_packets = 0;
static volatile uint32_t g_lost_packets = 0;
static uint16_t g_expected_seq = 0;
static bool g_seq_valid = false;

typedef struct {
    uint32_t bytes;
    uint32_t packets;
    uint32_t lost_packets;
} sle_uart_rx_stats_t;

static void sle_uart_rx_stats_add(ssapc_handle_value_t *data)
{
    uint16_t seq;
    uint16_t lost_delta;
    uint32_t irq_status = osal_irq_lock();

    g_rx_bytes += data->data_len;
    g_rx_packets++;

    if (data->data_len >= 2) {
        seq = ((uint16_t)data->data[0] << 8) | data->data[1];
        if (g_seq_valid) {
            lost_delta = (uint16_t)(seq - g_expected_seq);
            if (lost_delta != 0) {
                g_lost_packets += lost_delta;
            }
        } else {
            g_seq_valid = true;
        }
        g_expected_seq = (uint16_t)(seq + 1);
    }

    osal_irq_restore(irq_status);
}

static void sle_uart_rx_stats_take(sle_uart_rx_stats_t *stats)
{
    uint32_t irq_status = osal_irq_lock();
    stats->bytes = g_rx_bytes;
    stats->packets = g_rx_packets;
    stats->lost_packets = g_lost_packets;
    g_rx_bytes = 0;
    g_rx_packets = 0;
    g_lost_packets = 0;
    osal_irq_restore(irq_status);
}

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data_len == 0) {
        return;
    }

    sle_uart_rx_stats_add(data);
}

void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void *sle_uart_client_task(const char *arg)
{
    unused(arg);
    uint64_t last_time_us;

    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);
    osal_printk("[Speed Test] Node6 sink ready. Awaiting 5-hop data.\r\n");
    last_time_us = uapi_tcxo_get_us();

    while (1) {
        uint64_t now_us;
        uint64_t elapsed_us;
        sle_uart_rx_stats_t stats;
        uint32_t bytes_per_sec;
        uint32_t kbps;

        osal_msleep(SPEED_SAMPLE_INTERVAL_MS);
        now_us = uapi_tcxo_get_us();
        elapsed_us = now_us - last_time_us;
        last_time_us = now_us;

        sle_uart_rx_stats_take(&stats);
        if (stats.bytes == 0 || elapsed_us == 0) {
            continue;
        }

        bytes_per_sec = (uint32_t)(((uint64_t)stats.bytes * US_PER_SECOND) / elapsed_us);
        kbps = (bytes_per_sec * 8) / 1000;
        osal_printk("[5-HOP E2E LIMIT] Speed: %u Bytes/s (%u kbps), rx_pkt:%u lost:%u\r\n",
            bytes_per_sec, kbps, stats.packets, stats.lost_packets);
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
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_server_task, 0,
        "ThroughputTX", SLE_UART_TASK_STACK_SIZE);
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_client_task, 0,
        "ThroughputRX", SLE_UART_TASK_STACK_SIZE);
#endif
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
