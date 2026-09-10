# XR Camera Control iOS SDK

An iOS SDK (Swift 6) that wraps **BLE provisioning + WebSocket H.265 streaming + AVSampleBufferDisplayLayer hardware rendering + device control** for XR headsets.

The phone sends WiFi credentials to the headset over BLE; once the headset is online it reports its IP back, and the phone pulls the H.265 stream over WebSocket (`:8080`) for hardware decode and rendering. The SDK also covers camera-group switching, volume, projection toggles, dataset chunking/upload, remote power control, and more. E2 devices (SSC models) support dual-eye split-screen preview; E6 + ES202 handsets support handshank bonding/status and wrist-camera dual-stream live preview.

- **Module**: Swift Package `XRCamera`
- **Minimum deployment**: iOS 15 (Swift 6, strict concurrency)
- **Public API**: `Public` directory only (`XRCameraClient` + `Models` + `Events` + `XRCameraPreview` + `XRCameraConfiguration`); `BLE`/`Video`/`Command` are internal
- **Threading model**: `XRCameraClient` is an **actor** — every method is called with `await`; events are delivered through the `events()` AsyncStream

## Requirements

- iOS 15.0+
- Xcode 26+ (Swift 6.3 toolchain)
- Integration: XCFramework

## Integration


**XCFramework**

Unzip `xr-camera-ios-v<x>.xcframework.zip`, drag `XRCamera.xcframework` into your project → target `General → Frameworks, Libraries, and Embedded Content` → Embed. The XCFramework is built with library evolution enabled, so the host app does not need to match the SDK's compiler version.

```swift
import XRCamera
```

## Permissions

The host app's Info.plist must include:

| Key | Purpose |
|---|---|
| `NSBluetoothAlwaysUsageDescription` | BLE scan/connect to the headset (required; system prompts on first scan — no runtime API needed) |
| `NSLocalNetworkUsageDescription` | WebSocket connection to the headset's local IP (iOS 14+ local-network permission prompt) |

> WS uses plaintext `ws://` to a LAN address; the demo verifies this works under the default ATS policy. If a strict ATS configuration in your host app blocks it, add `NSAppTransportSecurity → NSAllowsLocalNetworking`.

## Quick Start

Minimum viable flow: construct → subscribe to events → scan → connect (writes WiFi credentials) → start preview (renders into an `XRCameraPreview`).

```swift
import XRCamera

// 0. (Optional) global configuration — set BEFORE creating the client
XRCameraConfiguration.logEnabled = true            // log toggle (default true)
XRCameraConfiguration.logHighFrequencyData = false // imu/stats/timeSync high-frequency logs (silent by default)

let client = XRCameraClient()   // actor: await every call; init logs the SDK version once to Console

// 1. Subscribe to the event stream (each call returns an independent stream; multiple subscriptions allowed)
Task {
    for await event in await client.events() {
        switch event {
        case .connectionChanged(let state): break  // connection state
        case .vrIpAddress(let ip):        break     // once you have the IP, you can start preview
        case .firstFrameDecoded:          break     // first frame rendered
        case .error(let err):             break     // errors
        default: break
        }
    }
}

// 2. Scan (AsyncStream streams discovered/SN-filled devices as they appear; default 5s timeout ends the stream)
var found: [XrDevice] = []
Task {
    for await device in await client.startScan() { found.append(device) }
}
// Pick one and connect (SSID/password are for the WiFi the headset will join)
try await client.connect(found[0], ssid: "YOUR_SSID", password: "YOUR_PASSWORD")

// 3. Preview (XRCameraPreview is a plain UIView; wrap with UIViewRepresentable for SwiftUI)
try await client.startPreview(.rgb, in: previewView)
// E2 (SSC) dual-eye: left eye → previewView, right eye → rightView, two independent decode pipelines
// try await client.startPreview(.rgb, in: previewView, second: rightView)

// 4. Stop / release
await client.stopPreview()
await client.disconnect()
await client.release()
```

After a successful connect the SDK automatically refreshes device info (`refreshDeviceInfo`). Model detection: a `DeviceInfo.deviceModel` starting with `SSC` means E2.

## API Reference

### XRCameraClient

Constructor: `XRCameraClient()`

**Observable state (actor-isolated; reading from another actor requires `await`)**

| Property | Type | Description |
|---|---|---|
| `connectionState` | `ConnectionState` | Connection state |
| `deviceInfo` | `DeviceInfo` | Full device-info snapshot (the SDK merges incremental pushes) |
| `videoStats` | `VideoStats` | Video statistics (zeroed when preview stops) |
| `sdkVersion` | `String` (static) | SDK version, matches the release artifact filename |

**Event stream (`events() -> AsyncStream<XRCameraEvent>`)**

| Event | Description |
|---|---|
| `connectionChanged(state)` | Connection state change |
| `wifiStatus(status)` | WiFi provisioning state |
| `vrIpAddress(ip)` | IP reported by the headset |
| `error(err)` / `commandFailed(cmd, err)` / `disconnected` | Errors / disconnect |
| `bluetoothStateChanged(state)` | Phone Bluetooth toggle change |
| `stats(stats)` / `playingChanged(playing)` / `firstFrameDecoded` | Video stats / playback state / first frame |
| `deviceInfo(info)` / `imu(data)` / `usbModeChanged(enabled)` / `handTrackingRestartRequired(enabled)` / `upgradeCheckResult(result)` | Device info / IMU / USB / hand tracking / upgrade |
| `timeSync(state)` | Time-sync state |
| `recordingPath(path)` / `chunkSaved(path)` / `recordingChanged(recording, path, reason, anchorMs)` / `recordingError(err)` | Recording start / chunk saved / stop / error |
| `handshankStatus(side, state)` / `handshankBondError(side, err)` | Handshank status push / bond failure (sync error and async push unified) |
| `wristDetail(side, detail)` / `wristImu(side, imu)` | Wrist detail response (`detail=nil` clears stale) / wrist IMU (~1Hz) |
| `wristStreamsChanged(streams)` / `wristPlaybackChanged(playback)` | Wrist stream URL snapshot / per-side playback state |

**Methods**

| Module | Method | Signature | E6 | E2 | Description |
|---|---|---|:--:|:--:|---|
| Provisioning | `startScan` | `(timeout: TimeInterval = 5) -> AsyncStream<XrDevice>` | ✅ | ✅ | BLE scan; the stream ends automatically on timeout |
| | `stopScan` | `() async` | ✅ | ✅ | Stop scanning |
| | `connect` | `(device, ssid, password) async throws` | ✅ | ✅ | Connect and write WiFi credentials; 5 retries with backoff, 60s provisioning timeout |
| | `disconnect` | `() async` | ✅ | ✅ | Disconnect BLE + video |
| Preview/Recording | `startPreview` | `(group, in preview, second: XRCameraPreview? = nil) async throws` | ✅ | ✅ | Tells the headset to stream, connects WS (min 3 attempts), decodes H.265 and renders; on E2, passing `second` gives dual-eye split screen; without it only the left eye is decoded |
| | `stopPreview` | `() async` | ✅ | ✅ | Stop preview |
| | `switchGroup` | `(group) async throws` | ✅ | ❌ | Switch tracking/rgb/ctrl; `wrist` uses the wrist dual-stream flow (pair with `setWristSurfaces`: auto-teardown of the main stream → wait for ack → connect both wrists; switching back rebuilds the main stream automatically) |
| | `startRecording` / `stopRecording` | `() async throws` | ✅ | ✅ | Remotely start/stop recording |
| Device control | `increaseVolume` / `decreaseVolume` | `() async throws` | ✅ | ❌ | Volume ±1 (skipped at min/max) |
| | `offsetTest` | `() async throws` | ✅ | ❌ | Offset test (result arrives via `TimeSyncState.offsetMs`) |
| | `startSync` / `cancelSync` | `() async throws` | ✅ | ❌ | Manually start/stop NTP time-sync collection |
| | `setProjectEnabled` | `(enabled) async throws` | ✅ | ❌ | Gesture/controller projection toggle |
| | `getInputMode` | `() async throws` | ✅ | ❌ | Query projection mode |
| | `getHandEnabled` / `setHandEnabled` | `()/(enabled) async throws` | ✅ | ❌ | Query/toggle hand tracking (setting requires an app restart) |
| | `getUsbMode` / `setUsbMode` | `()/(enabled) async throws` | ✅ | ❌ | Query/toggle USB (mtp,adb / none; requires restart) |
| | `getLanguage` / `setLanguage` | `()/(lang) async throws` | ✅ | ❌ | Query/set headset system language (zh/en only; invalid values throw) |
| Recording config | `getChunkDuration` / `setChunkDuration` | `()/(minutes) async throws` | ✅ | ❌ | Dataset chunk duration (minutes) |
| | `getUploadUrl` / `setUploadUrl` | `()/(url) async throws` | ✅ | ❌ | Chunk upload DatasetServer URL |
| Power/System | `shutdown` / `reboot` / `restartApp` | `() async throws` | ✅ | ✅ | Remote shutdown / device reboot / app restart |
| | `deleteData` | `() async throws` | ✅ | ✅ | Delete recorded data |
| State queries | `getDeviceInfo` / `getState` | `() async throws` | ✅ | ✅ | Refresh device info / runtime state snapshot |
| | `refreshDeviceInfo` | `() async throws` | ✅ | ✅ | Batch-refresh device info/config/state (called once automatically after connect) |
| Handshank/Wrist | `getHandshankStatus` | `() async throws` | ✅ | ❌ | Query handshank status + wrist capability bits (auto-pulled once once the model is known; skipped on ES201 legacy firmware) |
| | `bondHandshank` / `unbondHandshank` | `(_ side: HandshankSide) async throws` | ✅ | ❌ | Bond/unbond a handshank; failures arrive via the `handshankBondError` event (sync error and async push unified) |
| | `getWristDetail` | `(_ side: HandshankSide) async throws` | ✅ | ❌ | Query wrist camera details; when not ready the SDK emits `wristDetail(side, nil)` so callers can clear stale data |
| | `setWristImuEnabled` | `(_ enabled: Bool) async throws` | ✅ | ❌ | Toggle wrist IMU pushes (~1Hz, `wristImu` event) |
| | `setWristSurfaces` | `(left: XRCameraPreview?, right: XRCameraPreview?) async` | ✅ | ❌ | Inject the dual wrist render layers; after `switchGroup(.wrist)`, a side connects automatically once both its URL and layer are present |
| Upgrade/USB mode | `checkUpgrade` / `startUpgrade` | `() async throws` | ❌ | ✅ | OTA upgrade check / start (after confirmation the SDK disconnects automatically and notifies that the device will restart) |
| | `switchUsbDeviceMode` | `() async throws` | ❌ | ✅ | Switch USB device mode |
| Screenshot | `takeScreenshot` | `() async -> UIImage?` | ✅ | ✅ (left eye) | Decodes the most recent GOP to extract the current frame's pixels (iOS-specific) |

**Lifecycle**

| Method | Description |
|---|---|
| `release()` | Full teardown: stops scanning, disconnects BLE/video. Do not use the client afterwards |

### Models (`XRCamera` module, Public directory)

**ConnectionState**: `idle` / `scanning` / `connecting` / `connected` / `disconnected` (connection failures surface via the `error` event and the state returns to `disconnected`)

**CameraGroup**: `tracking` / `rgb` / `ctrl` / `wrist` (E6 + ES202 wrist-camera group; dual streams connect automatically and it does not go through the main `start_preview`)

**WifiStatus**: `idle` / `connecting` / `connected` / `serversStarted` (WS ready, streaming possible) / `failed`

**DeviceInfo** (full snapshot; `nil` fields not yet reported): `battery`, `sn`, `wifiSsid`, `wifiIp`, `storageTotalMb`, `storageUsedMb`, `volume`, `maxVolume`, `projectEnabled`, `handEnabled`, `chunkDurationMin`, `uploadUrl`, `datasetDir`, `deviceModel` (`SSC` prefix = E2), `vrVersion`, `usbEnabled`, `recording`, `recordStartMs` (local anchor for the recording timer — elapsed = local now − this value, immune to clock offset between the two ends), `currentGroup`, `language`, `wristSupported` (wrist-camera capability bit: any d303d status field >0 from `get_device_info`; both missing = nil, no overwrite), `es202Handshank` (ES202 handset model bit: true = `get_handshank_status` includes left/right objects, false = capability bits only; strict bool parsing)

> `wristSupported` and `es202Handshank` mean different things: the former says the **device has wrist-camera/handshank capability** (show the handshank card, allow switching to the wrist group), the latter describes the **response format of the status query** (whether it carries per-side state objects). Neither gates command sending itself.

**VideoStats**: `fps`, `byteRateKBps` (KB/s), `width`, `height`

**ImuData**: `accelX/Y/Z`, `gyroX/Y/Z`, `quatX/Y/Z/W`, `temperature` (on E2 only acceleration/gyro are valid)

**TimeSyncState**

| Field | Type | Description |
|---|---|---|
| `status` | `TimeSyncStatus` | Derived display state: `collecting` / `calibrated` / `stable` (≥60s) / `warming` (≥30s) / `noData` / `disconnected` / `error`, computed from the headset's `state`+`duration_ms` pushes |
| `offsetMs` / `rttMs` | `Double?` | Raw offset / RTT of the most recent NTP exchange |
| `predictedOffsetMs` | `Double?` | Current measured offset (setClock scheme: the headset corrects its system clock in real time, i.e. `offset_ms`; field kept for protocol compatibility) |
| `durationMs` / `totalMs` | `Int` | Collected duration / target duration |
| `serialAvailable` | `Bool` | Whether the PC time-sync service (serial) is reachable; when false `startSync`/`offsetTest` are no-ops |
| `onlineSync` | `Bool` | Headset sync mode: true = continuous online correction (default) / false = offline 30s calibration lock |
| `error` | `String?` | Error description |

Convenience properties: `remainingSec` (seconds remaining), `isOfflineCollecting` / `isOnlineCollecting` (collecting, split by mode).

**XrDevice (scan result)**: `id` (UUID), `name`, `serialNumber`, `rssi`

**Handshank / wrist models** (all `@frozen` + `Sendable` + `Equatable`)

| Model | Fields | Description |
|---|---|---|
| `HandshankSide` | `left` / `right` | Handshank side (protocol wire value) |
| `HandshankSideState` | `bonded` / `connected` / `bleConnected` / `wifiConnected` (Bool), `battery` (Int?) | Per-side handshank state; missing fields default to false / nil |
| `WristDetail` | `sn` / `wifiSsid` / `wifiIp` / `version` (String?), `battery` / `storageTotalMb` / `storageUsedMb` (Int?) | Wrist details; all nil = not ready (event carries `nil` to clear stale); storage accepts the long names first, falling back to `t_mb`/`u_mb` |
| `WristImuData` | `ax` / `ay` / `az` / `gx` / `gy` / `gz` (Double?) | Wrist IMU (accel/gyro only, ~1Hz) |
| `WristStreams` | `leftWsUrl` / `rightWsUrl` (String?) | Wrist stream URLs `ws://<wrist IP>:8080` (parsed from BLE notify `303d_left:`/`303d_right:` prefixes + IPv4 validation) |
| `WristPlaybackState` | `idle` / `connecting` / `playing` / `failed` | Per-side playback state (placeholder rendering decisions) |
| `WristPlayback` | `left` / `right` (WristPlaybackState) | Both-side playback snapshot |

**Wrist dual-stream usage sequence** (E6 + ES202):

```swift
// 1. Switch to the wrist group (the SDK automatically: tears down the main stream → switch_group ack → connects ws://<wrist IP>:8080 per side once ready)
try await client.switchGroup(.wrist)
// 2. Inject the dual render layers (if a URL arrived earlier, that side connects as soon as its layer is set)
await client.setWristSurfaces(left: leftPreview, right: rightPreview)
// 3. Observe playback states to render placeholders/video
for await event in await client.events() {
    if case .wristPlaybackChanged(let pb) = event { /* pb.left / pb.right */ }
}
// 4. Switch back to any main group: the SDK tears down both wrists and rebuilds the main stream from the pre-wrist snapshot (no start_preview resend — the VR session is still alive)
try await client.switchGroup(.rgb)
```

> The wrist stream is raw H.265 (no `cam:` frame prefix) and connects to the wrist device's IP, not the VR host; `startPreview` is rejected while the wrist group is active/pending — switch back to a main group first.

**XRCameraPreview**: a `UIView` subclass containing an `AVSampleBufferDisplayLayer` (`videoGravity = resizeAspect`); `@unchecked Sendable`, safe to pass across concurrency domains

**XRCameraError**: `bluetoothUnavailable` / `bluetoothPoweredOff` / `bluetoothUnauthorized` / `bluetoothUnsupported` / `serviceNotFound` / `characteristicNotFound(String)` / `connectFailed(underlying:)` / `wifiFailed` / `websocketFailed(underlying:)` / `decoderFailed(underlying:)` / `notConnected` / `timeout` / `unsupportedLanguage(String)` / `underlying(String)`

**XRCameraConfiguration** (set globally before creating a client): `logEnabled`, `logPrivacy` (.public/.private/.sensitive), `logHighFrequencyData` (high-frequency imu/stats/timeSync logs, default false), `wsRetryCount`, `wsRetryIntervalMs`, `wsTimeoutSeconds`, `maxGOPFrames`

## Connection State Machine

```
idle ──startScan()──> scanning ──stop/timeout──> idle
idle/disconnected ──connect()──> connecting ──WiFi ready (serversStarted)──> connected
                                   │                                        │
                                   └──5 failures/provisioning timeout──> disconnected
                                          (an error event is emitted too)  └──disconnect()/unexpected drop──> disconnected
```

> `scanning` never transitions directly to `connecting`: `connect()` stops the scan first, then starts connecting.

## Swift Concurrency & Threading

- `XRCameraClient` is an **actor**: every method is `await`-ed; state is actor-isolated. UI layers (MainActor) should update via the `events()` stream rather than property polling.
- BLE callbacks run on an internal serial queue; video decode runs on a dedicated serial queue with main-thread enqueueing (enqueuing to `AVSampleBufferDisplayLayer` off-main causes a black screen — handled inside the SDK).
- `XRCameraPreview` is `@unchecked Sendable`: the layer reference is immutable after creation, safe to pass across concurrency domains.
- Logging goes through `XRLog` (os.Logger); high-frequency events (imu/stats/timeSync) are silent by default — turn on with `XRCameraConfiguration.logHighFrequencyData = true`.

## Versioning

Release artifacts are named `xr-camera-ios-v<x>.xcframework.zip` and `xr-camera-ios-demo-src-v<x>.zip`, where `<x>` matches the runtime constant `XRCameraClient.sdkVersion` (printed once at init: `XRCamera iOS SDK version: <x>`). Archive script: `sdk/ios/archive_release_xcframework_demo.sh` (builds the xcframework + packages the demo source + copies docs into `release/sdk/ios/`).

### v1.4.0

- BLE provisioning: scan (private service-UUID filter + SN broadcast fill-in), 5-retry backoff connect, WiFi credential write, 60s total provisioning timeout
- Video: WS H.265 pull (min 3 retries) → streaming NAL parse → **multi-slice frame aggregation** (without it the screen stays black) → `AVSampleBufferDisplayLayer` hardware rendering → rolling GOP buffer for screenshots
- **E2 (SSC model) dual-eye stream**: WS frames carry a `[1B cam_id | H.265]` prefix for demuxing; `startPreview(group:in:second:)` decodes and renders both eyes independently
- Time-sync v1.2 protocol: 7 derived states from `state`+`duration_ms`, incremental `sync_result` merging, `startSync`/`cancelSync`
- Full device control: volume/projection/hand tracking/USB/language/chunking/upload/power/delete/upgrade (E2)/USB device mode (E2)
- Handshank / wrist camera (E6 + ES202): 5 BLE commands (status/bond/unbond/wrist detail/wrist IMU toggle) + 4 push event kinds + wrist dual-stream live preview (`switchGroup(.wrist)` → `setWristSurfaces`, per-side independent connections and playback states); ES201 legacy firmware (via `isES201`) skips the status auto-pull to avoid Unknown-response pollution
- Swift 6 strict concurrency: actor facade + AsyncStream events
