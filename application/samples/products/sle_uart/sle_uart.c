/*
 * Endpoint Node 1 (source) / Node 6 (sink).
 *   - Default: synthetic 1400-byte throughput stream (benchmark).
 *   - With CONFIG_SAMPLE_FILE_TRANSFER_MODE: real UART/USB byte stream
 *     framed with 12-byte header + CRC16 over 5 hops; Node 6 verifies and
 *     writes back to its UART. Use to send photo files PC -> Node1 ... Node6 -> PC.
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

#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
#include "uapi_crc.h"
#include "sle_file_transfer.h"
#endif

#define SLE_UART_TASK_PRIO                  27
#define SLE_UART_TASK_STACK_SIZE            0x2000
#define THROUGHPUT_PKT_LEN                  1200
#define THROUGHPUT_PACING_MS                10
#define NODE6_WATCHDOG_INTERVAL_MS          500

#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
#define FT_UART_BAUDRATE                    921600
#define FT_UART_RX_BUFFER_SIZE              4096
#define FT_RING_BUFFER_SIZE                 65536
#define FT_RING_MASK                        (FT_RING_BUFFER_SIZE - 1)
#define FT_FLUSH_TIMEOUT_US                 30000ULL
#define FT_FLOW_REPORT_INTERVAL_US          50000ULL
#define FT_ACK_STALL_RECOVERY_US          5000000ULL
#define FT_LINK_RECOVERY_COOLDOWN_US      10000000ULL
#define SLE_HOST_V5HP_MAGIC                 0x50483556u
#define SLE_HOST_V5H2_MAGIC                 0x32483556u
#define SLE_HOST_V5T1_MAGIC                 0x31543556u
#define SLE_HOST_V5H2_FIXED_LEN             52u
#define SLE_HOST_V5T1_FIXED_LEN             46u
#define SLE_RELIABLE_LENGTH_UNKNOWN         0xFFFFFFFFFFFFFFFFULL
#endif

extern uint8_t gle_tx_acb_data_num_get(void);
extern volatile bool g_mtu_negotiated;

/*
 * Node 6 RX-side prints share UART 0 with the data path in file-transfer mode
 * (CONFIG_DEBUG_UART = CONFIG_SLE_UART_BUS = 0). Letting them through would
 * splice ASCII log lines into the jpg byte stream and break md5. Silence them
 * on the receiver only — sender prints are harmless because PC doesn't read
 * Node 1's UART.
 */
#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE) && defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)
#define rx_log(fmt, ...)    do { } while (0)
#else
#define rx_log(fmt, ...)    osal_printk(fmt, ##__VA_ARGS__)
#endif

#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
static uint8_t g_ft_uart_rx_buff[FT_UART_RX_BUFFER_SIZE];
static uart_buffer_config_t g_ft_uart_buffer_config = {
    .rx_buffer = g_ft_uart_rx_buff,
    .rx_buffer_size = FT_UART_RX_BUFFER_SIZE
};

static void file_uart_init(void)
{
    if (CONFIG_SLE_UART_BUS == 0 || CONFIG_SLE_UART_BUS == 1) {
        uapi_pin_set_mode(CONFIG_UART_TXD_PIN, PIN_MODE_1);
        uapi_pin_set_mode(CONFIG_UART_RXD_PIN, PIN_MODE_1);
    }

    uart_attr_t attr = {
        .baud_rate = FT_UART_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE
    };
    uart_pin_config_t pin_config = {
        .tx_pin = CONFIG_UART_TXD_PIN,
        .rx_pin = CONFIG_UART_RXD_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE
    };
    uapi_uart_deinit(CONFIG_SLE_UART_BUS);
    uapi_uart_init(CONFIG_SLE_UART_BUS, &pin_config, &attr, NULL, &g_ft_uart_buffer_config);
}
#endif  /* CONFIG_SAMPLE_FILE_TRANSFER_MODE */

#if defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER)
static void sle_source_prepare_link(uint16_t conn_id)
{
    (void)sle_set_data_len(conn_id, 1500);
}

#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)

static uint8_t g_ft_ring[FT_RING_BUFFER_SIZE];
static volatile uint32_t g_ft_ring_head = 0;
static volatile uint32_t g_ft_ring_tail = 0;
static volatile uint32_t g_ft_uart_overflow = 0;

typedef struct {
    bool valid;
    uint16_t seq;
    uint16_t len;
    uint16_t retries;
    uint64_t last_send_us;
    uint8_t data[SLE_FT_MAX_PACKET];
} sle_ft_tx_window_entry_t;

static sle_ft_tx_window_entry_t g_ft_tx_window[SLE_FT_WINDOW_SIZE];
static uint16_t g_ft_session_id = 0;
static uint16_t g_ft_next_seq = 0;
static uint8_t g_ft_unacked = 0;
static uint8_t g_ft_cwnd = SLE_FT_CWND_INITIAL;
static uint8_t g_ft_cwnd_ack_accum = 0;
static volatile bool g_ft_control_pending = false;
static sle_ft_control_t g_ft_pending_control;
static uint32_t g_ft_retransmitted = 0;
static uint32_t g_ft_ack_received = 0;
static uint32_t g_ft_srtt_us = 0;
static uint32_t g_ft_rttvar_us = 0;
static uint32_t g_ft_rto_us = (uint32_t)SLE_FT_RETRY_INITIAL_US;
static uint64_t g_ft_last_ack_progress_us = 0;
static uint64_t g_ft_last_recovery_us = 0;
static bool g_diag_probe_pending = false;
static sle_diag_probe_t g_pending_diag_probe;

typedef enum {
    SLE_TX_MODE_AUTO = 0,
    SLE_TX_MODE_RELIABLE
} sle_tx_mode_t;

static sle_tx_mode_t g_sle_tx_mode = SLE_TX_MODE_AUTO;
static uint64_t g_reliable_remaining = 0;

static uint32_t sle_ring_available(void)
{
    return (g_ft_ring_head - g_ft_ring_tail) & FT_RING_MASK;
}

static bool sle_seq_after(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right) > 0;
}

static bool sle_seq_acked(uint16_t ack_seq, uint16_t seq)
{
    return (int16_t)(ack_seq - seq) >= 0;
}

static void sle_ring_copy(uint32_t offset, uint8_t *destination, uint32_t length)
{
    uint32_t tail = g_ft_ring_tail;
    uint32_t start = (tail + offset) & FT_RING_MASK;
    uint32_t first = length;

    if (first > FT_RING_BUFFER_SIZE - start) {
        first = FT_RING_BUFFER_SIZE - start;
    }
    if (first > 0) {
        (void)memcpy_s(destination, length, &g_ft_ring[start], first);
    }
    if (length > first) {
        (void)memcpy_s(destination + first, length - first, g_ft_ring, length - first);
    }
}

static void sle_file_reset_congestion_state(void)
{
    g_ft_cwnd = SLE_FT_CWND_INITIAL;
    g_ft_cwnd_ack_accum = 0;
    g_ft_srtt_us = 0;
    g_ft_rttvar_us = 0;
    g_ft_rto_us = (uint32_t)SLE_FT_RETRY_INITIAL_US;
    g_ft_last_ack_progress_us = 0;
    g_ft_last_recovery_us = 0;
}

static void sle_file_update_rtt(uint64_t sample_us)
{
    uint32_t sample;
    uint32_t error;
    uint64_t rto;

    if (sample_us == 0 || sample_us > SLE_FT_RETRY_MAX_US) {
        return;
    }
    sample = (uint32_t)sample_us;
    if (g_ft_srtt_us == 0) {
        g_ft_srtt_us = sample;
        g_ft_rttvar_us = sample / 2u;
    } else {
        error = (g_ft_srtt_us > sample) ? (g_ft_srtt_us - sample) : (sample - g_ft_srtt_us);
        g_ft_rttvar_us = (3u * g_ft_rttvar_us + error) / 4u;
        g_ft_srtt_us = (7u * g_ft_srtt_us + sample) / 8u;
    }
    rto = (uint64_t)g_ft_srtt_us + 4ULL * g_ft_rttvar_us;
    if (rto < SLE_FT_RETRY_MIN_US) {
        rto = SLE_FT_RETRY_MIN_US;
    } else if (rto > SLE_FT_RETRY_MAX_US) {
        rto = SLE_FT_RETRY_MAX_US;
    }
    g_ft_rto_us = (uint32_t)rto;
}

static void sle_ring_consume(uint32_t length)
{
    g_ft_ring_tail = (g_ft_ring_tail + length) & FT_RING_MASK;
}

static uint16_t sle_read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint64_t sle_read_le64(const uint8_t *value)
{
    uint64_t result = 0;

    for (uint8_t index = 0; index < 8; index++) {
        result |= (uint64_t)value[index] << (index * 8);
    }
    return result;
}

static void sle_file_abort_stale_tx(void)
{
    uint32_t irq_status;

    if (g_sle_tx_mode == SLE_TX_MODE_AUTO && g_ft_unacked == 0) {
        return;
    }

    irq_status = osal_irq_lock();
    for (uint8_t index = 0; index < SLE_FT_WINDOW_SIZE; index++) {
        g_ft_tx_window[index].valid = false;
    }
    g_ft_unacked = 0;
    g_ft_control_pending = false;
    osal_irq_restore(irq_status);

    g_sle_tx_mode = SLE_TX_MODE_AUTO;
    g_reliable_remaining = 0;
    g_ft_next_seq = 0;
    sle_file_reset_congestion_state();
    g_ft_session_id++;
    if (g_ft_session_id == 0) {
        g_ft_session_id = 1;
    }
    osal_printk("[FT TX] stale session cleared for link probe.\r\n");
}

static bool sle_diag_try_send_pending(void)
{
    if (!g_diag_probe_pending || !sle_uart_client_is_connected() || !g_mtu_negotiated ||
        gle_tx_acb_data_num_get() == 0) {
        return false;
    }
    if (sle_uart_server_send_report_by_handle((uint8_t *)&g_pending_diag_probe,
        sizeof(g_pending_diag_probe)) != ERRCODE_SUCC) {
        return false;
    }
    g_diag_probe_pending = false;
    return true;
}

static bool sle_diag_try_process_host_command(void)
{
    sle_host_diag_command_t command;
    uint8_t flags = SLE_DIAG_FLAG_ENDPOINT;
    uint32_t available = sle_ring_available();
    uint32_t magic;

    if (available < sizeof(magic)) {
        return false;
    }
    sle_ring_copy(0, (uint8_t *)&magic, sizeof(magic));
    if (magic != SLE_HOST_DIAG_MAGIC) {
        return false;
    }
    if (available < SLE_HOST_DIAG_LEN) {
        return false;
    }

    sle_ring_copy(0, (uint8_t *)&command, sizeof(command));
    sle_ring_consume(sizeof(command));
    sle_file_abort_stale_tx();
    if (sle_uart_client_is_connected()) {
        flags |= SLE_DIAG_FLAG_DOWNSTREAM_LINK | SLE_DIAG_FLAG_REVERSE_READY;
    }
    if (g_mtu_negotiated) {
        flags |= SLE_DIAG_FLAG_DOWNSTREAM_MTU;
    }
    osal_printk("[DIAG]%u,%u,%u\r\n", command.probe_id,
        SLE_DIAG_NODE_NODE1, flags);

    g_pending_diag_probe.magic = SLE_DIAG_PROBE_MAGIC;
    g_pending_diag_probe.probe_id = command.probe_id;
    g_pending_diag_probe.reserved = 0;
    g_pending_diag_probe.crc16 = uapi_crc16(0, (uint8_t *)&g_pending_diag_probe,
        SLE_DIAG_PROBE_LEN - sizeof(g_pending_diag_probe.crc16));
    g_diag_probe_pending = true;
    (void)sle_diag_try_send_pending();
    return true;
}

static bool sle_file_select_tx_mode(void)
{
    uint8_t fixed[16];
    uint32_t magic;
    uint32_t available = sle_ring_available();

    if (available < sizeof(magic)) {
        return false;
    }
    sle_ring_copy(0, (uint8_t *)&magic, sizeof(magic));
    if (magic == SLE_HOST_DIAG_MAGIC) {
        return sle_diag_try_process_host_command();
    }
    if (magic == SLE_HOST_V5HP_MAGIC) {
        if (available < 12) {
            return false;
        }
        sle_ring_copy(0, fixed, 12);
        g_reliable_remaining = 44ULL + sle_read_le64(&fixed[4]);
    } else if (magic == SLE_HOST_V5H2_MAGIC) {
        uint16_t filename_len;

        if (available < 16) {
            return false;
        }
        sle_ring_copy(0, fixed, 16);
        filename_len = sle_read_le16(&fixed[6]);
        g_reliable_remaining = SLE_HOST_V5H2_FIXED_LEN + filename_len + sle_read_le64(&fixed[8]);
    } else if (magic == SLE_HOST_V5T1_MAGIC) {
        if (available < 14) {
            return false;
        }
        sle_ring_copy(0, fixed, 14);
        g_reliable_remaining = SLE_HOST_V5T1_FIXED_LEN + sle_read_le16(&fixed[12]);
    } else {
        sle_ring_consume(1);
        return true;
    }
    g_sle_tx_mode = SLE_TX_MODE_RELIABLE;
    return true;
}

static void sle_file_report_flow(uint64_t now_us)
{
    static uint64_t last_report_us = 0;
    uint32_t head;
    uint32_t tail;
    uint32_t used;

    if (last_report_us != 0 && now_us - last_report_us < FT_FLOW_REPORT_INTERVAL_US) {
        return;
    }
    head = g_ft_ring_head;
    tail = g_ft_ring_tail;
    used = (head - tail) & FT_RING_MASK;
    osal_printk("[FTFC]%u,%u,%u,%u,%u\r\n", FT_RING_MASK - used, g_ft_unacked,
        g_ft_uart_overflow, g_ft_cwnd, g_ft_rto_us / 1000u);
    last_report_us = now_us;
}

static void file_uart_rx_handler(const void *buffer, uint16_t length, bool error)
{
    uint32_t copy_len;
    uint32_t first;
    uint32_t free_bytes;

    unused(error);
    if (buffer == NULL || length == 0) {
        return;
    }
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t head = g_ft_ring_head;
    uint32_t tail = g_ft_ring_tail;
    free_bytes = (tail - head - 1u) & FT_RING_MASK;
    copy_len = (length < free_bytes) ? length : free_bytes;
    first = copy_len;
    if (first > FT_RING_BUFFER_SIZE - head) {
        first = FT_RING_BUFFER_SIZE - head;
    }
    if (first > 0 && memcpy_s(&g_ft_ring[head], FT_RING_BUFFER_SIZE - head, src, first) != EOK) {
        copy_len = 0;
    } else if (copy_len > first &&
        memcpy_s(g_ft_ring, FT_RING_BUFFER_SIZE, src + first, copy_len - first) != EOK) {
        copy_len = first;
    }
    g_ft_ring_head = (head + copy_len) & FT_RING_MASK;
    g_ft_uart_overflow += (uint32_t)length - copy_len;
}

static void sle_file_control_write_cb(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    sle_ft_control_t control;
    sle_ft_status_t path_status;
    sle_diag_report_t report;
    uint32_t magic;
    uint32_t irq_status;

    unused(server_id);
    unused(conn_id);
    if (status != ERRCODE_SUCC || write_cb_para == NULL || write_cb_para->value == NULL ||
        write_cb_para->length < sizeof(magic) ||
        memcpy_s(&magic, sizeof(magic), write_cb_para->value, sizeof(magic)) != EOK) {
        return;
    }
    if (magic == SLE_DIAG_REPORT_MAGIC) {
        if (write_cb_para->length == SLE_DIAG_REPORT_LEN &&
            memcpy_s(&report, sizeof(report), write_cb_para->value, sizeof(report)) == EOK &&
            uapi_crc16(0, (uint8_t *)&report,
                SLE_DIAG_REPORT_LEN - sizeof(report.crc16)) == report.crc16) {
            osal_printk("[DIAG]%u,%u,%u\r\n", report.probe_id,
                report.node_id, report.flags);
        }
        return;
    }
    if (magic == SLE_FT_STATUS_MAGIC) {
        if (write_cb_para->length == SLE_FT_STATUS_LEN &&
            memcpy_s(&path_status, sizeof(path_status), write_cb_para->value,
                sizeof(path_status)) == EOK &&
            uapi_crc16(0, (uint8_t *)&path_status,
                SLE_FT_STATUS_LEN - sizeof(path_status.crc16)) == path_status.crc16) {
            path_status.link_mask |= SLE_FT_LINK_NODE1;
            osal_printk("[FTPATH]%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\r\n",
                path_status.session_id, path_status.ack_seq, path_status.link_mask,
                path_status.bottleneck_node, path_status.max_queue_percent,
                path_status.flags, path_status.credit_wait_delta,
                path_status.tx_fail_delta, path_status.drop_delta,
                g_ft_cwnd, g_ft_rto_us / 1000u);
        }
        return;
    }
    if (magic != SLE_FT_CTRL_MAGIC || write_cb_para->length != SLE_FT_CTRL_LEN ||
        memcpy_s(&control, sizeof(control), write_cb_para->value, sizeof(control)) != EOK ||
        uapi_crc16(0, (uint8_t *)&control, SLE_FT_CTRL_LEN - sizeof(control.crc16)) != control.crc16) {
        return;
    }

    irq_status = osal_irq_lock();
    if (!g_ft_control_pending || g_ft_pending_control.session_id != control.session_id) {
        g_ft_pending_control = control;
    } else {
        uint16_t merged_ack = g_ft_pending_control.ack_seq;
        uint16_t merged_nack = g_ft_pending_control.nack_seq;

        if (sle_seq_after(control.ack_seq, merged_ack)) {
            merged_ack = control.ack_seq;
        }
        if (merged_nack != SLE_FT_NO_NACK && sle_seq_acked(merged_ack, merged_nack)) {
            merged_nack = SLE_FT_NO_NACK;
        }
        if (control.nack_seq != SLE_FT_NO_NACK && !sle_seq_acked(merged_ack, control.nack_seq)) {
            uint16_t new_distance = (uint16_t)(control.nack_seq - merged_ack - 1u);
            uint16_t old_distance = (uint16_t)(merged_nack - merged_ack - 1u);
            if (merged_nack == SLE_FT_NO_NACK || new_distance < old_distance) {
                merged_nack = control.nack_seq;
            }
        }
        g_ft_pending_control.ack_seq = merged_ack;
        g_ft_pending_control.nack_seq = merged_nack;
    }
    g_ft_control_pending = true;
    osal_irq_restore(irq_status);
}

static void sle_file_process_control(void)
{
    sle_ft_control_t control;
    bool pending = false;
    uint8_t acked = 0;
    uint64_t rtt_sample_us = 0;
    uint64_t now_us = uapi_tcxo_get_us();
    uint32_t irq_status = osal_irq_lock();

    if (g_ft_control_pending) {
        control = g_ft_pending_control;
        g_ft_control_pending = false;
        pending = true;
    }
    osal_irq_restore(irq_status);
    if (!pending || control.session_id != g_ft_session_id) {
        return;
    }

    for (uint8_t i = 0; i < SLE_FT_WINDOW_SIZE; i++) {
        sle_ft_tx_window_entry_t *entry = &g_ft_tx_window[i];
        if (entry->valid && sle_seq_acked(control.ack_seq, entry->seq)) {
            if (entry->seq == control.ack_seq && entry->retries == 0 && entry->last_send_us != 0) {
                rtt_sample_us = now_us - entry->last_send_us;
            }
            entry->valid = false;
            acked++;
            if (g_ft_unacked > 0) {
                g_ft_unacked--;
            }
        }
    }
    if (rtt_sample_us != 0) {
        sle_file_update_rtt(rtt_sample_us);
    }
    if (acked != 0) {
        g_ft_last_ack_progress_us = now_us;
    }
    if (control.nack_seq != SLE_FT_NO_NACK) {
        sle_ft_tx_window_entry_t *entry = &g_ft_tx_window[control.nack_seq & SLE_FT_WINDOW_MASK];
        if (entry->valid && entry->seq == control.nack_seq) {
            entry->last_send_us = 0;
        }
        g_ft_cwnd = (g_ft_cwnd > SLE_FT_CWND_MIN) ?
            (uint8_t)(g_ft_cwnd / 2u) : SLE_FT_CWND_MIN;
        if (g_ft_cwnd < SLE_FT_CWND_MIN) {
            g_ft_cwnd = SLE_FT_CWND_MIN;
        }
        g_ft_cwnd_ack_accum = 0;
    } else if (acked != 0 && g_ft_cwnd < SLE_FT_CWND_MAX) {
        uint16_t accumulated = (uint16_t)g_ft_cwnd_ack_accum + acked;
        if (accumulated >= g_ft_cwnd) {
            g_ft_cwnd++;
            accumulated = 0;
        }
        g_ft_cwnd_ack_accum = (uint8_t)accumulated;
    }
    g_ft_ack_received++;
}

static bool sle_file_retransmit_due_packet(uint64_t now_us)
{
    for (uint8_t i = 0; i < SLE_FT_WINDOW_SIZE; i++) {
        sle_ft_tx_window_entry_t *entry = &g_ft_tx_window[i];
        uint64_t retry_timeout = g_ft_rto_us;

        if (entry->retries > 0) {
            uint8_t shifts = (entry->retries > 2u) ? 2u : (uint8_t)entry->retries;
            retry_timeout <<= shifts;
            if (retry_timeout > SLE_FT_RETRY_MAX_US) {
                retry_timeout = SLE_FT_RETRY_MAX_US;
            }
        }
        if (!entry->valid || (entry->last_send_us != 0 &&
            now_us - entry->last_send_us < retry_timeout)) {
            continue;
        }
        if (gle_tx_acb_data_num_get() == 0) {
            return false;
        }
        if (sle_uart_server_send_report_by_handle(entry->data, entry->len) == ERRCODE_SUCC) {
            entry->last_send_us = now_us;
            entry->retries++;
            g_ft_retransmitted++;
            g_ft_cwnd = (g_ft_cwnd > SLE_FT_CWND_MIN) ?
                (uint8_t)(g_ft_cwnd / 2u) : SLE_FT_CWND_MIN;
            if (g_ft_cwnd < SLE_FT_CWND_MIN) {
                g_ft_cwnd = SLE_FT_CWND_MIN;
            }
            g_ft_cwnd_ack_accum = 0;
        } else {
            entry->last_send_us = 0;
        }
        return true;
    }
    return false;
}

static void sle_file_tx_task(void)
{
    uint64_t partial_first_us = 0;
    uint32_t tx_pkt = 0;
    uint32_t tx_fail = 0;
    uint64_t last_print_us = 0;

    file_uart_init();
    uapi_uart_unregister_rx_callback(CONFIG_SLE_UART_BUS);
    if (uapi_uart_register_rx_callback(CONFIG_SLE_UART_BUS,
            UART_RX_CONDITION_FULL_OR_IDLE, 1, file_uart_rx_handler) != ERRCODE_SUCC) {
        osal_printk("[FT TX] UART RX callback register fail.\r\n");
        return;
    }
    osal_printk("[FT TX] Node1 file source ready. baud:%u\r\n", FT_UART_BAUDRATE);
    g_ft_session_id = (uint16_t)(uapi_tcxo_get_us() & 0xFFFFu);
    if (g_ft_session_id == 0) {
        g_ft_session_id = 1;
    }
    sle_file_reset_congestion_state();

    while (1) {
        while (!sle_uart_client_is_connected() || !g_mtu_negotiated) {
            sle_file_report_flow(uapi_tcxo_get_us());
            (void)sle_diag_try_process_host_command();
            (void)sle_diag_try_send_pending();
            sle_uart_server_watchdog_tick();
            osal_msleep(50);
        }
        uint16_t conn_id = get_connect_id();
        osal_msleep(500);
        sle_source_prepare_link(conn_id);
        osal_printk("[FT TX] 5-hop link up.\r\n");

        while (sle_uart_client_is_connected() && g_mtu_negotiated) {
            uint64_t now_us;
            uint32_t head = g_ft_ring_head;
            uint32_t tail = g_ft_ring_tail;
            uint32_t avail = (head - tail) & FT_RING_MASK;

            sle_file_process_control();
            sle_uart_server_watchdog_tick();
            now_us = uapi_tcxo_get_us();
            sle_file_report_flow(now_us);
            (void)sle_diag_try_send_pending();
            if (sle_diag_try_process_host_command()) {
                continue;
            }
            if (g_ft_unacked == 0) {
                g_ft_last_ack_progress_us = now_us;
            } else if (g_ft_last_ack_progress_us == 0) {
                g_ft_last_ack_progress_us = now_us;
            } else if (now_us - g_ft_last_ack_progress_us >= FT_ACK_STALL_RECOVERY_US &&
                (g_ft_last_recovery_us == 0 ||
                now_us - g_ft_last_recovery_us >= FT_LINK_RECOVERY_COOLDOWN_US)) {
                g_ft_last_recovery_us = now_us;
                g_ft_last_ack_progress_us = now_us;
                if (sle_uart_server_force_reconnect()) {
                    osal_printk("[FT Recovery] No end-to-end ACK progress for 5s; rebuilding chain.\r\n");
                }
                osal_msleep(10);
                continue;
            }
            if (g_sle_tx_mode == SLE_TX_MODE_AUTO && g_ft_unacked != 0) {
                if (sle_file_retransmit_due_packet(now_us)) {
                    osal_msleep(1);
                } else {
                    osal_msleep(1);
                }
                continue;
            }
            if (g_sle_tx_mode == SLE_TX_MODE_AUTO) {
                if (!sle_file_select_tx_mode()) {
                    osal_msleep(1);
                    continue;
                }
                head = g_ft_ring_head;
                tail = g_ft_ring_tail;
                avail = (head - tail) & FT_RING_MASK;
                if (g_sle_tx_mode == SLE_TX_MODE_AUTO) {
                    continue;
                }
            }
            if (sle_file_retransmit_due_packet(now_us)) {
                osal_msleep(1);
                continue;
            }
            if (g_ft_unacked >= g_ft_cwnd) {
                osal_msleep(1);
                continue;
            }
            if (avail == 0) {
                partial_first_us = 0;
                osal_msleep(2);
                continue;
            }
            if (avail < SLE_FT_MAX_PAYLOAD) {
                now_us = uapi_tcxo_get_us();
                if (partial_first_us == 0) {
                    partial_first_us = now_us;
                }
                if (now_us - partial_first_us < FT_FLUSH_TIMEOUT_US) {
                    osal_msleep(1);
                    continue;
                }
            }
            partial_first_us = 0;

            if (gle_tx_acb_data_num_get() == 0) {
                osal_msleep(1);
                continue;
            }

            uint16_t take = (avail > SLE_FT_MAX_PAYLOAD) ? SLE_FT_MAX_PAYLOAD : (uint16_t)avail;
            if (g_reliable_remaining != SLE_RELIABLE_LENGTH_UNKNOWN &&
                g_reliable_remaining < take) {
                take = (uint16_t)g_reliable_remaining;
            }
            sle_ft_tx_window_entry_t *entry = &g_ft_tx_window[g_ft_next_seq & SLE_FT_WINDOW_MASK];
            sle_ft_header_t *hdr = (sle_ft_header_t *)entry->data;
            uint8_t *payload = entry->data + SLE_FT_HEADER_LEN;
            sle_ring_copy(0, payload, take);
            hdr->magic = SLE_FT_MAGIC;
            hdr->seq = g_ft_next_seq;
            hdr->session_id = g_ft_session_id;
            hdr->payload_len = take;
            hdr->crc16 = uapi_crc16(0, payload, take);
            entry->valid = true;
            entry->seq = g_ft_next_seq;
            entry->len = (uint16_t)(SLE_FT_HEADER_LEN + take);
            entry->retries = 0;
            entry->last_send_us = now_us;

            if (sle_uart_server_send_report_by_handle(entry->data, entry->len) == ERRCODE_SUCC) {
                g_ft_ring_tail = (tail + take) & FT_RING_MASK;
                if (g_reliable_remaining != SLE_RELIABLE_LENGTH_UNKNOWN) {
                    if (take >= g_reliable_remaining) {
                        g_reliable_remaining = 0;
                        g_sle_tx_mode = SLE_TX_MODE_AUTO;
                    } else {
                        g_reliable_remaining -= take;
                    }
                }
                g_ft_next_seq++;
                g_ft_unacked++;
                tx_pkt++;
            } else {
                entry->valid = false;
                tx_fail++;
                osal_msleep(1);
            }
            osal_msleep(1);

            uint64_t now = uapi_tcxo_get_us();
            if (last_print_us == 0) {
                last_print_us = now;
            } else if (now - last_print_us >= 5000000) {
                osal_printk("[FT TX] tx:%u fail:%u ovf:%u unacked:%u ack:%u retry:%u cwnd:%u rto:%ums\r\n",
                    tx_pkt, tx_fail, g_ft_uart_overflow, g_ft_unacked,
                    g_ft_ack_received, g_ft_retransmitted, g_ft_cwnd, g_ft_rto_us / 1000u);
                last_print_us = now;
            }
        }
        osal_printk("[FT TX] Link lost.\r\n");
    }
}

#else  /* !CONFIG_SAMPLE_FILE_TRANSFER_MODE */

static uint8_t g_test_payload[THROUGHPUT_PKT_LEN];
static uint32_t g_source_credit_stall = 0;
static uint32_t g_source_tx_fail = 0;

static void sle_throughput_tx_task(void)
{
    uint32_t pkt_cnt = 0;
    uint32_t last_pkt_cnt = 0;
    uint32_t last_stall = 0;
    uint32_t last_fail = 0;
    uint64_t last_stat_us = 0;

    memset(g_test_payload, 'A', sizeof(g_test_payload));
    osal_printk("[Speed Test] Node1 source ready. pkt:%u pacing:%ums\r\n",
        THROUGHPUT_PKT_LEN, THROUGHPUT_PACING_MS);

    while (1) {
        while (!sle_uart_client_is_connected()) {
            sle_uart_server_watchdog_tick();
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
        sle_source_prepare_link(conn_id);
        osal_printk("[Speed Test] 5-hop source traffic started.\r\n");
        last_stat_us = uapi_tcxo_get_us();

        while (sle_uart_client_is_connected() && g_mtu_negotiated) {
            if (gle_tx_acb_data_num_get() > 0) {
                pkt_cnt++;
                g_test_payload[0] = (pkt_cnt >> 8) & 0xFF;
                g_test_payload[1] = pkt_cnt & 0xFF;
                if (sle_uart_server_send_report_by_handle(g_test_payload, THROUGHPUT_PKT_LEN) != ERRCODE_SUCC) {
                    g_source_tx_fail++;
                }
                osal_msleep(THROUGHPUT_PACING_MS);
            } else {
                /* Credit exhausted — let the SLE stack process pending
                 * notifications and replenish the credit pool. */
                g_source_credit_stall++;
                osal_msleep(1);
            }

            /* Every 5s emit a one-line diagnostic so we can see why end-to-end
             * throughput dips: high stall/fail = downstream backpressure or
             * dropped link, near-zero stall/fail = source is the bottleneck. */
            uint64_t now = uapi_tcxo_get_us();
            if (now - last_stat_us >= 5000000ULL) {
                osal_printk("[SRC STAT] sent:%u stall:%u fail:%u (in 5s)\r\n",
                    pkt_cnt - last_pkt_cnt,
                    g_source_credit_stall - last_stall,
                    g_source_tx_fail - last_fail);
                last_pkt_cnt = pkt_cnt;
                last_stall = g_source_credit_stall;
                last_fail = g_source_tx_fail;
                last_stat_us = now;
            }
        }

        osal_printk("[Speed Test] source link lost. stall:%u tx_fail:%u\r\n",
            g_source_credit_stall, g_source_tx_fail);
    }
}
#endif  /* CONFIG_SAMPLE_FILE_TRANSFER_MODE */

static void *sle_uart_server_task(const char *arg)
{
    unused(arg);
#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
    sle_uart_server_init(NULL, sle_file_control_write_cb);
    sle_file_tx_task();
#else
    sle_uart_server_init(NULL, NULL);
    sle_throughput_tx_task();
#endif
    return NULL;
}

#elif defined(CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT)

#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)

typedef struct {
    bool valid;
    uint16_t seq;
    uint16_t len;
    uint8_t payload[SLE_FT_MAX_PAYLOAD];
} sle_ft_rx_window_entry_t;

static sle_ft_rx_window_entry_t g_ft_rx_window[SLE_FT_WINDOW_SIZE];
static bool g_ft_rx_session_valid = false;
static uint16_t g_ft_rx_session_id = 0;
static uint16_t g_ft_rx_expected_seq = 0;
static volatile bool g_ft_ack_dirty = false;
static volatile uint16_t g_ft_nack_seq = SLE_FT_NO_NACK;
static volatile uint8_t g_ft_ack_packets_pending = 0;
static volatile bool g_ft_ack_urgent = false;
static volatile uint64_t g_ft_ack_first_pending_us = 0;
static uint32_t g_ft_rx_bytes = 0;
static uint32_t g_ft_crc_fail = 0;
static uint32_t g_ft_corrupt = 0;
static uint32_t g_ft_duplicates = 0;
static uint32_t g_ft_reordered = 0;
static uint8_t g_ft_uart_tx_ring[FT_RING_BUFFER_SIZE];
static uint32_t g_ft_uart_tx_head = 0;
static uint32_t g_ft_uart_tx_tail = 0;
static uint64_t g_ft_status_last_send_us = 0;
static uint32_t g_ft_status_last_rx_errors = 0;

static uint32_t sle_file_uart_tx_used(void)
{
    return (g_ft_uart_tx_head - g_ft_uart_tx_tail) & FT_RING_MASK;
}

static uint32_t sle_file_uart_tx_free(void)
{
    return FT_RING_MASK - sle_file_uart_tx_used();
}

static void sle_file_rx_clear_window_locked(void)
{
    for (uint8_t i = 0; i < SLE_FT_WINDOW_SIZE; i++) {
        g_ft_rx_window[i].valid = false;
    }
    g_ft_uart_tx_head = 0;
    g_ft_uart_tx_tail = 0;
}

static void sle_file_rx_request_control(uint16_t nack_seq)
{
    uint32_t irq_status = osal_irq_lock();

    g_ft_ack_dirty = true;
    g_ft_ack_urgent = true;
    if (g_ft_ack_first_pending_us == 0) {
        g_ft_ack_first_pending_us = uapi_tcxo_get_us();
    }
    if (nack_seq != SLE_FT_NO_NACK) {
        g_ft_nack_seq = nack_seq;
    }
    osal_irq_restore(irq_status);
}

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    sle_diag_probe_t probe;
    sle_diag_report_t report;
    uint32_t magic;

    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data == NULL || data->data_len < SLE_FT_HEADER_LEN) {
        g_ft_corrupt++;
        return;
    }

    if (memcpy_s(&magic, sizeof(magic), data->data, sizeof(magic)) != EOK) {
        g_ft_corrupt++;
        return;
    }
    if (magic == SLE_DIAG_PROBE_MAGIC) {
        if (data->data_len != SLE_DIAG_PROBE_LEN ||
            memcpy_s(&probe, sizeof(probe), data->data, sizeof(probe)) != EOK ||
            uapi_crc16(0, (uint8_t *)&probe,
                SLE_DIAG_PROBE_LEN - sizeof(probe.crc16)) != probe.crc16 ||
            probe.reserved != SLE_DIAG_PATH_EXPECTED) {
            g_ft_corrupt++;
            return;
        }
        report.magic = SLE_DIAG_REPORT_MAGIC;
        report.probe_id = probe.probe_id;
        report.node_id = SLE_DIAG_NODE_NODE6;
        report.flags = SLE_DIAG_FLAG_UPSTREAM_READY |
            SLE_DIAG_FLAG_REVERSE_READY | SLE_DIAG_FLAG_ENDPOINT;
        report.crc16 = uapi_crc16(0, (uint8_t *)&report,
            SLE_DIAG_REPORT_LEN - sizeof(report.crc16));
        (void)sle_uart_client_write_upstream((uint8_t *)&report, sizeof(report));
        return;
    }

    sle_ft_header_t hdr;
    if (memcpy_s(&hdr, sizeof(hdr), data->data, SLE_FT_HEADER_LEN) != EOK) {
        g_ft_corrupt++;
        return;
    }
    if (hdr.magic != SLE_FT_MAGIC) {
        g_ft_corrupt++;
        return;
    }
    if (hdr.payload_len > SLE_FT_MAX_PAYLOAD ||
        (uint32_t)hdr.payload_len + SLE_FT_HEADER_LEN > data->data_len) {
        g_ft_corrupt++;
        return;
    }

    const uint8_t *payload = data->data + SLE_FT_HEADER_LEN;
    if (uapi_crc16(0, payload, hdr.payload_len) != hdr.crc16) {
        g_ft_crc_fail++;
        sle_file_rx_request_control(g_ft_rx_expected_seq);
        return;
    }

    uint32_t irq_status = osal_irq_lock();
    if (!g_ft_rx_session_valid || hdr.session_id != g_ft_rx_session_id) {
        sle_file_rx_clear_window_locked();
        g_ft_rx_session_valid = true;
        g_ft_rx_session_id = hdr.session_id;
        g_ft_rx_expected_seq = 0;
        g_ft_ack_dirty = false;
        g_ft_nack_seq = SLE_FT_NO_NACK;
        g_ft_ack_packets_pending = 0;
        g_ft_ack_urgent = false;
        g_ft_ack_first_pending_us = 0;
    }

    int16_t delta = (int16_t)(hdr.seq - g_ft_rx_expected_seq);
    if (delta < 0) {
        g_ft_duplicates++;
        g_ft_ack_dirty = true;
        g_ft_ack_urgent = true;
        osal_irq_restore(irq_status);
        return;
    }
    if (delta >= SLE_FT_WINDOW_SIZE) {
        g_ft_nack_seq = g_ft_rx_expected_seq;
        g_ft_ack_dirty = true;
        g_ft_ack_urgent = true;
        osal_irq_restore(irq_status);
        return;
    }

    sle_ft_rx_window_entry_t *entry = &g_ft_rx_window[hdr.seq & SLE_FT_WINDOW_MASK];
    if (entry->valid) {
        if (entry->seq == hdr.seq) {
            g_ft_duplicates++;
        } else {
            g_ft_corrupt++;
        }
        g_ft_ack_dirty = true;
        g_ft_ack_urgent = true;
        osal_irq_restore(irq_status);
        return;
    }
    entry->seq = hdr.seq;
    entry->len = hdr.payload_len;
    if (memcpy_s(entry->payload, sizeof(entry->payload), payload, hdr.payload_len) != EOK) {
        g_ft_corrupt++;
        g_ft_nack_seq = g_ft_rx_expected_seq;
        g_ft_ack_dirty = true;
        g_ft_ack_urgent = true;
        osal_irq_restore(irq_status);
        return;
    }
    entry->valid = true;
    if (delta > 0) {
        g_ft_reordered++;
        g_ft_nack_seq = g_ft_rx_expected_seq;
        g_ft_ack_dirty = true;
        g_ft_ack_urgent = true;
    }
    osal_irq_restore(irq_status);
}

static bool sle_file_send_control(void)
{
    sle_ft_control_t control;
    uint16_t nack_seq;
    uint8_t packets_pending;
    bool urgent;
    uint64_t first_pending_us;
    uint64_t now_us;
    uint32_t irq_status;

    if (!g_ft_rx_session_valid || !sle_uart_client_write_ready()) {
        return false;
    }
    irq_status = osal_irq_lock();
    if (!g_ft_ack_dirty) {
        osal_irq_restore(irq_status);
        return false;
    }
    packets_pending = g_ft_ack_packets_pending;
    urgent = g_ft_ack_urgent;
    first_pending_us = g_ft_ack_first_pending_us;
    now_us = uapi_tcxo_get_us();
    if (!urgent && packets_pending < SLE_FT_ACK_BATCH_PACKETS &&
        first_pending_us != 0 && now_us - first_pending_us < SLE_FT_ACK_MAX_DELAY_US) {
        osal_irq_restore(irq_status);
        return false;
    }
    control.magic = SLE_FT_CTRL_MAGIC;
    control.session_id = g_ft_rx_session_id;
    control.ack_seq = (uint16_t)(g_ft_rx_expected_seq - 1);
    control.nack_seq = g_ft_nack_seq;
    control.crc16 = 0;
    nack_seq = g_ft_nack_seq;
    osal_irq_restore(irq_status);

    control.crc16 = uapi_crc16(0, (uint8_t *)&control,
        SLE_FT_CTRL_LEN - sizeof(control.crc16));
    if (sle_uart_client_write_upstream((uint8_t *)&control, sizeof(control)) != ERRCODE_SUCC) {
        return false;
    }

    irq_status = osal_irq_lock();
    if ((uint16_t)(g_ft_rx_expected_seq - 1) == control.ack_seq &&
        g_ft_nack_seq == nack_seq) {
        g_ft_ack_dirty = false;
        g_ft_nack_seq = SLE_FT_NO_NACK;
        g_ft_ack_packets_pending = 0;
        g_ft_ack_urgent = false;
        g_ft_ack_first_pending_us = 0;
    }
    osal_irq_restore(irq_status);
    return true;
}

static bool sle_file_rx_stage_once(void)
{
    const uint8_t *payload;
    uint16_t payload_len;
    uint16_t seq;
    uint32_t head;
    uint32_t first;
    uint32_t irq_status = osal_irq_lock();
    sle_ft_rx_window_entry_t *entry =
        &g_ft_rx_window[g_ft_rx_expected_seq & SLE_FT_WINDOW_MASK];

    if (!entry->valid || entry->seq != g_ft_rx_expected_seq) {
        osal_irq_restore(irq_status);
        return false;
    }
    payload_len = entry->len;
    seq = entry->seq;
    payload = entry->payload;
    osal_irq_restore(irq_status);

    if (sle_file_uart_tx_free() < payload_len) {
        return false;
    }
    head = g_ft_uart_tx_head;
    first = payload_len;
    if (first > FT_RING_BUFFER_SIZE - head) {
        first = FT_RING_BUFFER_SIZE - head;
    }
    if ((first > 0 && memcpy_s(&g_ft_uart_tx_ring[head], FT_RING_BUFFER_SIZE - head,
        payload, first) != EOK) ||
        (payload_len > first && memcpy_s(g_ft_uart_tx_ring, FT_RING_BUFFER_SIZE,
        payload + first, payload_len - first) != EOK)) {
        g_ft_corrupt++;
        return false;
    }

    irq_status = osal_irq_lock();
    entry = &g_ft_rx_window[seq & SLE_FT_WINDOW_MASK];
    if (!entry->valid || entry->seq != seq || seq != g_ft_rx_expected_seq) {
        osal_irq_restore(irq_status);
        return false;
    }
    g_ft_uart_tx_head = (head + payload_len) & FT_RING_MASK;
    entry->valid = false;
    g_ft_rx_expected_seq++;
    g_ft_rx_bytes += payload_len;
    g_ft_ack_dirty = true;
    if (g_ft_ack_packets_pending < UINT8_MAX) {
        g_ft_ack_packets_pending++;
    }
    if (g_ft_ack_first_pending_us == 0) {
        g_ft_ack_first_pending_us = uapi_tcxo_get_us();
    }
    if (g_ft_nack_seq == seq) {
        g_ft_nack_seq = SLE_FT_NO_NACK;
    }
    osal_irq_restore(irq_status);
    return true;
}

static bool sle_file_uart_drain_once(void)
{
    uint32_t used = sle_file_uart_tx_used();
    uint32_t tail = g_ft_uart_tx_tail;
    uint32_t contiguous;
    int32_t written;

    if (used == 0) {
        return false;
    }
    contiguous = used;
    if (contiguous > FT_RING_BUFFER_SIZE - tail) {
        contiguous = FT_RING_BUFFER_SIZE - tail;
    }
    if (contiguous > FT_UART_RX_BUFFER_SIZE) {
        contiguous = FT_UART_RX_BUFFER_SIZE;
    }
    written = uapi_uart_write(CONFIG_SLE_UART_BUS, &g_ft_uart_tx_ring[tail], contiguous, 0);
    if (written <= 0) {
        return false;
    }
    g_ft_uart_tx_tail = (tail + (uint32_t)written) & FT_RING_MASK;
    return true;
}

static bool sle_file_send_status(uint64_t now_us)
{
    sle_ft_status_t status = {0};
    uint32_t rx_errors;
    uint32_t error_delta;
    uint32_t tx_used;
    uint32_t irq_status;

    if (!g_ft_rx_session_valid || !sle_uart_client_write_ready() ||
        (g_ft_status_last_send_us != 0 &&
        now_us - g_ft_status_last_send_us < SLE_FT_STATUS_INTERVAL_US)) {
        return false;
    }

    irq_status = osal_irq_lock();
    status.session_id = g_ft_rx_session_id;
    status.ack_seq = (uint16_t)(g_ft_rx_expected_seq - 1u);
    osal_irq_restore(irq_status);
    tx_used = sle_file_uart_tx_used();
    status.magic = SLE_FT_STATUS_MAGIC;
    status.link_mask = SLE_FT_LINK_NODE6;
    status.bottleneck_node = SLE_DIAG_NODE_NODE6;
    status.max_queue_percent = (uint8_t)((tx_used * 100u) / FT_RING_MASK);
    if (status.max_queue_percent >= 50u) {
        status.flags |= SLE_FT_STATUS_FLAG_OUTPUT_PRESSURE;
    }
    rx_errors = g_ft_crc_fail + g_ft_corrupt;
    error_delta = rx_errors - g_ft_status_last_rx_errors;
    if (error_delta != 0) {
        status.flags |= SLE_FT_STATUS_FLAG_RX_ERROR;
        status.drop_delta = (error_delta > UINT16_MAX) ? UINT16_MAX : (uint16_t)error_delta;
    }
    status.crc16 = uapi_crc16(0, (uint8_t *)&status,
        SLE_FT_STATUS_LEN - sizeof(status.crc16));
    if (sle_uart_client_write_upstream((uint8_t *)&status, sizeof(status)) != ERRCODE_SUCC) {
        return false;
    }
    g_ft_status_last_rx_errors = rx_errors;
    g_ft_status_last_send_us = now_us;
    return true;
}

#else  /* !CONFIG_SAMPLE_FILE_TRANSFER_MODE */

static uint32_t g_rx_bytes = 0;
static uint64_t g_last_time_us = 0;

void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id,
    ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    unused(conn_id);
    unused(status);

    if (data == NULL || data->data_len == 0) {
        return;
    }

    g_rx_bytes += data->data_len;
    uint64_t current_time_us = uapi_tcxo_get_us();

    if (g_last_time_us == 0) {
        g_last_time_us = current_time_us;
    } else if (current_time_us - g_last_time_us >= 1000000) {
        uint32_t bytes_per_sec = g_rx_bytes;
        uint32_t kbps = (bytes_per_sec * 8) / 1000;

        osal_printk("[5-HOP E2E LIMIT] Speed: %u Bytes/s (%u kbps)\r\n", bytes_per_sec, kbps);

        g_rx_bytes = 0;
        g_last_time_us = current_time_us;
    }
}
#endif  /* CONFIG_SAMPLE_FILE_TRANSFER_MODE */

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
    uint64_t last_watchdog_us = 0;

    unused(arg);
#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
    file_uart_init();
#endif
    sle_uart_client_init(sle_uart_notification_cb, sle_uart_indication_cb);
    rx_log("[Speed Test] Node6 sink ready. Awaiting 5-hop data.\r\n");
    while (1) {
#if defined(CONFIG_SAMPLE_FILE_TRANSFER_MODE)
        uint64_t now_us = uapi_tcxo_get_us();
        bool progressed = false;

        for (uint8_t index = 0; index < 8u; index++) {
            if (!sle_file_rx_stage_once()) {
                break;
            }
            progressed = true;
        }
        (void)sle_file_send_control();
        for (uint8_t index = 0; index < 8u; index++) {
            if (!sle_file_uart_drain_once()) {
                break;
            }
            progressed = true;
        }
        (void)sle_file_send_status(now_us);
        if (last_watchdog_us == 0 ||
            now_us - last_watchdog_us >= NODE6_WATCHDOG_INTERVAL_MS * 1000ULL) {
            sle_uart_client_watchdog_tick();
            last_watchdog_us = now_us;
        }
        if (!progressed) {
            osal_msleep(1);
        }
#else
        sle_uart_client_watchdog_tick();
        osal_msleep(NODE6_WATCHDOG_INTERVAL_MS);
#endif
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
