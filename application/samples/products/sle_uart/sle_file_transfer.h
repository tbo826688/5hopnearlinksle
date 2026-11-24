/*
 * 5-hop SLE end-to-end file transfer framing.
 * Data header is 12 bytes; payload up to 900 bytes (<= 912 total).
 * A 12-byte cumulative ACK/selective NACK travels in the reverse direction.
 */
#ifndef SLE_FILE_TRANSFER_H
#define SLE_FILE_TRANSFER_H

#include <stdint.h>

#define SLE_FT_MAGIC          0x55454C53u  /* 'SLEU' little-endian */
#define SLE_FT_CTRL_MAGIC     0x43454C53u  /* 'SLEC' little-endian */
#define SLE_HOST_DIAG_MAGIC   0x47443556u  /* 'V5DG' little-endian */
#define SLE_DIAG_PROBE_MAGIC  0x50443556u  /* 'V5DP' little-endian */
#define SLE_DIAG_REPORT_MAGIC 0x52443556u  /* 'V5DR' little-endian */
#define SLE_FT_STATUS_MAGIC   0x53544656u  /* 'VFTS' little-endian */
#define SLE_FT_HEADER_LEN     12
#define SLE_FT_CTRL_LEN       12
#define SLE_FT_STATUS_LEN     20
#define SLE_HOST_DIAG_LEN     8
#define SLE_DIAG_PROBE_LEN    12
#define SLE_DIAG_REPORT_LEN   12
#define SLE_FT_MAX_PAYLOAD    900
#define SLE_FT_MAX_PACKET     (SLE_FT_HEADER_LEN + SLE_FT_MAX_PAYLOAD)
#define SLE_FT_WINDOW_SIZE    64
#define SLE_FT_WINDOW_MASK    (SLE_FT_WINDOW_SIZE - 1)
#define SLE_FT_NO_NACK        0xFFFFu

#define SLE_FT_ACK_BATCH_PACKETS 2
#define SLE_FT_ACK_MAX_DELAY_US  40000ULL
#define SLE_FT_RETRY_INITIAL_US  240000ULL
#define SLE_FT_RETRY_MIN_US       80000ULL
#define SLE_FT_RETRY_MAX_US      600000ULL
#define SLE_FT_CWND_MIN               4u
#define SLE_FT_CWND_INITIAL           8u
#define SLE_FT_CWND_MAX              16u
#define SLE_FT_STATUS_INTERVAL_US 200000ULL

#define SLE_DIAG_NODE_NODE1 0
#define SLE_DIAG_NODE_R1    1
#define SLE_DIAG_NODE_R2    2
#define SLE_DIAG_NODE_R3    3
#define SLE_DIAG_NODE_R4    4
#define SLE_DIAG_NODE_NODE6 5

#define SLE_DIAG_FLAG_UPSTREAM_READY   0x01u
#define SLE_DIAG_FLAG_DOWNSTREAM_LINK  0x02u
#define SLE_DIAG_FLAG_DOWNSTREAM_MTU   0x04u
#define SLE_DIAG_FLAG_REVERSE_READY    0x08u
#define SLE_DIAG_FLAG_ENDPOINT         0x10u

#define SLE_FT_LINK_NODE1              0x01u
#define SLE_FT_LINK_R1                 0x02u
#define SLE_FT_LINK_R2                 0x04u
#define SLE_FT_LINK_R3                 0x08u
#define SLE_FT_LINK_R4                 0x10u
#define SLE_FT_LINK_NODE6              0x20u
#define SLE_FT_LINK_ALL                0x3Fu

#define SLE_FT_STATUS_FLAG_CREDIT_PRESSURE 0x01u
#define SLE_FT_STATUS_FLAG_TX_FAILURE      0x02u
#define SLE_FT_STATUS_FLAG_DROP            0x04u
#define SLE_FT_STATUS_FLAG_LINK_NOT_READY  0x08u
#define SLE_FT_STATUS_FLAG_RX_ERROR        0x10u
#define SLE_FT_STATUS_FLAG_OUTPUT_PRESSURE 0x20u

#define SLE_DIAG_PATH_R1               0x0001u
#define SLE_DIAG_PATH_R2               0x0002u
#define SLE_DIAG_PATH_R3               0x0004u
#define SLE_DIAG_PATH_R4               0x0008u
#define SLE_DIAG_PATH_ALL_ROUTERS      0x000Fu

#if defined(CONFIG_SLE_TWO_HOP_VALIDATION_MODE)
#define SLE_DIAG_PATH_EXPECTED         SLE_DIAG_PATH_R1
#else
#define SLE_DIAG_PATH_EXPECTED         SLE_DIAG_PATH_ALL_ROUTERS
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t seq;
    uint16_t session_id;
    uint16_t payload_len;
    uint16_t crc16;
} sle_ft_header_t;

typedef struct {
    uint32_t magic;
    uint16_t session_id;
    uint16_t ack_seq;
    uint16_t nack_seq;
    uint16_t crc16;
} sle_ft_control_t;

typedef struct {
    uint32_t magic;
    uint16_t session_id;
    uint16_t ack_seq;
    uint8_t link_mask;
    uint8_t bottleneck_node;
    uint8_t max_queue_percent;
    uint8_t flags;
    uint16_t credit_wait_delta;
    uint16_t tx_fail_delta;
    uint16_t drop_delta;
    uint16_t crc16;
} sle_ft_status_t;

typedef struct {
    uint32_t magic;
    uint32_t probe_id;
} sle_host_diag_command_t;

typedef struct {
    uint32_t magic;
    uint32_t probe_id;
    uint16_t reserved;
    uint16_t crc16;
} sle_diag_probe_t;

typedef struct {
    uint32_t magic;
    uint32_t probe_id;
    uint8_t node_id;
    uint8_t flags;
    uint16_t crc16;
} sle_diag_report_t;

#pragma pack(pop)

#endif
