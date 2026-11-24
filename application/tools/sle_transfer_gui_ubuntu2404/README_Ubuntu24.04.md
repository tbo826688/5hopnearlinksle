# NearLink SLE 五跳文件透传 Stable260 - Ubuntu 24.04

本目录是 Windows `SLE-5Hop-Transfer-Stable260.exe` 对应的 Ubuntu 24.04 原生版，使用相同的 V5HP/V5H2 协议、SLE 闭环稳定 260 节奏、逐跳建链检测、全链路队列反馈、文件长度和 SHA-256 校验。Linux 版不需要 Wine。

## 推荐安装：DEB 软件包

在 Ubuntu 24.04 x64 或 ARM64 桌面版中打开终端，进入软件包所在目录后执行：

```bash
sudo apt install ./sle-5hop-transfer-stable260_1.0.0-1_all.deb
```

安装完成后，可以在应用菜单搜索“星闪五跳文件透传 Stable260”，也可以运行：

```bash
sle-5hop-transfer-stable260
```

DEB 包会声明 `python3-tk`、`python3-serial`、`xdg-utils` 和中文字体依赖，由 APT 自动安装缺少的组件。

卸载命令：

```bash
sudo apt remove sle-5hop-transfer-stable260
```

## 直接运行源码包

先安装系统组件：

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-tk xdg-utils fonts-noto-cjk
```

给予脚本执行权限并启动：

```bash
chmod +x start_ubuntu24.sh check_ubuntu24.sh build_ubuntu24.sh
./check_ubuntu24.sh
./start_ubuntu24.sh
```

首次运行会在当前目录创建 `.venv` 并安装 `pyserial`。

## 构建单文件 Linux 程序

PyInstaller 必须在 Ubuntu 24.04 中执行，不能在 Windows 上交叉生成 Linux ELF。进入源码目录后运行：

```bash
./build_ubuntu24.sh
```

输出文件为：

```text
dist/SLE-5Hop-Transfer-Stable260-Ubuntu24.04
```

## USB 串口权限

开发板通常显示为 `/dev/ttyUSB0` 或 `/dev/ttyACM0`。如果提示 `Permission denied`，执行：

```bash
sudo usermod -aG dialout "$USER"
```

之后注销并重新登录。Node1、Node6 数据串口保持与固件一致，当前项目默认使用 `921600 8N1`。

## 现场使用顺序

1. Node6 电脑先选择“接收端”、串口和保存目录，点击“开始监听”。
2. Node1 电脑选择“发送端”，拓扑保持“完整5跳模式”。
3. 点击“开始建链”，等待 Node1、R1、R2、R3、R4、Node6 连续稳定全绿。
4. 发送节奏保持“SLE闭环稳定260（推荐）”，选择文件并开始发送。
5. 最终以 Node6 接收文件长度和 SHA-256 一致为准；发送页 100% 只表示电脑已经把数据写入 Node1。

`260 kbps` 是应用层目标发送速率，不是 SLE PHY 标称速率。Ubuntu 与 Windows 两端可以混合使用，只要两端选择相同的文件协议。
