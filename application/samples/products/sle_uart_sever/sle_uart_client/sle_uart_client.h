/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 */
#ifndef SLE_UART_CLIENT_H
#define SLE_UART_CLIENT_H

#include "sle_ssap_client.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb);
void sle_uart_start_scan(void);

uint16_t get_g_sle_uart_conn_id(void);
uint8_t get_g_sle_uart_client_id(void);
ssapc_write_param_t *get_g_sle_uart_send_param(void);

/* [新增] 核心判断函数，用于区分连进来的是板子3(Uplink)还是板子2(Downlink) */
bool sle_uart_client_is_uplink(uint16_t conn_id);

void sle_uart_client_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                                      sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                                      sle_disc_reason_t disc_reason);
void sle_uart_client_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);

void sle_uart_client_sample_sle_enable_cbk(errcode_t status);
void sle_uart_client_sample_seek_enable_cbk(errcode_t status);
void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data);
void sle_uart_client_sample_seek_disable_cbk(errcode_t status);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif