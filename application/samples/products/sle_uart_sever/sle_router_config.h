/*
 * Single source of truth for router identity.
 *
 * Change only ROUTER_NODE_ID before compiling each router board:
 *   R1: 1, R2: 2, R3: 3, R4: 4
 *
 * Do not use SLE_OUTDOOR_LONG_RANGE as the router id. It is only a
 * 0/1 outdoor profile switch.
 */
#ifndef SLE_ROUTER_CONFIG_H
#define SLE_ROUTER_CONFIG_H

#ifndef ROUTER_NODE_ID
#define ROUTER_NODE_ID 4
#endif

/*
 * Outdoor mode favors link budget over peak throughput:
 * - keep each hop on the long-range PHY profile by default
 * - avoid automatic promotion to the high-speed MCS10 profile
 * - use a longer supervision timeout for short fades outdoors
 */
#ifndef SLE_OUTDOOR_LONG_RANGE
#define SLE_OUTDOOR_LONG_RANGE 1
#endif

#if (SLE_OUTDOOR_LONG_RANGE != 0) && (SLE_OUTDOOR_LONG_RANGE != 1)
#error "SLE_OUTDOOR_LONG_RANGE must be 0 or 1. Use ROUTER_NODE_ID=1/2/3/4 to select R1/R2/R3/R4."
#endif

#define SLE_OUTDOOR_SUPERVISION_TIMEOUT 0x7D0
#define SLE_INDOOR_SUPERVISION_TIMEOUT  0x1F4

#if ROUTER_NODE_ID == 1
#define TARGET_UPSTREAM_NAME "SLE_SVR"
#define MY_ROUTER_NAME       "SLE_R1"
#elif ROUTER_NODE_ID == 2
#define TARGET_UPSTREAM_NAME "SLE_R1"
#define MY_ROUTER_NAME       "SLE_R2"
#elif ROUTER_NODE_ID == 3
#define TARGET_UPSTREAM_NAME "SLE_R2"
#define MY_ROUTER_NAME       "SLE_R3"
#elif ROUTER_NODE_ID == 4
#define TARGET_UPSTREAM_NAME "SLE_R3"
#define MY_ROUTER_NAME       "SLE_END"
#else
#error "ROUTER_NODE_ID must be 1, 2, 3, or 4"
#endif

#endif
