/*
 * Single source of truth for router identity.
 *
 * Select CONFIG_ROUTE_NODE_ID in menuconfig before compiling each router:
 *   R1: 1, R2: 2, R3: 3, R4: 4
 *
 * Do not use SLE_OUTDOOR_LONG_RANGE as the router id. It is only a
 * 0/1 outdoor profile switch.
 */
#ifndef SLE_ROUTER_CONFIG_H
#define SLE_ROUTER_CONFIG_H

#ifndef ROUTER_NODE_ID
#if defined(CONFIG_ROUTE_NODE_ID)
#define ROUTER_NODE_ID CONFIG_ROUTE_NODE_ID
#else
#define ROUTER_NODE_ID 2
#endif
#endif

/*
 * Connections start with the stack-managed PHY for reliable discovery.
 * After MTU exchange, each downstream server upgrades to the shared data
 * profile in sle_link_profile.h; outdoor mode also extends supervision timeout.
 */
#ifndef SLE_OUTDOOR_LONG_RANGE
#if defined(CONFIG_ROUTE_OUTDOOR_LONG_RANGE)
#define SLE_OUTDOOR_LONG_RANGE 1
#elif defined(CONFIG_SAMPLE_SUPPORT_ROUTE)
#define SLE_OUTDOOR_LONG_RANGE 0
#else
#define SLE_OUTDOOR_LONG_RANGE 1
#endif
#endif

#if (SLE_OUTDOOR_LONG_RANGE != 0) && (SLE_OUTDOOR_LONG_RANGE != 1)
#error "SLE_OUTDOOR_LONG_RANGE must be 0 or 1. Use ROUTER_NODE_ID=1/2/3/4 to select R1/R2/R3/R4."
#endif

#define SLE_OUTDOOR_SUPERVISION_TIMEOUT 0x7D0  /* 2000 * 10ms = 20s, tolerates short outdoor fades */
#define SLE_INDOOR_SUPERVISION_TIMEOUT  0x1F4  /* 500 * 10ms = 5s, original aggressive default */

#if SLE_OUTDOOR_LONG_RANGE
#define SLE_ROUTER_SUPERVISION_TIMEOUT SLE_OUTDOOR_SUPERVISION_TIMEOUT
#else
#define SLE_ROUTER_SUPERVISION_TIMEOUT SLE_INDOOR_SUPERVISION_TIMEOUT
#endif

#if ROUTER_NODE_ID == 1
#define TARGET_UPSTREAM_NAME "SLE_SVR"
#define MY_ROUTER_NAME       "SLE_R1"
#pragma message ("Compiling router R1: scans SLE_SVR, advertises SLE_R1")
#elif ROUTER_NODE_ID == 2
#define TARGET_UPSTREAM_NAME "SLE_R1"
#define MY_ROUTER_NAME       "SLE_R2"
#pragma message ("Compiling router R2: scans SLE_R1, advertises SLE_R2")
#elif ROUTER_NODE_ID == 3
#define TARGET_UPSTREAM_NAME "SLE_R2"
#define MY_ROUTER_NAME       "SLE_R3"
#pragma message ("Compiling router R3: scans SLE_R2, advertises SLE_R3")
#elif ROUTER_NODE_ID == 4
#define TARGET_UPSTREAM_NAME "SLE_R3"
#define MY_ROUTER_NAME       "SLE_END"
#pragma message ("Compiling router R4: scans SLE_R3, advertises SLE_END")
#else
#error "ROUTER_NODE_ID must be 1, 2, 3, or 4"
#endif

#endif
