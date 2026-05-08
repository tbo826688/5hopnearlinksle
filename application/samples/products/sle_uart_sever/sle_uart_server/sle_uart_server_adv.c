/*
 * Router downstream advertise configuration.
 */
#include "common_def.h"
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "product.h"
#include "sle_common.h"
#include "sle_uart_server.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "osal_debug.h"
#include "osal_task.h"
#include "string.h"
#include "sle_uart_server_adv.h"
#include "sle_router_config.h"

#define NAME_MAX_LENGTH 16
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT      0x1F4
#define SLE_CONN_INTV_MIN_DEFAULT                 0x14
#define SLE_CONN_INTV_MAX_DEFAULT                 0x14
#define SLE_ADV_INTERVAL_MIN_DEFAULT              0x20
#define SLE_ADV_INTERVAL_MAX_DEFAULT              0x20
#define SLE_CONN_MAX_LATENCY                      0
#define SLE_ADV_TX_POWER                          18
#define SLE_ADV_HANDLE_DEFAULT                    1
#define SLE_ADV_DATA_LEN_MAX                      251

static uint8_t sle_local_name[NAME_MAX_LENGTH] = MY_ROUTER_NAME;
static bool g_adv_started = false;

static uint16_t sle_set_adv_local_name(uint8_t *adv_data, uint16_t max_len)
{
    uint8_t index = 0;
    uint8_t *local_name = sle_local_name;
    uint8_t local_name_len = sizeof(sle_local_name) - 1;

    adv_data[index++] = local_name_len + 1;
    adv_data[index++] = SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME;
    if (memcpy_s(&adv_data[index], max_len - index, local_name, local_name_len) != EOK) {
        return 0;
    }
    return (uint16_t)index + local_name_len;
}

static uint16_t sle_set_adv_data(uint8_t *adv_data)
{
    size_t len = sizeof(struct sle_adv_common_value);
    uint16_t idx = 0;
    struct sle_adv_common_value adv_disc_level = {
        .length = len - 1,
        .type = SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
        .value = SLE_ANNOUNCE_LEVEL_NORMAL
    };
    struct sle_adv_common_value adv_access_mode = {
        .length = len - 1,
        .type = SLE_ADV_DATA_TYPE_ACCESS_MODE,
        .value = 0
    };

    if (memcpy_s(&adv_data[idx], SLE_ADV_DATA_LEN_MAX - idx, &adv_disc_level, len) != EOK) {
        return 0;
    }
    idx += len;
    if (memcpy_s(&adv_data[idx], SLE_ADV_DATA_LEN_MAX - idx, &adv_access_mode, len) != EOK) {
        return 0;
    }
    idx += len;
    return idx;
}

static uint16_t sle_set_scan_response_data(uint8_t *scan_rsp_data)
{
    uint16_t idx = 0;
    size_t scan_rsp_data_len = sizeof(struct sle_adv_common_value);
    struct sle_adv_common_value tx_power_level = {
        .length = scan_rsp_data_len - 1,
        .type = SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
        .value = SLE_ADV_TX_POWER
    };

    if (memcpy_s(scan_rsp_data, SLE_ADV_DATA_LEN_MAX, &tx_power_level, scan_rsp_data_len) != EOK) {
        return 0;
    }
    idx += scan_rsp_data_len;
    idx += sle_set_adv_local_name(&scan_rsp_data[idx], SLE_ADV_DATA_LEN_MAX - idx);
    return idx;
}

static int sle_set_default_announce_param(void)
{
    sle_announce_param_t param = {0};
    sle_addr_t local_addr_struct = {0};

    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = SLE_ADV_INTERVAL_MIN_DEFAULT;
    param.announce_interval_max = SLE_ADV_INTERVAL_MAX_DEFAULT;
    param.conn_interval_min = SLE_CONN_INTV_MIN_DEFAULT;
    param.conn_interval_max = SLE_CONN_INTV_MAX_DEFAULT;
    param.conn_max_latency = SLE_CONN_MAX_LATENCY;
    param.conn_supervision_timeout = SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT;
    param.announce_tx_power = SLE_ADV_TX_POWER;

    if (sle_get_local_addr(&local_addr_struct) == ERRCODE_SLE_SUCCESS) {
        param.own_addr.type = local_addr_struct.type;
        memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, local_addr_struct.addr, SLE_ADDR_LEN);
    }
    return sle_set_announce_param(param.announce_handle, &param);
}

static int sle_set_default_announce_data(void)
{
    sle_announce_data_t data = {0};
    uint8_t announce_data[SLE_ADV_DATA_LEN_MAX] = {0};
    uint8_t seek_rsp_data[SLE_ADV_DATA_LEN_MAX] = {0};

    data.announce_data = announce_data;
    data.announce_data_len = sle_set_adv_data(announce_data);
    data.seek_rsp_data = seek_rsp_data;
    data.seek_rsp_data_len = sle_set_scan_response_data(seek_rsp_data);
    return sle_set_announce_data(SLE_ADV_HANDLE_DEFAULT, &data);
}

extern void sle_uart_client_enable_cbk(void);
extern void sle_uart_client_sample_seek_enable_cbk(errcode_t status);
extern void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data);
extern void sle_uart_client_sample_seek_disable_cbk(errcode_t status);

void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status == ERRCODE_SUCC) {
        g_adv_started = true;
    }
}

void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    unused(announce_id);
    if (status == ERRCODE_SUCC) {
        g_adv_started = false;
    }
}

void sle_announce_terminal_cbk(uint32_t announce_id)
{
    unused(announce_id);
    g_adv_started = false;
}

void sle_enable_cbk(errcode_t status)
{
    if (status == ERRCODE_SUCC) {
        sle_uart_server_enable_cbk();
        sle_uart_client_enable_cbk();
    }
}

errcode_t sle_uart_announce_register_cbks(void)
{
    sle_announce_seek_callbacks_t seek_cbks = {0};

    seek_cbks.announce_enable_cb = sle_announce_enable_cbk;
    seek_cbks.announce_disable_cb = sle_announce_disable_cbk;
    seek_cbks.announce_terminal_cb = sle_announce_terminal_cbk;
    seek_cbks.sle_enable_cb = sle_enable_cbk;
    seek_cbks.seek_enable_cb = sle_uart_client_sample_seek_enable_cbk;
    seek_cbks.seek_result_cb = sle_uart_client_sample_seek_result_info_cbk;
    seek_cbks.seek_disable_cb = sle_uart_client_sample_seek_disable_cbk;
    return sle_announce_seek_register_callbacks(&seek_cbks);
}

errcode_t sle_uart_server_adv_stop(void)
{
    errcode_t ret;

    if (!g_adv_started) {
        return ERRCODE_SUCC;
    }

    ret = sle_stop_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret == ERRCODE_SUCC) {
        g_adv_started = false;
    }
    return ret;
}

errcode_t sle_uart_server_adv_init(void)
{
    errcode_t ret;
    static bool config_printed = false;

    if (!config_printed) {
        osal_printk("[Router Config] MY_ROUTER_NAME:%s\r\n", MY_ROUTER_NAME);
        config_printed = true;
    }

    sle_set_default_announce_param();
    sle_set_default_announce_data();
    if (g_adv_started) {
        return ERRCODE_SUCC;
    }

    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret == ERRCODE_SUCC) {
        g_adv_started = true;
    }
    return ret;
}
