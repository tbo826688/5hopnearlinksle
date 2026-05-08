/*
 * Single source of truth for router identity.
 *
 * Change only ROUTER_NODE_ID before compiling each router board:
 *   R1: 1, R2: 2, R3: 3, R4: 4
 */
#ifndef SLE_ROUTER_CONFIG_H
#define SLE_ROUTER_CONFIG_H

#ifndef ROUTER_NODE_ID
#define ROUTER_NODE_ID 1
#endif

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
