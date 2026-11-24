#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "未找到 python3。请先安装 python3、python3-venv 和 python3-tk。"
    exit 1
fi

if ! "$PYTHON_BIN" -c 'import tkinter' >/dev/null 2>&1; then
    echo "缺少 Tk。请执行：sudo apt update && sudo apt install -y python3-tk python3-venv"
    exit 1
fi

cd "$APP_DIR"
if [ ! -x ".build-venv/bin/python" ]; then
    "$PYTHON_BIN" -m venv .build-venv
fi

.build-venv/bin/python -m pip install --upgrade pip
.build-venv/bin/python -m pip install -r requirements.txt "pyinstaller>=6.10,<7"
.build-venv/bin/python -m PyInstaller \
    --noconfirm \
    --clean \
    SLE-5Hop-Transfer-Stable260-Ubuntu24.04.spec

echo
echo "构建完成：$APP_DIR/dist/SLE-5Hop-Transfer-Stable260-Ubuntu24.04"
