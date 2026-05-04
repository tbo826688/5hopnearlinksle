/*
 * Router downstream server. The next hop connects to this server and receives
 * forwarded notifications.
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
#include "sle_low_latency.h"

#define OCTET_BIT_LEN           8
#define UUID_LEN_2              2
#define SLE_INVALID_CONN_ID     0xFFFF

static char g_sle_uuid_app_uuid[UUID_LEN_2] = { 0x12, 0x34 };
static char g_sle_property_value[OCTET_BIT_LEN] = { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
static uint16_t g_sle_conn_hdl = SLE_INVALID_CONN_ID;
static uint8_t g_server_id = 0;
static uint16_t g_service_handle = 0;
static uint16_t g_property_handle = 0;
uint16_t g_sle_pair_hdl = 0;
static uint8_t g_sle_uart_base[] = {
    0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA,
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

volatile bool g_mtu_negotiated = false;

extern errcode_t sle_set_mcs(uint16_t conn_id, uint8_t mcs);

uint16_t get_connect_id(void)
{
    return g_sle_conn_hdl;
}

bool sle_uart_server_has_downstream(void)
{
    return g_sle_conn_hdl != SLE_INVALID_CONN_ID;
}

bool sle_uart_server_is_downstream_link(uint16_t conn_id)
{
    return g_sle_conn_hdl != SLE_INVALID_CONN_ID && conn_id == g_sle_conn_hdl;
}

void sle_uart_server_clear_downstream(void)
{
    g_sle_conn_hdl = SLE_INVALID_CONN_ID;
    g_mtu_negotiated = false;
}

static void encode2byte_little(uint8_t *ptr, uint16_t data)
{
    ptr[1] = (uint8_t)(data >> 8);
    ptr[0] = (uint8_t)data;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    memcpy_s(out->uuid, 16, g_sle_uart_base, 16);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[14], u2);
}

void sle_server_set_nv(void)
{
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;

    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {
        nv_value = 7;
        uapi_nv_write(0x20A0, (uint8_t *)&nv_value, sizeof(nv_value));
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,
    ssap_exchange_info_t *mtu_size, errcode_t status)
{
    unused(server_id);

    if (status != ERRCODE_SUCC || !sle_uart_server_is_downstream_link(conn_id)) {
        return;
    }

    osal_printk("[Router Server] Downstream MTU OK:%u. Upgrading PHY.\r\n", mtu_size->mtu_size);
    sle_set_data_len(conn_id, 1500);

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
    g_mtu_negotiated = true;
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(handle);
    unused(status);
}

static void ssaps_add_service_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(uuid);
    unused(handle);
    unused(status);
}

static void ssaps_add_property_cbk(uint8_t server_id, sle_uuid_t *uuid,
    uint16_t service_handle, uint16_t handle, errcode_t status)
{
    unused(server_id);
    unused(uuid);
    unused(service_handle);
    unused(handle);
    unused(status);
}

static void ssaps_add_descriptor_cbk(uint8_t server_id, sle_uuid_t *uuid,
    uint16_t service_handle, uint16_t property_handle, errcode_t status)
{
    unused(server_id);
    unused(uuid);
    unused(service_handle);
    unused(property_handle);
    unused(status);
}

static void ssaps_delete_all_service_cbk(uint8_t server_id, errcode_t status)
{
    unused(server_id);
    unused(status);
}

static errcode_t sle_ssaps_register_cbks(ssaps_read_request_callback read_callback,
    ssaps_write_request_callback write_callback)
{
    ssaps_callbacks_t ssaps_cbk = {
        .add_service_cb = ssaps_add_service_cbk,
        .add_property_cb = ssaps_add_property_cbk,
        .add_descriptor_cb = ssaps_add_descriptor_cbk,
        .start_service_cb = ssaps_start_service_cbk,
        .delete_all_service_cb = ssaps_delete_all_service_cbk,
        .mtu_changed_cb = ssaps_mtu_changed_cbk,
        .read_request_cb = read_callback,
        .write_request_cb = write_callback
    };
    return ssaps_register_callbacks(&ssaps_cbk);
}

static errcode_t sle_uuid_server_service_add(void)
{
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    return ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
}

static errcode_t sle_uuid_server_property_add(void)
{
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES | SSAP_PERMISSION_WRITE;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY |
        SSAP_OPERATE_INDICATION_BIT_WRITE | SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP;

    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = (uint8_t *)osal_vmalloc(sizeof(g_sle_property_value));
    memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value, sizeof(g_sle_property_value));
    ssaps_add_property_sync(g_server_id, g_service_handle, &property, &g_property_handle);

    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);
    ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    osal_vfree(property.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uart_server_add(void)
{
    sle_uuid_t app_uuid = { .len = sizeof(g_sle_uuid_app_uuid) };

    memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid));
    ssaps_register_server(&app_uuid, &g_server_id);
    sle_uuid_server_service_add();
    sle_uuid_server_property_add();
    return ssaps_start_service(g_server_id, g_service_handle);
}

errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0 || g_sle_conn_hdl == SLE_INVALID_CONN_ID) {
        return ERRCODE_FAIL;
    }

    ssaps_ntf_ind_t param = {
        .handle = g_property_handle,
        .type = SSAP_PROPERTY_TYPE_VALUE,
        .value = (uint8_t *)data,
        .value_len = len
    };
    return ssaps_notify_indicate(g_server_id, g_sle_conn_hdl, &param);
}

void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(addr);
    unused(pair_state);
    unused(disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_sle_conn_hdl = conn_id;
        g_mtu_negotiated = false;
        sle_uart_server_adv_stop();
        osal_printk("[Router Server] Downstream connected. conn_id:%u\r\n", conn_id);

        sle_connection_param_update_t param = {
            .conn_id = conn_id,
            .interval_min = 0x14,
            .interval_max = 0x14,
            .max_latency = 0,
            .supervision_timeout = 0x1F4
        };
        sle_update_connect_param(&param);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        if (conn_id == g_sle_conn_hdl || g_sle_conn_hdl == SLE_INVALID_CONN_ID) {
            sle_uart_server_clear_downstream();
            osal_printk("[Router Server] Downstream disconnected. Restarting advertise.\r\n");
            sle_uart_server_adv_init();
        }
    }
}

void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(conn_id);
    unused(addr);
    unused(status);
}

void sle_server_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param)
{
    unused(param);

    if (!sle_uart_server_is_downstream_link(conn_id)) {
        return;
    }

    if (status == ERRCODE_SUCC) {
        sle_set_mcs(conn_id, 10);
        osal_printk("[Router Server] Downstream PHY upgraded to MCS10.\r\n");
    }
}

void sle_uart_server_enable_cbk(void)
{
    sle_server_set_nv();
    sle_uart_server_add();

    ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
    ssaps_set_info(g_server_id, &info);
    sle_uart_server_adv_init();
}

errcode_t sle_uart_server_init(ssaps_read_request_callback read_callback,
    ssaps_write_request_callback write_callback)
{
    sle_ssaps_register_cbks(read_callback, write_callback);
    return ERRCODE_SLE_SUCCESS;
}

errcode_t sle_uart_server_send_report_by_uuid(const uint8_t *data, uint8_t len)
{
    unused(data);
    unused(len);
    return ERRCODE_FAIL;
}
