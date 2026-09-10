[English](README_en.md) · **简体中文** ｜ [E2 双目数采头环 SDK](https://github.com/china-langhui-ego/e2-sdk)

# E6 XR 头显（EGO 数采）· 开发者资料

E6 是基于 Android + OpenXR 的**第一视角多模态数据采集头显**：通过相机扩展采集 RGB / 灰度相机、IMU、音频、6DOF 头部位姿、手柄位姿与 21 点手部骨骼，硬件编码后同步落盘为带时间戳的数据集（目录型 MP4 或单文件 MCAP）。

- SDK 版本：**1.8.2**（见 [`sdk/VERSION`](sdk/VERSION)），变更记录见 [`sdk/CHANGELOG.md`](sdk/CHANGELOG.md)
- 应用包名：`com.ssnwt.helloxr`，Activity：`com.ssnwt.helloxr.VrNativeActivity`
- ★ 权威技术文档：[`sdk/Readme.md`](sdk/Readme.md) · [English](sdk/Readme_en.md)（529 行，含坐标系推导、时间戳体系、编码管线、API 参考）
- MCAP 单文件数据集格式：[`sdk/mcap-dataset-format.md`](sdk/mcap-dataset-format.md)

---

## 目录结构

```
e6/
├── README.md / README_en.md
├── sdk/                                xrcamerademo 完整 Android 工程（v1.8.2）
│   ├── Readme.md / Readme_en.md           ★ EGO SDK 使用说明（权威，529 行）
│   ├── CHANGELOG.md · VERSION              版本与变更记录
│   ├── mcap-dataset-format.md              MCAP 数据集格式规范
│   ├── settings.gradle · build.gradle · gradle/ · gradlew    Gradle 8.7 工程
│   ├── app/src/main/cpp/                   核心 C++：录制编排、编码、数据集落盘
│   │   ├── main.cpp                           主逻辑 / 相机回调 / 录制编排 / 流间对齐
│   │   ├── DatasetRecorder.{h,cpp}            数据集录制协调器
│   │   ├── RecordingGatekeeper / RecordingEncodeWorker   多源同步起停、编码 worker 化
│   │   ├── CameraEncoder / AudioEncoder / FMP4Writer      MediaCodec Surface 编码 + fMP4
│   │   ├── ImuPoseCollector / ControllerPoseSaver / HandOverlayRenderer
│   │   ├── SharedTexture / TextureUtils / AnnexBConverter  AHardwareBuffer → GL → 编码
│   │   ├── sxr_camera.h · sxr_common.h                    相机 API（dlopen 动态加载）
│   │   └── mcap/ + thirdparty/mcap/                       MCAP 写出（CDR + jsonschema）
│   ├── app/src/main/java/ · res/ · assets/    VR Activity 与资源
│   ├── 3rdlibs/                              libcamera-release.aar、libopenxr_loader.aar
│   ├── External/                             OpenXR-SDK（头文件 + arm64-v8a loader）、glm、tinyobjloader
│   ├── dataset_analysis/                     时间戳对齐 / 坐标系 / GOP / schema 验证脚本
│   ├── camera_analysis.py                    内外参验证（生成 HTML 报告）
│   └── visualize_imu_camera_positions.py     IMU 与相机轨迹可视化
├── phone/                              手机端 / PC 端配套工具（脚本 + 文档 + 移动 SDK）
│   ├── README_zh.md / README_en.md          三端安装与使用总说明
│   ├── EGO_Docs.html                         文档中心（中英双语）
│   ├── install_vr.sh · install_vr.bat        一键安装 VR 应用
│   ├── Copy_Dataset_Here.bat                 数据集拉取到本地
│   ├── DatasetServer/                        数据集 HTTP 上传服务（server.py，:9000）
│   ├── time-sync-server/                     串口 NTP 时间同步（PC ↔ 头环，免驱 CDC-ACM）
│   └── sdk/
│       ├── android/                             xr-camera-control-v1.4.aar + 示例源码/APK
│       └── ios/                                 xr-camera-ios-v1.4.0.xcframework + 示例源码
├── rom/                                刷机说明与脚本（**镜像不入库**）
│   ├── flash-guide-zh.txt / flash-guide-en.txt / flash-guide-macos.txt
│   └── 920_fastboot_all.bat                     fastboot 全量刷写脚本
└── docs/                               EGO_SDK_Readme_zh.html · EGO_SDK_usage_zh.html · 产品介绍 PPT
```

---

## 快速开始

### 1. 安装

```bash
# 从云端「手机端APP与SDK」下载整包后
adb install -r app-release-vr.apk        # 头显端采集应用
./phone/install_vr.sh                    # 或：一键安装（含权限与配置）
```

手机端遥控 APP、PC 端 EgoVision（Windows `.exe` / macOS `.dmg` / iOS `.ipa`）体积超限，均在云端「手机端APP与SDK」分类中。

### 2. 系统属性开关

```bash
adb shell setprop persist.xr.project_hand 1        # 手势骨骼投影到编码视频（重启生效）
adb shell setprop persist.xr.project_controller 1  # 手柄坐标系投影（重启生效）
adb shell setprop persist.sxr.cam.rgb.fps 60       # RGB 帧率 30/60（重启应用生效）
adb shell setprop persist.xr.dataset_type mcap     # 数据集格式 mp4/mcap（下次录制生效）
```

### 3. 录制

```bash
adb shell am broadcast -a com.ssnwt.helloxr.START_RECORDING
adb shell am broadcast -a com.ssnwt.helloxr.STOP_RECORDING
adb shell am broadcast -a com.ssnwt.helloxr.SAVE_IMAGE      # 截图（所有相机）

# 按键：VOLUME_UP = 截图；Right A / DPAD_CENTER = 切换录制
adb pull /sdcard/Android/data/com.ssnwt.helloxr/files/dataset/ ./dataset/
adb logcat | grep -E "HelloXr|sxrcam|DatasetRecorder|Encoder"
```

### 4. 从源码构建

Android Studio + NDK（`compileSdk 34` / `minSdk 30` / `targetSdk 34`，仅 `arm64-v8a`）：

```bash
cd sdk
./gradlew :app:assembleDebug      # Gradle 8.7（wrapper 已内置）
```

### 5. 时间同步与数据回传

```bash
# PC ↔ 头环串口 NTP 时间同步（Windows/macOS 已内置于 EgoVision；Linux 用脚本）
cd phone/time-sync-server && ./linux_run_time_sync.sh

# 数据集 HTTP 上传服务（手机/头显在设置页填 http://<PC_IP>:9000）
cd phone/DatasetServer && ./start_server.sh --port 9000
```

---

## 刷机（ROM）

镜像 `VQ920-La1.5-CS40-201-user-202608171956_fastboot.tgz`（2.1 GB，含 `super.img` 3.9 GB 等）**不在仓库内**，见[云端下载](#云端下载)。

1. 阅读对应平台的说明：[`rom/flash-guide-zh.txt`](rom/flash-guide-zh.txt) / [`flash-guide-en.txt`](rom/flash-guide-en.txt) / [`flash-guide-macos.txt`](rom/flash-guide-macos.txt)
2. Windows：安装 USB 驱动（云端「其他调试环境」内 `adb-setup`），电量 > 40%，进入 fastboot 后运行 `rom/920_fastboot_all.bat`
3. macOS：使用 `platform-tools`（云端「其他调试环境」）并配置 `PATH`
4. 终端出现 `fastboot success` 即完成

---

## 数据集

### 目录型（默认 `mp4`）

```
/sdcard/Android/data/com.ssnwt.helloxr/files/
├── dataset/<YYYYMMDD_HHMMSS>/
│   ├── rgb.mp4 / rgb_metainfo.csv             # RGB SBS（2W×H, H.265, 30fps）+ 逐帧元数据
│   ├── tracking.mp4 / tracking_metainfo.csv   # Tracking 灰度（W×H, H.265, 60fps）
│   ├── ctrl.mp4 / ctrl_metainfo.csv           # Ctrl 灰度（W×H, H.265, 60fps）
│   ├── audio.m4a / audio_metainfo.csv         # AAC（44.1kHz mono 96kbps）
│   ├── accel.csv / gyro.csv                   # IMU
│   ├── head_pose.csv                          # 6DOF 头部位姿
│   ├── hand_tracking.csv                      # 手部 21 关键点（无手势时仅首行空行）
│   ├── controller_poses.csv                   # 手柄位姿（无手柄时仅首行空行）
│   ├── imu_calibration.json                   # IMU 标定 sidecar
│   └── camera_params_{rgb,tracking,ctrl}.json # 内参 + 外参
└── images/                                    # 截图 rgb_*.png / tracking_*.png / ctrl_*.png
```

单文件 **MCAP** 格式（全部通道 + 附件嵌入同一文件）见 [`sdk/mcap-dataset-format.md`](sdk/mcap-dataset-format.md)。

**格式版本**：`camera_params_*.json` / `imu_calibration.json` 首字段 `version` 与 mcap `session` metadata 的 `datasetFormatVersion` 同源。`version = 2` 对应 tag ≥ 1.8.2；外参自 tag 1.7.0 起为 **convention 1**（tag ≤ 1.6.0 为已废弃的 convention 0 共轭输出）。

### 坐标系与畸变（要点）

- **API 与数据集都用 OpenXR Body 系：X 右、Y 上、Z 后**，SDK 不做转换。
- 转 OpenCV Body（X 右、Y 下、Z 前）：位置 `(x,y,z) → (x,-y,-z)`，四元数 `(qx,qy,qz,qw) → (qx,-qy,-qz,qw)`，即 `F = diag([1,-1,-1])`。
- convention 1 下 `camera_params` 相机侧已是**存储图像 optical 系**（X 图像右 / Y 图像下 / Z 光轴前，tracking 竖装 90° 滚转已含在 `R` 内），配 OpenCV 只需翻转 Body。
- 内参为 OpenCV 标准 K 矩阵；畸变为 **Kannala-Brandt 鱼眼**（前 4 系数），去畸变用 `cv2.fisheye`。
- Pitch 绕 X、Yaw 绕 Y、Roll 绕 Z；单位米，四元数顺序 `(x, y, z, w)`。

推导、完整配方与验证脚本见 [`sdk/Readme.md` §3](sdk/Readme.md)；量化校验：

```bash
python3 sdk/dataset_analysis/analyze_alignment.py <dataset_dir>
python3 sdk/camera_analysis.py <dataset_dir>                      # 输出 HTML 报告
python3 sdk/visualize_imu_camera_positions.py <dataset_dir> [--opencv]
```

### 时间戳

录制起点一次性采样 `BOOTTIME → REALTIME` 偏移，全部流统一到绝对 UTC 纳秒；pose / hand / controller 在 RGB **mid-exposure** 时刻用 `xrLocateSpace(camXrTime)` 重打戳，实现逐帧对齐。单位换算：相机帧 ns、MediaCodec µs、OpenXR XrTime ns（`us = ns/1000`）。

### 编码管线

硬件 **Surface 模式零拷贝**：`AHardwareBuffer×2 → GL_TEXTURE_EXTERNAL_OES →` 左右眼 FBO 拼 **SBS** `→ encoder surface → eglPresentationTimeANDROID → MediaCodec`。每相机组独立 EGL 上下文。`FMP4Writer` 前置 `ftyp + moov`、每 sample 一个 `styp+moof+mdat` fragment —— **崩溃安全**；视频 PTS 零基、HEVC `max-bframes=0`（仅 I/P，解码序==显示序）。

| 流 | 编码 | 分辨率 | 帧率 | 码率 |
| --- | --- | --- | --- | --- |
| RGB (SBS) | H.265（可切 H.264） | 2W×H | 30（可设 60） | 8 Mbps |
| Tracking / Ctrl | H.265 | W×H | 60 | 4 Mbps |
| Audio | AAC-LC | — | 44.1 kHz mono | 96 kbps |

10 秒数据量参考：`rgb.mp4` ≈ 10 MB · `tracking/ctrl.mp4` ≈ 5 MB · `audio.m4a` ≈ 120 KB · `accel/gyro.csv` ≈ 300 KB · `head_pose.csv` ≈ 50 KB · `hand_tracking.csv` ≈ 100 KB。

---

## 云端下载

| 分类 | Google Drive | 百度网盘（提取码） |
| --- | --- | --- |
| 设备 SDK（源码快照 zip） | [Drive](https://drive.google.com/drive/folders/16Y6iROamaWflcctZbijl6fWvWbhRnvXG?usp=drive_link) | [pan.baidu.com/s/1aQHjmbzBTfeB0tTe3KVScw](https://pan.baidu.com/s/1aQHjmbzBTfeB0tTe3KVScw?pwd=vm4d) `vm4d` |
| 头环 ROM（2.1 GB fastboot 包） | [Drive](https://drive.google.com/drive/folders/1qc1QkXSPuALmLYsMfFo5n_9fOH7bzMEM?usp=drive_link) | [pan.baidu.com/s/1S9xzCtdoffnLpoaHuRrLFg](https://pan.baidu.com/s/1S9xzCtdoffnLpoaHuRrLFg?pwd=q91z) `q91z` |
| 手机端 APP 与 SDK（EgoVision / apk / ipa） | [Drive](https://drive.google.com/drive/folders/1F9Cys88ZDyv6Tqah4J8EYZKN1XBa046U?usp=drive_link) | [pan.baidu.com/s/1zcub_0RCqYZSA1F1dh8FGg](https://pan.baidu.com/s/1zcub_0RCqYZSA1F1dh8FGg?pwd=2gs2) `2gs2` |
| 其他调试环境（scrcpy / Zadig / platform-tools / VLC…） | [Drive](https://drive.google.com/drive/folders/1qKy0RMa08XoyqgrehR5dEdEn-R6hGwSI?usp=drive_link) | [pan.baidu.com/s/1nBTr6o7zQmv2axhPCTngoQ](https://pan.baidu.com/s/1nBTr6o7zQmv2axhPCTngoQ) `evwe` |
| 设备使用说明（视频） | [Drive](https://drive.google.com/file/d/1n0XaW1L7HYTJisRWn9oZaAzrPmSq2yVI/view?usp=drive_link) | [pan.baidu.com/s/1WV1ErmWRrqUeAb5dMU3WRQ](https://pan.baidu.com/s/1WV1ErmWRrqUeAb5dMU3WRQ?pwd=mqng) `mqng` |
| 样例视频 / 样例数据集 | [Drive](https://drive.google.com/drive/folders/1Hk8FNgSJKZ1-HzIP3Ra6NpytXkmLOzWB?usp=drive_link) | [pan.baidu.com/s/1VXc7_XKXn4wXcP1Z7Hv0LA](https://pan.baidu.com/s/1VXc7_XKXn4wXcP1Z7Hv0LA?pwd=rfd7) `rfd7` |

---

## 许可证

本项目采用 [MIT License](LICENSE)。

第三方组件（如 `sdk/External/` 下的开源库）遵循各自的原始许可证，详见对应目录内的说明文件。
