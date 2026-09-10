# XR Camera Control — Installation & Usage

## Directory Contents

```
release/
├── app-release-vr.apk              — VR APK (auto-copied by Gradle build)
├── app-release-phone-android.apk   — Phone APK (placed manually)
├── install_vr.bat                  — One-click VR install (Windows)
├── install_vr.sh                   — One-click VR install (Linux)
├── Copy_Dataset_Here.bat           — Pull dataset from VR
├── EgoVision_Setup_v*.exe          — Windows PC installer (EgoVision)
├── EgoVision_v*.dmg                — macOS PC installer (EgoVision)
├── time-sync-server/               — Serial time sync service (Python, CDC-ACM, driver-free; built into EgoVision on Windows/macOS, this directory targets Linux)
│   ├── linux_run_time_sync.sh      — Linux launcher (auto-select Python + setup environment)
│   ├── scripts/                    — NTP sync scripts
│   │   ├── time_sync_server.py     — NTP time sync server
│   │   ├── find_port.py            — Auto-detect serial port by VID-PID (COM / ttyACM)
│   │   └── serial_common.py        — NTP protocol constants + NTP timestamp clock (reads system time directly)
│   └── tools/                      — Runtime environments (adb installer, etc.)
├── DatasetServer/                  — Dataset chunk upload service (Python, HTTP multipart)
│   ├── server.py                   — HTTP upload server (:9000)
│   └── start_server.sh             — Linux one-click launch
├── sdk/android/                    — Native Android SDK artifacts (aar / demo apk / source zip / README)
├── analyze/                        — Dataset analysis scripts
├── dataset/                        — Datasets pulled by Copy_Dataset_Here.bat
└── EGO_Docs.html                   — Documentation center (bilingual)
```

## System Requirements

- VR Headset
- Android Phone
- PC: Windows 10/11 x64, macOS, or Linux x86_64

## 1. Install VR App

1. Connect VR headset to PC via USB
2. Installation:

   **Windows** — double-click `install_vr.bat`:

   **Linux** — run in terminal:
   ```sh
   chmod +x install_vr.sh
   ./install_vr.sh
   ```

   - Press Enter or type `n` → keep-data upgrade install
   - Type `y` → uninstall (clear all app data) + fresh install
3. The script auto-completes: install → launch app → configure auto-start → reboot after 5s countdown

> ⚠️ Choosing uninstall will **clear ALL VR app data**. Use with caution. Only use this when keep-data install fails.

## 2. Install Phone App

1. Transfer `app-release-phone-android.apk` to the phone
2. Open and install on the phone, grant all permissions
3. Launch the app after installation

## 3. Install PC App (Windows / macOS)

**Windows** — run `EgoVision_Setup_v*.exe` and follow the installer wizard

**macOS** — open `EgoVision_v*.dmg` and drag EgoVision.app into Applications; if macOS reports the developer cannot be verified on first launch, right-click the app → "Open"

Then launch EgoVision and connect the VR headset via USB.

The PC app has the **time sync service** and **dataset upload service** built in — no separate launcher scripts required. Both services are controlled by in-app toggles (home page / "More" page) and are **off by default — enable them manually once on first use**; once enabled they restore automatically on subsequent launches (Linux PC has no built-in services — see the corresponding sections below).

## 4. Time Sync

NTP time synchronization with the VR headset via CDC-ACM serial port. **Driver-free** —
Windows built-in `usbser.sys` automatically recognizes CDC-ACM devices as COM ports;
Linux kernel `cdc_acm` module exposes them as `/dev/ttyACM*`. No Zadig/WinUSB/admin privileges required.

### Windows / macOS

The NTP time sync service is integrated into the EgoVision PC app — no separate launcher script required: enable the "Time Sync" toggle in the app (off by default), and it auto-responds to VR time requests over USB. Runtime logs are viewable inside the app.

### Linux

```sh
cd time-sync-server
chmod +x linux_run_time_sync.sh
./linux_run_time_sync.sh    # start (Ctrl+C to exit)
```

Normal output of the standalone Python service:

```
=== Serial Time Sync Server (pyserial) ===
VR detected.
Opening COM3 @ 2000000 baud...
Device connected. Waiting for time requests...
#   1 | 50 reqs | 17050ms batch | gap 0.0- 2.3ms | RTT=2.345ms | offset=  +15.678ms
```

The script auto-selects a Python runtime with this priority:
1. **`tools/linux-venv/`** — use directly if exists
2. **System Python 3.10+** — run directly if pyserial available; otherwise auto-create `tools/linux-venv/`
3. **Auto-download standalone Python 3.12** — if no system 3.10+, download to `tools/python-linux/` then create venv

On first run, it also configures udev rules and the dialout group (sudo only needed once). Subsequent runs require no privileges.

### Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `No VR serial port found` | ACM not enabled on VR | `adb shell setprop sys.usb.config "mtp,acm,adb"`, reconnect USB |
| `Open COMx failed` (Windows) | Port in use | Close other serial tools (Putty, serial debuggers) |
| `adb devices` empty (Linux) | udev rules missing `4ee2` PID | Re-run `./linux_run_time_sync.sh` |
| `Permission denied: /dev/ttyACM0` (Linux) | Not in dialout group | Script adds it automatically; re-login or `newgrp dialout` |

## 5. Dataset Upload

After recording, VR uploads chunked data to the PC-side DatasetServer via HTTP multipart.

### Start the Server

**Windows / macOS** — the dataset receive service is integrated into the EgoVision PC app, no separate launcher script required (controlled by an in-app toggle, off by default — enable it manually on first use)

**Linux** — `./DatasetServer/start_server.sh --port 9000`; the terminal prints all accessible URLs on startup.

### Phone Configuration

1. After BLE connection with VR, tap the "More" button
2. **Chunk Duration**: Set recording chunk duration (5/10/30/60 min), synced to VR via BLE
3. **Upload URL**: Enter the PC DatasetServer URL (e.g., `http://192.168.1.100:9000`), synced to VR via BLE
4. After recording stops, `ChunkUploadManager` automatically uploads chunk subdirectories to DatasetServer via HTTP multipart

For the DatasetServer HTTP API details, see `DatasetServer/README_EN.md`.

## 6. Usage Flow

### Device Connection

1. Open the phone app, configure WiFi credentials (SSID and password)
2. Tap scan, select the VR device for BLE connection
3. After connection, VR auto-connects to WiFi; the app shows the video preview

### Camera Control

- Select camera group in preview UI (RGB / Tracking / Ctrl)
- Tap fullscreen button for immersive preview
