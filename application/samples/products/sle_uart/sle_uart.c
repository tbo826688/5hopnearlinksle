/*
 * Endpoint speed test:
 * Node1 (server/source) -> R1 -> R2 -> R3 -> R4 -> Node6 (client/sink)
 */
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"
#include "pinctrl.h"
#include "uart.h"
#include "tcxo.h"
#include "watchdog.h"
#include "sle_low_latency.h"
#include "securec.h"
#include "sle_connection_manager.h"

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
#include "sle_ssap_client.h"
#include "sle_uart_client.h"
#endif

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define THROUGHPUT_PKT_LEN                  1200
#define THROUGHPUT_PACING_MS                8
#define SPEED_SAMPLE_INTERVAL_MS            1000
#define SOURCE_STATS_INTERVAL_MS            5000
#define US_PER_SECOND                       1000000ULL
#define HOP_MARKER_OFFSET                   3
#define EXPECTED_ROUTER_HOPS                4
#define BUSINESS_MAGIC0                     0xA5
#define BUSINESS_MAGIC1                     0x5A
#define BUSINESS_TYPE_DATA                  0x01
#define BUSINESS_TYPE_ACK                   0x02
#define BUSINESS_HEADER_LEN                 10
#define BUSINESS_SEQ_HI_OFFSET              4
#define BUSINESS_LEN_HI_OFFSET              6
#define BUSINESS_CRC_HI_OFFSET              8
#define BUSINESS_PAYLOAD_LEN                (THROUGHPUT_PKT_LEN - BUSINESS_HEADER_LEN)
#define BUSINESS_ACK_PAYLOAD_LEN            8
#define BUSINESS_ACK_PACKET_LEN             (BUSINESS_HEADER_LEN + BUSINESS_ACK_PAYLOAD_LEN)
#define BUSINESS_SACK_LO_OFFSET             BUSINESS_HEADER_LEN
#define BUSINESS_SACK_HI_OFFSET             (BUSINESS_HEADER_LEN + 4)
#define BUSINESS_TX_WINDOW                  32
#define BUSINESS_RX_WINDOW                  64
#define BUSINESS_RETX_TIMEOUT_MS            1000
#define BUSINESS_ACK_BATCH_PACKETS          4
#define BUSINESS_ACK_MAX_INTERVAL_MS        25
#define BUSINESS_STREAM_MODE                1
#define BUSINESS_STATUS_ACK_INTERVAL_MS     200
#define BUSINESS_SEQ_RESET_THRESHOLD        64
#define SLE_ENDPOINT_OUTDOOR_PILOT          SLE_PHY_PILOT_DENSITY_4_TO_1

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

static uint16_t business_read_u16(const uint8_t *data, uint16_t offset)
{
    return ((uint16_t)data[offset] << 8) | data[offset + 1];
}

static void business_write_u16(uint8_t *data, uint16_t offset, uint16_t value)
{
    data[offset] = (uint8_t)(value >> 8);
    data[offset + 1] = (uint8_t)value;
}

static uint32_t business_read_u32(const uint8_t *data, uint16_t offset)
{
    return ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
        ((uint32_t)data[offset + 2] << 8) | data[offset + 3];
}

static void business_write_u32(uint8_t *data, uint16_t offset, uint32_t value)
{
    data[offset] = (uint8_t)(value >> 24);
    data[offset + 1] = (uint8_t)(value >> 16);
    data[offset + 2] = (uint8_t)(value >> 8);
    data[offset + 3] = (uint8_t)value;
}

static uint16_t business_checksum(const uint8_t *data, uint16_t len)
{
    uint32_t sum = 0;

    for (uint16_t i = 0; i < len; i++) {
        if (i == BUSINESS_CRC_HI_OFFSET || i == (BUSINESS_CRC_HI_OFFSET + 1)) {
            continue;
        }
        sum += data[i];
    }

    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}

static bool business_validate_packet(const uint8_t *data, uint16_t len, uint8_t type)
{
    uint16_t payload_len;
    uint16_t crc;

    if (data == NULL || len < BUSINESS_HEADER_LEN || data[0] != BUSINESS_MAGIC0 ||
        data[1] != BUSINESS_MAGIC1 || data[2] != type) {
        return false;
    }

    payload_len = business_read_u16(data, BUSINESS_LEN_HI_OFFSET);
    if ((uint16_t)(payload_len + BUSINESS_HEADER_LEN) != len) {
        return false;
    }

    crc = business_read_u16(data, BUSINESS_CRC_HI_OFFSET);
    return crc == business_checksum(data, len);
}

static void business_fill_header(uint8_t *data, uint8_t type, uint8_t hop, uint16_t seq, uint16_t payload_len)
{
    data[0] = BUSINESS_MAGIC0;
    data[1] = BUSINESS_MAGIC1;
    data[2] = type;
    data[HOP_MARKER_OFFSET] = hop;
    business_write_u16(data, BUSINESS_SEQ_HI_OFFSET, seq);
    business_write_u16(data, BUSINESS_LEN_HI_OFFSET, payload_len);
    business_write_u16(data, BUSINESS_CRC_HI_OFFSET, 0);
    business_write_u16(data, BUSINESS_CRC_HI_OFFSET,
        business_checksum(data, (uint16_t)(payload_len + BUSINESS_HEADER_LEN)));
}

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
typedef struct {
    bool in_use;
    bool sacked;
    uint16_t seq;
    uint16_t len;
    uint64_t last_tx_us;
    uint8_t data[THROUGHPUT_PKT_LEN];
} business_tx_slot_t;

static business_tx_slot_t g_tx_window[BUSINESS_TX_WINDOW];
static uint8_t g_source_tx_buf[THROUGHPUT_PKT_LEN];
static uint16_t g_source_next_seq = 1;
static uint16_t g_source_oldest_seq = 1;
static uint8_t g_source_window_used = 0;
static uint32_t g_source_credit_stall = 0;
static uint32_t g_source_tx_fail = 0;
static uint32_t g_source_tx_new = 0;
static uint32_t g_source_tx_retx = 0;
static uint32_t g_source_ack = 0;
static uint32_t g_source_sack = 0;
static uint32_t g_source_timeout = 0;
static uint64_t g_source_last_cum_ack_us = 0;

static int16_t business_seq_diff(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right);
}

static void sle_source_upgrade_link(uint16_t conn_id)
{
    sle_set_data_len(conn_id, 1500);
    osal_msleep(100);

    sle_set_phy_t phy_parm = {
        .tx_format = SLE_RADIO_FRAME_2,
        .rx_format = SLE_RADIO_FRAME_2,
        .tx_phy = SLE_PHY_4M,
        .rx_phy = SLE_PHY_4M,
        .tx_pilot_density = SLE_ENDPOINT_OUTDOOR_PILOT,
        .rx_pilot_density = SLE_ENDPOINT_OUTDOOR_PILOT,
        .g_feedback = 0,
        .t_feedback = 0,
    };
    sle_set_phy_param(conn_id, &phy_parm);
}

static business_tx_slot_t *source_find_slot(uint16_t seq)
{
    for (uint8_t i = 0; i < BUSINESS_TX_WINDOW; i++) {
        if (g_tx_window[i].in_use && g_tx_window[i].seq == seq) {
            return &g_tx_window[i];
        }
    }
    return NULL;
}

static business_tx_slot_t *source_find_free_slot(void)
{
    for (uint8_t i = 0; i < BUSINESS_TX_WINDOW; i++) {
        if (!g_tx_window[i].in_use) {
            return &g_tx_window[i];
        }
    }
    return NULL;
}

static void source_reset_window(void)
{
    uint64_t now_us = uapi_tcxo_get_us();
    uint32_t irq_status = osal_irq_lock();

    memset(g_tx_window, 0, sizeof(g_tx_window));
    g_source_next_seq = 1;
    g_source_oldest_seq = 1;
    g_source_window_used = 0;
    g_source_last_cum_ack_us = now_us;
    osal_irq_restore(irq_status);
}

static void source_fill_data_packet(business_tx_slot_t *slot, uint16_t seq)
{
    slot->seq = seq;
    slot->len = THROUGHPUT_PKT_LEN;
    slot->sacked = false;
    memset(&slot->data[BUSINESS_HEADER_LEN], (int)(seq & 0xFF), BUSINESS_PAYLOAD_LEN);
    business_fill_header(slot->data, BUSINESS_TYPE_DATA, 0, seq, BUSINESS_PAYLOAD_LEN);
}

static uint32_t source_get_ack_gap_ms(uint64_t now_us)
{
    uint64_t last_ack_us;
    uint32_t irq_status = osal_irq_lock();

    last_ack_us = g_source_last_cum_ack_us;
    osal_irq_restore(irq_status);
    if (last_ack_us == 0 || now_us <= last_ack_us) {
        return 0;
    }
    return (uint32_t)((now_us - last_ack_us) / 1000ULL);
}

static bool source_select_retx(uint8_t *out, uint16_t *len, uint64_t now_us)
{
    business_tx_slot_t *slot;
    uint16_t seq;
    uint16_t newest_seq;
    uint32_t irq_status;

    if (out == NULL || len == NULL) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_source_window_used == 0 ||
        now_us - g_source_last_cum_ack_us < (BUSINESS_RETX_TIMEOUT_MS * 1000ULL)) {
        osal_irq_restore(irq_status);
        return false;
    }

    seq = g_source_oldest_seq;
    newest_seq = (uint16_t)(g_source_next_seq - 1);
    while (business_seq_diff(seq, newest_seq) <= 0) {
        slot = source_find_slot(seq);
        if (slot != NULL && !slot->sacked &&
            now_us - slot->last_tx_us >= (BUSINESS_RETX_TIMEOUT_MS * 1000ULL)) {
            slot->last_tx_us = now_us;
            *len = slot->len;
            (void)memcpy_s(out, THROUGHPUT_PKT_LEN, slot->data, slot->len);
            g_source_timeout++;
            osal_irq_restore(irq_status);
            return true;
        }
        seq++;
    }

    osal_irq_restore(irq_status);
    return false;
}

static bool source_select_new(uint8_t *out, uint16_t *len, uint64_t now_us)
{
    business_tx_slot_t *slot;
    uint32_t irq_status;

    if (out == NULL || len == NULL) {
        return false;
    }

    irq_status = osal_irq_lock();
    if (g_source_window_used >= BUSINESS_TX_WINDOW) {
        osal_irq_restore(irq_status);
        return false;
    }

    slot = source_find_free_slot();
    if (slot == NULL) {
        osal_irq_restore(irq_status);
        return false;
    }

    source_fill_data_packet(slot, g_source_next_seq);
    slot->in_use = true;
    slot->last_tx_us = now_us;
    *len = slot->len;
    (void)memcpy_s(out, THROUGHPUT_PKT_LEN, slot->data, slot->len);
    g_source_next_seq++;
    g_source_window_used++;
    osal_irq_restore(irq_status);
    return true;
}

static bool source_select_stream_new(uint8_t *out, uint16_t *len)
{
    uint16_t seq;
    uint32_t irq_status;

    if (out == NULL || len == NULL) {
        return false;
    }

    irq_status = osal_irq_lock();
    seq = g_source_next_seq++;
    osal_irq_restore(irq_status);

    memset(&out[BUSINESS_HEADER_LEN], (int)(seq & 0xFF), BUSINESS_PAYLOAD_LEN);
    business_fill_header(out, BUSINESS_TYPE_DATA, 0, seq, BUSINESS_PAYLOAD_LEN);
    *len = THROUGHPUT_PKT_LEN;
    return true;
}

static void source_handle_ack(uint16_t ack_seq, uint64_t sack_bitmap)
{
    uint64_t now_us = uapi_tcxo_get_us();
    uint32_t irq_status = osal_irq_lock();
#if BUSINESS_STREAM_MODE
    unused(ack_seq);
    unused(sack_bitmap);
    g_source_ack++;
    g_source_last_cum_ack_us = now_us;
    osal_irq_restore(irq_status);
    return;
#else
    uint16_t newest_seq = (uint16_t)(g_source_next_seq - 1);
    uint16_t lower_bound = (uint16_t)(g_source_oldest_seq - 1);
    bool ack_progress = false;

    if (g_source_window_used == 0 ||
        business_seq_diff(ack_seq, lower_bound) < 0 ||
        business_seq_diff(ack_seq, newest_seq) > 0) {
        osal_irq_restore(irq_status);
        return;
    }

    while (g_source_window_used > 0 && business_seq_diff(ack_seq, g_source_oldest_seq) >= 0) {
        business_tx_slot_t *slot = source_find_slot(g_source_oldest_seq);
        if (slot == NULL) {
            break;
        }
        slot->in_use = false;
        g_source_window_used--;
        g_source_ack++;
        g_source_oldest_seq++;
        ack_progress = true;
    }

    if (ack_progress) {
        g_source_last_cum_ack_us = now_us;
    }

    for (uint8_t bit = 0; bit < 64; bit++) {
        uint16_t seq;
        business_tx_slot_t *slot;

        if ((sack_bitmap & (1ULL << bit)) == 0) {
            continue;
        }

        seq = (uint16_t)(ack_seq + 1 + bit);
        if (business_seq_diff(seq, g_source_oldest_seq) < 0 ||
            business_seq_diff(seq, newest_seq) > 0) {
            continue;
        }

        slot = source_find_slot(seq);
        if (slot != NULL && !slot->sacked) {
            slot->sacked = true;
            g_source_sack++;
        }
    }

    osal_irq_restore(irq_status);
#endif
}

static void ssaps_server_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(conn_id);

    if (status != ERRCODE_SUCC || write_cb_para == NULL || write_cb_para->value == NULL) {
        return;
    }

    if (business_validate_packet(write_cb_para->value, write_cb_para->length, BUSINESS_TYPE_ACK)) {
        uint64_t sack_bitmap = 0;

        if (write_cb_para->length >= BUSINESS_ACK_PACKET_LEN) {
            sack_bitmap = ((uint64_t)business_read_u32(write_cb_para->value, BUSINESS_SACK_HI_OFFSET) << 32) |
                business_read_u32(write_cb_para->value, BUSINESS_SACK_LO_OFFSET);
        }
        source_handle_ack(business_read_u16(write_cb_para->value, BUSINESS_SEQ_HI_OFFSET), sack_bitmap);
    }
}

static void sle_throughput_tx_task(void)
{
    uint32_t last_tx_new = 0;
    uint32_t last_tx_retx = 0;
    uint32_t last_ack = 0;
    uint32_t last_sack = 0;
    uint32_t last_timeout = 0;
    uint32_t last_credit_stall = 0;
    uint32_t last_tx_fail = 0;
    uint64_t last_stats_us = 0;

    osal_printk("[Business TX] Node1 ready. pkt:%u payload:%u pacing:%ums window:%u stream:%u\r\n",
        THROUGHPUT_PKT_LEN, BUSINESS_PAYLOAD_LEN, THROUGHPUT_PACING_MS, BUSINESS_TX_WINDOW, BUSINESS_STREAM_MODE);

    while (1) {
        while (!sle_uart_client_is_connected()) {
            osal_msleep(100);
        }

        uint16_t conn_id = get_connect_id();
        osal_printk("[Speed Test] R1 linked. Waiting MTU.\r\n");
        while (sle_uart_client_is_connected() && !g_mtu_negotiated) {
            osal_msleep(100);
        }

        if (!sle_uart_client_is_connected()) {
            continue;
        }

        osal_msleep(500);
        source_reset_window();
        sle_source_upgrade_link(conn_id);
        osal_printk("[Business TX] 5-hop stream traffic started.\r\n");
        last_stats_us = uapi_tcxo_get_us();

        while (sle_uart_client_is_connected() && g_mtu_negotiated) {
            uint64_t now_us = uapi_tcxo_get_us();
            uint16_t tx_len = 0;
            bool has_packet = false;
            bool selected_retx = false;

            if (now_us - last_stats_us >= (SOURCE_STATS_INTERVAL_MS * 1000ULL)) {
                osal_printk("[Source Stats] tx_new:%u tx_retx:%u ack:%u sack:%u window:%u timeout:%u ack_gap:%u credit_stall:%u tx_fail:%u\r\n",
                    g_source_tx_new - last_tx_new,
                    g_source_tx_retx - last_tx_retx,
                    g_source_ack - last_ack,
                    g_source_sack - last_sack,
                    g_source_window_used,
                    g_source_timeout - last_timeout,
                    source_get_ack_gap_ms(now_us),
                    g_source_credit_stall - last_credit_stall,
                    g_source_tx_fail - last_tx_fail);
                last_tx_new = g_source_tx_new;
                last_tx_retx = g_source_tx_retx;
                last_ack = g_source_ack;
                last_sack = g_source_sack;
                last_timeout = g_source_timeout;
                last_credit_stall = g_source_credit_stall;
                last_tx_fail = g_source_tx_fail;
                last_stats_us = now_us;
            }

            if (gle_tx_acb_data_num_get() > 0) {
#if BUSINESS_STREAM_MODE
                has_packet = source_select_stream_new(g_source_tx_buf, &tx_len);
                selected_retx = false;
#else
                has_packet = source_select_retx(g_source_tx_buf, &tx_len, now_us);
                selected_retx = has_packet;
                if (!has_packet) {
                    has_packet = source_select_new(g_source_tx_buf, &tx_len, now_us);
                }
#endif
                if (!has_packet) {
                    osal_msleep(1);
                    continue;
                }

                if (sle_uart_server_send_report_by_handle(g_source_tx_buf, tx_len) != ERRCODE_SUCC) {
                    g_source_tx_fail++;
                } else {
                    if (selected_retx) {
                        g_source_tx_retx++;
                    } else {
                        g_source_tx_new++;
                    }
                }
                osal_msleep(THROUGHPUT_PACING_MS);
            } else {
                g_source_credit_stall++;
                osal_yield();
            }
        }

        osal_printk("[Business TX] source link lost. stall:%u tx_fail:%u\r\n",
            g_source_credit_stall, g_source_tx_fail);
        source_reset_window();
    }
}

static void *sle_uart_server_task(const char *arg)
{
    unused(arg);
    sle_uart_server_init(NULL, ssaps_server_write_request_cbk);
    sle_throughput_tx_task();
    return NULL;
}

#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)

static volatile uint32_t g_rx_bytes = 0;
static volatile uint32_t g_rx_packets = 0;
static volatile uint32_t g_gap_seen_packets = 0;
static volatile uint32_t g_invalid_hop_packets = 0;
static volatile uint32_t g_dup_packets = 0;
static volatile uint32_t g_crc_drop_packets = 0;
static volatile uint32_t g_recovered_packets = 0;
static volatile uint32_t g_ack_sent_packets = 0;
static volatile uint32_t g_ack_suppressed_packets = 0;
static uint16_t g_expected_seq = 1;
static uint16_t g_gap_seq = 0;
static bool g_seq_valid = true;
static bool g_gap_active = false;
static uint16_t g_last_ack_seq = 0;
static uint64_t g_last_ack_sack_bitmap = 0;
static uint64_t g_last_ack_us = 0;
static bool g_last_ack_valid = false;

typedef struct {
    bool valid;
    uint16_t seq;
    uint16_t payload_len;
} business_rx_slot_t;

static business_rx_slot_t g_rx_window[BUSINESS_RX_WINDOW];

typedef struct {
    uint32_t bytes;
    uint32_t packets;
    uint32_t gap_seen_packets;
    uint32_t invalid_hop_packets;
    uint32_t dup_packets;
    uint32_t crc_drop_packets;
    uint32_t recovered_packets;
    uint32_t ack_sent_packets;
    uint32_t ack_suppressed_packets;
    uint16_t expected_seq;
} sle_uart_rx_stats_t;

static int16_t business_seq_diff(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right);
}

static void node6_send_ack(uint16_t ack_seq, uint64_t sack_bitmap)
{
    static uint8_t ack_packet[BUSINESS_ACK_PACKET_LEN];

    business_write_u32(ack_packet, BUSINESS_SACK_LO_OFFSET, (uint32_t)sack_bitmap);
    business_write_u32(ack_packet, BUSINESS_SACK_HI_OFFSET, (uint32_t)(sack_bitmap >> 32));
    business_fill_header(ack_packet, BUSINESS_TYPE_ACK, EXPECTED_ROUTER_HOPS, ack_seq, BUSINESS_ACK_PAYLOAD_LEN);
    (void)sle_uart_client_write_upstream_cmd(ack_packet, sizeof(ack_packet));
    g_ack_sent_packets++;
}

static void node6_maybe_send_ack(uint16_t ack_seq, uint64_t sack_bitmap)
{
    uint64_t now_us = uapi_tcxo_get_us();
#if BUSINESS_STREAM_MODE
    bool timer_ready = !g_last_ack_valid ||
        now_us - g_last_ack_us >= (BUSINESS_STATUS_ACK_INTERVAL_MS * 1000ULL);

    unused(sack_bitmap);
    if (timer_ready) {
        g_last_ack_seq = ack_seq;
        g_last_ack_sack_bitmap = 0;
        g_last_ack_us = now_us;
        g_last_ack_valid = true;
        node6_send_ack(ack_seq, 0);
    } else {
        g_ack_suppressed_packets++;
    }
#else
    int16_t ack_advance = g_last_ack_valid ? business_seq_diff(ack_seq, g_last_ack_seq) :
        BUSINESS_ACK_BATCH_PACKETS;
    bool force_ack = !g_last_ack_valid || sack_bitmap != g_last_ack_sack_bitmap;
    bool batch_ready = ack_advance >= BUSINESS_ACK_BATCH_PACKETS;
    bool timer_ready = now_us - g_last_ack_us >= (BUSINESS_ACK_MAX_INTERVAL_MS * 1000ULL);

    if (force_ack || batch_ready || timer_ready) {
        g_last_ack_seq = ack_seq;
        g_last_ack_sack_bitmap = sack_bitmap;
        g_last_ack_us = now_us;
        g_last_ack_valid = true;
        node6_send_ack(ack_seq, sack_bitmap);
    } else {
        g_ack_suppressed_packets++;
    }
#endif
}

static void node6_reset_sequence(uint16_t seq)
{
    g_expected_seq = seq;
    g_gap_seq = 0;
    g_seq_valid = true;
    g_gap_active = false;
    g_last_ack_valid = false;
    memset(g_rx_window, 0, sizeof(g_rx_window));
}

static business_rx_slot_t *node6_find_rx_slot(uint16_t seq)
{
    business_rx_slot_t *slot = &g_rx_window[seq % BUSINESS_RX_WINDOW];

    if (slot->valid && slot->seq == seq) {
        return slot;
    }
    return NULL;
}

static void node6_deliver_packet(uint16_t payload_len, bool recovered)
{
    g_rx_bytes += payload_len;
    g_rx_packets++;
    if (recovered) {
        g_recovered_packets++;
    }
}

static bool node6_store_out_of_order(uint16_t seq, uint16_t payload_len)
{
    business_rx_slot_t *slot = &g_rx_window[seq % BUSINESS_RX_WINDOW];

    if (slot->valid && slot->seq == seq) {
        g_dup_packets++;
        return true;
    }

    if (slot->valid) {
        return false;
    }

    slot->valid = true;
    slot->seq = seq;
    slot->payload_len = payload_len;
    return true;
}

static void node6_drain_rx_window(void)
{
    business_rx_slot_t *slot;

    while ((slot = node6_find_rx_slot(g_expected_seq)) != NULL) {
        node6_deliver_packet(slot->payload_len, true);
        slot->valid = false;
        g_expected_seq++;
    }
}

static uint64_t node6_build_sack_bitmap(void)
{
    uint64_t bitmap = 0;

    for (uint8_t bit = 0; bit < BUSINESS_RX_WINDOW; bit++) {
        uint16_t seq = (uint16_t)(g_expected_seq + bit);

        if (node6_find_rx_slot(seq) != NULL) {
            bitmap |= (1ULL << bit);
        }
    }

    return bitmap;
}

static void node6_update_gap_state(void)
{
    g_gap_active = (node6_build_sack_bitmap() != 0);
    g_gap_seq = g_gap_active ? g_expected_seq : 0;
}

static void sle_uart_rx_stats_add(ssapc_handle_value_t *data)
{
    uint16_t seq;
    uint16_t ack_seq;
    uint16_t payload_len;
    uint64_t sack_bitmap;
    int16_t seq_delta;
    uint32_t irq_status = osal_irq_lock();

    if (data->data_len < BUSINESS_HEADER_LEN || data->data[HOP_MARKER_OFFSET] != EXPECTED_ROUTER_HOPS) {
        g_invalid_hop_packets++;
        osal_irq_restore(irq_status);
        return;
    }

    if (!business_validate_packet(data->data, data->data_len, BUSINESS_TYPE_DATA)) {
        g_crc_drop_packets++;
        osal_irq_restore(irq_status);
        return;
    }

    seq = business_read_u16(data->data, BUSINESS_SEQ_HI_OFFSET);
    payload_len = business_read_u16(data->data, BUSINESS_LEN_HI_OFFSET);
    if (!g_seq_valid) {
        node6_reset_sequence(seq);
    } else if (business_seq_diff(seq, g_expected_seq) < 0 &&
        business_seq_diff(g_expected_seq, seq) > BUSINESS_SEQ_RESET_THRESHOLD) {
        node6_reset_sequence(seq);
    }

    seq_delta = business_seq_diff(seq, g_expected_seq);
#if BUSINESS_STREAM_MODE
    if (seq_delta == 0) {
        node6_deliver_packet(payload_len, false);
        g_expected_seq++;
    } else if (seq_delta < 0) {
        g_dup_packets++;
    } else {
        g_gap_seen_packets += (uint16_t)seq_delta;
        node6_deliver_packet(payload_len, false);
        g_expected_seq = (uint16_t)(seq + 1);
    }

    ack_seq = (uint16_t)(g_expected_seq - 1);
    osal_irq_restore(irq_status);
    node6_maybe_send_ack(ack_seq, 0);
    return;
#else
    if (seq_delta == 0) {
        if (g_gap_active && seq == g_gap_seq) {
            node6_deliver_packet(payload_len, true);
        } else {
            node6_deliver_packet(payload_len, false);
        }
        g_expected_seq++;
        node6_drain_rx_window();
        node6_update_gap_state();
    } else if (seq_delta < 0) {
        g_dup_packets++;
    } else if (seq_delta < BUSINESS_RX_WINDOW) {
        if (!node6_store_out_of_order(seq, payload_len)) {
            g_gap_seen_packets++;
        } else if (!g_gap_active || g_gap_seq != g_expected_seq) {
            g_gap_seen_packets += (uint16_t)seq_delta;
            g_gap_seq = g_expected_seq;
            g_gap_active = true;
        }
    } else {
        g_gap_seen_packets += (uint16_t)seq_delta;
        g_gap_seq = g_expected_seq;
        g_gap_active = true;
    }

    ack_seq = (uint16_t)(g_expected_seq - 1);
    sack_bitmap = node6_build_sack_bitmap();
    osal_irq_restore(irq_status);
    node6_maybe_send_ack(ack_seq, sack_bitmap);
#endif
}

static void sle_uart_rx_stats_take(sle_uart_rx_stats_t *stats)
{
    uint32_t irq_status = osal_irq_lock();
    stats->bytes = g_rx_bytes;
    stats->packets = g_rx_packets;
    stats->gap_seen_packets = g_gap_seen_packets;
    stats->invalid_hop_packets = g_invalid_hop_packets;
    stats->dup_packets = g_dup_packets;
    stats->crc_drop_packets = g_crc_drop_packets;
    stats->recovered_packets = g_recovered_packets;
    stats->ack_sent_packets = g_ack_sent_packets;
    stats->ack_suppressed_packets = g_ack_suppressed_packets;
    stats->expected_seq = g_expected_seq;
    g_rx_bytes = 0;
    g_rx_packets = 0;
    g_gap_seen_packets = 0;
    g_invalid_hop_packets = 0;
    g_dup_packets = 0;
    g_crc_drop_packets = 0;
    g_recovered_packets = 0;
    g_ack_sent_packets = 0;
    g_ack_suppressed_packets = 0;
    osal_irq_restore(irq_status);
}

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data_len == 0) {
        return;
    }

    sle_uart_rx_stats_add(data);
}

void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(data);
    unused(status);
}

static void *sle_uart_client_task(const char *arg)
{
    unused(arg);
    uint64_t last_time_us;

    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);
    osal_printk("[Business RX] Node6 sink ready. Awaiting 5-hop stream data.\r\n");
    last_time_us = uapi_tcxo_get_us();

    while (1) {
        uint64_t now_us;
        uint64_t elapsed_us;
        sle_uart_rx_stats_t stats;
        uint32_t bytes_per_sec;
        uint32_t kbps;

        osal_msleep(SPEED_SAMPLE_INTERVAL_MS);
        now_us = uapi_tcxo_get_us();
        elapsed_us = now_us - last_time_us;
        last_time_us = now_us;

        sle_uart_rx_stats_take(&stats);
        if (stats.bytes == 0 || elapsed_us == 0) {
            if (stats.invalid_hop_packets != 0 || stats.crc_drop_packets != 0 || stats.dup_packets != 0) {
                osal_printk("[5-HOP BUSINESS] Waiting full path invalid_hop:%u crc_drop:%u dup:%u ack_sent:%u ack_supp:%u expected:%u\r\n",
                    stats.invalid_hop_packets, stats.crc_drop_packets, stats.dup_packets,
                    stats.ack_sent_packets, stats.ack_suppressed_packets, stats.expected_seq);
            }
            continue;
        }

        bytes_per_sec = (uint32_t)(((uint64_t)stats.bytes * US_PER_SECOND) / elapsed_us);
        kbps = (bytes_per_sec * 8) / 1000;
        osal_printk("[5-HOP BUSINESS] Speed:%u Bytes/s (%u kbps), rx_pkt:%u recovered:%u dup:%u gap_seen:%u crc_drop:%u invalid_hop:%u ack_sent:%u ack_supp:%u expected:%u\r\n",
            bytes_per_sec, kbps, stats.packets, stats.recovered_packets, stats.dup_packets,
            stats.gap_seen_packets, stats.crc_drop_packets, stats.invalid_hop_packets,
            stats.ack_sent_packets, stats.ack_suppressed_packets, stats.expected_seq);
    }
    return NULL;
}
#endif

static void sle_uart_entry(void)
{
    osal_task *task_handle = NULL;

    uapi_watchdog_disable();
    osal_kthread_lock();
#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_server_task, 0,
        "ThroughputTX", SLE_UART_TASK_STACK_SIZE);
#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
    task_handle = osal_kthread_create((osal_kthread_handler)sle_uart_client_task, 0,
        "ThroughputRX", SLE_UART_TASK_STACK_SIZE);
#endif
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UART_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(sle_uart_entry);
