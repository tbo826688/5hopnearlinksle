/**
 * Endpoint Node 6 (sink) — pure SLE client.
 * Includes scan/connect watchdog (8s scan restart, 12s connect timeout)
 * mirrored from the router upstream-client to ensure auto-reconnect parity.
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
#include "tcxo.h"
#include <string.h>

#ifndef SLE_UART_SERVER_NAME
#define SLE_UART_SERVER_NAME           "SLE_END"
#endif

#ifndef SLE_OUTDOOR_LONG_RANGE
#define SLE_OUTDOOR_LONG_RANGE 1
#endif
#if SLE_OUTDOOR_LONG_RANGE
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT 0x7D0
#else
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT 0x1F4
#endif

#define SLE_INVALID_CONN_ID             0xFFFF
#define SLE_ENDPOINT_CLIENT_ID          1
#define SLE_SCAN_RESTART_TIMEOUT_US     8000000ULL
#define SLE_CONNECT_TIMEOUT_US          12000000ULL
#define SLE_READY_TIMEOUT_US            30000000ULL
#define SLE_PAIR_RESET_THRESHOLD        3
#define SLE_ADV_TYPE_COMPLETE_LOCAL_NAME 0x0B

/* Node 6 shares UART 0 with the data path in file-transfer mode — keep it
 * clean. Throughput mode keeps the prints for visibility. */
#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
#define rx_log(fmt, ...)    do { } while (0)
#else
#define rx_log(fmt, ...)    osal_printk(fmt, ##__VA_ARGS__)
#endif

static sle_announce_seek_callbacks_t g_sle_uart_seek_cbk = { 0 };
static sle_connection_callbacks_t g_sle_uart_connect_cbk = { 0 };
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

uint16_t g_sle_uart_conn_id = SLE_INVALID_CONN_ID;
static uint16_t g_upstream_property_handle = 0;

uint16_t get_g_sle_uart_conn_id(void) { return g_sle_uart_conn_id; }

bool sle_uart_client_write_ready(void)
{
    return g_sle_uart_conn_id != SLE_INVALID_CONN_ID && g_upstream_property_handle != 0;
}

errcode_t sle_uart_client_write_upstream(const uint8_t *data, uint16_t len)
{
    ssapc_write_param_t param = {0};

    if (!sle_uart_client_write_ready() || data == NULL || len == 0) {
        return ERRCODE_FAIL;
    }
    param.handle = g_upstream_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = len;
    param.data = (uint8_t *)data;
    return ssapc_write_cmd(SLE_ENDPOINT_CLIENT_ID, g_sle_uart_conn_id, &param);
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

static bool sle_adv_data_contains_name(const sle_seek_result_info_t *seek_result_data)
{
    const uint8_t *target = (const uint8_t *)SLE_UART_SERVER_NAME;
    uint8_t target_len = (uint8_t)strlen(SLE_UART_SERVER_NAME);
    uint16_t offset = 0;

    if (seek_result_data == NULL || seek_result_data->data == NULL ||
        seek_result_data->data_length < target_len) {
        return false;
    }
    while (offset + 2u <= seek_result_data->data_length) {
        uint8_t field_len = seek_result_data->data[offset];
        uint16_t field_end;

        if (field_len < 1) {
            offset++;
            continue;
        }
        field_end = offset + 1u + field_len;
        if (field_end > seek_result_data->data_length) {
            offset++;
            continue;
        }
        if (seek_result_data->data[offset + 1] == SLE_ADV_TYPE_COMPLETE_LOCAL_NAME &&
            sle_adv_name_matches(&seek_result_data->data[offset + 2],
                (uint8_t)(field_len - 1u), target, target_len)) {
            return true;
        }
        offset++;
    }
    return false;
}

void sle_client_set_nv(void)
{
    uint16_t nv_value_len = 0;
    uint8_t nv_value = 0;
    uapi_nv_read(0x20A0, sizeof(uint16_t), &nv_value_len, &nv_value);
    if (nv_value != 7) {
        nv_value = 7;
        uapi_nv_write(0x20A0, (uint8_t *)&nv_value, sizeof(nv_value));
    }
}

void sle_speed_connect_param_init(void)
{
    sle_default_connect_param_t param = {0};
    param.enable_filter_policy = 0;
    param.gt_negotiate = SLE_ANNOUNCE_ROLE_G_CAN_NEGO;
    param.initiate_phys = 1;
    param.max_interval = 0x14;
    param.min_interval = 0x14;
    param.scan_interval = 100;
    param.scan_window = 100;
    param.timeout = SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT;
    sle_default_connection_param_set(&param);
}

void sle_uart_start_scan(void)
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
        rx_log("[Node6 Client] Set scan parameter failed:%x.\r\n", ret);
        return;
    }
    ret = sle_start_seek();
    if (ret == ERRCODE_SUCC) {
        g_scan_active = true;
        g_scan_started_us = uapi_tcxo_get_us();
    } else {
        rx_log("[Node6 Client] Start scan failed:%x.\r\n", ret);
    }
}

void sle_uart_client_sample_sle_enable_cbk(errcode_t status)
{
    if (status == 0) {
        sle_client_set_nv();
        sle_speed_connect_param_init();
        sle_uart_start_scan();
    }
}

void sle_uart_client_sample_seek_enable_cbk(errcode_t status)
{
    if (status == ERRCODE_SUCC) {
        g_scan_active = true;
        g_scan_started_us = uapi_tcxo_get_us();
    }
}

void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data)
{
    errcode_t ret;

    if (seek_result_data == NULL) return;
    if (sle_adv_data_contains_name(seek_result_data)) {
        memcpy_s(&g_sle_uart_remote_addr, sizeof(sle_addr_t), &seek_result_data->addr, sizeof(sle_addr_t));
        g_remote_addr_valid = true;
        rx_log("[Node6 Client] Found upstream \"%s\". Connecting.\r\n", SLE_UART_SERVER_NAME);
        ret = sle_stop_seek();
        if (ret != ERRCODE_SUCC) {
            rx_log("[Node6 Client] Stop scan failed:%x.\r\n", ret);
        }
    }
}

void sle_uart_client_sample_seek_disable_cbk(errcode_t status)
{
    errcode_t ret;

    g_scan_active = false;
    if (status != ERRCODE_SUCC || !g_remote_addr_valid || g_sle_uart_conn_id != SLE_INVALID_CONN_ID) {
        if (g_restart_scan_after_stop && g_sle_uart_conn_id == SLE_INVALID_CONN_ID && !g_connect_pending) {
            g_restart_scan_after_stop = false;
            sle_uart_start_scan();
        }
        return;
    }

    g_restart_scan_after_stop = false;
    ret = sle_connect_remote_device(&g_sle_uart_remote_addr);
    if (ret == ERRCODE_SUCC) {
        g_connect_pending = true;
        g_connect_started_us = uapi_tcxo_get_us();
    } else {
        rx_log("[Node6 Client] Connect request failed:%x. Re-scanning.\r\n", ret);
        g_connect_pending = false;
        g_remote_addr_valid = false;
        sle_uart_start_scan();
    }
}

void sle_uart_client_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    unused(disc_reason);

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
        rx_log("[Node6 Client] Connected. conn_id:%u\r\n", conn_id);
        if (pair_state == SLE_PAIR_NONE) {
            sle_pair_remote_device(&g_sle_uart_remote_addr);
        } else {
            ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
            ssapc_exchange_info_req(SLE_ENDPOINT_CLIENT_ID, g_sle_uart_conn_id, &info);
        }
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_sle_uart_conn_id = SLE_INVALID_CONN_ID;
        g_upstream_property_handle = 0;
        g_connect_pending = false;
        g_remote_addr_valid = false;
        g_ready_started_us = 0;
        g_ready_recovery_pending = false;
        rx_log("[Node6 Client] Disconnected. Restarting scan.\r\n");
        sle_uart_start_scan();
    }
}

void sle_uart_client_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    unused(addr);
    if (status == ERRCODE_SUCC && conn_id == g_sle_uart_conn_id) {
        ssap_exchange_info_t info = { .mtu_size = 1500, .version = 1 };
        ssapc_exchange_info_req(SLE_ENDPOINT_CLIENT_ID, g_sle_uart_conn_id, &info);
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

static void sle_uart_client_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id,
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

static void sle_uart_client_sample_find_property_cbk(uint8_t client_id, uint16_t conn_id,
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
        rx_log("[Node6 Client] Reverse control ready. handle:%u\r\n", property->handle);
    }
}

void sle_uart_client_watchdog_tick(void)
{
    uint64_t now = uapi_tcxo_get_us();

    if (g_sle_uart_conn_id != SLE_INVALID_CONN_ID) {
        if (sle_uart_client_write_ready()) {
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
            rx_log("[Node6 Client] Service discovery timeout. Reconnecting.\r\n");
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
            rx_log("[Node6 Client] Connect timeout. Re-scan.\r\n");
            sle_uart_start_scan();
        }
        return;
    }

    if (!g_scan_active) {
        sle_uart_start_scan();
        return;
    }

    if (now - g_scan_started_us >= SLE_SCAN_RESTART_TIMEOUT_US) {
        g_remote_addr_valid = false;
        g_restart_scan_after_stop = true;
        g_scan_active = false;
        if (sle_stop_seek() != ERRCODE_SUCC) {
            g_restart_scan_after_stop = false;
            sle_uart_start_scan();
        }
    }
}

void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb)
{
    g_sle_uart_seek_cbk.sle_enable_cb = sle_uart_client_sample_sle_enable_cbk;
    g_sle_uart_seek_cbk.seek_enable_cb = sle_uart_client_sample_seek_enable_cbk;
    g_sle_uart_seek_cbk.seek_result_cb = sle_uart_client_sample_seek_result_info_cbk;
    g_sle_uart_seek_cbk.seek_disable_cb = sle_uart_client_sample_seek_disable_cbk;
    sle_announce_seek_register_callbacks(&g_sle_uart_seek_cbk);

    g_sle_uart_connect_cbk.connect_state_changed_cb = sle_uart_client_sample_connect_state_changed_cbk;
    g_sle_uart_connect_cbk.pair_complete_cb = sle_uart_client_sample_pair_complete_cbk;
    sle_connection_register_callbacks(&g_sle_uart_connect_cbk);

    g_sle_uart_ssapc_cbk.exchange_info_cb = sle_uart_client_sample_exchange_info_cbk;
    g_sle_uart_ssapc_cbk.ssapc_find_property_cbk = sle_uart_client_sample_find_property_cbk;
    g_sle_uart_ssapc_cbk.notification_cb = notification_cb;
    g_sle_uart_ssapc_cbk.indication_cb = indication_cb;
    ssapc_register_callbacks(&g_sle_uart_ssapc_cbk);

    enable_sle();
}
