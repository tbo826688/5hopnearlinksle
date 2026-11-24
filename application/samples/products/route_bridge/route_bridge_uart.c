#include "route_bridge_uart.h"
#include "common_def.h"
#include "securec.h"
#include "soc_osal.h"
#include "pinctrl.h"
#include "uart.h"
#include "tcxo.h"
#include "uapi_crc.h"

#define ROUTE_BRIDGE_MAGIC_0              0xA5u
#define ROUTE_BRIDGE_MAGIC_1              0x5Au
#define ROUTE_BRIDGE_VERSION              1u
#define ROUTE_BRIDGE_HEADER_LEN           10u
#define ROUTE_BRIDGE_FOOTER_LEN           2u
#define ROUTE_BRIDGE_WIRE_MAX             (ROUTE_BRIDGE_HEADER_LEN + ROUTE_BRIDGE_MAX_PAYLOAD + \
                                           ROUTE_BRIDGE_FOOTER_LEN)
#define ROUTE_BRIDGE_RX_RING_SIZE         16384u
#define ROUTE_BRIDGE_DRIVER_RX_SIZE       4096u
#define ROUTE_BRIDGE_TX_QUEUE_DEPTH       32u
#define ROUTE_BRIDGE_HEARTBEAT_INTERVAL_US 500000ULL
#define ROUTE_BRIDGE_PEER_TIMEOUT_US      6000000ULL

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint16_t len;
    uint8_t data[ROUTE_BRIDGE_MAX_PAYLOAD];
} route_bridge_tx_slot_t;

static uint8_t g_driver_rx_buffer[ROUTE_BRIDGE_DRIVER_RX_SIZE];
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_driver_rx_buffer,
    .rx_buffer_size = sizeof(g_driver_rx_buffer)
};

static uint8_t g_rx_ring[ROUTE_BRIDGE_RX_RING_SIZE];
static uint32_t g_rx_head;
static uint32_t g_rx_tail;
static uint32_t g_rx_count;

static route_bridge_tx_slot_t g_tx_queue[ROUTE_BRIDGE_TX_QUEUE_DEPTH];
static uint8_t g_tx_head;
static uint8_t g_tx_tail;
static uint8_t g_tx_count;
static uint16_t g_next_tx_seq;

static uint8_t g_wire_rx_buffer[ROUTE_BRIDGE_WIRE_MAX];
static uint8_t g_wire_tx_buffer[ROUTE_BRIDGE_WIRE_MAX];
static uint16_t g_wire_tx_len;
static uint16_t g_wire_tx_offset;
static bool g_wire_tx_active;

static uint8_t g_local_side;
static uint8_t g_expected_peer_side;
static uint8_t g_node_id;
static volatile uint8_t g_local_status;
static volatile uint8_t g_peer_status;
static volatile uint64_t g_peer_last_rx_us;
static uint64_t g_last_heartbeat_us;
static route_bridge_uart_frame_callback_t g_frame_callback;
static route_bridge_uart_stats_t g_stats;

static uint16_t route_bridge_get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void route_bridge_put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFu);
    data[1] = (uint8_t)(value >> 8);
}

static uint32_t route_bridge_ring_count(void)
{
    uint32_t count;
    uint32_t irq_status = osal_irq_lock();

    count = g_rx_count;
    osal_irq_restore(irq_status);
    return count;
}

static bool route_bridge_ring_peek(uint32_t offset, uint8_t *data, uint32_t len)
{
    uint32_t first;
    uint32_t start;
    uint32_t irq_status;

    if (data == NULL || len == 0) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (offset + len > g_rx_count) {
        osal_irq_restore(irq_status);
        return false;
    }
    start = (g_rx_head + offset) % ROUTE_BRIDGE_RX_RING_SIZE;
    first = len;
    if (first > ROUTE_BRIDGE_RX_RING_SIZE - start) {
        first = ROUTE_BRIDGE_RX_RING_SIZE - start;
    }
    if (memcpy_s(data, len, &g_rx_ring[start], first) != EOK ||
        (first < len && memcpy_s(data + first, len - first, g_rx_ring, len - first) != EOK)) {
        osal_irq_restore(irq_status);
        return false;
    }
    osal_irq_restore(irq_status);
    return true;
}

static void route_bridge_ring_drop(uint32_t len)
{
    uint32_t irq_status = osal_irq_lock();

    if (len > g_rx_count) {
        len = g_rx_count;
    }
    g_rx_head = (g_rx_head + len) % ROUTE_BRIDGE_RX_RING_SIZE;
    g_rx_count -= len;
    osal_irq_restore(irq_status);
}

static bool route_bridge_ring_take(uint8_t *data, uint32_t len)
{
    uint32_t first;
    uint32_t irq_status;

    if (data == NULL || len == 0) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (len > g_rx_count) {
        osal_irq_restore(irq_status);
        return false;
    }
    first = len;
    if (first > ROUTE_BRIDGE_RX_RING_SIZE - g_rx_head) {
        first = ROUTE_BRIDGE_RX_RING_SIZE - g_rx_head;
    }
    if (memcpy_s(data, len, &g_rx_ring[g_rx_head], first) != EOK ||
        (first < len && memcpy_s(data + first, len - first, g_rx_ring, len - first) != EOK)) {
        osal_irq_restore(irq_status);
        return false;
    }
    g_rx_head = (g_rx_head + len) % ROUTE_BRIDGE_RX_RING_SIZE;
    g_rx_count -= len;
    osal_irq_restore(irq_status);
    return true;
}

static void route_bridge_uart_rx_callback(const void *buffer, uint16_t length, bool error)
{
    uint32_t free_bytes;
    uint32_t copy_len;
    uint32_t first;
    uint32_t irq_status;

    if (error) {
        g_stats.uart_errors++;
    }
    if (buffer == NULL || length == 0) {
        return;
    }

    irq_status = osal_irq_lock();
    free_bytes = ROUTE_BRIDGE_RX_RING_SIZE - g_rx_count;
    copy_len = length;
    if (copy_len > free_bytes) {
        copy_len = free_bytes;
        g_stats.rx_overflow_bytes += (uint32_t)length - copy_len;
    }
    first = copy_len;
    if (first > ROUTE_BRIDGE_RX_RING_SIZE - g_rx_tail) {
        first = ROUTE_BRIDGE_RX_RING_SIZE - g_rx_tail;
    }
    if (first > 0 && memcpy_s(&g_rx_ring[g_rx_tail], ROUTE_BRIDGE_RX_RING_SIZE - g_rx_tail,
        buffer, first) == EOK) {
        if (first < copy_len) {
            if (memcpy_s(g_rx_ring, ROUTE_BRIDGE_RX_RING_SIZE,
                (const uint8_t *)buffer + first, copy_len - first) != EOK) {
                copy_len = first;
            }
        }
        g_rx_tail = (g_rx_tail + copy_len) % ROUTE_BRIDGE_RX_RING_SIZE;
        g_rx_count += copy_len;
    } else if (copy_len > 0) {
        g_stats.rx_overflow_bytes += copy_len;
    }
    osal_irq_restore(irq_status);
}

bool route_bridge_uart_send(uint8_t type, const uint8_t *data, uint16_t len)
{
    route_bridge_tx_slot_t *slot;
    uint32_t irq_status;

    if (type < ROUTE_BRIDGE_FRAME_FORWARD || type > ROUTE_BRIDGE_FRAME_HEARTBEAT ||
        len > ROUTE_BRIDGE_MAX_PAYLOAD || (len > 0 && data == NULL)) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_tx_count >= ROUTE_BRIDGE_TX_QUEUE_DEPTH) {
        g_stats.tx_queue_drops++;
        osal_irq_restore(irq_status);
        return false;
    }
    slot = &g_tx_queue[g_tx_tail];
    slot->type = type;
    slot->seq = g_next_tx_seq++;
    slot->len = len;
    if (len > 0 && memcpy_s(slot->data, sizeof(slot->data), data, len) != EOK) {
        osal_irq_restore(irq_status);
        return false;
    }
    g_tx_tail = (uint8_t)((g_tx_tail + 1u) % ROUTE_BRIDGE_TX_QUEUE_DEPTH);
    g_tx_count++;
    osal_irq_restore(irq_status);
    return true;
}

static void route_bridge_process_heartbeat(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len != 4 || data[0] != g_expected_peer_side || data[1] != g_node_id) {
        g_stats.frame_errors++;
        return;
    }
    g_peer_status = data[2];
    g_peer_last_rx_us = uapi_tcxo_get_us();
}

static void route_bridge_process_rx(void)
{
    uint8_t header[ROUTE_BRIDGE_HEADER_LEN];
    uint16_t header_crc;
    uint16_t payload_crc;
    uint16_t len;
    uint32_t frame_len;

    while (route_bridge_ring_count() >= 2u) {
        if (!route_bridge_ring_peek(0, header, 2)) {
            return;
        }
        if (header[0] != ROUTE_BRIDGE_MAGIC_0 || header[1] != ROUTE_BRIDGE_MAGIC_1) {
            route_bridge_ring_drop(1);
            g_stats.frame_errors++;
            continue;
        }
        if (route_bridge_ring_count() < ROUTE_BRIDGE_HEADER_LEN) {
            return;
        }
        if (!route_bridge_ring_peek(0, header, sizeof(header))) {
            return;
        }
        len = route_bridge_get_u16(&header[6]);
        header_crc = route_bridge_get_u16(&header[8]);
        if (header[2] != ROUTE_BRIDGE_VERSION ||
            header[3] < ROUTE_BRIDGE_FRAME_FORWARD || header[3] > ROUTE_BRIDGE_FRAME_HEARTBEAT ||
            len > ROUTE_BRIDGE_MAX_PAYLOAD || uapi_crc16(0, header, 8) != header_crc) {
            route_bridge_ring_drop(1);
            g_stats.frame_errors++;
            continue;
        }
        frame_len = ROUTE_BRIDGE_HEADER_LEN + len + ROUTE_BRIDGE_FOOTER_LEN;
        if (route_bridge_ring_count() < frame_len) {
            return;
        }
        if (!route_bridge_ring_take(g_wire_rx_buffer, frame_len)) {
            return;
        }
        payload_crc = route_bridge_get_u16(&g_wire_rx_buffer[ROUTE_BRIDGE_HEADER_LEN + len]);
        if (uapi_crc16(0, &g_wire_rx_buffer[ROUTE_BRIDGE_HEADER_LEN], len) != payload_crc) {
            g_stats.crc_errors++;
            continue;
        }
        g_stats.rx_frames++;
        if (header[3] == ROUTE_BRIDGE_FRAME_HEARTBEAT) {
            route_bridge_process_heartbeat(&g_wire_rx_buffer[ROUTE_BRIDGE_HEADER_LEN], len);
        } else if (g_peer_last_rx_us != 0 && g_frame_callback != NULL) {
            g_peer_last_rx_us = uapi_tcxo_get_us();
            g_frame_callback(header[3], &g_wire_rx_buffer[ROUTE_BRIDGE_HEADER_LEN], len);
        } else {
            g_stats.frame_errors++;
        }
    }
}

static void route_bridge_start_tx(void)
{
    route_bridge_tx_slot_t *slot;
    uint16_t header_crc;
    uint16_t payload_crc;
    uint32_t irq_status;

    irq_status = osal_irq_lock();
    if (g_tx_count == 0) {
        osal_irq_restore(irq_status);
        return;
    }
    slot = &g_tx_queue[g_tx_head];
    osal_irq_restore(irq_status);
    g_wire_tx_buffer[0] = ROUTE_BRIDGE_MAGIC_0;
    g_wire_tx_buffer[1] = ROUTE_BRIDGE_MAGIC_1;
    g_wire_tx_buffer[2] = ROUTE_BRIDGE_VERSION;
    g_wire_tx_buffer[3] = slot->type;
    route_bridge_put_u16(&g_wire_tx_buffer[4], slot->seq);
    route_bridge_put_u16(&g_wire_tx_buffer[6], slot->len);
    header_crc = uapi_crc16(0, g_wire_tx_buffer, 8);
    route_bridge_put_u16(&g_wire_tx_buffer[8], header_crc);
    if (slot->len > 0 && memcpy_s(&g_wire_tx_buffer[ROUTE_BRIDGE_HEADER_LEN],
        ROUTE_BRIDGE_MAX_PAYLOAD, slot->data, slot->len) != EOK) {
        g_stats.uart_errors++;
        return;
    }
    payload_crc = uapi_crc16(0, slot->data, slot->len);
    route_bridge_put_u16(&g_wire_tx_buffer[ROUTE_BRIDGE_HEADER_LEN + slot->len], payload_crc);
    g_wire_tx_len = ROUTE_BRIDGE_HEADER_LEN + slot->len + ROUTE_BRIDGE_FOOTER_LEN;
    g_wire_tx_offset = 0;
    g_wire_tx_active = true;
}

static void route_bridge_process_tx(void)
{
    int32_t written;
    uint32_t irq_status;

    if (!g_wire_tx_active) {
        route_bridge_start_tx();
    }
    if (!g_wire_tx_active) {
        return;
    }

    written = uapi_uart_write((uart_bus_t)CONFIG_ROUTE_BRIDGE_UART_BUS,
        &g_wire_tx_buffer[g_wire_tx_offset], g_wire_tx_len - g_wire_tx_offset, 0);
    if (written <= 0) {
        g_stats.uart_errors++;
        return;
    }
    g_wire_tx_offset = (uint16_t)(g_wire_tx_offset + written);
    if (g_wire_tx_offset < g_wire_tx_len) {
        return;
    }

    irq_status = osal_irq_lock();
    if (g_tx_count > 0) {
        g_tx_head = (uint8_t)((g_tx_head + 1u) % ROUTE_BRIDGE_TX_QUEUE_DEPTH);
        g_tx_count--;
    }
    osal_irq_restore(irq_status);
    g_wire_tx_active = false;
    g_stats.tx_frames++;
}

static void route_bridge_send_heartbeat(void)
{
    uint8_t heartbeat[4];
    uint64_t now = uapi_tcxo_get_us();

    if (g_last_heartbeat_us != 0 && now - g_last_heartbeat_us < ROUTE_BRIDGE_HEARTBEAT_INTERVAL_US) {
        return;
    }
    heartbeat[0] = g_local_side;
    heartbeat[1] = g_node_id;
    heartbeat[2] = g_local_status;
    heartbeat[3] = 0;
    if (route_bridge_uart_send(ROUTE_BRIDGE_FRAME_HEARTBEAT, heartbeat, sizeof(heartbeat))) {
        g_last_heartbeat_us = now;
    }
}

errcode_t route_bridge_uart_init(uint8_t local_side, uint8_t node_id,
    route_bridge_uart_frame_callback_t frame_callback)
{
    uart_attr_t attr = {
        .baud_rate = CONFIG_ROUTE_BRIDGE_UART_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE
    };
    uart_pin_config_t pin_config = {
        .tx_pin = CONFIG_ROUTE_BRIDGE_UART_TX_PIN,
        .rx_pin = CONFIG_ROUTE_BRIDGE_UART_RX_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };
    errcode_t ret;

    if ((local_side != ROUTE_BRIDGE_SIDE_RX && local_side != ROUTE_BRIDGE_SIDE_TX) || node_id < 1 || node_id > 4) {
        return ERRCODE_INVALID_PARAM;
    }

    g_local_side = local_side;
    g_expected_peer_side = (local_side == ROUTE_BRIDGE_SIDE_RX) ? ROUTE_BRIDGE_SIDE_TX : ROUTE_BRIDGE_SIDE_RX;
    g_node_id = node_id;
    g_frame_callback = frame_callback;
    g_local_status = 0;
    g_peer_status = 0;
    g_peer_last_rx_us = 0;
    g_last_heartbeat_us = 0;

    uapi_pin_set_mode(CONFIG_ROUTE_BRIDGE_UART_TX_PIN, PIN_MODE_1);
    uapi_pin_set_mode(CONFIG_ROUTE_BRIDGE_UART_RX_PIN, PIN_MODE_1);
    (void)uapi_uart_deinit((uart_bus_t)CONFIG_ROUTE_BRIDGE_UART_BUS);
    ret = uapi_uart_init((uart_bus_t)CONFIG_ROUTE_BRIDGE_UART_BUS,
        &pin_config, &attr, NULL, &g_uart_buffer_config);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    (void)uapi_uart_unregister_rx_callback((uart_bus_t)CONFIG_ROUTE_BRIDGE_UART_BUS);
    return uapi_uart_register_rx_callback((uart_bus_t)CONFIG_ROUTE_BRIDGE_UART_BUS,
        UART_RX_CONDITION_FULL_OR_IDLE, 1, route_bridge_uart_rx_callback);
}

void route_bridge_uart_process(void)
{
    route_bridge_process_rx();
    route_bridge_send_heartbeat();
    route_bridge_process_tx();
}

void route_bridge_uart_set_local_status(uint8_t status)
{
    g_local_status = status;
}

bool route_bridge_uart_peer_alive(void)
{
    uint64_t last_rx = g_peer_last_rx_us;

    return last_rx != 0 && uapi_tcxo_get_us() - last_rx <= ROUTE_BRIDGE_PEER_TIMEOUT_US;
}

uint8_t route_bridge_uart_peer_status(void)
{
    return g_peer_status;
}

void route_bridge_uart_get_stats(route_bridge_uart_stats_t *stats)
{
    if (stats != NULL) {
        *stats = g_stats;
    }
}
