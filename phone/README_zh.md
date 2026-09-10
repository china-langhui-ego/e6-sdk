# XR Camera Control 安装使用说明

## 目录内容

```
release/
├── app-release-vr.apk              — VR 端 APK（Gradle build 自动拷贝）
├── app-release-phone-android.apk   — 手机端 APK（手动放置）
├── install_vr.bat                  — VR 一键安装（Windows）
├── install_vr.sh                   — VR 一键安装（Linux）
├── Copy_Dataset_Here.bat           — 从 VR 拉取 dataset 数据
├── EgoVision_Setup_v*.exe          — Windows PC 端安装包（EgoVision）
├── EgoVision_v*.dmg                — macOS PC 端安装包（EgoVision）
├── time-sync-server/               — 串口时间同步服务（Python，CDC-ACM，免驱动；Windows/macOS 已内置 EgoVision，本目录面向 Linux）
│   ├── linux_run_time_sync.sh      — Linux 启动（自动选 Python + 配置环境）
│   ├── scripts/                    — NTP 同步脚本
│   │   ├── time_sync_server.py     — NTP 时间同步服务器
│   │   ├── find_port.py            — 串口自动发现（按 VID-PID 匹配 COM/ttyACM 口）
│   │   └── serial_common.py        — NTP 协议常量 + NTP 时间戳时钟（直读系统时间）
│   └── tools/                      — 运行环境（adb 安装包等）
├── DatasetServer/                  — 数据集分片上传服务（Python，HTTP multipart）
│   ├── server.py                   — HTTP 上传服务器（:9000）
│   └── start_server.sh             — Linux 一键启动
├── sdk/android/                    — 原生 Android SDK 产物（aar / demo apk / 源码 zip / README）
├── analyze/                        — 数据集分析脚本
├── dataset/                        — Copy_Dataset_Here.bat 拉取的数据集
└── EGO_Docs.html                   — 文档中心（中英双语）
```

## 系统要求

- VR 头显
- Android 手机
- PC：Windows 10/11 x64、macOS 或 Linux x86_64

## 一、安装 VR 端

1. VR 头显通过 USB 连接 PC
2. 安装方式：

   **Windows** — 双击 `install_vr.bat`:

   **Linux** — 终端运行：
   ```sh
   chmod +x install_vr.sh
   ./install_vr.sh
   ```

   - 直接回车或输入 `n` → 保留数据升级安装
   - 输入 `y` → 卸载旧版本（清除所有应用数据）全新安装
3. 脚本自动完成：安装 → 启动应用 → 配置开机自启 → 5s 倒计时后重启

> ⚠️ 选择卸载会**清除 VR 端所有应用数据**，请慎重。一般仅在保留数据安装失败时使用。

## 二、安装手机端

1. 将 `app-release-phone-android.apk` 传输到手机
2. 在手机上打开安装，授予所有权限
3. 安装完成后打开应用

## 三、安装 PC 端（Windows / macOS）

**Windows** — 运行 `EgoVision_Setup_v*.exe`，按向导完成安装

**macOS** — 打开 `EgoVision_v*.dmg`，将 EgoVision.app 拖入「应用程序」；首次启动如提示"无法验证开发者"，右键 App →「打开」即可

安装后启动 EgoVision，通过 USB 连接 VR 头显即可使用。

PC 端已内置**时间同步服务**与**数据集上传服务**，无需单独启动脚本。两项服务由 App 内开关控制（首页/「更多」页），**默认关闭，首次使用需手动打开一次**；打开后下次启动自动恢复（Linux PC 无内置服务，见下文对应章节）。

## 四、时间同步

通过 CDC-ACM 串口与 VR 头显进行 NTP 时间同步。**免驱动**——
Windows 自带 `usbser.sys` 自动识别 CDC-ACM 设备为 COM 口，
Linux 内核 `cdc_acm` 模块枚举为 `/dev/ttyACM*`，均无需 Zadig/WinUSB/管理员权限。

### Windows / macOS

NTP 时间同步服务已集成进 EgoVision PC 端，无需单独启动脚本：在 App 内打开「时间同步」开关（默认关闭），连接 USB 后即自动响应 VR 的时间请求，运行日志在 App 内查看。

### Linux

```sh
cd time-sync-server
chmod +x linux_run_time_sync.sh
./linux_run_time_sync.sh    # 启动（Ctrl+C 退出）
```

独立 Python 服务正常输出：

```
=== Serial Time Sync Server (pyserial) ===
VR detected.
Opening COM3 @ 2000000 baud...
Device connected. Waiting for time requests...
#   1 | 50 reqs | 17050ms batch | gap 0.0- 2.3ms | RTT=2.345ms | offset=  +15.678ms
```

脚本自动选择 Python 运行环境，优先级从高到低：
1. **`tools/linux-venv/`** — 已存在则直接用
2. **系统 Python 3.10+** — 有 pyserial 直接跑；没有则自动创建 `tools/linux-venv/`
3. **自动下载独立 Python 3.12** — 系统没有 3.10+ 时，下载到 `tools/python-linux/`，再创建 venv

首次运行还会自动配置 udev 规则和 dialout 组（仅首次需要 sudo），后续运行无需权限。

### 故障排查

| 现象 | 原因 | 解决方法 |
|------|------|---------|
| `No VR serial port found` | VR 未开 acm 或驱动未加载 | `adb shell setprop sys.usb.config "mtp,acm,adb"`，重连 USB |
| `Open COMx failed` (Windows) | 端口被占用 | 关闭其他串口工具（如 Putty、串口调试助手） |
| `adb devices` 空 (Linux) | udev 规则缺 `4ee2` PID | 重跑 `./linux_run_time_sync.sh` |
| `Permission denied: /dev/ttyACM0` (Linux) | 不在 dialout 组 | 脚本已自动添加，重新登录或 `newgrp dialout` |

## 五、数据集上传

VR 端录制后通过 HTTP multipart 将分片数据上传到 PC 端 DatasetServer。

### 启动服务

**Windows / macOS** — 数据集接收服务已集成进 EgoVision PC 端，无需单独启动脚本（App 内开关控制，默认关闭，首次使用需手动打开）

**Linux** — `./DatasetServer/start_server.sh --port 9000`，启动后终端会打印本机所有可访问的 URL。

### 手机端配置

1. 手机端与 VR 建立 BLE 连接后，点击"更多"按钮
2. **切片时长**：设置录制分片时长（5/10/30/60 分钟），通过 BLE 同步到 VR 端
3. **分片上传地址**：填入 PC 端 DatasetServer 的 URL（如 `http://192.168.1.100:9000`），通过 BLE 同步到 VR 端
4. VR 端录制停止后，ChunkUploadManager 自动将分片子目录通过 HTTP multipart 上传到 DatasetServer

DatasetServer 的 HTTP API 细节见 `DatasetServer/README.md`。

## 六、使用流程

### 连接设备

1. 手机端打开 APP，配置 WiFi 信息（SSID 和密码）
2. 点击扫描，选择 VR 设备进行 BLE 连接
3. 连接成功后 VR 自动连接 WiFi，APP 显示视频预览画面

### 摄像头控制

- 在预览界面选择摄像头组（RGB / Tracking / Ctrl）
- 点击全屏按钮可沉浸式预览
