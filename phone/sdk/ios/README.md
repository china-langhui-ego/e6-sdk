# XR Camera Control iOS SDK

封装 VR 头显的 **BLE 配网 + WebSocket H.265 拉流 + AVSampleBufferDisplayLayer 硬解渲染 + 设备控制** 的 iOS SDK（Swift 6）。

手机端通过 BLE 给 VR 头显下发 WiFi 凭据，头显联网后回传 IP，手机再经 WebSocket（`:8080`）拉取 H.265 流并硬件解码渲染，同时支持摄像头组切换、音量、投影开关、数据集切片/上传、远程电源等控制能力。E2（SSC 型号）设备支持双目流分屏预览；E6 + ES202 手柄支持手柄绑定/状态与腕部相机双路直播。

- **模块**：Swift Package `XRCamera`
- **最低部署**：iOS 15（Swift 6，strict concurrency）
- **对外 API**：仅 `Public` 目录（`XRCameraClient` + `Models` + `Events` + `XRCameraPreview` + `XRCameraConfiguration`）；`BLE`/`Video`/`Command` 为 internal
- **线程模型**：`XRCameraClient` 为 **actor**，方法一律 `await` 调用；事件经 `events()` AsyncStream 派发

## 环境要求

- iOS 15.0+
- Xcode 26+（Swift 6.3 toolchain）
- 集成方式：XCFramework

## 集成


**XCFramework**

解压 `xr-camera-ios-v<x>.xcframework.zip`，将 `XRCamera.xcframework` 拖入工程 → target `General → Frameworks, Libraries, and Embedded Content` → Embed。XCFramework 以 library evolution 构建，不要求宿主与 SDK 同版本编译器。

```swift
import XRCamera
```

## 权限

宿主 App Info.plist 需包含：

| Key | 用途 |
|---|---|
| `NSBluetoothAlwaysUsageDescription` | BLE 扫描/连接 VR（必须；首次扫描系统弹窗，无需运行时 API） |
| `NSLocalNetworkUsageDescription` | WebSocket 连接 VR 本地 IP（iOS 14+ 本地网络权限弹窗） |

> WS 走 `ws://` 明文连接局域网地址，demo 已验证默认 ATS 策略可直连；若宿主配置了严格 ATS 被拦截，添加 `NSAppTransportSecurity → NSAllowsLocalNetworking`。

## 快速开始

最小可用流程：构造 → 订阅事件 → 扫描 → 连接（写 WiFi 凭据）→ 开始预览（渲染到 `XRCameraPreview`）。

```swift
import XRCamera

// 0. （可选）全局配置：创建 client 前设置
XRCameraConfiguration.logEnabled = true            // 日志开关（默认 true）
XRCameraConfiguration.logHighFrequencyData = false // imu/stats/timeSync 高频日志（默认静默）

let client = XRCameraClient()   // actor：所有方法 await；init 时 Console 打印一次 SDK 版本号

// 1. 订阅事件流（每次调用返回独立流，可多处同时订阅）
Task {
    for await event in await client.events() {
        switch event {
        case .connectionChanged(let state): break  // 连接状态
        case .vrIpAddress(let ip):        break     // 拿到 IP 后可开始预览
        case .firstFrameDecoded:          break     // 首帧出图
        case .error(let err):             break     // 错误
        default: break
        }
    }
}

// 2. 扫描（AsyncStream 实时推送发现/序列号补全的设备；默认 5s 超时自动结束）
var found: [XrDevice] = []
Task {
    for await device in await client.startScan() { found.append(device) }
}
// 从结果中选一台连接（SSID/密码为 VR 将要连接的 WiFi）
try await client.connect(found[0], ssid: "YOUR_SSID", password: "YOUR_PASSWORD")

// 3. 预览（XRCameraPreview 是普通 UIView；SwiftUI 里用 UIViewRepresentable 包装）
try await client.startPreview(.rgb, in: previewView)
// E2（SSC）双目：左目→previewView、右目→rightView，双路独立解码
// try await client.startPreview(.rgb, in: previewView, second: rightView)

// 4. 停止 / 释放
await client.stopPreview()
await client.disconnect()
await client.release()
```

连接成功后 SDK 自动刷新一次设备信息（`refreshDeviceInfo`）。设备型号判定：`DeviceInfo.deviceModel` 以 `SSC` 开头即 E2。

## API 参考

### XRCameraClient

构造：`XRCameraClient()`

**可观察状态（actor 隔离，跨 actor 读取需 `await`）**

| 属性 | 类型 | 说明 |
|---|---|---|
| `connectionState` | `ConnectionState` | 连接状态 |
| `deviceInfo` | `DeviceInfo` | 设备信息全量快照（SDK 内部合并增量推送） |
| `videoStats` | `VideoStats` | 视频统计（停止播放时归零） |
| `sdkVersion` | `String`（static） | SDK 版本号，与发布物文件名一致 |

**事件流（`events() -> AsyncStream<XRCameraEvent>`）**

| 事件 | 说明 |
|---|---|
| `connectionChanged(state)` | 连接状态变化 |
| `wifiStatus(status)` | WiFi 配网状态 |
| `vrIpAddress(ip)` | VR 回传 IP |
| `error(err)` / `commandFailed(cmd, err)` / `disconnected` | 错误/断开 |
| `bluetoothStateChanged(state)` | 手机蓝牙开关变化 |
| `stats(stats)` / `playingChanged(playing)` / `firstFrameDecoded` | 视频统计/播放状态/首帧 |
| `deviceInfo(info)` / `imu(data)` / `usbModeChanged(enabled)` / `handTrackingRestartRequired(enabled)` / `upgradeCheckResult(result)` | 设备信息/IMU/USB/手势/升级 |
| `timeSync(state)` | 时间同步状态 |
| `recordingPath(path)` / `chunkSaved(path)` / `recordingChanged(recording, path, reason, anchorMs)` / `recordingError(err)` | 录制开始/分片/停止/错误 |
| `handshankStatus(side, state)` / `handshankBondError(side, err)` | 手柄状态推送 / 绑定失败（同步与异步双通道归一） |
| `wristDetail(side, detail)` / `wristImu(side, imu)` | 腕部详情应答（`detail=nil` 表未就绪清 stale）/ 腕部 IMU（约 1Hz） |
| `wristStreamsChanged(streams)` / `wristPlaybackChanged(playback)` | 腕部流地址快照 / 双侧播放态 |

**方法**

| 模块 | 方法 | 签名 | E6 | E2 | 说明 |
|---|---|---|:--:|:--:|---|
| 配网 | `startScan` | `(timeout: TimeInterval = 5) -> AsyncStream<XrDevice>` | ✅ | ✅ | BLE 扫描，超时自动结束流 |
| | `stopScan` | `() async` | ✅ | ✅ | 停止扫描 |
| | `connect` | `(device, ssid, password) async throws` | ✅ | ✅ | 连接并写 WiFi 凭据，5 次重试退避，配网总超时 60s |
| | `disconnect` | `() async` | ✅ | ✅ | 断开 BLE + 视频 |
| 预览/录制 | `startPreview` | `(group, in preview, second: XRCameraPreview? = nil) async throws` | ✅ | ✅ | 通知 VR 推流，连 WS（最少 3 次尝试）收 H.265 硬解渲染；E2 传 `second` 双目分屏，不传只解左目 |
| | `stopPreview` | `() async` | ✅ | ✅ | 停止预览 |
| | `switchGroup` | `(group) async throws` | ✅ | ❌ | 切 tracking/rgb/ctrl；`wrist` 组走腕部双流（配 `setWristSurfaces`，自动断主流→等 ack→连双腕，切回自动重建主流） |
| | `startRecording` / `stopRecording` | `() async throws` | ✅ | ✅ | 远程开始/停止录制 |
| 设备控制 | `increaseVolume` / `decreaseVolume` | `() async throws` | ✅ | ❌ | 音量 ±1（达上下限不发） |
| | `offsetTest` | `() async throws` | ✅ | ❌ | 偏移测试（结果经 `TimeSyncState.offsetMs` 回传） |
| | `startSync` / `cancelSync` | `() async throws` | ✅ | ❌ | 手动开启/停止 NTP 时间同步采集 |
| | `setProjectEnabled` | `(enabled) async throws` | ✅ | ❌ | 手势/手柄投影开关 |
| | `getInputMode` | `() async throws` | ✅ | ❌ | 查询投影模式 |
| | `getHandEnabled` / `setHandEnabled` | `()/(enabled) async throws` | ✅ | ❌ | 查询/开关手势追踪（设置需重启 APP） |
| | `getUsbMode` / `setUsbMode` | `()/(enabled) async throws` | ✅ | ❌ | 查询/开关 USB（mtp,adb / none，需重启） |
| | `getLanguage` / `setLanguage` | `()/(lang) async throws` | ✅ | ❌ | 查询/设置 VR 系统语言（仅 zh/en，非法值抛错） |
| 录制配置 | `getChunkDuration` / `setChunkDuration` | `()/(minutes) async throws` | ✅ | ❌ | 数据集切片时长（分） |
| | `getUploadUrl` / `setUploadUrl` | `()/(url) async throws` | ✅ | ❌ | 分片上传 DatasetServer URL |
| 电源/系统 | `shutdown` / `reboot` / `restartApp` | `() async throws` | ✅ | ✅ | 远程关机 / 重启设备 / 重启 APP |
| | `deleteData` | `() async throws` | ✅ | ✅ | 删除录制数据 |
| 状态查询 | `getDeviceInfo` / `getState` | `() async throws` | ✅ | ✅ | 刷新设备信息 / 运行时状态快照 |
| | `refreshDeviceInfo` | `() async throws` | ✅ | ✅ | 批量刷新设备信息/配置/状态（连接后自动调一次） |
| 手柄/腕部 | `getHandshankStatus` | `() async throws` | ✅ | ❌ | 查询手柄状态 + wrist 能力位（机型就绪后自动补拉一次；ES201 老固件跳过） |
| | `bondHandshank` / `unbondHandshank` | `(_ side: HandshankSide) async throws` | ✅ | ❌ | 绑定/解绑手柄；失败经 `handshankBondError` 事件（同步 error 与异步推送双通道归一） |
| | `getWristDetail` | `(_ side: HandshankSide) async throws` | ✅ | ❌ | 查询腕部相机详情；未就绪经 `wristDetail(side, nil)` 通知调用方清 stale |
| | `setWristImuEnabled` | `(_ enabled: Bool) async throws` | ✅ | ❌ | 开关腕部 IMU 推送（约 1Hz，`wristImu` 事件） |
| | `setWristSurfaces` | `(left: XRCameraPreview?, right: XRCameraPreview?) async` | ✅ | ❌ | 注入腕部双路渲染层；`switchGroup(.wrist)` 后地址+层齐备即自动连接对应侧 |
| 升级/USB模式 | `checkUpgrade` / `startUpgrade` | `() async throws` | ❌ | ✅ | OTA 升级检测 / 开始升级（确认后 SDK 自动断连并提示设备将重启） |
| | `switchUsbDeviceMode` | `() async throws` | ❌ | ✅ | 切换 USB Device 模式 |
| 截图 | `takeScreenshot` | `() async -> UIImage?` | ✅ | ✅（左目） | 解码最近 GOP 取当前帧像素（iOS 特有） |

**生命周期**

| 方法 | 说明 |
|---|---|
| `release()` | 彻底释放：停止扫描、断开 BLE/视频。调用后不再使用 |

### Model（`XRCamera` 模块，Public 目录）

**ConnectionState（连接状态）**：`idle` / `scanning` / `connecting` / `connected` / `disconnected`（连接失败经 `error` 事件抛出、状态回 `disconnected`）

**CameraGroup（摄像头组）**：`tracking` / `rgb` / `ctrl` / `wrist`（E6 + ES202 腕部相机组，双路自动连接，不走主流 `start_preview`）

**WifiStatus**：`idle` / `connecting` / `connected` / `serversStarted`（WS 就绪，可拉流）/ `failed`

**DeviceInfo**（全量快照，`nil` 字段表示未上报）：`battery`、`sn`、`wifiSsid`、`wifiIp`、`storageTotalMb`、`storageUsedMb`、`volume`、`maxVolume`、`projectEnabled`、`handEnabled`、`chunkDurationMin`、`uploadUrl`、`datasetDir`、`deviceModel`（`SSC` 前缀 = E2）、`vrVersion`、`usbEnabled`、`recording`、`recordStartMs`（录制计时本地锚点，时长 = 本地 now − 本值，免疫两端时钟偏移）、`currentGroup`、`language`、`wristSupported`（腕部相机能力位：`get_device_info` d303d 位域任一侧 >0；双缺失 = nil 不覆盖）、`es202Handshank`（ES202 手柄机型位：true = `get_handshank_status` 含 left/right 对象，false = 仅能力位；仅 bool 采用）

> `wristSupported` 与 `es202Handshank` 语义不同：前者表示**设备具备腕部相机/手柄能力**（可显示手柄卡片、允许切 wrist 组），后者表示**手柄状态应答的具体格式**（是否含每侧状态对象），均不影响命令收发本身。

**VideoStats**：`fps`、`byteRateKBps`（KB/s）、`width`、`height`

**ImuData**：`accelX/Y/Z`、`gyroX/Y/Z`、`quatX/Y/Z/W`、`temperature`（E2 仅加速度/角速度有效）

**TimeSyncState**

| 字段 | 类型 | 说明 |
|---|---|---|
| `status` | `TimeSyncStatus` | 派生展示态：`collecting`(采集中) / `calibrated`(校准完成) / `stable`(≥60s) / `warming`(≥30s) / `noData` / `disconnected` / `error`，由 VR 推送的 `state`+`duration_ms` 计算 |
| `offsetMs` / `rttMs` | `Double?` | 最近一次 NTP 交换的原始偏移 / RTT |
| `predictedOffsetMs` | `Double?` | 当前测量偏移（setClock 方案：VR 实时校正系统时钟，即 `offset_ms`；保留字段为协议兼容） |
| `durationMs` / `totalMs` | `Int` | 已采集时长 / 目标时长 |
| `serialAvailable` | `Bool` | PC 时间同步服务（串口）是否可达，false 时 `startSync`/`offsetTest` 无效 |
| `onlineSync` | `Bool` | VR 端同步模式：true=在线持续校正（默认）/ false=离线 30s 校准锁定 |
| `error` | `String?` | 错误描述 |

便捷属性：`remainingSec`（剩余秒数）、`isOfflineCollecting` / `isOnlineCollecting`（采集中按模式区分）。

**XrDevice（扫描结果）**：`id`(UUID)、`name`、`serialNumber`、`rssi`

**手柄 / 腕部模型**（全部 `@frozen` + `Sendable` + `Equatable`）

| 模型 | 字段 | 说明 |
|---|---|---|
| `HandshankSide` | `left` / `right` | 手柄侧别（协议 wire 值） |
| `HandshankSideState` | `bonded` / `connected` / `bleConnected` / `wifiConnected`(Bool)、`battery`(Int?) | 单侧手柄状态；字段缺失按 false / nil |
| `WristDetail` | `sn` / `wifiSsid` / `wifiIp` / `version`(String?)、`battery` / `storageTotalMb` / `storageUsedMb`(Int?) | 腕部详情；全 nil = 未就绪（事件携 `nil` 清 stale）；存储兼容长名优先、`t_mb`/`u_mb` 短名兜底 |
| `WristImuData` | `ax` / `ay` / `az` / `gx` / `gy` / `gz`(Double?) | 腕部 IMU（仅 accel/gyro，约 1Hz） |
| `WristStreams` | `leftWsUrl` / `rightWsUrl`(String?) | 腕部流地址 `ws://<腕部IP>:8080`（BLE notify `303d_left:`/`303d_right:` 前缀 + IPv4 校验） |
| `WristPlaybackState` | `idle` / `connecting` / `playing` / `failed` | 单侧播放态（UI 占位判定） |
| `WristPlayback` | `left` / `right`(WristPlaybackState) | 双侧播放态快照 |

**wrist 双流使用时序**（E6 + ES202）：

```swift
// 1. 切 wrist 组（SDK 自动：断主流 → switch_group 等 ack → 双侧就绪即连 ws://<腕部IP>:8080）
try await client.switchGroup(.wrist)
// 2. 注入双路渲染层（此前后地址已到也会在层注入时自动补连）
await client.setWristSurfaces(left: leftPreview, right: rightPreview)
// 3. 观察播放态渲染占位/画面
for await event in await client.events() {
    if case .wristPlaybackChanged(let pb) = event { /* pb.left / pb.right */ }
}
// 4. 切回任意主流组：SDK 自动断双腕并按切组前快照重建主流（不重发 start_preview，VR 会话仍在）
try await client.switchGroup(.rgb)
```

> 腕部流为纯 H.265（无 `cam:` 帧前缀），直连腕部设备 IP 而非 VR 主机；主流 `startPreview` 在 wrist 组活跃/挂起时会被拒绝，先切回主流组。

**XRCameraPreview**：`UIView` 子类，内含 `AVSampleBufferDisplayLayer`（`videoGravity = resizeAspect`）；`@unchecked Sendable` 可跨并发域传递

**XRCameraError**：`bluetoothUnavailable` / `bluetoothPoweredOff` / `bluetoothUnauthorized` / `bluetoothUnsupported` / `serviceNotFound` / `characteristicNotFound(String)` / `connectFailed(underlying:)` / `wifiFailed` / `websocketFailed(underlying:)` / `decoderFailed(underlying:)` / `notConnected` / `timeout` / `unsupportedLanguage(String)` / `underlying(String)`

**XRCameraConfiguration**（创建 client 前全局设置）：`logEnabled`、`logPrivacy`（.public/.private/.sensitive）、`logHighFrequencyData`（imu/stats/timeSync 高频日志，默认 false）、`wsRetryCount`、`wsRetryIntervalMs`、`wsTimeoutSeconds`、`maxGOPFrames`

## 连接状态机

```
idle ──startScan()──> scanning ──stop/超时──> idle
idle/disconnected ──connect()──> connecting ──WiFi 就绪(serversStarted)──> connected
                                   │                                        │
                                   └──5 次失败/配网超时──> disconnected      └──disconnect()/意外断开──> disconnected
                                          （同时发 error 事件）
```

> `scanning` 不会直接转 `connecting`：`connect()` 内部先结束扫描，再发起连接。

## Swift 并发与线程模型

- `XRCameraClient` 为 **actor**：所有方法 `await` 调用；状态属性 actor 隔离，UI 层（MainActor）建议经 `events()` 流更新，避免逐属性轮询。
- BLE 回调在内部串行队列，视频解码在专属串行队列 + 主线程入队（`AVSampleBufferDisplayLayer` 后台入队会黑屏，SDK 已内部处理）。
- `XRCameraPreview` 标 `@unchecked Sendable`：layer 引用创建后不变，跨并发域传递安全。
- 日志统一走 `XRLog`（os.Logger），高频事件（imu/stats/timeSync）默认静默，`XRCameraConfiguration.logHighFrequencyData = true` 打开。

## 版本

发布物文件名形如 `xr-camera-ios-v<x>.xcframework.zip`、`xr-camera-ios-demo-src-v<x>.zip`，`<x>` 与运行时常量 `XRCameraClient.sdkVersion`（init 时 Console 打印 `XRCamera iOS SDK version: <x>`）一致。归档脚本：`sdk/ios/archive_release_xcframework_demo.sh`（构建 xcframework + 打包 demo 源码 + 拷贝文档到 `release/sdk/ios/`）。

### v1.4.0

- BLE 配网：扫描（私有 service UUID 过滤 + SN 广播补全）、5 次重试退避连接、WiFi 凭据下发、配网 60s 总超时
- 视频预览：WS H.265 拉流
- 设备控制全量：音量/投影/手势/USB/语言/切片/上传/电源/删除/升级（E2）/USB Device 模式（E2）
- 手柄/腕部相机（E6 + ES202）：5 条 BLE 命令（状态/绑定/解绑/腕部详情/腕部 IMU 开关）+ 4 类推送事件 + wrist 双路直播（`switchGroup(.wrist)` → `setWristSurfaces`，双侧独立连接、独立播放态）；ES201 老固件（`isES201` 判定）自动跳过状态补拉，防 Unknown 应答污染

