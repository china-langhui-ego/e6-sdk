# XR Camera Control Android SDK

封装 VR 头显的 **BLE 配网 + WebSocket H.265 拉流 + MediaCodec 硬解 + 设备控制** 的原生 Android SDK（Kotlin）。

手机端通过 BLE 给 VR 头显下发 WiFi 凭据，头显联网后回传 IP，手机再经 WebSocket（`:8080`）拉取 H.265 裸流并硬件解码渲染，同时支持摄像头组切换、音量、投影开关、数据集切片/上传、远程电源等控制能力。

- **包名**：`com.ssnwt.xrcamera.control`
- **minSdk**：24（`compileSdk = 36`）
- **对外 API**：仅 `api` 包（`XrCameraClient` + `api.listener.*` + `api.model.*`）；`ble`/`video`/`command`/`internal` 为 internal

## 环境要求

- Kotlin 1.9+ / Java 8
- Android Gradle Plugin 8.x
- `compileSdk = 36`，`minSdk = 24`

## 集成

将 `xr-camera-control-v<x>.aar` 放入模块 `libs/`，并在 `build.gradle.kts` 中加载目录下所有 aar（文件名带版本号，避免硬编码版本）：

```kotlin
dependencies {
    implementation(fileTree("libs") { include("*.aar") })

    // AAR 不自带传递依赖，宿主必须显式提供 SDK 运行期依赖
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
```

## 权限

SDK 需要以下权限（AndroidManifest.xml），其中 dangerous 项需运行时申请：

| 权限 | 用途 | 运行时申请 |
|---|---|---|
| `BLUETOOTH_SCAN` | BLE 扫描（API 31+，声明 `neverForLocation`） | 是（API 31+） |
| `BLUETOOTH_CONNECT` | BLE 连接（API 31+） | 是（API 31+） |
| `ACCESS_FINE_LOCATION` | BLE 扫描后备权限（API 30-） | 是（API 30-） |
| `ACCESS_COARSE_LOCATION` | 配合 FINE 定位（API 30-，满足 lint；API 31+ 仅授予粗略定位时也可扫描） | 是（API 30-） |
| `INTERNET` | WebSocket 拉流 | 否 |
| `ACCESS_NETWORK_STATE` | 网络状态检测 | 否 |

> WebSocket 默认走 `:8080` 明文端口，宿主清单需 `android:usesCleartextTraffic="true"`（或为该端口配置 network security config），否则连接被禁。

运行时申请策略：API 31+ 申请 `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`；API 30- 申请 `ACCESS_FINE_LOCATION`。

## 快速开始

最小可用流程：构造 → 注册回调 → 扫描 → 连接（写 WiFi 凭据）→ 开始预览（渲染到 Surface）。

```kotlin
// 1. 构造（enableDebugLogging=true 时通过 Logcat TAG "XrCamera" 输出诊断日志）
val client = XrCameraClient(context, enableDebugLogging = BuildConfig.DEBUG)

// 2. 注册回调（均主线程派发）
client.setConnectionListener(object : XrConnectionListener {
    override fun onBleStateChanged(state: ConnectionState) { /* 连接状态 */ }
    override fun onVrIpAddress(ip: String) { /* 拿到 VR IP，可开始预览 */ }
    override fun onError(message: String) { /* 错误 */ }
    override fun onWifiStatus(status: WifiStatus) {}
    override fun onDisconnected() {}
})
client.setScanListener(object : XrScanListener {
    override fun onScanFinished(devices: List<XrDevice>) {
        // 3. 从扫描结果选一台设备连接（SSID/密码为 VR 将要连接的 WiFi）
        devices.firstOrNull()?.let { client.connect(it, "YOUR_SSID", "YOUR_PASSWORD") }
    }
    override fun onDeviceFound(device: XrDevice) {}
    override fun onScanError(message: String) {}
})
client.setVideoStatsListener(object : XrVideoStatsListener {
    override fun onPlayingChanged(playing: Boolean) {}
    override fun onFirstFrameDecoded() {}
    override fun onStats(stats: VideoStats) {}
})

// 4. 扫描 VR 设备（默认 5 秒超时）
client.startScan()

// 拿到 IP 后开始预览（surface 来自 SurfaceView/Holder）
// client.startPreview(CameraGroup.RGB, surface)

// 退出
// client.stopPreview()
// client.disconnect()
// client.release()
```

连接成功后 SDK 自动刷新一次设备信息（`refreshDeviceInfo`）。`XrCameraClient` 类加载时会在 Logcat 打印一次 `XrCamera SDK version: <x>`，便于确认集成版本。

## API 参考

### XrCameraClient

构造：`XrCameraClient(context: Context, enableDebugLogging: Boolean = false)`

**可观察状态（StateFlow，主线程）**

| 属性 | 类型 | 说明 |
|---|---|---|
| `connectionState` | `StateFlow<ConnectionState>` | 连接状态 |
| `videoStats` | `StateFlow<VideoStats>` | 视频统计 |
| `deviceInfo` | `StateFlow<DeviceInfo>` | 设备信息 |

**Listener 注册**

| 方法 | 说明 |
|---|---|
| `setConnectionListener(l)` | 连接 / WiFi / IP / 错误 / 断开 |
| `setScanListener(l)` | 扫描结果 |
| `setVideoStatsListener(l)` | 帧率 / 首帧 / 播放状态 |
| `setDeviceListener(l)` | 设备信息 / IMU / USB变更 / 手势重启 / 升级检测 |
| `setTimeSyncListener(l)` | 时间同步状态 |
| `setRecordingListener(l)` | 录制分片保存 / 停止 / 开始 / 错误 |

| 模块 | 方法 | 签名 | E6 | E2 | 说明 |
|---|---|---|:--:|:--:|---|
| 配网 | `startScan` | `(timeoutMs: Long = 5000)` | ✅ | ✅ | BLE 扫描，超时自动停止 |
| | `stopScan` | `()` | ✅ | ✅ | 停止扫描 |
| | `connect` | `(device, ssid, password)` | ✅ | ✅ | 连接并写 WiFi 凭据，共 5 次尝试（1 次初始 + 4 次重试） |
| | `disconnect` | `()` | ✅ | ✅ | 断开 BLE + 视频 |
| 预览/录制 | `startPreview` | `(group, surface, surface1?)` | ✅ | ✅ | 通知 VR 推流，连 WS 收 H.265 流硬解到 Surface；`surface1` 为可选右目 Surface（E2 双路解码用） |
| | `stopPreview` | `()` | ✅ | ✅ | 停止预览 |
| | `switchGroup` | `(group)` | ✅ | ❌ | 切 rgb/tracking/ctrl，自动重建 decoder + 重连 WS |
| | `startRecording` | `()` | ✅ | ✅ | 远程开始录制 |
| | `stopRecording` | `()` | ✅ | ✅ | 远程停止录制 |
| 设备控制 | `increaseVolume` / `decreaseVolume` | `()` | ✅ | ❌ | 音量 ±1（达上下限不发） |
| | `offsetTest` | `()` | ✅ | ❌ | 偏移测试（10 次快速交换，结果经 `TimeSyncState.offsetMs` 回传） |
| | `startSync` | `()` | ✅ | ❌ | 手动开启 NTP 时间同步采集（需 PC 时间同步服务可达） |
| | `cancelSync` | `()` | ✅ | ❌ | 停止正在进行的时间同步采集 |
| | `setProjectionEnabled` | `(enabled)` | ✅ | ❌ | 手势/手柄投影开关 |
| | `getHandEnabled` | `()` | ✅ | ❌ | 查询手势追踪 |
| | `setHandEnabled` | `(enabled)` | ✅ | ❌ | 手势追踪开关（需重启 APP） |
| | `getUsbMode` | `()` | ✅ | ❌ | 查询 USB 功能 |
| | `setUsbMode` | `(enabled)` | ✅ | ❌ | 开关 USB（mtp,adb / none，需重启） |
| | `getLanguage` | `()` | ✅ | ❌ | 查询 VR 系统语言（zh/en，结果到 `DeviceInfo.language`） |
| | `setLanguage` | `(lang)` | ✅ | ❌ | 设置 VR 系统语言（仅 zh/en，非法值抛 `IllegalArgumentException`） |
| 录制配置 | `setChunkDuration` | `(minutes)` | ✅ | ✅ | 数据集切片时长（分） |
| | `setUploadUrl` | `(url)` | ✅ | ✅ | 分片上传 DatasetServer URL |
| 电源/系统 | `shutdown` | `()` | ✅ | ✅ | 远程关机（fire-and-forget） |
| | `reboot` | `()` | ✅ | ✅ | 远程重启设备 |
| | `deleteData` | `()` | ✅ | ✅ | 删除录制数据（fire-and-forget） |
| 重启APP | `restartApp` | `()` | ✅ | ❌ | 重启 VR APP 进程 |
| 状态查询 | `getState` | `()` | ✅ | ✅ | 运行时状态快照 |
| | `refreshDeviceInfo` | `()` | ✅ | ✅ | 批量刷新设备信息（连接后自动调一次） |
| 升级/USB模式 | `checkUpgrade` | `()` | ❌ | ✅ | OTA 升级检测 |
| | `startUpgrade` | `()` | ❌ | ✅ | 开始 OTA 升级 |
| | `switchUsbDeviceMode` | `()` | ❌ | ✅ | 切换 USB Device 模式 |

**生命周期**

| 方法 | 说明 |
|---|---|
| `release()` | 彻底释放：停止扫描、断开 BLE/视频、清空回调。调用后不再使用 |

### Listener 接口（`api.listener.*`，均主线程回调）

| 接口 | 回调 | 说明 |
|---|---|---|
| `XrConnectionListener` | `onBleStateChanged(state)` / `onWifiStatus(status)` / `onVrIpAddress(ip)` / `onError(msg)` / `onDisconnected()` | 连接相关 |
| `XrScanListener` | `onDeviceFound(device)` / `onScanFinished(devices)` / `onScanError(msg)` | 扫描 |
| `XrVideoStatsListener` | `onStats(stats)` / `onPlayingChanged(playing)` / `onFirstFrameDecoded()` | 视频统计 |
| `XrDeviceListener` | `onDeviceInfo(info)` / `onImuData(imu)` / `onUsbModeChanged(enabled)` / `onHandTrackingRestartRequired(enabled)` / `onUpgradeCheckResult(result)` | 设备信息 / IMU / USB变更 / 手势重启 / 升级检测 |
| `XrTimeSyncListener` | `onTimeSyncState(state)` | 时间同步状态 |
| `XrRecordingListener` | `onChunkSaved(path)` / `onRecordingStopped(reason)` / `onRecordingStarted(savePath, recordStartMs)` / `onRecordingError(error)` | 录制分片保存 / 停止 / 开始 / 错误 |

### Model（`api.model.*`）

**ConnectionState（连接状态）**：`DISCONNECTED` / `SCANNING` / `CONNECTING` / `CONNECTED` / `FAILED`

**CameraGroup（摄像头组）**：`RGB`("rgb") / `TRACKING`("tracking") / `CTRL`("ctrl")；`CameraGroup.fromValue(v)` 反查

**WifiStatus**：`IDLE` / `CONNECTING` / `CONNECTED` / `SERVERS_STARTED`（WS/文件服务就绪，可拉流）/ `FAILED`

**DeviceInfo**：`batteryPercent`、`serialNumber`、`wifiSsid`、`vrIp`、`storageTotalMb`、`storageUsedMb`、`volume`、`maxVolume`、`projectEnabled`、`handEnabled`、`chunkDurationMin`、`uploadUrl`、`datasetDir`、`deviceModel`、`vrVersion`、`usbEnabled`、`recording`、`recordStartMs`（录制计时本地锚点，时长 = 本地now − 本值，免疫两端时钟偏移）、`currentGroup`、`language`（VR 系统语言，get/set_language）

**VideoStats**：`fps`、`byteRateKBps`（KB/s）、`width`、`height`

**ImuData**：`accelX/Y/Z`、`gyroX/Y/Z`、`quatX/Y/Z/W`、`temperature`（`null` 表示该轴无数据）

**TimeSyncState**（v1.2 重构）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | `TimeSyncStatus` | 派生展示态：`COLLECTING`(采集中) / `CALIBRATED`(校准完成) / `STABLE`(≥60s) / `WARMING`(≥30s) / `NO_DATA` / `DISCONNECTED` / `ERROR`，由 VR 推送的 `state`+`duration_ms` 计算 |
| `offsetMs` / `rttMs` | `Double?` | 最近一次 NTP 交换的原始偏移 / RTT |
| `predictedOffsetMs` | `Double?` | 当前测量偏移（setClock 方案：VR 实时校正系统时钟，predicted_offset_ms 即 offset_ms；保留字段为协议兼容） |
| `durationMs` / `totalMs` | `Int` | 已采集时长 / 目标时长（离线模式倒计时由 `remainingSec` 派生） |
| `serialAvailable` | `Boolean` | PC 时间同步服务（串口）是否可达，false 时 startSync/offsetTest 无效 |
| `onlineSync` | `Boolean` | VR 端同步模式：true=在线持续校正（默认） / false=离线 30s 校准锁定 |
| `error` | `String?` | 错误描述 |

便捷属性：`remainingSec`（剩余秒数）、`isOfflineCollecting` / `isOnlineCollecting`（采集中按模式区分）。

**XrDevice（扫描结果）**：`mac`、`name`、`serialNumber`、`rssi`

### 手柄 / 腕部相机（E6+ES202）

```kotlin
client.setHandshankListener(object : XrHandshankListener {
    override fun onHandshankStatus(side: HandshankSide, state: HandshankSideState) {}
    override fun onHandshankBondError(side: HandshankSide, error: String) {}
    override fun onWristDetail(side: HandshankSide, detail: WristDetail?) {} // detail=null=该侧未就绪
    override fun onWristImu(side: HandshankSide, imu: WristImuData) {}
})
client.getHandshankStatus()          // 连接后自动补拉一次（非 E2）
client.bondHandshank(HandshankSide.LEFT)
client.unbondHandshank(HandshankSide.RIGHT)
client.getWristDetail(HandshankSide.LEFT)
client.setWristImuEnabled(true)
client.setWristSurfaces(leftSurface, rightSurface) // wrist 组渲染目标
client.updateVideoSurface(surface)   // 主流 Surface 重建后重投递
// StateFlow: handshankLeft / handshankRight / wristStreams / wristPlayback
client.switchGroup(CameraGroup.WRIST) // E6 切腕部组：断主流→直连左右腕部 :8080
```

- 能力位 `deviceInfo.wristSupported`：get_device_info 的 d303d_* 位域 ∨ get_handshank_status
  的 wrist_supported；null=未知。
- 能力位 `deviceInfo.es202Handshank`：get_handshank_status 的 es202_handshank（VR 按
  displayId 平台判定）；null=旧固件未下发。**与 `wristSupported` 语义独立**——那是 E2
  腕部相机能力位，无 E2 的 CS02 设备手柄仍是 ES202；按手柄型号区分的场景（如绑定文案）
  用 `es202Handshank`，旧固件 null 时回退 `wristSupported`（对齐 Flutter）。
- ES201(E2) 老固件不识别手柄命令，SDK 按机型守卫不下发。

## 连接状态机

```
DISCONNECTED ──startScan()──> SCANNING ──stop/超时──> DISCONNECTED
DISCONNECTED ──connect()────> CONNECTING ──成功+写凭据──> CONNECTED
                                │                        │
                                └──5 次失败──> FAILED     └──disconnect/意外断开──> DISCONNECTED
```

> `SCANNING` 不会直接转 `CONNECTING`：扫描停止后回到 `DISCONNECTED`，再从结果中选设备 `connect()`。

## 版本

AAR 文件名形如 `xr-camera-control-v<x>.aar`，`<x>` 为 `xr-camera-control/build.gradle.kts` 的 `sdkVersion`，与运行时 Logcat 打印的版本一致。升级 AAR 时替换 `libs/` 下旧文件即可（依赖用 `fileTree` 不感知文件名版本）。

### v1.4（当前）

- 新增 E6+ES202 腕部手柄支持：`XrHandshankListener`（状态 / 绑定错误 / 腕部详情 / 腕部 IMU）、`bondHandshank` / `unbondHandshank` / `getHandshankStatus` / `getWristDetail` / `setWristImuEnabled` 命令、`handshankLeft` / `handshankRight` / `wristStreams` / `wristPlayback` StateFlow、`setWristSurfaces` 双流渲染、`switchGroup(CameraGroup.WRIST)` 腕部组切换；能力位 `DeviceInfo.wristSupported`；ES201(E2) 按机型守卫不下发手柄命令

### v1.3

- 时间同步自动化对齐 VR 端 2026-08-24 改动：`startSync()`/`cancelSync()` 标记 `@Deprecated`（VR 端已自动采集，调用仅回 error），状态展示不受影响

### v1.2


- **时间同步模型重构**：`TimeSyncState` 改为 `state`+`duration_ms` 派生 7 态（COLLECTING/CALIBRATED/STABLE/WARMING/NO_DATA/DISCONNECTED/ERROR），新增 `predictedOffsetMs`/`serialAvailable`/`onlineSync`；移除 `countdownSeconds`/`driftRatePpm`/`totalRounds`（倒计时由 `remainingSec` 派生）——**breaking change**，使用旧字段需迁移
- 新增 `startSync()` / `cancelSync()` 手动控制时间同步采集（对应新协议命令 `start_sync`/`cancel_sync`）
- 修复偶现不能预览/不能停止录制：WS 建连 10s 超时 + 幂等防并发（对齐 VR :8080 保新踢旧语义：新连接接管时旧连接被关闭）；BLE 重试 stale GATT 回调过滤
- 移除旧推送 `time_sync_countdown` / `time_sync_regression_complete` 的处理
- 修复录制时长显示虚高数天：对齐 ef924bb，`recording_started`/`get_state` 优先用 VR 下发的 `record_elapsed_ms`（boottime 单调时钟）换算本地锚点 `recordStartMs`（= 本地now − 已录时长），全程本地时钟域免疫两端时钟偏移；旧 VR 缺字段时，有有效锚点沿用、无锚点（如录制中途接入）兜底本地 now 从 0 起计（不回退读 VR epoch 的 `record_start_ms`，避免两端时钟差重新引入虚高）
- 新增 `getLanguage()` / `setLanguage()` 读写 VR 系统语言（zh/en，对应新协议命令 `get_language`/`set_language`，结果到 `DeviceInfo.language`）

### v1.1

协议对齐（切片时长/上传地址/电源/删除数据）、中英文注释、Demo UI 重构。

## 混淆

SDK release 构建**不开启混淆**（`isMinifyEnabled = false`），内部包（`ble`/`video`/`command`/`internal`）随 AAR 原样保留，便于第三方排障；api 边界由 Kotlin `internal` 修饰符保证。

SDK 自带 `consumer-rules.pro`：当**宿主应用**开启 R8 混淆时，自动保留 SDK 的 `api` 包不被混淆，宿主无需额外配置。
