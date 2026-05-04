/**
 * 5级路由双向透传 - 极速下行瀑布流版 (中间路由器 R1~R4 专用)
 * 路径: products/sle_uart_sever/sle_uart.c
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

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated; // Server 端的 MTU 锁

/* 🚀 核心瀑布流转发 (Client收到上一跳Notify -> Server发给下一跳Notify) */
void sle_uart_router_notify_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    if (data && data->data_len > 0) {
        // 防雪崩机制：下一跳已经连上、MTU就绪，且底层射频队列还有空位时，才转发！
        if (sle_uart_client_is_connected() && g_mtu_negotiated) {
            if (gle_tx_acb_data_num_get() > 0) {
                sle_uart_server_send_report_by_handle(data->data, data->data_len);
            }
        }
    }
}
void sle_uart_router_indic_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status) {}

extern errcode_t sle_uart_announce_register_cbks(void);

static void *sle_uart_router_task(const char *arg)
{
    unused(arg);
    osal_msleep(1000);

    // 初始化 Server 和 Client 的逻辑
    sle_uart_server_init(NULL, NULL);
    sle_uart_client_init(sle_uart_router_notify_cb, sle_uart_router_indic_cb); 
    
    // 统一注册广播和扫描回调，并启动协议栈
    sle_uart_announce_register_cbks();
    enable_sle();

    osal_printk("[SLE Router] High-Speed Downlink Router Ready.\r\n");
    while (1) osal_msleep(5000);
    return NULL;
}

static void sle_uart_entry(void) {
    osal_task *task_handle = NULL;
    uapi_watchdog_disable(); 
    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_router_task, 0, "RouterTask", SLE_UART_TASK_STACK_SIZE);
    if (task_handle != NULL) osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    osal_kthread_unlock();
}
app_run(sle_uart_entry);