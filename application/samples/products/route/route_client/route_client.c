/*
 * Router upstream client. Each router connects to the previous hop whose
 * advertised name is TARGET_UPSTREAM_NAME.
 */
#include "common_def.h"
#include "soc_osal.h"
#include "securec.h"
#include "product.h"
#include "nv.h"
#include "bts_le_gap.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "route_client.h"
#include "tcxo.h"
#include <string.h>
#include "sle_router_config.h"

#define SLE_INVALID_CONN_ID             0xFFFF
#define SLE_UPSTREAM_CLIENT_ID          1
#define SLE_SCAN_RESTART_TIMEOUT_US     8000000ULL
#define SLE_CONNECT_TIMEOUT_US          12000000ULL
#define SLE_READY_TIMEOUT_US            30000000ULL
#define SLE_PAIR_RESET_THRESHOLD        3
#define SLE_ADV_TYPE_COMPLETE_LOCAL_NAME 0x0B

static ssapc_callbacks_t g_sle_uart_ssapc_cbk = { 0 };
static sle_addr_t g_sle_uart_remote_addr = { 0 };
static bool g_remote_addr_valid = false;
static bool g_scan_active = false;
static bool g_connect_pending = false;
static bool g_restart_scan_after_stop = false;
static uint64_t g_scan_started_us = 0;
static uint64_t g_connect_started_us = 0;
static uint64_t g_ready_started_us = 0;
static bool g_ready_recovery_pending = false;
static uint8_t g_ready_recovery_count = 0;

static uint16_t g_sle_uart_conn_id = SLE_INVALID_CONN_ID;
static uint16_t g_upstream_property_handle = 0;

static bool sle_addr_equal(const sle_addr_t *left, const sle_addr_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    return (left->type == right->type) &&
        (memcmp(left->addr, right->addr, SLE_ADDR_LEN) == 0);
}

uint16_t route_client_get_conn_id(void)
{
    return g_sle_uart_conn_id;
}

bool route_client_is_connected(void)
{
    return g_sle_uart_conn_id != SLE_INVALID_CONN_ID;
}

bool route_client_write_ready(void)
{
    return route_client_is_connected() && g_upstream_property_handle != 0;
}

errcode_t route_client_write_upstream(const uint8_t *data, uint16_t len)
{
    ssapc_write_param_t param = {0};

    if (!route_client_write_ready() || data == NULL || len == 0) {
        return ERRCODE_FAIL;
    }
    param.handle = g_upstream_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)data;
    return ssapc_write_cmd(SLE_UPSTREAM_CLIENT_ID, g_sle_uart_conn_id, &param);
}

bool route_client_is_upstream_connect_pending(void)
{
    return g_connect_pending;
}

bool route_client_is_upstream_link(uint16_t conn_id, const sle_addr_t *addr)
{
    if (g_sle_uart_conn_id != SLE_INVALID_CONN_ID && conn_id == g_sle_uart_conn_id) {
        return true;
    }

    if (g_remote_addr_valid && sle_addr_equal(addr, &g_sle_uart_remote_addr)) {
        return true;
    }

    return g_connect_pending && addr == NULL;
}

void route_client_clear_upstream(void)
{
    g_sle_uart_conn_id = SLE_INVALID_CONN_ID;
    g_upstream_property_handle = 0;
    g_connect_pending = false;
    g_remote_addr_valid = false;
    g_ready_started_us = 0;
    g_ready_recovery_pending = false;
}

void route_client_set_nv(void)
{
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;

    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {
        nv_value = 7;
        uapi_nv_write(0x20A0, (uint8_t *)&nv_value, sizeof(nv_value));
    }
}

void route_client_connect_param_init(void)
{
    sle_default_connect_param_t param = {0};

    param.enable_filter_policy = 0;
    param.gt_negotiate = SLE_ANNOUNCE_ROLE_G_CAN_NEGO;
    param.initiate_phys = 1;
    param.max_interval = 0x14;
    param.min_interval = 0x14;
    param.scan_interval = 100;
    param.scan_window = 100;
    param.timeout = SLE_ROUTER_SUPERVISION_TIMEOUT;
    sle_default_connection_param_set(&param);
}

static void route_client_start_exchange(uint16_t conn_id)
{
    ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
    ssapc_exchange_info_req(SLE_UPSTREAM_CLIENT_ID, conn_id, &info);
}

static bool sle_adv_name_matches(const uint8_t *value, uint8_t value_len,
    const uint8_t *target, uint8_t target_len)
{
    uint8_t index;

    if (value == NULL || target == NULL || value_len < target_len ||
        memcmp(value, target, target_len) != 0) {
        return false;
    }
    for (index = target_len; index < value_len; index++) {
        if (value[index] != 0) {
            return false;
        }
    }
    return true;
}

static bool sle_adv_data_contains_target(const sle_seek_result_info_t *seek_result_data)
{
    const uint8_t *data;
    const uint8_t *target = (const uint8_t *)TARGET_UPSTREAM_NAME;
    uint8_t data_len;
    uint8_t target_len = (uint8_t)strlen(TARGET_UPSTREAM_NAME);
    uint16_t offset = 0;

    if (seek_result_data == NULL || seek_result_data->data == NULL || seek_result_data->data_length == 0) {
        return false;
    }

    data = seek_result_data->data;
    data_len = seek_result_data->data_length;
    while (offset + 2u <= data_len) {
        uint8_t field_len = data[offset];
        uint16_t field_end;

        if (field_len < 1) {
            offset++;
            continue;
        }
        field_end = offset + 1u + field_len;
        if (field_end > data_len) {
            offset++;
            continue;
        }
        if (data[offset + 1] == SLE_ADV_TYPE_COMPLETE_LOCAL_NAME &&
            sle_adv_name_matches(&data[offset + 2], (uint8_t)(field_len - 1u), target, target_len)) {
            return true;
        }
        offset++;
    }
    return false;
}

void route_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data)
{
    errcode_t ret;

    if (!sle_adv_data_contains_target(seek_result_data)) {
        return;
    }

    memcpy_s(&g_sle_uart_remote_addr, sizeof(sle_addr_t), &seek_result_data->addr, sizeof(sle_addr_t));
    g_remote_addr_valid = true;
    osal_printk("[Router Client] Found upstream \"%s\". Connecting.\r\n", TARGET_UPSTREAM_NAME);
    ret = sle_stop_seek();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Router Client] Stop scan failed:%x.\r\n", ret);
    }
}

void route_client_start_scan(void)
{
    errcode_t ret;

    if (g_sle_uart_conn_id != SLE_INVALID_CONN_ID || g_connect_pending || g_scan_active) {
        return;
    }

    sle_seek_param_t param = {
        .own_addr_type = 0,
        .filter_duplicates = 0,
        .seek_filter_policy = 0,
        .seek_phys = 1,
        .seek_type = {1},
        .seek_interval = {100},
        .seek_window = {100}
    };

    ret = sle_set_seek_param(&param);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[Router Client] Set scan parameter failed:%x.\r\n", ret);
        return;
    }
    ret = sle_start_seek();
    if (ret == ERRCODE_SUCC) {
        g_scan_active = true;
        g_scan_started_us = uapi_tcxo_get_us();
    } else {
        osal_printk("[Router Client] Start scan failed:%x.\r\n", ret);
    }
}

void route_client_enable_cbk(void)
{
    route_client_set_nv();
    route_client_connect_param_init();
    route_client_start_scan();
}

void route_client_sample_seek_enable_cbk(errcode_t status)
{
    if (status == ERRCODE_SUCC) {
        g_scan_active = true;
        g_scan_started_us = uapi_tcxo_get_us();
    }
}

void route_client_sample_seek_disable_cbk(errcode_t status)
{
    errcode_t ret;

    g_scan_active = false;
    if (status != ERRCODE_SUCC || !g_remote_addr_valid || g_sle_uart_conn_id != SLE_INVALID_CONN_ID) {
        if (g_restart_scan_after_stop && g_sle_uart_conn_id == SLE_INVALID_CONN_ID && !g_connect_pending) {
            g_restart_scan_after_stop = false;
            route_client_start_scan();
        }
        return;
    }

    g_restart_scan_after_stop = false;
    ret = sle_connect_remote_device(&g_sle_uart_remote_addr);
    if (ret == ERRCODE_SUCC) {
        g_connect_pending = true;
        g_connect_started_us = uapi_tcxo_get_us();
    } else {
        osal_printk("[Router Client] Connect request failed:%x. Re-scanning.\r\n", ret);
        g_connect_pending = false;
        g_remote_addr_valid = false;
        route_client_start_scan();
    }
}

void route_client_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_sle_uart_conn_id = conn_id;
        g_connect_pending = false;
        g_scan_active = false;
        g_ready_started_us = uapi_tcxo_get_us();
        g_ready_recovery_pending = false;
        if (addr != NULL) {
            memcpy_s(&g_sle_uart_remote_addr, sizeof(sle_addr_t), addr, sizeof(sle_addr_t));
            g_remote_addr_valid = true;
        }

        osal_printk("[Router Client] Upstream connected. conn_id:%u\r\n", conn_id);
        if (pair_state == SLE_PAIR_NONE) {
            sle_pair_remote_device(&g_sle_uart_remote_addr);
        } else {
            route_client_start_exchange(conn_id);
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        route_client_clear_upstream();
        osal_printk("[Router Client] Upstream disconnected. reason:%u. Restarting scan.\r\n",
            disc_reason);
        route_client_start_scan();
    }
}

void route_client_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(addr);

    if (status == ERRCODE_SUCC && conn_id == g_sle_uart_conn_id) {
        route_client_start_exchange(conn_id);
    } else if (conn_id == g_sle_uart_conn_id) {
        if (g_remote_addr_valid) {
            sle_remove_paired_remote_device(&g_sle_uart_remote_addr);
            if (sle_disconnect_remote_device(&g_sle_uart_remote_addr) == ERRCODE_SUCC) {
                g_ready_recovery_pending = true;
                g_ready_started_us = uapi_tcxo_get_us();
            }
        }
    }
}

static void route_client_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id,
    ssap_exchange_info_t *param, errcode_t status)
{
    unused(param);

    if (status == ERRCODE_SUCC) {
        ssapc_find_structure_param_t find_param = {
            .type = SSAP_FIND_TYPE_PROPERTY,
            .start_hdl = 1,
            .end_hdl = 0xFFFF
        };
        ssapc_find_structure(client_id, conn_id, &find_param);
    }
}

static void route_client_sample_find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    unused(client_id);
    unused(conn_id);

    if (status == ERRCODE_SUCC && property != NULL &&
        (property->operate_indication & SSAP_OPERATE_INDICATION_BIT_WRITE_NO_RSP) != 0) {
        g_upstream_property_handle = property->handle;
        g_ready_started_us = 0;
        g_ready_recovery_pending = false;
        g_ready_recovery_count = 0;
        osal_printk("[Router Client] Reverse control ready. handle:%u\r\n", property->handle);
    }
}

void route_client_watchdog_tick(void)
{
    uint64_t now = uapi_tcxo_get_us();

    if (g_sle_uart_conn_id != SLE_INVALID_CONN_ID) {
        if (route_client_write_ready()) {
            g_ready_started_us = 0;
            g_ready_recovery_pending = false;
            return;
        }
        if (g_ready_started_us == 0) {
            g_ready_started_us = now;
        }
        if (g_ready_recovery_pending && now - g_ready_started_us >= SLE_CONNECT_TIMEOUT_US) {
            g_ready_recovery_pending = false;
        }
        if (!g_ready_recovery_pending && now - g_ready_started_us >= SLE_READY_TIMEOUT_US &&
            g_remote_addr_valid) {
            g_ready_recovery_count++;
            if (g_ready_recovery_count >= SLE_PAIR_RESET_THRESHOLD) {
                sle_remove_paired_remote_device(&g_sle_uart_remote_addr);
                g_ready_recovery_count = 0;
            }
            osal_printk("[Router Client] Service discovery timeout. Reconnecting upstream.\r\n");
            if (sle_disconnect_remote_device(&g_sle_uart_remote_addr) == ERRCODE_SUCC) {
                g_ready_recovery_pending = true;
            }
            g_ready_started_us = now;
        }
        return;
    }

    if (g_connect_pending) {
        if (now - g_connect_started_us >= SLE_CONNECT_TIMEOUT_US) {
            g_connect_pending = false;
            osal_printk("[Router Client] Connect timeout. Re-scanning \"%s\".\r\n",
                TARGET_UPSTREAM_NAME);
            route_client_start_scan();
        }
        return;
    }

    if (!g_scan_active) {
        route_client_start_scan();
        return;
    }

    if (now - g_scan_started_us >= SLE_SCAN_RESTART_TIMEOUT_US) {
        g_remote_addr_valid = false;
        g_restart_scan_after_stop = true;
        g_scan_active = false;
        if (sle_stop_seek() != ERRCODE_SUCC) {
            g_restart_scan_after_stop = false;
            route_client_start_scan();
        }
    }
}

void route_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb)
{
    g_sle_uart_ssapc_cbk.exchange_info_cb = route_client_sample_exchange_info_cbk;
    g_sle_uart_ssapc_cbk.ssapc_find_property_cbk = route_client_sample_find_property_cbk;
    g_sle_uart_ssapc_cbk.notification_cb = notification_cb;
    g_sle_uart_ssapc_cbk.indication_cb = indication_cb;
    ssapc_register_callbacks(&g_sle_uart_ssapc_cbk);
}
