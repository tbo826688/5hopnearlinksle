/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 */
#ifndef SLE_UART_CLIENT_H
#define SLE_UART_CLIENT_H

#include "sle_ssap_client.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"

// 客户端最大支持连接数
#define SLE_CLIENT_MAX_LINKS 4

void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb);
void sle_uart_start_scan(void);
void sle_uart_client_watchdog_tick(void);
bool sle_uart_client_write_ready(void);
errcode_t sle_uart_client_write_upstream(const uint8_t *data, uint16_t len);

// [关键修复] 补充缺失的函数声明
uint16_t *get_g_sle_uart_client_conn_ids(void);
uint8_t get_g_sle_uart_client_id(void);
uint16_t get_g_sle_uart_conn_id(void); // 兼容旧接口

ssapc_write_param_t *get_g_sle_uart_send_param(void);

// 声明回调函数，供 sle_uart_client.c 内部调用
void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);
void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);

/* 暴露给 sle_uart.c 使用的回调声明 */
void sle_uart_client_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                                      sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                                      sle_disc_reason_t disc_reason);
void sle_uart_client_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);

/* 扫描相关回调 */
void sle_uart_client_sample_sle_enable_cbk(errcode_t status);
void sle_uart_client_sample_seek_enable_cbk(errcode_t status);
void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data);
void sle_uart_client_sample_seek_disable_cbk(errcode_t status);

#endif
