# Serial Time Sync Server (Windows / Linux)

NTP time synchronization with VR headset via CDC-ACM serial port. **Driver-free** —
Windows built-in `usbser.sys` automatically recognizes CDC-ACM devices as COM ports;
Linux kernel `cdc_acm` module exposes them as `/dev/ttyACM*`. No Zadig/WinUSB/admin privileges required.

## Usage

### Windows / macOS

The NTP time sync service is integrated into the EgoVision PC app — no separate launcher script required: enable the "Time Sync" toggle in the app (off by default; once enabled it restores automatically on subsequent launches), and it auto-responds to VR time requests over USB.

### Linux

```sh
chmod +x linux_run_time_sync.sh
./linux_run_time_sync.sh   # start (Ctrl+C to exit)
```

The script auto-selects a Python runtime with this priority:

1. **`tools/linux-venv/`** — use directly if exists
2. **System Python 3.10+** — run directly if pyserial available; otherwise auto-create `tools/linux-venv/`
3. **Auto-download standalone Python 3.12** — if no system 3.10+, download to `tools/python-linux/` then create venv

On first run, it also configures udev rules and the dialout group (sudo only needed once). Subsequent runs require no privileges.

## Files

| File | Purpose |
|------|---------|
| `scripts/time_sync_server.py` | NTP sync main program |
| `scripts/find_port.py` | Auto-detect serial port by VID-PID 18D1:903B |
| `scripts/serial_common.py` | NTP protocol constants + NTP timestamp clock (reads system time directly) |
| `linux_run_time_sync.sh` | Linux launcher (auto-select Python + setup environment) |
| `tools/linux-venv/` | Linux venv (auto-generated, not in repo) |
| `tools/python-linux/` | Linux standalone Python 3.12 (auto-downloaded when system lacks 3.10+) |

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `No VR serial port found` | Ego VR app not running (the app enables ACM itself; this server never sets it, to avoid breaking non-Ego devices) | Start/restart the Ego VR app, reconnect USB |
| `Open COMx failed` (Windows) | Port in use | Close other serial tools |
| `adb devices` empty (Linux) | udev rules missing `4ee2` PID | Re-run `./linux_run_time_sync.sh` |
| `Permission denied: /dev/ttyACM0` (Linux) | Not in dialout group | Script adds it automatically; re-login or `newgrp dialout` |
