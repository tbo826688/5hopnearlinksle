 5跳 SLE 链路自动重连技术升级说明

## 1. 项目背景

本项目基于 WS63 / BearPi SLE 示例工程，实现一条固定 5 跳线性链路：

```text
Node1 源头 -> R1 -> R2 -> R3 -> R4 -> Node6 终点
```

其中 Node1 负责持续发送测速数据，Node6 负责接收并打印端到端速率，R1 到 R4 作为中间路由节点逐跳转发数据。

上一套代码在链路初次连接时可以工作，但一旦某个中间节点因为断电、距离过远或临时掉线导致连接中断，经常无法自动恢复，需要人工复位相关节点后才能重新建立链路。当前版本针对这一问题进行了应用层连接状态机升级，实现了单节点掉线后的自动重连。

## 2. 上一套代码存在的问题

上一套代码主要依赖 SLE 协议栈的连接事件和初始化流程，缺少完整的掉线恢复机制。典型问题如下：

| 问题 | 表现 | 影响 |
|---|---|---|
| 上游和下游连接状态混在一起 | 路由节点同时作为 Client 和 Server，但连接事件没有可靠区分方向 | 断线后可能清错状态，导致重连逻辑失效 |
| 断开后没有持续扫描 | 上游断开后，如果扫描流程停止或连接失败，没有稳定的重新扫描机制 | 上一跳恢复供电后，路由节点不一定能重新找到它 |
| 断开后没有持续广播 | 下游断开后，如果广播没有重新开启，下一跳无法再次连接 | 后级节点恢复后找不到当前路由节点 |
| 旧配对信息没有清理 | 设备断电重连时保留旧配对和旧连接信息 | 可能出现“连上立即断开”或必须复位才能恢复 |
| 缺少超时兜底 | 扫描、连接过程中如果卡在中间状态，没有 watchdog 纠正 | 系统可能长期停在无效状态 |

因此，上一套代码的核心问题不是 SLE 不能连接，而是应用层没有维护一个确定性的连接生命周期。

## 3. 当前版本升级目标

当前版本的升级目标是：

1. 每个中间路由节点能够同时维护上游连接和下游连接。
2. 任意单个节点断电、离距或掉线后，恢复供电和距离后可以自动重连。
3. 自动重连过程中不需要复位其他节点。
4. 重连逻辑不明显影响热路径数据转发速率。
5. 保持固定 5 跳拓扑，不引入复杂 Mesh 路由开销。

## 4. 新版总体技术方案

当前版本采用的是应用层固定拓扑自动重连方案，不是自动 Mesh 协议。

每个路由节点 R1 到 R4 同时扮演两个角色：

| 方向 | 节点角色 | 行为 |
|---|---|---|
| 上游 | SLE Client / Central | 扫描并主动连接上一跳 |
| 下游 | SLE Server / Peripheral | 广播自身名称，等待下一跳连接 |

也就是说，每个路由节点都维护两条独立状态：

```text
upstream_conn_id    当前节点连接上一跳的连接 ID
downstream_conn_id  下一跳连接当前节点的连接 ID
```

当前版本的自动重连由以下几部分共同完成：

1. 统一连接回调分流。
2. 上游 Client 状态机。
3. 下游 Server 状态机。
4. 周期 watchdog 兜底。
5. 重连前清理旧配对。

## 5. 关键升级点

### 5.1 统一连接回调，区分 upstream 和 downstream

路由节点同时注册 Client 和 Server 相关逻辑。如果分别注册多套连接回调，后注册的回调可能覆盖先注册的回调，导致某些连接事件无法被正确处理。

当前版本改为只注册一套统一的 `sle_connection_callbacks_t`，然后在回调内部判断连接属于上游还是下游。

核心文件：

```text
application/samples/products/sle_uart_sever/sle_uart.c
```

核心逻辑：

```c
static void sle_uart_router_connect_state_changed_cbk(...)
{
    if (sle_uart_server_is_downstream_link(conn_id)) {
        sle_connect_state_changed_cbk(...);
        return;
    }

    is_upstream = sle_uart_client_is_upstream_link(conn_id, addr);

    if (is_upstream) {
        sle_uart_client_sample_connect_state_changed_cbk(...);
    } else {
        sle_connect_state_changed_cbk(...);
    }
}
```

这个改动解决了“上游连接事件和下游连接事件互相干扰”的问题，是自动重连稳定的基础。

### 5.2 上游断开后重新扫描上一跳

每个路由节点的上游 Client 只连接指定名称的上一跳：

```text
R1 -> SLE_SVR
R2 -> SLE_R1
R3 -> SLE_R2
R4 -> SLE_R3
```

核心文件：

```text
application/samples/products/sle_uart_sever/sle_uart_client/sle_uart_client.c
```

当上游断开时，新版会：

1. 删除旧配对信息。
2. 清空 upstream 状态。
3. 重新启动扫描。

核心逻辑：

```c
} else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
    if (addr != NULL) {
        sle_remove_paired_remote_device(addr);
    } else if (g_remote_addr_valid) {
        sle_remove_paired_remote_device(&g_sle_uart_remote_addr);
    }
    sle_uart_client_clear_upstream();
    sle_uart_start_scan();
}
```

这样上一跳掉电后，当前节点不会一直停留在旧连接状态。上一跳恢复广播后，当前节点可以再次扫描到并重新连接。

### 5.3 下游断开后重新广播自身

每个路由节点的下游 Server 会广播自己的名称：

```text
R1 广播 SLE_R1
R2 广播 SLE_R2
R3 广播 SLE_R3
R4 广播 SLE_END
```

核心文件：

```text
application/samples/products/sle_uart_sever/sle_uart_server/sle_uart_server.c
application/samples/products/sle_uart_sever/sle_uart_server/sle_uart_server_adv.c
```

当下游断开时，新版会：

1. 清空 downstream 状态。
2. 清空 MTU 完成标志。
3. 重新开启广播。

核心逻辑：

```c
} else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
    if (conn_id == g_sle_conn_hdl || g_sle_conn_hdl == SLE_INVALID_CONN_ID) {
        sle_uart_server_clear_downstream();
        sle_uart_server_adv_init();
    }
}
```

这保证了下一跳节点掉电恢复后，仍然能够重新扫描并连接当前节点。

### 5.4 watchdog 兜底，避免扫描或连接卡死

仅依赖连接事件还不够，因为实际测试中扫描、连接、停止扫描等过程可能卡在中间状态。

当前版本增加了周期 watchdog：

```text
application/samples/products/sle_uart_sever/sle_uart.c
application/samples/products/sle_uart_sever/sle_uart_client/sle_uart_client.c
```

路由主循环每 500 ms 执行一次：

```c
while (1) {
    if (!sle_uart_server_has_downstream()) {
        sle_uart_server_adv_init();
    }
    sle_uart_client_watchdog_tick();
    sle_uart_router_print_stats();
    osal_msleep(500);
}
```

watchdog 的作用：

| 状态 | 处理方式 |
|---|---|
| 上游未连接且未扫描 | 重新启动扫描 |
| 上游正在连接但超时 | 清理连接状态并重新扫描 |
| 扫描持续过久没有结果 | 停止扫描后重新开始 |
| 下游未连接 | 确保广播处于开启状态 |

这相当于给连接状态机加了一个周期性纠错机制，避免系统长时间停留在错误状态。

### 5.5 重连前清理旧配对信息

新版在重新连接前调用：

```c
sle_remove_paired_remote_device(...)
```

这个改动主要解决以下问题：

1. 对端断电后，本端保留旧配对状态。
2. 对端重新上电后，协议栈认为已经有历史连接或历史配对。
3. 实际连接过程出现状态不一致。
4. 表现为“连上立刻断开”或必须手动复位。

清除旧配对后，每次重连都从干净状态重新建立连接，稳定性明显提高。

## 6. 新版自动重连流程

### 6.1 上游节点掉线

以 R3 的上一跳 R2 掉电为例：

```text
R2 掉电
R3 收到 upstream disconnected
R3 删除 R2 旧配对
R3 清空 upstream_conn_id
R3 重新扫描 TARGET_UPSTREAM_NAME = SLE_R2
R2 恢复供电并广播 SLE_R2
R3 扫描到 R2
R3 重新连接 R2
R3 重新进行 MTU / SSAP 流程
数据继续转发
```

### 6.2 下游节点掉线

以 R3 的下一跳 R4 掉电为例：

```text
R4 掉电
R3 收到 downstream disconnected
R3 清空 downstream_conn_id
R3 重新广播 MY_ROUTER_NAME = SLE_R3
R4 恢复供电并扫描 SLE_R3
R4 重新连接 R3
R3 与 R4 重新协商 MTU / PHY
数据继续转发
```

### 6.3 尾节点 Node6 掉线

以 Node6 掉电为例：

```text
Node6 掉电
R4 发现 downstream disconnected
R4 重新广播 SLE_END
Node6 恢复供电
Node6 扫描 SLE_END
Node6 重新连接 R4
Node6 恢复测速输出
```

## 7. 与自动 Mesh 的区别

当前方案不是 Mesh，而是固定拓扑的应用层自动重连。

| 项目 | 当前方案 | Mesh 方案 |
|---|---|---|
| 拓扑 | 固定 5 跳线性链路 | 动态多路径网络 |
| 下一跳选择 | 手动配置节点名称 | 自动发现和路由选择 |
| 转发开销 | 低 | 较高 |
| 速率 | 更适合测速 | 可能因路由维护下降 |
| 适用场景 | 固定链路、测速、稳定传输 | 节点位置不固定、路径不确定 |

本项目目标是固定 5 跳测速和断线恢复，因此当前方案比 Mesh 更直接，开销也更低。

## 8. 当前测试结果

根据实际串口日志测试，当前版本已经验证：

1. 5 跳链路可以稳定跑通。
2. Node6 端持续输出端到端测速。
3. 2 分钟连续运行速率稳定在约 560 至 571 kbit/s。
4. 单独断开 R2、R3、R4 或 Node6 后，恢复供电和距离后可以自动重连。
5. 重连过程中不需要复位其他节点。

典型 Node6 日志：

```text
[5-HOP E2E LIMIT] Speed: 71400 Bytes/s (571 kbps)
[5-HOP E2E LIMIT] Speed: 70000 Bytes/s (560 kbps)
```

## 9. 结论

本次升级的核心不是更换底层协议，而是在 SLE 示例应用层补全了可靠的连接生命周期管理。

上一套代码的主要问题是断线后状态无法自动回到“可重新连接”的状态。当前版本通过统一连接事件分流、上下游独立状态管理、扫描/广播重启、重连前清配对和 watchdog 兜底，实现了从“断线后需要人工复位”到“断线后自动恢复连接”的升级。

该方案适合当前固定 5 跳 SLE 传输系统，能在保持较低转发开销的同时，提高现场测试和演示时的稳定性。
