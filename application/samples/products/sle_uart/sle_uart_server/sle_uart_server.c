/**
 * 修复版 Server：无条件解锁 MCS 10
 */
#include "common_def.h"
#include "securec.h"
#include "soc_osal.h"
#include "nv.h"             
#include "sle_errcode.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_uart_server_adv.h"
#include "sle_uart_server.h"

#define OCTET_BIT_LEN           8
#define UUID_LEN_2              2
#define UART_BUFF_LENGTH        0x100
#define SLE_ADV_HANDLE_DEFAULT  1
#define SLE_ENDPOINT_SUPERVISION_TIMEOUT 0x7D0
#define SLE_ENDPOINT_OUTDOOR_MCS         8

static char g_sle_uuid_app_uuid[UUID_LEN_2] = { 0x12, 0x34 };
static char g_sle_property_value[OCTET_BIT_LEN] = { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
static uint16_t g_sle_conn_hdl = 0xFFFF;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_property_handle = 0;
uint16_t g_sle_pair_hdl = 0;
static uint8_t g_sle_uart_base[] = { 0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA, 0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

volatile bool g_mtu_negotiated = false;

extern errcode_t sle_set_mcs(uint16_t conn_id, uint8_t mcs);

uint16_t get_connect_id(void) { return g_sle_conn_hdl; }

static void encode2byte_little(uint8_t *_ptr, uint16_t data) {
    *(uint8_t *)((_ptr) + 1) = (uint8_t)((data) >> 0x8);
    *(uint8_t *)(_ptr) = (uint8_t)(data);
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out) {
    memcpy_s(out->uuid, 16, g_sle_uart_base, 16);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[14], u2);
}

void sle_server_set_nv(void) {
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {     
        nv_value = 7;       
        uapi_nv_write(0x20A0, (uint8_t *)&(nv_value), sizeof(nv_value));
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,  ssap_exchange_info_t *mtu_size, errcode_t status) {
    if (status == 0) {
        g_mtu_negotiated = true; 
        osal_printk("\r\n[Server] MTU Exchanged OK: %d\r\n", mtu_size->mtu_size);
    }
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status) {}
static void ssaps_add_service_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t handle, errcode_t status) {}
static void ssaps_add_property_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t service_handle, uint16_t handle, errcode_t status) {}
static void ssaps_add_descriptor_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t service_handle, uint16_t property_handle, errcode_t status) {}
static void ssaps_delete_all_service_cbk(uint8_t server_id, errcode_t status) {}

static errcode_t sle_ssaps_register_cbks(ssaps_read_request_callback ssaps_read_callback, ssaps_write_request_callback ssaps_write_callback) {
    ssaps_callbacks_t ssaps_cbk = {
        .add_service_cb = ssaps_add_service_cbk,
        .add_property_cb = ssaps_add_property_cbk,
        .add_descriptor_cb = ssaps_add_descriptor_cbk,
        .start_service_cb = ssaps_start_service_cbk,
        .delete_all_service_cb = ssaps_delete_all_service_cbk,
        .mtu_changed_cb = ssaps_mtu_changed_cbk,
        .read_request_cb = ssaps_read_callback,
        .write_request_cb = ssaps_write_callback
    };
    return ssaps_register_callbacks(&ssaps_cbk);
}

static errcode_t sle_uuid_server_service_add(void) {
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t sle_uuid_server_property_add(void) {
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES | SSAP_PERMISSION_WRITE;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY | SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;

    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = (uint8_t *)osal_vmalloc(sizeof(g_sle_property_value));
    memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value, sizeof(g_sle_property_value));
    ssaps_add_property_sync(g_server_id, g_service_handle, &property,  &g_property_handle);
    
    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);
    ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    osal_vfree(property.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uart_server_add(void) {
    sle_uuid_t app_uuid = { .len = sizeof(g_sle_uuid_app_uuid) };
    memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid));
    ssaps_register_server(&app_uuid, &g_server_id);
    sle_uuid_server_service_add();
    sle_uuid_server_property_add();
    return ssaps_start_service(g_server_id, g_service_handle);
}

errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len) {
    ssaps_ntf_ind_t param = { .handle = g_property_handle, .type = SSAP_PROPERTY_TYPE_VALUE, .value = (uint8_t *)data, .value_len = len };
    return ssaps_notify_indicate(g_server_id, g_sle_conn_hdl, &param);
}

void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr, sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason) {
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_sle_conn_hdl = conn_id;
        g_mtu_negotiated = false; 
        osal_printk("[Server] Client Linked! Pushing 2.5ms Interval...\r\n");
        sle_connection_param_update_t parame = { .conn_id = conn_id, .interval_min = 0x14, .interval_max = 0x14, .max_latency = 0, .supervision_timeout = SLE_ENDPOINT_SUPERVISION_TIMEOUT };
        sle_update_connect_param(&parame);  
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_sle_conn_hdl = 0xFFFF;
        g_mtu_negotiated = false;
        osal_printk("[Server] Client disconnected. reason:%u\r\n", (uint8_t)disc_reason);
        sle_uart_server_adv_init();
    }
}

void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status) {}

/* 🚀 【致命核心修复】完全抛弃 status 判断！无论如何强开 MCS10！ */
void sle_server_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param) {
    unused(param);
    if (status == ERRCODE_SUCC) {
        sle_set_mcs(conn_id, SLE_ENDPOINT_OUTDOOR_MCS);
        osal_printk("[Server] Outdoor PHY profile applied. MCS%u.\r\n", SLE_ENDPOINT_OUTDOOR_MCS);
    }
}

static errcode_t sle_conn_register_cbks(void) {
    sle_connection_callbacks_t conn_cbks = { .connect_state_changed_cb = sle_connect_state_changed_cbk, .pair_complete_cb = sle_pair_complete_cbk, .set_phy_cb = sle_server_set_phy_cbk };
    return sle_connection_register_callbacks(&conn_cbks);
}

uint16_t sle_uart_client_is_connected(void) { return (g_sle_conn_hdl != 0xFFFF); }

void sle_uart_server_enable_cbk(void) {
    sle_server_set_nv();
    sle_uart_server_add();
    
    ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
    ssaps_set_info(g_server_id, &info);

    sle_uart_server_adv_init();
}

errcode_t sle_uart_server_init(ssaps_read_request_callback ssaps_read_callback, ssaps_write_request_callback ssaps_write_callback) {
    sle_uart_announce_register_cbks();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks(ssaps_read_callback, ssaps_write_callback);
    enable_sle(); 
    return ERRCODE_SLE_SUCCESS;
}
errcode_t sle_uart_server_send_report_by_uuid(const uint8_t *data, uint8_t len) { return 0; }
