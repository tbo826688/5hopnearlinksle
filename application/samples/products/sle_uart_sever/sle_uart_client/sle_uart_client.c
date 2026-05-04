/**
 * Client 驱动 (连接上一跳)
 * 路径: products/sle_uart_sever/sle_uart_client/sle_uart_client.c
 */
#include "common_def.h"
#include "soc_osal.h"
#include "securec.h"
#include "product.h"
#include "nv.h"
#include "bts_le_gap.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_uart_client.h"
#include <string.h> 

// 🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴
// 调车师傅注意：修改这里的名字，代表“我要连谁”（上一跳的名字）
// R1 烧录时填 "SLE_SVR" (假设你的源头发送板子名字叫SLE_SVR)
// R2 烧录时填 "SLE_R1"
// R3 烧录时填 "SLE_R2"
// R4 烧录时填 "SLE_R3"
#define TARGET_UPSTREAM_NAME "SLE_SVR"
// 🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴🔴

static sle_connection_callbacks_t g_sle_uart_connect_cbk = { 0 };
static ssapc_callbacks_t g_sle_uart_ssapc_cbk = { 0 };
static sle_addr_t g_sle_uart_remote_addr = { 0 };
static bool g_remote_addr_valid = false; 

uint16_t g_sle_uart_conn_id = 0xFFFF;
uint16_t get_g_sle_uart_conn_id(void) { return g_sle_uart_conn_id; }
bool sle_uart_client_is_connected(void) { return (g_sle_uart_conn_id != 0xFFFF); }

void sle_client_set_nv(void) {
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {     
        nv_value = 7;       
        uapi_nv_write(0x20A0, (uint8_t *)&(nv_value), sizeof(nv_value));
    }
}

void sle_speed_connect_param_init(void) {
    sle_default_connect_param_t param = {0};
    param.enable_filter_policy = 0;
    param.gt_negotiate = SLE_ANNOUNCE_ROLE_G_CAN_NEGO;
    param.initiate_phys = 1;
    param.max_interval = 0x14; 
    param.min_interval = 0x14; 
    param.scan_interval = 400;
    param.scan_window = 20;
    param.timeout = 0x1f4;      
    sle_default_connection_param_set(&param);
}

void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data) {
    if (seek_result_data == NULL) return;
    if (strstr((const char *)seek_result_data->data, TARGET_UPSTREAM_NAME) != NULL) {
        memcpy_s(&g_sle_uart_remote_addr, sizeof(sle_addr_t), &seek_result_data->addr, sizeof(sle_addr_t));
        g_remote_addr_valid = true;
        sle_stop_seek();
    }
}

void sle_uart_start_scan(void) {
    if (g_sle_uart_conn_id != 0xFFFF) return; 
    sle_seek_param_t param = { .own_addr_type = 0, .seek_phys = 1, .seek_type = {1}, .seek_interval = {100}, .seek_window = {100} };
    sle_set_seek_param(&param);
    sle_start_seek();
}

void sle_uart_client_enable_cbk(void) {
    sle_client_set_nv();
    sle_speed_connect_param_init(); 
    sle_uart_start_scan();
}

void sle_uart_client_sample_seek_enable_cbk(errcode_t status) { (void)status; }
void sle_uart_client_sample_seek_disable_cbk(errcode_t status) {
    if (status == 0 && g_remote_addr_valid) {
        if (sle_connect_remote_device(&g_sle_uart_remote_addr) != 0) sle_uart_start_scan();
    }
}

void sle_uart_client_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr, sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason) {
    unused(pair_state);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_sle_uart_conn_id = conn_id;
        osal_printk("[Router Client] Connected to PREV Node! [ID: %d]\r\n", conn_id);
        if (pair_state == SLE_PAIR_NONE) sle_pair_remote_device(&g_sle_uart_remote_addr);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_sle_uart_conn_id = 0xFFFF;
        sle_uart_start_scan();
    }
}

void sle_uart_client_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status) {
    if (status == 0 && conn_id == g_sle_uart_conn_id) {
        ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
        ssapc_exchange_info_req(1, g_sle_uart_conn_id, &info);
    }
}

static void sle_uart_client_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param, errcode_t status) {
    if (status == 0) {
        ssapc_find_structure_param_t find_param = { .type = SSAP_FIND_TYPE_PRIMARY_SERVICE, .start_hdl = 1, .end_hdl = 0xFFFF };
        ssapc_find_structure(client_id, conn_id, &find_param);
    }
}

void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb) {
    g_sle_uart_connect_cbk.connect_state_changed_cb = sle_uart_client_sample_connect_state_changed_cbk;
    g_sle_uart_connect_cbk.pair_complete_cb =  sle_uart_client_sample_pair_complete_cbk;
    sle_connection_register_callbacks(&g_sle_uart_connect_cbk);

    g_sle_uart_ssapc_cbk.exchange_info_cb = sle_uart_client_sample_exchange_info_cbk;
    g_sle_uart_ssapc_cbk.notification_cb = notification_cb;
    g_sle_uart_ssapc_cbk.indication_cb = indication_cb;
    ssapc_register_callbacks(&g_sle_uart_ssapc_cbk);
}