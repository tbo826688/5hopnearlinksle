# Stable260 六节点五跳文件传输

本分支保存 2026-09-08 Stable260 板端基线及配套上位机。2026-11-24 完成六角色源码恢复和重新编译，六个生成固件与原始包逐字节一致。R2 为普通中继，没有展示副本输出。

## 用途和结构

```text
发送电脑 → Type-C UART0 → Node1 → R1 → R2 → R3 → R4 → Node6 → Type-C UART0 → 接收电脑
```

共 6 块板、5 段 SLE 无线链路。Node1 接收文件字节，R1～R4 单板主从一体转发，Node6 恢复文件流；接收电脑验证文件长度和 SHA-256。文字、图片等均按文件传输，不是已经完成的双向聊天或实时视频系统。

无线可靠层含会话、序号、CRC16、ACK/NACK、重传、动态窗口、队列反馈及链路恢复。历史负责人反馈室内五跳约 257 kbit/s；户外距离、长时稳定性和本轮硬件回归尚未验证。260 kbit/s 是上位机发送目标，不是 PHY 速率。

## 文件导航

| 路径 | 作用 |
|---|---|
| [sle_uart](application/samples/products/sle_uart) | Node1/Node6 的串口、发送窗口、重传与接收重组 |
| [sle_file_transfer.h](application/samples/products/sle_uart/sle_file_transfer.h) | 分包、控制和状态协议 |
| [route](application/samples/products/route) | R1～R4 单板中继；`ROUTE_NODE_ID` 为 1～4 |
| [sle_link_profile.h](application/samples/products/sle_link_profile.h) | 共享无线数据档位配置 |
| [route_bridge](application/samples/products/route_bridge) | 保留的双板桥接备选实现，当前六板方案不启用 |
| [build_stable260_firmware.ps1](application/samples/products/build_stable260_firmware.ps1) | 分别生成六角色固件的构建脚本 |
| [默认构建配置](build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config) | 目前选择 Node1 / Server / 文件模式 / 完整五跳 |
| [NV 参数配置](middleware/chips/ws63/nv/nv_config/cfg/acore/app.json) | 与已验证固件配套的原有非易失参数；重现打包时需要保留 |
| [六角色固件和配置](output/Stable260-5Hop-Firmware-20260908-024441) | 可直接烧录的原包、各角色 `mconfig.h`、哈希及构建核对记录 |
| [Windows 上位机源码](application/tools/sle_transfer_gui) | 文件发送/接收、建链、节奏控制、进度与 SHA-256 |
| [Windows Stable260 EXE](application/tools/sle_transfer_gui/dist/SLE-5Hop-Transfer-Stable260.exe) | 可直接运行的现有 Windows 上位机 |
| [Ubuntu 24.04 源码](application/tools/sle_transfer_gui_ubuntu2404) | Linux 运行、环境检查及打包脚本 |
| [Ubuntu 24.04 源码包](application/tools/release/NearLink-SLE-5Hop-Transfer-Stable260-Ubuntu24.04.tar.gz) | 可复制到 Ubuntu 的源码包 |
| [Ubuntu DEB](application/tools/release/sle-5hop-transfer-stable260_1.0.0-1_all.deb) | 需要系统依赖，尤其是 `python3-tk` |
| [本轮构建验证](output/Stable260-5Hop-Firmware-20260908-024441/VALIDATION_20261124.json) | 六角色退出码、哈希、与原包逐字节比对及硬件验证边界 |
| [回退记录](ROLLBACK_STABLE260_20261124.md) | 源码恢复范围、旧串口配置和本机备份位置 |

Qt 页面、socket 后台不在本分支交付范围；上位机保留现有兼容版本，没有声称恢复到 9 月 8 日的历史源码。

## 烧录与最小验证

在固件目录按标签选择对应文件：

| 板子 | 文件 |
|---|---|
| Node1 | `01_Node1_SLE_Server_Stable260.fwpkg` |
| R1 | `02_R1_SLE_Route_ID1_Stable260.fwpkg` |
| R2 | `03_R2_SLE_Route_ID2_Stable260.fwpkg` |
| R3 | `04_R3_SLE_Route_ID3_Stable260.fwpkg` |
| R4 | `05_R4_SLE_Route_ID4_Stable260.fwpkg` |
| Node6 | `06_Node6_SLE_Client_Stable260.fwpkg` |

HiSpark 中选择 WS63、实际 COM、串口烧录、921600 和对应 `.fwpkg`；等待设备时按 RST。Node1/Node6 运行时均使用 Type-C UART0、GPIO17/18、921600 8N1；普通路由调试口使用 UART0/115200，不连接 UART1 展示端。

六块全部上电，先在 Node6 电脑监听接收，再在 Node1 电脑选择完整五跳、稳定 260 节奏，建链后发送小文本和图片。以接收端长度及 SHA-256 校验通过为成功；发送端进度 100% 不等于接收端完成。

## 开发和重新构建

工程基于 WS63 SDK，在 Windows 使用 Python 3.11 与配套 RISC-V/HiSpark 工具链。工程自带工具链位于 `tools/bin/compiler/riscv/cc_riscv32_musl_105/cc_riscv32_musl_fp_win/bin`；补齐所需运行环境后，在仓库根目录执行：

```powershell
# 构建当前默认 Node1
py -3.11 build.py ws63-liteos-app -j8

# 构建六角色，目标目录选一个尚未使用的新目录
& .\application\samples\products\build_stable260_firmware.ps1 -OutputDirectory .\output\Stable260-NewBuild -Jobs 8
```

相对输出目录以仓库根目录为基准。构建六角色的脚本会临时切换角色，结束时恢复原配置；烧录必须选分角色导出的包。新固件需重新烧录和做硬件测试。

`ttttt7878.hiproj` 是现有 HiSpark 工程文件，保留了原电脑的 SDK/工具链路径及 COM 设置；换目录或换电脑后需在 HiSpark 中更新对应路径和实际端口。

Windows 上位机源码运行与测试：

```powershell
Set-Location application\tools\sle_transfer_gui
py -3.11 -m pip install -r requirements.txt
py -3.11 stable260_app.py
py -3.11 -m unittest discover -v
```

2026-11-24 在 Windows Python 3.11 环境重跑上述单元测试，30 项全部通过；这不是 Qt/Ubuntu 运行或实板收发验证。

Ubuntu 按 [Ubuntu 运行说明](application/tools/sle_transfer_gui_ubuntu2404/README_Ubuntu24.04.md) 处理 Tk、pyserial 和串口权限。已有平板曾出现 `python3-tk` 无候选包，需先解决系统软件源，不能把本仓库 DEB 当作免依赖可执行文件。

## 本次 Git 保存范围

包括板端源码与配置、六角色构建入口、配套 Windows/Ubuntu 24.04 上位机源码与测试、选定运行包、六角色固件及可携带的核对记录。

不收录本机回退备份、临时演示材料、Python 缓存、SDK 编译树和旧上位机重复打包目录。SDK 早期已跟踪的生成文件属于既有仓库历史，本次没有把它们的本地编译变化加入提交。回退记录中的本机备份目录不会随 Git 克隆获得；理解当前版本请以本文与已提交的验证 JSON 为准。
