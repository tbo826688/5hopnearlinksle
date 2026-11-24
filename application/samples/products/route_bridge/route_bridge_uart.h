#ifndef ROUTE_BRIDGE_UART_H
#define ROUTE_BRIDGE_UART_H

#include <stdbool.h>
#include <stdint.h>
#include "errcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_BRIDGE_MAX_PAYLOAD 1500

#define ROUTE_BRIDGE_SIDE_RX 1
#define ROUTE_BRIDGE_SIDE_TX 2

#define ROUTE_BRIDGE_FRAME_FORWARD   1
#define ROUTE_BRIDGE_FRAME_REVERSE   2
#define ROUTE_BRIDGE_FRAME_HEARTBEAT 3

#define ROUTE_BRIDGE_STATUS_SLE_LINK  0x01u
#define ROUTE_BRIDGE_STATUS_SLE_READY 0x02u

typedef void (*route_bridge_uart_frame_callback_t)(uint8_t type, const uint8_t *data, uint16_t len);

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t tx_queue_drops;
    uint32_t rx_overflow_bytes;
    uint32_t frame_errors;
    uint32_t crc_errors;
    uint32_t uart_errors;
} route_bridge_uart_stats_t;

errcode_t route_bridge_uart_init(uint8_t local_side, uint8_t node_id,
    route_bridge_uart_frame_callback_t frame_callback);
bool route_bridge_uart_send(uint8_t type, const uint8_t *data, uint16_t len);
void route_bridge_uart_process(void);
void route_bridge_uart_set_local_status(uint8_t status);
bool route_bridge_uart_peer_alive(void);
uint8_t route_bridge_uart_peer_status(void);
void route_bridge_uart_get_stats(route_bridge_uart_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
