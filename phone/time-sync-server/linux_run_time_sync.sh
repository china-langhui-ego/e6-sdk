#!/bin/bash
# Linux 启动脚本：自动检查环境 + 启动时间同步服务
# 首次运行自动配置 udev/dialout（仅首次需要 sudo），后续无需权限
# Python 优先级: tools/linux-venv > 系统 Python 3.10+ > 自动下载独立 Python
set -e
cd "$(dirname "$0")"

VENV_DIR="tools/linux-venv"
VENV_PY="$VENV_DIR/bin/python"
RULES_FILE="/etc/udev/rules.d/99-usb-sync.rules"

# ====== 选 Python ======
PYTHON=""

# 1) tools/linux-venv 已存在，优先用
if [ -x "$VENV_PY" ]; then
    PYTHON="$VENV_PY"
    echo "[setup] Using venv: $VENV_PY"
fi

# 2) 检查系统 Python
if [ -z "$PYTHON" ]; then
    SYS_PY=""
    for P in python3 python3.13 python3.12 python3.11 python3.10; do
        if command -v "$P" >/dev/null 2>&1; then
            SYS_PY="$P"
            break
        fi
    done
    if [ -n "$SYS_PY" ]; then
        SYS_VER=$("$SYS_PY" -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')" 2>/dev/null || echo "0.0")
        SYS_PATH=$(command -v "$SYS_PY")
        SYS_MAJOR=$(echo "$SYS_VER" | cut -d. -f1)
        SYS_MINOR=$(echo "$SYS_VER" | cut -d. -f2)
        if [ "$SYS_MAJOR" -ge 3 ] 2>/dev/null && [ "$SYS_MINOR" -ge 10 ] 2>/dev/null; then
            PYTHON="$SYS_PATH"
            echo "[setup] Using system Python: $PYTHON ($SYS_VER)"
        else
            echo "[setup] System $SYS_PY is $SYS_VER (need 3.10+), skip"
        fi
    fi
fi

# 3) 都不行，下载独立 Python 到 tools/python-linux/
if [ -z "$PYTHON" ]; then
    STANDALONE_DIR="tools/python-linux"
    STANDALONE_PY="$STANDALONE_DIR/bin/python3"
    if [ ! -x "$STANDALONE_PY" ]; then
        echo "[setup] No usable Python found, downloading standalone to $STANDALONE_DIR/ ..."
        URL="https://github.com/astral-sh/python-build-standalone/releases/download/20241016/cpython-3.12.7+20241016-x86_64-unknown-linux-gnu-install_only.tar.gz"
        mkdir -p "$STANDALONE_DIR"
        if command -v wget >/dev/null 2>&1; then
            wget -q --show-progress -O /tmp/py-standalone.tar.gz "$URL"
        else
            curl -L -# -o /tmp/py-standalone.tar.gz "$URL"
        fi
        tar xzf /tmp/py-standalone.tar.gz -C "$STANDALONE_DIR" --strip-components=1
        rm -f /tmp/py-standalone.tar.gz
        echo "[setup] Standalone Python ready at $STANDALONE_PY"
    fi
    PYTHON="$STANDALONE_PY"
fi

# ====== 检查 pyserial（仅 venv 模式需要安装） ======
if [ "$PYTHON" = "$VENV_PY" ]; then
    # venv 已有 pyserial，跳过
    :
elif ! "$PYTHON" -c "import serial" 2>/dev/null; then
    # 系统/独立 Python 缺 pyserial → 创建 venv
    echo "[setup] pyserial not found, creating venv at $VENV_DIR using $PYTHON..."
    "$PYTHON" -m venv --copies "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip
    "$VENV_DIR/bin/pip" install --quiet pyserial
    PYTHON="$VENV_PY"
    echo "[setup] venv + pyserial ready at $VENV_DIR"
fi

# ====== 检查 udev 规则 ======
NEED_UDEV=0
if [ ! -f "$RULES_FILE" ]; then
    NEED_UDEV=1
elif ! grep -qE '4ee2|903b' "$RULES_FILE" 2>/dev/null; then
    NEED_UDEV=1
fi
if [ "$NEED_UDEV" = "1" ]; then
    echo "[setup] Installing udev rules (needs sudo)..."
    sudo tee "$RULES_FILE" > /dev/null <<'UDEVEOF'
# VR headset for serial time sync
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="4ee2", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="903b", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d00", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d01", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d02", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d03", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d04", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="18d1", ATTR{idProduct}=="2d05", MODE="0666"
KERNEL=="ttyACM[0-9]*", ATTRS{idVendor}=="18d1", ATTRS{idProduct}=="903b", MODE="0666"
UDEVEOF
    sudo udevadm control --reload-rules
    sudo udevadm trigger --action=add --subsystem-match=usb 2>/dev/null || true
    sudo udevadm trigger --action=add --subsystem-match=tty 2>/dev/null || true
    echo "[setup] udev rules installed"
fi

# ====== 检查 dialout 组 ======
if ! id -nG 2>/dev/null | tr ' ' '\n' | grep -qx dialout; then
    echo "[setup] Adding $USER to dialout group (needs sudo)..."
    sudo usermod -aG dialout "$USER"
    echo "[setup] NOTE: dialout group takes effect after re-login or 'newgrp dialout'"
fi

# ====== 启动服务 ======
echo "[setup] Final Python: $PYTHON ($("$PYTHON" --version 2>&1))"
exec "$PYTHON" scripts/time_sync_server.py "$@"
