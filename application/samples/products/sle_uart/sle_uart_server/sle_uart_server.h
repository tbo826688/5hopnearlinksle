/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE uart server Config. \n
 *
 * History: \n
 * 2023-07-17, Create file. \n
 */

#ifndef SLE_UART_SERVER_H
#define SLE_UART_SERVER_H

#include <stdint.h>
#include "sle_ssap_server.h"
#include "errcode.h"
#include "sle_connection_manager.h" // [修复] 引入连接管理头文件以识别 sle_acb_state_t 等
#include "sle_device_discovery.h"   // [修复] 引入设备发现头文件

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif /* __cplusplus */
#endif /* __cplusplus */

/* Service UUID */
#define SLE_UUID_SERVER_SERVICE        0x2222

/* Property UUID */
#define SLE_UUID_SERVER_NTF_REPORT     0x2323

/* Property Property */
#define SLE_UUID_TEST_PROPERTIES  (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)

/* Operation indication */
#define SLE_UUID_TEST_OPERATION_INDICATION  (SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE)

/* Descriptor Property */
#define SLE_UUID_TEST_DESCRIPTOR   (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)

errcode_t sle_uart_server_init(ssaps_read_request_callback ssaps_read_callback, ssaps_write_request_callback
    ssaps_write_callback);

errcode_t sle_uart_server_send_report_by_uuid(const uint8_t *data, uint8_t len);

errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len);

uint16_t sle_uart_client_is_connected(void);

typedef void (*sle_uart_server_msg_queue)(uint8_t *buffer_addr, uint16_t buffer_size);

void sle_uart_server_register_msg(sle_uart_server_msg_queue sle_uart_server_msg);

uint16_t get_connect_id(void);

/* 暴露回调函数供主从一体模式使用 */
void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
                                   sle_acb_state_t conn_state, sle_pair_state_t pair_state,
                                   sle_disc_reason_t disc_reason);
void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);

#ifdef __cplusplus
#if __cplusplus
}
#endif /* __cplusplus */
#endif /* __cplusplus */

#endif