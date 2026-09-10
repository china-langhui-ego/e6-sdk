**English** · [简体中文](README.md) ｜ [E2 Binocular Head-Mounted SDK](https://github.com/china-langhui-ego/e2-sdk)

# E6 XR Headset (EGO Capture) · Developer Resources

E6 is an Android + OpenXR **first-person multimodal data-capture headset**: through the camera extension it collects RGB / grayscale cameras, IMU, audio, 6DOF head pose, controller poses and a hand skeleton, then hardware-encodes and synchronously writes them into a timestamped dataset (directory-based MP4 or single-file MCAP).

- SDK version: **1.8.2** (see [`sdk/VERSION`](sdk/VERSION)), changes in [`sdk/CHANGELOG.md`](sdk/CHANGELOG.md)
- Application id: `com.ssnwt.helloxr`, activity: `com.ssnwt.helloxr.VrNativeActivity`
- ★ Authoritative technical reference: [`sdk/Readme_en.md`](sdk/Readme_en.md) · [中文](sdk/Readme.md) (529 lines — coordinate-frame derivation, timestamp scheme, encoding pipeline, API reference)
- MCAP single-file dataset format: [`sdk/mcap-dataset-format.md`](sdk/mcap-dataset-format.md)

---

## Repository layout

```
e6/
├── README.md / README_en.md
├── sdk/                                Full xrcamerademo Android project (v1.8.2)
│   ├── Readme_en.md / Readme.md           ★ EGO SDK reference (authoritative, 529 lines)
│   ├── CHANGELOG.md · VERSION              Version and change log
│   ├── mcap-dataset-format.md              MCAP dataset format specification
│   ├── settings.gradle · build.gradle · gradle/ · gradlew    Gradle 8.7 project
│   ├── app/src/main/cpp/                   Core C++: recording orchestration, encoding, dataset writing
│   │   ├── main.cpp                           Main logic / camera callbacks / recording / cross-stream alignment
│   │   ├── DatasetRecorder.{h,cpp}            Dataset recording coordinator
│   │   ├── RecordingGatekeeper / RecordingEncodeWorker   Multi-source synchronized start/stop, workerized encoding
│   │   ├── CameraEncoder / AudioEncoder / FMP4Writer      MediaCodec Surface encoding + fMP4
│   │   ├── ImuPoseCollector / ControllerPoseSaver / HandOverlayRenderer
│   │   ├── SharedTexture / TextureUtils / AnnexBConverter  AHardwareBuffer → GL → encoder
│   │   ├── sxr_camera.h · sxr_common.h                    Camera API (dynamically loaded via dlopen)
│   │   └── mcap/ + thirdparty/mcap/                       MCAP output (CDR + jsonschema)
│   ├── app/src/main/java/ · res/ · assets/    VR activity and resources
│   ├── 3rdlibs/                              libcamera-release.aar, libopenxr_loader.aar
│   ├── External/                             OpenXR-SDK (headers + arm64-v8a loader), glm, tinyobjloader
│   ├── dataset_analysis/                     Timestamp alignment / frame convention / GOP / schema checks
│   ├── camera_analysis.py                    Intrinsics & extrinsics validation (emits an HTML report)
│   └── visualize_imu_camera_positions.py     IMU and camera trajectory visualization
├── phone/                              Phone / PC companion tools (scripts + docs + mobile SDKs)
│   ├── README_en.md / README_zh.md          Installation and usage across the three platforms
│   ├── EGO_Docs.html                         Documentation hub (bilingual)
│   ├── install_vr.sh · install_vr.bat        One-click install of the VR app
│   ├── Copy_Dataset_Here.bat                 Pull datasets to the local machine
│   ├── DatasetServer/                        Dataset HTTP upload service (server.py, :9000)
│   ├── time-sync-server/                     Serial NTP time sync (PC ↔ headset, driver-free CDC-ACM)
│   └── sdk/
│       ├── android/                             xr-camera-control-v1.4.aar + sample sources/APK
│       └── ios/                                 xr-camera-ios-v1.4.0.xcframework + sample sources
├── rom/                                Flash guides and scripts (**images not in the repo**)
│   ├── flash-guide-en.txt / flash-guide-zh.txt / flash-guide-macos.txt
│   └── 920_fastboot_all.bat                     Full fastboot flashing script
└── docs/                               EGO_SDK_Readme_zh.html · EGO_SDK_usage_zh.html · product deck
```

---

## Quick start

### 1. Install

```bash
# after downloading the bundle from the cloud "Phone APP & SDK" folder
adb install -r app-release-vr.apk        # headset-side capture app
./phone/install_vr.sh                    # or: one-click install (includes permissions and config)
```

The phone remote-control app and the PC client EgoVision (Windows `.exe` / macOS `.dmg` / iOS `.ipa`) exceed the size limits and are in the cloud "Phone APP & SDK" category.

### 2. System-property switches

```bash
adb shell setprop persist.xr.project_hand 1        # project hand skeleton into encoded video (reboot to apply)
adb shell setprop persist.xr.project_controller 1  # project controller coordinate axes (reboot to apply)
adb shell setprop persist.sxr.cam.rgb.fps 60       # RGB frame rate 30/60 (restart the app to apply)
adb shell setprop persist.xr.dataset_type mcap     # dataset format mp4/mcap (applies to next recording)
```

### 3. Record

```bash
adb shell am broadcast -a com.ssnwt.helloxr.START_RECORDING
adb shell am broadcast -a com.ssnwt.helloxr.STOP_RECORDING
adb shell am broadcast -a com.ssnwt.helloxr.SAVE_IMAGE      # screenshot (all cameras)

# Buttons: VOLUME_UP = screenshot; Right A / DPAD_CENTER = toggle recording
adb pull /sdcard/Android/data/com.ssnwt.helloxr/files/dataset/ ./dataset/
adb logcat | grep -E "HelloXr|sxrcam|DatasetRecorder|Encoder"
```

### 4. Build from sources

Android Studio + NDK (`compileSdk 34` / `minSdk 30` / `targetSdk 34`, `arm64-v8a` only):

```bash
cd sdk
./gradlew :app:assembleDebug      # Gradle 8.7 (wrapper included)
```

### 5. Time sync and data upload

```bash
# PC ↔ headset serial NTP time sync (built into EgoVision on Windows/macOS; use the script on Linux)
cd phone/time-sync-server && ./linux_run_time_sync.sh

# Dataset HTTP upload service (enter http://<PC_IP>:9000 on the phone/headset settings page)
cd phone/DatasetServer && ./start_server.sh --port 9000
```

---

## Flashing (ROM)

The image `VQ920-La1.5-CS40-201-user-202608171956_fastboot.tgz` (2.1 GB, containing `super.img` at 3.9 GB and more) is **not in the repository** — see [Cloud downloads](#cloud-downloads).

1. Read the guide for your platform: [`rom/flash-guide-en.txt`](rom/flash-guide-en.txt) / [`flash-guide-zh.txt`](rom/flash-guide-zh.txt) / [`flash-guide-macos.txt`](rom/flash-guide-macos.txt)
2. Windows: install the USB driver (`adb-setup` in the cloud "Debug environment"), keep the battery above 40 %, enter fastboot and run `rom/920_fastboot_all.bat`
3. macOS: use `platform-tools` (cloud "Debug environment") and set `PATH`
4. The flash is complete when the terminal prints `fastboot success`

---

## Dataset

### Directory-based (default, `mp4`)

```
/sdcard/Android/data/com.ssnwt.helloxr/files/
├── dataset/<YYYYMMDD_HHMMSS>/
│   ├── rgb.mp4 / rgb_metainfo.csv             # RGB SBS (2W×H, H.265, 30fps) + per-frame metadata
│   ├── tracking.mp4 / tracking_metainfo.csv   # Tracking grayscale (W×H, H.265, 60fps)
│   ├── ctrl.mp4 / ctrl_metainfo.csv           # Ctrl grayscale (W×H, H.265, 60fps)
│   ├── audio.m4a / audio_metainfo.csv         # AAC (44.1 kHz mono 96 kbps)
│   ├── accel.csv / gyro.csv                   # IMU
│   ├── head_pose.csv                          # 6DOF head pose
│   ├── hand_tracking.csv                      # hand joints (a single empty row when no hand is active)
│   ├── controller_poses.csv                   # controller poses (a single empty row when inactive)
│   ├── imu_calibration.json                   # IMU calibration sidecar
│   └── camera_params_{rgb,tracking,ctrl}.json # intrinsics + extrinsics
└── images/                                    # screenshots rgb_*.png / tracking_*.png / ctrl_*.png
```

The single-file **MCAP** format (all channels plus attachments embedded in one file) is specified in [`sdk/mcap-dataset-format.md`](sdk/mcap-dataset-format.md).

**Format version**: the first field `version` in `camera_params_*.json` / `imu_calibration.json` shares its source with `datasetFormatVersion` in the mcap `session` metadata. `version = 2` corresponds to tag ≥ 1.8.2; extrinsics use **convention 1** since tag 1.7.0 (tag ≤ 1.6.0 emits the deprecated conjugated convention 0).

### Coordinate frames and distortion (key points)

- **Both the API and the dataset use the OpenXR Body frame: X right, Y up, Z backward.** The SDK performs no conversion.
- To OpenCV Body (X right, Y down, Z forward): positions `(x,y,z) → (x,-y,-z)`, quaternions `(qx,qy,qz,qw) → (qx,-qy,-qz,qw)`, i.e. `F = diag([1,-1,-1])`.
- Under convention 1 the `camera_params` camera side is already the **stored-image optical frame** (X image-right / Y image-down / Z optical forward; the 90° roll of the portrait-mounted tracking camera is folded into `R`), so only the Body frame needs flipping for OpenCV.
- Intrinsics are the standard OpenCV K matrix; distortion is **Kannala-Brandt fisheye** (first 4 coefficients) — undistort with `cv2.fisheye`.
- Pitch about X, Yaw about Y, Roll about Z; units are metres, quaternion order is `(x, y, z, w)`.

Derivation, the complete recipe and validation scripts: [`sdk/Readme_en.md` §3](sdk/Readme_en.md). Quantitative checks:

```bash
python3 sdk/dataset_analysis/analyze_alignment.py <dataset_dir>
python3 sdk/camera_analysis.py <dataset_dir>                      # emits an HTML report
python3 sdk/visualize_imu_camera_positions.py <dataset_dir> [--opencv]
```

### Timestamps

A single `BOOTTIME → REALTIME` offset is sampled once at recording start and all streams are normalized to absolute UTC nanoseconds; pose / hand / controller samples are re-stamped at the RGB **mid-exposure** instant with `xrLocateSpace(camXrTime)`, giving frame-accurate alignment. Unit conversions: camera frames in ns, MediaCodec in µs, OpenXR XrTime in ns (`us = ns/1000`).

### Encoding pipeline

Hardware **Surface-mode zero-copy**: `AHardwareBuffer×2 → GL_TEXTURE_EXTERNAL_OES →` per-eye FBOs stitched to **SBS** `→ encoder surface → eglPresentationTimeANDROID → MediaCodec`. Each camera group gets its own EGL context. `FMP4Writer` emits `ftyp + moov` up front and one `styp+moof+mdat` fragment per sample — **crash-safe**; video PTS is zero-based and HEVC runs with `max-bframes=0` (I/P only, decode order == presentation order).

| Stream | Codec | Resolution | Frame rate | Bitrate |
| --- | --- | --- | --- | --- |
| RGB (SBS) | H.265 (switchable to H.264) | 2W×H | 30 (configurable to 60) | 8 Mbps |
| Tracking / Ctrl | H.265 | W×H | 60 | 4 Mbps |
| Audio | AAC-LC | — | 44.1 kHz mono | 96 kbps |

Volume reference for a 10-second capture: `rgb.mp4` ≈ 10 MB · `tracking/ctrl.mp4` ≈ 5 MB · `audio.m4a` ≈ 120 KB · `accel/gyro.csv` ≈ 300 KB · `head_pose.csv` ≈ 50 KB · `hand_tracking.csv` ≈ 100 KB.

---

## Cloud downloads

| Category | Google Drive | Baidu Netdisk (access code) |
| --- | --- | --- |
| Device SDK (source snapshot zip) | [Drive](https://drive.google.com/drive/folders/16Y6iROamaWflcctZbijl6fWvWbhRnvXG?usp=drive_link) | [pan.baidu.com/s/1aQHjmbzBTfeB0tTe3KVScw](https://pan.baidu.com/s/1aQHjmbzBTfeB0tTe3KVScw?pwd=vm4d) `vm4d` |
| Headset ROM (2.1 GB fastboot bundle) | [Drive](https://drive.google.com/drive/folders/1qc1QkXSPuALmLYsMfFo5n_9fOH7bzMEM?usp=drive_link) | [pan.baidu.com/s/1S9xzCtdoffnLpoaHuRrLFg](https://pan.baidu.com/s/1S9xzCtdoffnLpoaHuRrLFg?pwd=q91z) `q91z` |
| Phone APP & SDK (EgoVision / apk / ipa) | [Drive](https://drive.google.com/drive/folders/1F9Cys88ZDyv6Tqah4J8EYZKN1XBa046U?usp=drive_link) | [pan.baidu.com/s/1zcub_0RCqYZSA1F1dh8FGg](https://pan.baidu.com/s/1zcub_0RCqYZSA1F1dh8FGg?pwd=2gs2) `2gs2` |
| Debug environment (scrcpy / Zadig / platform-tools / VLC…) | [Drive](https://drive.google.com/drive/folders/1qKy0RMa08XoyqgrehR5dEdEn-R6hGwSI?usp=drive_link) | [pan.baidu.com/s/1nBTr6o7zQmv2axhPCTngoQ](https://pan.baidu.com/s/1nBTr6o7zQmv2axhPCTngoQ) `evwe` |
| User guide (video) | [Drive](https://drive.google.com/file/d/1n0XaW1L7HYTJisRWn9oZaAzrPmSq2yVI/view?usp=drive_link) | [pan.baidu.com/s/1WV1ErmWRrqUeAb5dMU3WRQ](https://pan.baidu.com/s/1WV1ErmWRrqUeAb5dMU3WRQ?pwd=mqng) `mqng` |
| Sample videos / datasets | [Drive](https://drive.google.com/drive/folders/1Hk8FNgSJKZ1-HzIP3Ra6NpytXkmLOzWB?usp=drive_link) | [pan.baidu.com/s/1VXc7_XKXn4wXcP1Z7Hv0LA](https://pan.baidu.com/s/1VXc7_XKXn4wXcP1Z7Hv0LA?pwd=rfd7) `rfd7` |

---

## License

This project is licensed under the [MIT License](LICENSE).

Third-party components (for example, open-source libraries under `sdk/External/`) remain under their own original licenses; see the notice file in each directory.
