/* samples/Hybrid_mode/sle_uart/sle_uart_hybrid.c */

#include "common_def.h"
#include "soc_osal.h"
#include "uart.h"                 
#include "sle_device_discovery.h" 
#include "sle_low_latency.h"
#include "sle_uart_server.h"
// 【关键修改】强制引用本地头文件，解决编译报错
#include "sle_uart_client/sle_uart_client.h" 
#include "securec.h"
#include "osal_debug.h"
#include "osal_task.h"
#include "sle_connection_manager.h"

extern uint16_t g_sle_conn_hdl;
extern uint16_t g_sle_uart_conn_id_hybrid;

// Server 回调声明
extern void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                          sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                          sle_disc_reason_t disc_reason);
extern void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);

// Client 回调声明
extern void sle_uart_client_sample_connect_state_changed_cbk_hybrid(uint16_t conn_id, const sle_addr_t *addr,
                                                             sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                                             sle_disc_reason_t disc_reason);
extern void sle_uart_client_sample_pair_complete_cbk_hybrid(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);

static void hybrid_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                             sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                             sle_disc_reason_t disc_reason)
{
    osal_printk("[Hybrid] Conn State Changed. ID: %d, State: %d\r\n", conn_id, conn_state);
    sle_connect_state_changed_cbk(conn_id, addr, conn_state, pair_state, disc_reason);
    sle_uart_client_sample_connect_state_changed_cbk_hybrid(conn_id, addr, conn_state, pair_state, disc_reason);
}

static void hybrid_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    osal_printk("[Hybrid] Pair Complete. ID: %d\r\n", conn_id);
    sle_pair_complete_cbk(conn_id, addr, status);
    sle_uart_client_sample_pair_complete_cbk_hybrid(conn_id, addr, status);
}

static void hybrid_register_callbacks(void)
{
    sle_connection_callbacks_t conn_cbks = {0};
    conn_cbks.connect_state_changed_cb = hybrid_connect_state_changed_cbk;
    conn_cbks.pair_complete_cb = hybrid_pair_complete_cbk;
    sle_connection_register_callbacks(&conn_cbks);
}

// Server 回调
static void hybrid_server_read_cb(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para, errcode_t status)
{
    osal_printk("[Hybrid Server] Read request.\r\n");
}
static void hybrid_server_write_cb(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    osal_printk("[Hybrid Server] Recv Data: %s\r\n", write_cb_para->value);
    #ifdef CONFIG_SLE_UART_BUS
    uapi_uart_write(CONFIG_SLE_UART_BUS, (uint8_t *)write_cb_para->value, write_cb_para->length, 0);
    #endif
}

// Client 回调
static void hybrid_client_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    osal_printk("[Hybrid Client] Recv Notify: %s\r\n", data->data);
    #ifdef CONFIG_SLE_UART_BUS
    uapi_uart_write(CONFIG_SLE_UART_BUS, (uint8_t *)data->data, data->data_len, 0);
    #endif
}
static void hybrid_client_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    osal_printk("[Hybrid Client] Recv Indicate.\r\n");
}

void sle_uart_hybrid_init(void)
{
    osal_printk("[Hybrid] Init Start...\r\n");
    if (enable_sle() != ERRCODE_SUCC) {
        osal_printk("[Hybrid] SLE Enable Failed! (Or already enabled)\r\n");
    }
    
    // Server Init
    sle_uart_server_init(hybrid_server_read_cb, hybrid_server_write_cb);
    osal_printk("[Hybrid] Server Init Done.\r\n");
    
    osal_msleep(100);

    // Client Init
    sle_uart_client_init_hybrid(hybrid_client_notification_cb, hybrid_client_indication_cb);
    osal_printk("[Hybrid] Client Init Done.\r\n");

    hybrid_register_callbacks();
    osal_printk("[Hybrid] Unified Callbacks Registered.\r\n");
}

void hybrid_send_to_client(uint8_t *data, uint16_t len)
{
    if (g_sle_conn_hdl != 0) {
        sle_uart_server_send_report_by_handle(data, len);
    }
}

void hybrid_send_to_server(uint8_t *data, uint16_t len)
{
    ssapc_write_param_t *param = get_g_sle_uart_send_param_hybrid();
    uint16_t conn_id = get_g_sle_uart_conn_id_hybrid();
    
    if (conn_id != 0 && param != NULL) {
        param->data = data;
        param->data_len = len;
        ssapc_write_req(0, conn_id, param);
    }
}