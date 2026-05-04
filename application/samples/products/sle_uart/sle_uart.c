/**
 * 极速测速 Endpoint (Node 1: 源头发送端, Node 6: 终点接收端)
 * 路径: products/sle_uart/sle_uart.c
 * 状态: 包含 5跳 救命限流延时 (Pacing)
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
#define THROUGHPUT_PKT_LEN                  1200 

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated; 

/* =============================================================
   逻辑 A: Server (Node 1 - 源头发送端，必须带限速)
   ============================================================= */
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
static uint8_t g_test_payload[THROUGHPUT_PKT_LEN];

static void sle_throughput_tx_task(void) {
    memset(g_test_payload, 'A', sizeof(g_test_payload));
    osal_printk("[Speed Test] Node 1 (Source) Ready. Waiting for R1...\r\n");

    while (!sle_uart_client_is_connected()) {
        osal_msleep(100);
    }
    uint16_t conn_id = get_connect_id();
    
    osal_printk("\r\n[Speed Test] R1 Linked! Waiting for MTU Negotiation...\r\n");

    while (!g_mtu_negotiated) {
        osal_msleep(100);
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
    
    osal_printk("[Speed Test] 🚀 5-Hop Pacing Burst Start! 🚀\r\n");
    
    uint32_t pkt_cnt = 0;
    while (1) {
        if (gle_tx_acb_data_num_get() > 0) {
            pkt_cnt++;
            g_test_payload[0] = (pkt_cnt >> 8) & 0xFF;
            g_test_payload[1] = pkt_cnt & 0xFF;
            sle_uart_server_send_report_by_handle(g_test_payload, THROUGHPUT_PKT_LEN);
            
            // 🔴🔴🔴 救命延时！在5跳网络中，源头必须限速到约 800 kbps
            // 不加这行，中间路由器会瞬间拥塞死机！
            osal_msleep(10); 
        } else {
            // 如果底层意外满了，稍微让出CPU
            osal_yield();
        }
    }
}

static void *sle_uart_server_task(const char *arg) {
    unused(arg);
    sle_uart_server_init(NULL, NULL);
    sle_throughput_tx_task();
    return NULL;
}

/* =============================================================
   逻辑 B: Client (Node 6 - 终点接收端，纯打印测速)
   ============================================================= */
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)

static uint32_t g_rx_bytes = 0;
static uint64_t g_last_time_us = 0;

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status) {
    if (data && data->data_len > 0) {
        g_rx_bytes += data->data_len;
        uint64_t current_time_us = uapi_tcxo_get_us();

        if (g_last_time_us == 0) {
            g_last_time_us = current_time_us;
        } else if (current_time_us - g_last_time_us >= 1000000) {
            uint32_t bytes_per_sec = g_rx_bytes;
            uint32_t kbps = (bytes_per_sec * 8) / 1000;
            
            osal_printk("[5-HOP E2E LIMIT] Speed: %u Bytes/s (%u kbps)\r\n", bytes_per_sec, kbps);
            
            g_rx_bytes = 0;
            g_last_time_us = current_time_us;
        }
    }
}
void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status) {}

static void *sle_uart_client_task(const char *arg) {
    unused(arg);
    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);
    osal_printk("[Speed Test] Node 6 (Sink) Ready. Awaiting 5-Hop Data...\r\n");
    while (1) osal_msleep(5000);
    return NULL;
}
#endif

static void sle_uart_entry(void) {
    osal_task *task_handle = NULL;
    uapi_watchdog_disable(); 
    osal_kthread_lock();
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_server_task, 0, "ThroughputTX", SLE_UART_TASK_STACK_SIZE);
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_client_task, 0, "ThroughputRX", SLE_UART_TASK_STACK_SIZE);
#endif
    if (task_handle != NULL) osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO); 
    osal_kthread_unlock();
}
app_run(sle_uart_entry);