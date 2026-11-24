#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN="${PYTHON_BIN:-python3}"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "未找到 python3。请先执行：sudo apt update && sudo apt install -y python3 python3-venv python3-tk"
    exit 1
fi

if ! "$PYTHON_BIN" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)'; then
    echo "需要 Python 3.10 或更高版本；Ubuntu 24.04 默认提供 Python 3.12。"
    exit 1
fi

if ! "$PYTHON_BIN" -c 'import tkinter' >/dev/null 2>&1; then
    echo "缺少 Tk 图形界面组件。请执行：sudo apt update && sudo apt install -y python3-tk python3-venv"
    exit 1
fi

cd "$APP_DIR"
if [ ! -x ".venv/bin/python" ]; then
    "$PYTHON_BIN" -m venv .venv
fi

if ! .venv/bin/python -c 'import serial' >/dev/null 2>&1; then
    .venv/bin/python -m pip install -r requirements.txt
fi

exec .venv/bin/python stable260_app.py "$@"
