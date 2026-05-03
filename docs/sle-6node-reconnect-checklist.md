# SLE 6-Node Burn And Reconnect Checklist

## 1. Topology

Full path:

`Node1 -> R1 -> R2 -> R3 -> R4 -> Node6`

Advertise / scan chain:

`SLE_SVR -> SLE_R1 -> SLE_R2 -> SLE_R3 -> SLE_END`

## 2. Node Role Table

| Node | Role | Sample | Mode | Upstream target | Downstream advertise |
| --- | --- | --- | --- | --- | --- |
| Node1 | Source sender | `application/samples/products/sle_uart` | `SERVER` | None | `SLE_SVR` |
| Node2 | Router R1 | `application/samples/products/sle_uart_sever` | `ALL` | `SLE_SVR` | `SLE_R1` |
| Node3 | Router R2 | `application/samples/products/sle_uart_sever` | `ALL` | `SLE_R1` | `SLE_R2` |
| Node4 | Router R3 | `application/samples/products/sle_uart_sever` | `ALL` | `SLE_R2` | `SLE_R3` |
| Node5 | Router R4 | `application/samples/products/sle_uart_sever` | `ALL` | `SLE_R3` | `SLE_END` |
| Node6 | Sink receiver | `application/samples/products/sle_uart` | `CLIENT` | `SLE_END` | None |

## 3. Fixed Names In Code

Source advertise name is fixed in:

`application/samples/products/sle_uart/sle_uart_server/sle_uart_server_adv.c`

Current value:

`"SLE_SVR"`

Sink scan target is fixed in:

`application/samples/products/sle_uart/sle_uart_client/sle_uart_client.c`

Current value:

`"SLE_END"`

## 4. Router Boards: Only Change These Two Macros

Upstream target:

`application/samples/products/sle_uart_sever/sle_uart_client/sle_uart_client.c`

Macro:

`TARGET_UPSTREAM_NAME`

Downstream advertise name:

`application/samples/products/sle_uart_sever/sle_uart_server/sle_uart_server_adv.c`

Macro:

`MY_ROUTER_NAME`

Recommended values:

| Router board | `TARGET_UPSTREAM_NAME` | `MY_ROUTER_NAME` |
| --- | --- | --- |
| R1 | `"SLE_SVR"` | `"SLE_R1"` |
| R2 | `"SLE_R1"` | `"SLE_R2"` |
| R3 | `"SLE_R2"` | `"SLE_R3"` |
| R4 | `"SLE_R3"` | `"SLE_END"` |

## 5. Build Config Cheat Sheet

Current checked-in active config:

`build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config`

Current meaning:

`CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_ALL=y`

This is the router build.

Existing old config snapshot:

`build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config.old`

Current meaning:

`CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT=y`

This is the sink build.

### Source build

For Node1, enable:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART=y`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER=y`

Disable:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_ALL`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER`

### Sink build

For Node6, use the same family but client mode:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART=y`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT=y`

Disable:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_ALL`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER`

### Router build

For R1 to R4:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER=y`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_ALL=y`

Disable:

- `CONFIG_SAMPLE_SUPPORT_SLE_UART`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_SERVER`
- `CONFIG_SAMPLE_SUPPORT_SLE_UART_SEVER_CLIENT`

## 6. Burn Order

Recommended order:

1. Burn Node6 sink first.
2. Burn Node5 R4.
3. Burn Node4 R3.
4. Burn Node3 R2.
5. Burn Node2 R1.
6. Burn Node1 source last.

Reason:

This order makes downstream already discoverable when upstream boards start seeking or forwarding.

## 7. What Changed For Auto-Reconnect

This repo now includes reconnect self-healing in three places:

1. Source server side:
   disconnect after sending no longer requires reset; it waits for reconnect and renegotiates `MTU/PHY`.
2. Router client side:
   connect timeout and stale scan state will auto-refresh.
3. Router and source advertise side:
   if advertising side is disconnected, it periodically reopens advertising.
4. Sink client side:
   if target disappears or connect stalls, it removes stale pairing info and rescans.

## 8. Fast Verification Steps

### Baseline

1. Power all six boards.
2. Confirm Node1 log shows waiting for R1, then start sending.
3. Confirm Node6 prints throughput once per second.

Pass condition:

`Node6` keeps printing speed and does not require manual reset.

### Reconnect Test A: Mid-router power loss

Use `R3` first because it is the easiest to observe.

1. Let all six boards run normally for at least 10 seconds.
2. Power off `R3`.
3. Wait 10 to 20 seconds.
4. Power `R3` back on.
5. Do not reset any other board.

Pass condition:

- `R2` and `R4` do not need manual reset.
- chain re-forms by itself.
- `Node6` resumes throughput printing.

### Reconnect Test B: Router missing at startup

1. Keep `R2` powered off.
2. Power on all other boards.
3. Wait until logs show repeated scanning / reconnect waiting.
4. Power on `R2`.

Pass condition:

The chain eventually recovers without resetting Node1, R1, R3, R4, or Node6.

### Reconnect Test C: Sink power loss

1. Let full chain run.
2. Power off `Node6`.
3. Wait 10 seconds.
4. Power `Node6` back on.

Pass condition:

`R4` keeps advertising and `Node6` reconnects automatically.

### Reconnect Test D: Source side interruption

1. Let full chain run.
2. Power off `Node2` or move it out of range.
3. Wait 10 seconds.
4. Restore it.

Pass condition:

`Node1` resumes after reconnect without reset.

## 9. Expected Log Keywords

Useful signs of recovery:

- Source:
  `Waiting for R1`
  `Waiting for auto-reconnect`
- Router client:
  `Connect timeout, rescanning`
  `Refreshing scan`
- Server side:
  `Client disconnected. Restarting advertise`
- Sink:
  throughput line prints again after chain recovers

## 10. Current Build Output

Current router-all package:

`output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`

## 11. Practical Advice

1. Always burn one role at a time and label the board physically.
2. After changing router names, rebuild before burning the next router.
3. Keep only one board for each advertise name powered nearby during single-board debugging.
4. If a reconnect case fails, save all six serial logs with timestamps.

