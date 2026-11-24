# SLE 双板路由桥

每个逻辑路由器由两块 BearPi-Pico H3863 组成：

```text
上游 SLE Server -> RX板（纯Client） -> UART1 -> TX板（纯Server） -> 下游 SLE Client
```

Node1 和 Node6 继续使用 `products/sle_uart`，PC 数据口继续使用板载 USB 串口。

## 接线

两块路由板分别使用 USB 供电，只交叉连接 UART1 和地线：

| RX板 | TX板 |
|---|---|
| GPIO15 / UART1_TX | GPIO16 / UART1_RX |
| GPIO16 / UART1_RX | GPIO15 / UART1_TX |
| GND | GND |

不要连接两块独立供电开发板的 5V 或 3.3V 引脚。

默认参数为 UART1、GPIO15/16、921600 8N1。UART帧带有长度、序号和CRC16；正向传输文件数据及诊断探针，反向传输ACK、NACK及诊断报告。

启用本示例时，Kconfig 会把系统 HSO 日志串口设为“禁用”，同时关闭运行时串口动态绑定，确保任何系统日志都不会混入 UART1 桥接数据。调试信息不再从 UART1 输出。

## 编译矩阵

菜单中启用 `Support SLE dual-board route bridge sample.`，然后为每块板设置：

| 固件 | Board side | Logical router id | SLE行为 |
|---|---|---:|---|
| R1-RX | RX board | 1 | 扫描 SLE_SVR |
| R1-TX | TX board | 1 | 广播 SLE_R1 |
| R2-RX | RX board | 2 | 扫描 SLE_R1 |
| R2-TX | TX board | 2 | 广播 SLE_R2 |
| R3-RX | RX board | 3 | 扫描 SLE_R2 |
| R3-TX | TX board | 3 | 广播 SLE_R3 |
| R4-RX | RX board | 4 | 扫描 SLE_R3 |
| R4-TX | TX board | 4 | 广播 SLE_END |

不要同时启用旧的 `Support SLE Route sample.`。

## 两跳验证模式

验证以下最小双板路由链路时：

```text
Node1 -> R1-RX -> UART1 -> R1-TX -> Node6
```

仅编译和烧录四种固件：

| 固件 | 角色配置 | Logical router id | 两跳模式 |
|---|---|---:|---|
| Node1 | SLE UART Server | - | 开启 |
| R1-RX | RX board | 1 | 开启 |
| R1-TX | TX board | 1 | 开启 |
| Node6 | SLE UART Client | - | 开启 |

四种固件都勾选 `Enable SLE two-hop validation mode`。开启后 R1-TX 改为广播 `SLE_END`，Node6 接受只经过 R1 的诊断路径；UART1 帧、文件传输和 ACK/NACK 机制不变。上位机选择“两跳验证模式”，只等待 `Node1、R1、Node6` 连续三次响应。

完整五跳时不要勾选该选项，R1-TX 会恢复广播 `SLE_R1`，Node6 也会恢复要求 R1~R4 全部经过。

## 建链顺序

1. 烧录并连接每一组 RX/TX 板的 UART1。
2. 先给四组双板路由和 Node6 上电。
3. 最后给 Node1 上电。
4. 使用原上位机的链路测试功能确认 R1、R2、R3、R4、Node6 均返回。
5. 链路测试通过后再发送文件。

TX板只有在检测到同编号RX板的UART心跳且RX板上游SLE连续稳定1秒后才会向下游广播。UART同伴超时为6秒；瞬时心跳或SLE状态抖动时，TX板继续保持下游最多8秒，只有确认上游持续丢失后才关闭下游，避免级联误断。短时下游不可用期间会保留SLE待发队列。

## 射频摆放

同一逻辑路由的两根天线不要并排紧贴。首次测试建议比较 20cm、1m、3m 三种间距；距离较长时使用差分串口收发器，不要直接延长3.3V TTL UART线。
