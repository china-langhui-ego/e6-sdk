#!/bin/bash
# XR Camera Control — VR APK Installer (Linux)
# Usage: chmod +x install_vr.sh && ./install_vr.sh

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ERRFILE="/tmp/xr_install_vr_error.$$"
FAILED=0

# ====== Functions ======

cleanup() {
    rm -f "$ERRFILE"
}

failed_exit() {
    echo ""
    echo "============================================"
    echo "  INSTALL FAILED!"
    echo "============================================"
    cleanup
    read -r -p "Press Enter to continue..."
    exit 1
}

check_result() {
    if [ -s "$ERRFILE" ]; then
        echo "  -> Failed"
        cat "$ERRFILE"
        FAILED=1
    else
        echo "  -> Success"
    fi
}

install_app() {
    echo ""
    echo "[$1]"
    adb install -r "app-release-vr.apk" >/dev/null 2>"$ERRFILE"
    check_result
}

uninstall_app() {
    echo ""
    echo "[Uninstall old app (clear data)]"
    adb uninstall com.ssnwt.egoserver >/dev/null 2>&1 || true
    adb uninstall com.ssnwt.helloxr >/dev/null 2>&1 || true
    echo "  -> Done"
}

start_app() {
    echo ""
    echo "[Start app]"
    adb shell am force-stop com.ssnwt.egoserver >/dev/null 2>&1
    adb shell am start -n com.ssnwt.egoserver/com.ssnwt.helloxr.VrNativeActivity >/dev/null 2>&1
}

set_auto_start() {
    echo ""
    echo "[Configure auto-start on boot]"
    adb shell setprop persist.vr.autostartapp.pkg com.ssnwt.egoserver >/dev/null 2>"$ERRFILE"
    adb shell setprop persist.vr.autostartapp.entry com.ssnwt.helloxr.VrNativeActivity >/dev/null 2>>"$ERRFILE"
    adb shell setprop persist.sxr.autostartapp.pkg com.ssnwt.egoserver >/dev/null 2>"$ERRFILE"
    adb shell setprop persist.sxr.autostartapp.entry com.ssnwt.helloxr.VrNativeActivity >/dev/null 2>>"$ERRFILE"
    check_result
}

reboot_device() {
    echo ""
    local SEC=5
    echo "[Reboot device, waiting ${SEC}s]"
    while [ "$SEC" -gt 0 ]; do
        sleep 1
        SEC=$((SEC - 1))
        printf "."
    done
    echo ""
    adb reboot >/dev/null 2>"$ERRFILE"
    check_result
}

# ====== Main ======

echo "Waiting for device..."
adb wait-for-device >/dev/null 2>&1

echo ""
echo "============================================"
echo "  XR Camera Control - VR APK Installer"
echo "============================================"
echo ""
echo "Hint:"
echo "  Choose Y (uninstall + clear data) if keep-data install failed."
echo ""
echo "  WARNING: Choosing Y will CLEAR ALL APP DATA!"
echo "============================================"
echo ""

read -r -p "Uninstall old version (clear data)? [y/N]: " CLEAR
if [ "${CLEAR,,}" = "y" ]; then
    uninstall_app
    if [ "$FAILED" = "1" ]; then
        failed_exit
    fi
    LABEL="Install new app"
else
    LABEL="Install app (keep data)"
fi

install_app "$LABEL"
if [ "$FAILED" = "1" ]; then
    failed_exit
fi
start_app
if [ "$FAILED" = "1" ]; then
    failed_exit
fi
set_auto_start
if [ "$FAILED" = "1" ]; then
    failed_exit
fi
reboot_device
if [ "$FAILED" = "1" ]; then
    failed_exit
fi

echo ""
echo "============================================"
echo "  INSTALL COMPLETE!"
echo "============================================"
cleanup
exit 0
