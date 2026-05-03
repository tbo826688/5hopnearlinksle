/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 */
#ifndef SLE_UART_CLIENT_H
#define SLE_UART_CLIENT_H

#include "sle_ssap_client.h"

void sle_uart_client_init_hybrid(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb);
void sle_uart_start_scan_hybrid(void);
uint16_t get_g_sle_uart_conn_id_hybrid(void);
ssapc_write_param_t *get_g_sle_uart_send_param_hybrid(void);
void sle_uart_notification_cb_hybrid(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);
void sle_uart_indication_cb_hybrid(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);

#endif