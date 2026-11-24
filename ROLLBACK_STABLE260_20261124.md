# 当前版本：回退到 2026-09-08 Stable260 六角色

回退日期：2026-11-24。主工程：`C:\bearpi-picospeed56011`。

根据负责人明确要求，**整个六板工程**恢复 9 月 8 日 Stable260 基线：Node1 → R1 → R2 → R3 → R4 → Node6。R2 恢复普通无线中继，不再输出 RDSP 展示副本。

## 直接烧录的原版固件

使用原目录 [Stable260-5Hop-Firmware-20260908-024441](output/Stable260-5Hop-Firmware-20260908-024441)，六个包已按原 `SHA256SUMS.txt` 全部核对。

| 板子 | 原版文件 |
|---|---|
| Node1 | `01_Node1_SLE_Server_Stable260.fwpkg` |
| R1 | `02_R1_SLE_Route_ID1_Stable260.fwpkg` |
| R2 | `03_R2_SLE_Route_ID2_Stable260.fwpkg` |
| R3 | `04_R3_SLE_Route_ID3_Stable260.fwpkg` |
| R4 | `05_R4_SLE_Route_ID4_Stable260.fwpkg` |
| Node6 | `06_Node6_SLE_Client_Stable260.fwpkg` |

Node1、Node6 的文件收发均使用板载 Type-C 对应 UART0、GPIO17/18、921600、8N1。普通路由通过 SLE 转发，调试 UART0 按 115200 使用，不接 UART1 展示端。

本次严格恢复旧宏配置，包括 `DEBUG_UART=0`、`DEBUG_UART_BAUDRATE=115200`、`AT_UART=0`、`UART0_BAUDRATE=115200`、`UART1_BAUDRATE=921600`；文件任务运行时会重新把数据 UART0 初始化为 921600。这与 10 月端点版统一串口保护配置不同。Node6 保留旧版接收侧日志抑制，不要再给文件口新增调试文字。

烧录时关闭占串口的软件，HiSpark 选择 WS63、实际 COM、串口烧录、921600，并手动选择对应包，等待时按 RST。若六块当前版本不确定，则六块都烧对应原版。烧录完成后先启动 Node6 接收，再在 Node1 发送端建链、发送小文本及图片，最终以接收文件长度和 SHA-256 一致为成功依据。

## 源码恢复范围

恢复了端点 `sle_uart/Kconfig`、`sle_uart/sle_uart.c`，路由 `route/Kconfig`、`route/CMakeLists.txt`、`route/sle_uart.c`，以及六角色 `build_stable260_firmware.ps1` 的旧配置。

由于 9 月 8 日业务改动没有提交到 Git，本次没有用 Git HEAD 当作旧版，而是从当时的修改记录逐项撤销后加的串口和展示改动。构建时使用 9 月 8 日保存的每个角色 `mconfig.h` 作为核对基准。

R2 展示模块、展示测试、10 月端点串口保护测试及对应构建入口已移到备份的 `retired` 目录，不再作为当前工程的构建入口。旧的 `route_bridge` 备选实现保留源码，当前配置不启用。上位机源码不在本次回退范围内。

六角色构建脚本唯一保留的执行环境调整是把 `py -3` 明确为 `py -3.11`，避免本机默认 Python 升级后缺少构建依赖；不改变旧固件的功能配置。

## 当前配置与构建

当前默认角色恢复为 **Node1 / SLE Server / 文件模式 / UART0 GPIO17/18 / 完整五跳**，不再默认选择双板桥接 R2-TX。

在工程根目录编译当前 Node1：

```powershell
Set-Location C:\bearpi-picospeed56011
py -3.11 build.py ws63-liteos-app -j8
```

重新构建六角色并放入一个全新目录：

```powershell
& .\application\samples\products\build_stable260_firmware.ps1 -OutputDirectory C:\bearpi-picospeed56011\output\Stable260-Legacy-Rebuild-New -Jobs 8
```

不要选择 10 月的 `TypeC-UART0` 或 `DualUART` 固件来做此次整套回退。保存的 HiSpark 下载项已核对指向 9 月 8 日原 Node1 包，其他板仍需手动选其角色包。

## 备份与验证记录

**本次六角色全量编译、链接、签名及打包均成功；六个重新生成的 `.fwpkg` 与 9 月 8 日对应原包逐字节一致。** 每个角色的生成配置也与原快照完全一致，当前 `output/ws63` 和默认配置均为旧版 Node1。

备份目录：[Rollback-Stable260-20260908-20261124-190813](output/Rollback-Stable260-20260908-20261124-190813)。

以上备份仅保留在原开发电脑，不上传 Git。仓库中可直接查看的精简验证记录为 [VALIDATION_20261124.json](output/Stable260-5Hop-Firmware-20260908-024441/VALIDATION_20261124.json)，克隆后优先阅读 [当前版本使用说明](README_STABLE260.md)。

- `backup/`：回退前的源文件、原配置、旧交接文档；旧通用生成目录单独保留于 `backup/output-ws63-before`。
- `backup_manifest.json`：源文件备份哈希，共 53 个文件。
- `retired/`：本次退出当前构建的新增模块和入口，可以恢复。
- `history-patch-*.txt`、`restored-source/`：历史修改依据和还原结果。
- `pinned-20260908/`：六个已核对的原版包副本。
- `rebuild-logs/`、`rebuilt/`、`rebuild-verification.json`：本次实际编译日志、新包及逐角色核对结果。

本次恢复及编译的最终结果以 `rebuild-verification.json` 为准。没有进行实板烧录或硬件传输测试；历史室内约 257 kbit/s 不等于此次回退后重新测得的结果。

10 月 19 日交接文档描述的是回退前版本，当前版本状态以本文和上述验证记录为准。
