#!/usr/bin/env bash
set -euo pipefail

echo "[系统]"
if [ -r /etc/os-release ]; then
    . /etc/os-release
    echo "${PRETTY_NAME:-未知 Linux 发行版}"
else
    uname -a
fi
echo "架构：$(uname -m)"

echo
echo "[Python]"
python3 --version

echo
echo "[Tkinter]"
python3 -c 'import tkinter; print("Tk", tkinter.TkVersion)'

echo
echo "[串口设备]"
found=0
for port in /dev/ttyUSB* /dev/ttyACM*; do
    if [ -e "$port" ]; then
        ls -l "$port"
        found=1
    fi
done
if [ "$found" -eq 0 ]; then
    echo "未找到 /dev/ttyUSB* 或 /dev/ttyACM*；请检查 USB 连接和数据线。"
fi

echo
echo "[当前用户组]"
id -nG
if ! id -nG | tr ' ' '\n' | grep -qx dialout; then
    echo "提示：当前用户不在 dialout 组，串口可能无权限。"
    echo "执行 sudo usermod -aG dialout \"$USER\" 后注销并重新登录。"
fi
