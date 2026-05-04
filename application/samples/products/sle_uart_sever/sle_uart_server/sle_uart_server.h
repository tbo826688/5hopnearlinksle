/*
 * Router downstream server interface.
 */
#ifndef SLE_UART_SERVER_H
#define SLE_UART_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "sle_ssap_server.h"
#include "errcode.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLE_UUID_SERVER_SERVICE        0x2222
#define SLE_UUID_SERVER_NTF_REPORT     0x2323
#define SLE_UUID_TEST_PROPERTIES       (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)
#define SLE_UUID_TEST_OPERATION_INDICATION  (SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE)
#define SLE_UUID_TEST_DESCRIPTOR       (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)

errcode_t sle_uart_server_init(ssaps_read_request_callback read_callback,
    ssaps_write_request_callback write_callback);
errcode_t sle_uart_server_send_report_by_uuid(const uint8_t *data, uint8_t len);
errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len);

uint16_t get_connect_id(void);
bool sle_uart_server_has_downstream(void);
bool sle_uart_server_is_downstream_link(uint16_t conn_id);
void sle_uart_server_clear_downstream(void);

void sle_uart_server_enable_cbk(void);
void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason);
void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);
void sle_server_set_phy_cbk(uint16_t conn_id, errcode_t status, const sle_set_phy_t *param);

#ifdef __cplusplus
}
#endif

#endif
