# XR Camera Control Android SDK

A native Android SDK (Kotlin) that wraps **BLE provisioning + WebSocket H.265 streaming + MediaCodec hardware decoding + device control** for VR headsets.

The phone sends WiFi credentials to the VR headset over BLE; once the headset is online it reports its IP, then the phone pulls an H.265 elementary stream over WebSocket (`:8080`), hardware-decodes and renders it. The SDK also supports camera-group switching, volume, projection toggles, dataset chunking/upload, and remote power control.

- **Package**: `com.ssnwt.xrcamera.control`
- **minSdk**: 24 (`compileSdk = 36`)
- **Public API**: only the `api` package (`XrCameraClient` + `api.listener.*` + `api.model.*`); `ble`/`video`/`command`/`internal` are internal

## Requirements

- Kotlin 1.9+ / Java 8
- Android Gradle Plugin 8.x
- `compileSdk = 36`, `minSdk = 24`

## Integration

Place `xr-camera-control-v<x>.aar` in your module's `libs/` and load all aars in that directory (filename carries a version, so the dependency does not hardcode it):

```kotlin
dependencies {
    implementation(fileTree("libs") { include("*.aar") })

    // The AAR does not bundle transitive deps; the host must provide SDK runtime deps explicitly
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
```

## Permissions

The SDK requires the following permissions (AndroidManifest.xml); dangerous ones need runtime grant:

| Permission | Purpose | Runtime grant |
|---|---|---|
| `BLUETOOTH_SCAN` | BLE scan (API 31+, declared with `neverForLocation`) | Yes (API 31+) |
| `BLUETOOTH_CONNECT` | BLE connect (API 31+) | Yes (API 31+) |
| `ACCESS_FINE_LOCATION` | BLE scan fallback (API 30-) | Yes (API 30-) |
| `ACCESS_COARSE_LOCATION` | Companion to FINE location (API 30-, satisfies lint; on API 31+ also allows scanning when only coarse location is granted) | Yes (API 30-) |
| `INTERNET` | WebSocket streaming | No |
| `ACCESS_NETWORK_STATE` | Network state detection | No |

> WebSocket uses the cleartext port `:8080` by default; the host manifest needs `android:usesCleartextTraffic="true"` (or a network security config for that port), otherwise the connection is blocked.

Runtime strategy: on API 31+ request `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`; on API 30- request `ACCESS_FINE_LOCATION`.

## Quick Start

Minimal flow: construct → register listeners → scan → connect (write WiFi credentials) → start preview (render to a Surface).

```kotlin
// 1. Construct (enableDebugLogging=true prints diagnostics to Logcat with TAG "XrCamera")
val client = XrCameraClient(context, enableDebugLogging = BuildConfig.DEBUG)

// 2. Register listeners (all dispatched on the main thread)
client.setConnectionListener(object : XrConnectionListener {
    override fun onBleStateChanged(state: ConnectionState) { /* connection state */ }
    override fun onVrIpAddress(ip: String) { /* VR IP obtained; preview can begin */ }
    override fun onError(message: String) { /* errors */ }
    override fun onWifiStatus(status: WifiStatus) {}
    override fun onDisconnected() {}
})
client.setScanListener(object : XrScanListener {
    override fun onScanFinished(devices: List<XrDevice>) {
        // 3. Pick a device from the scan results (SSID/password = the WiFi the VR will join)
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

// 4. Scan for VR devices (5s timeout by default)
client.startScan()

// Once you have the IP, start preview (surface comes from a SurfaceView/Holder)
// client.startPreview(CameraGroup.RGB, surface)

// Teardown
// client.stopPreview()
// client.disconnect()
// client.release()
```

On a successful connection the SDK auto-refreshes device info once (`refreshDeviceInfo`). When the `XrCameraClient` class is loaded it prints `XrCamera SDK version: <x>` to Logcat once, so the integrated version can be confirmed.

## API Reference

### XrCameraClient

Constructor: `XrCameraClient(context: Context, enableDebugLogging: Boolean = false)`

**Observable state (StateFlow, main thread)**

| Property | Type | Description |
|---|---|---|
| `connectionState` | `StateFlow<ConnectionState>` | Connection state |
| `videoStats` | `StateFlow<VideoStats>` | Video statistics |
| `deviceInfo` | `StateFlow<DeviceInfo>` | Device info |

**Listener registration**

| Method | Description |
|---|---|
| `setConnectionListener(l)` | Connection / WiFi / IP / errors / disconnect |
| `setScanListener(l)` | Scan results |
| `setVideoStatsListener(l)` | Fps / first frame / playing state |
| `setDeviceListener(l)` | Device info / IMU / USB change / hand restart / upgrade check |
| `setTimeSyncListener(l)` | Time-sync state |
| `setRecordingListener(l)` | Recording chunk-saved / stopped / started / error |

| Module | Method | Signature | E6 | E2 | Description |
|---|---|---|:--:|:--:|---|
| Provisioning | `startScan` | `(timeoutMs: Long = 5000)` | ✅ | ✅ | BLE scan; auto-stops on timeout |
| | `stopScan` | `()` | ✅ | ✅ | Stop scanning |
| | `connect` | `(device, ssid, password)` | ✅ | ✅ | Connect and write WiFi credentials; 5 attempts total (1 initial + 4 retries) |
| | `disconnect` | `()` | ✅ | ✅ | Disconnect BLE + video |
| Preview/Recording | `startPreview` | `(group, surface, surface1?)` | ✅ | ✅ | Start video streaming; connect WS to hardware-decode H.265; `surface1` is the optional right-eye Surface (E2 dual-stream decoding) |
| | `stopPreview` | `()` | ✅ | ✅ | Stop preview |
| | `switchGroup` | `(group)` | ✅ | ❌ | Switch rgb/tracking/ctrl; auto rebuild decoder + reconnect WS |
| | `startRecording` | `()` | ✅ | ✅ | Start remote recording |
| | `stopRecording` | `()` | ✅ | ✅ | Stop remote recording |
| Device control | `increaseVolume` / `decreaseVolume` | `()` | ✅ | ❌ | Volume ±1 (not sent at limits) |
| | `offsetTest` | `()` | ✅ | ❌ | Offset test (10 rapid exchanges; result returned via `TimeSyncState.offsetMs`) |
| | `startSync` | `()` | ✅ | ❌ | Manually start NTP time-sync collection (requires the PC time-sync service) |
| | `cancelSync` | `()` | ✅ | ❌ | Stop an ongoing time-sync collection |
| | `setProjectionEnabled` | `(enabled)` | ✅ | ❌ | Toggle hand/controller projection |
| | `getHandEnabled` | `()` | ✅ | ❌ | Query hand tracking state |
| | `setHandEnabled` | `(enabled)` | ✅ | ❌ | Hand tracking toggle (requires app restart) |
| | `getUsbMode` | `()` | ✅ | ❌ | Query USB mode |
| | `setUsbMode` | `(enabled)` | ✅ | ❌ | Toggle USB (mtp,adb / none; reboot required) |
| | `getLanguage` | `()` | ✅ | ❌ | Query VR system language (zh/en, into `DeviceInfo.language`) |
| | `setLanguage` | `(lang)` | ✅ | ❌ | Set VR system language (zh/en only; invalid value throws `IllegalArgumentException`) |
| Recording config | `setChunkDuration` | `(minutes)` | ✅ | ✅ | Dataset chunk duration (minutes) |
| | `setUploadUrl` | `(url)` | ✅ | ✅ | DatasetServer upload URL |
| Power/System | `shutdown` | `()` | ✅ | ✅ | Remote power-off (fire-and-forget) |
| | `reboot` | `()` | ✅ | ✅ | Remote device reboot |
| | `deleteData` | `()` | ✅ | ✅ | Delete recorded data (fire-and-forget) |
| Restart App | `restartApp` | `()` | ✅ | ❌ | Restart VR APP process |
| State query | `getState` | `()` | ✅ | ✅ | Runtime state snapshot |
| | `refreshDeviceInfo` | `()` | ✅ | ✅ | Batch refresh device info (auto-called after connect) |
| Upgrade/USB mode | `checkUpgrade` | `()` | ❌ | ✅ | OTA upgrade check |
| | `startUpgrade` | `()` | ❌ | ✅ | Start OTA upgrade |
| | `switchUsbDeviceMode` | `()` | ❌ | ✅ | Switch USB device mode |

**Lifecycle**

| Method | Description |
|---|---|
| `release()` | Full release: stop scan, disconnect BLE/video, clear callbacks. Do not use the instance after this |

### Listener interfaces (`api.listener.*`, all callbacks on the main thread)

| Interface | Callbacks | Description |
|---|---|---|
| `XrConnectionListener` | `onBleStateChanged(state)` / `onWifiStatus(status)` / `onVrIpAddress(ip)` / `onError(msg)` / `onDisconnected()` | Connection |
| `XrScanListener` | `onDeviceFound(device)` / `onScanFinished(devices)` / `onScanError(msg)` | Scanning |
| `XrVideoStatsListener` | `onStats(stats)` / `onPlayingChanged(playing)` / `onFirstFrameDecoded()` | Video stats |
| `XrDeviceListener` | `onDeviceInfo(info)` / `onImuData(imu)` / `onUsbModeChanged(enabled)` / `onHandTrackingRestartRequired(enabled)` / `onUpgradeCheckResult(result)` | Device info / IMU / USB change / hand restart / upgrade check |
| `XrTimeSyncListener` | `onTimeSyncState(state)` | Time-sync state |
| `XrRecordingListener` | `onChunkSaved(path)` / `onRecordingStopped(reason)` / `onRecordingStarted(savePath, recordStartMs)` / `onRecordingError(error)` | Recording chunk-saved / stopped / started / error |

### Models (`api.model.*`)

**ConnectionState**: `DISCONNECTED` / `SCANNING` / `CONNECTING` / `CONNECTED` / `FAILED`

**CameraGroup**: `RGB`("rgb") / `TRACKING`("tracking") / `CTRL`("ctrl"); `CameraGroup.fromValue(v)` to reverse-lookup

**WifiStatus**: `IDLE` / `CONNECTING` / `CONNECTED` / `SERVERS_STARTED` (WS/file server ready, can stream) / `FAILED`

**DeviceInfo**: `batteryPercent`, `serialNumber`, `wifiSsid`, `vrIp`, `storageTotalMb`, `storageUsedMb`, `volume`, `maxVolume`, `projectEnabled`, `handEnabled`, `chunkDurationMin`, `uploadUrl`, `datasetDir`, `deviceModel`, `vrVersion`, `usbEnabled`, `recording`, `recordStartMs` (recording-timer local anchor; duration = localNow − this, immune to clock skew), `currentGroup`, `language` (VR system language, get/set_language)

**VideoStats**: `fps`, `byteRateKBps` (KB/s), `width`, `height`

**ImuData**: `accelX/Y/Z`, `gyroX/Y/Z`, `quatX/Y/Z/W`, `temperature` (`null` means no data for that axis)

**TimeSyncState** (reworked in v1.2):

| Field | Type | Description |
|---|---|---|
| `status` | `TimeSyncStatus` | Derived display state: `COLLECTING` / `CALIBRATED` / `STABLE` (≥60s) / `WARMING` (≥30s) / `NO_DATA` / `DISCONNECTED` / `ERROR`, computed from the pushed `state` + `duration_ms` |
| `offsetMs` / `rttMs` | `Double?` | Raw offset / RTT of the latest NTP exchange |
| `predictedOffsetMs` | `Double?` | Current measured offset (setClock scheme: VR corrects its system clock in real time, so predicted_offset_ms equals offset_ms; field kept for protocol compatibility) |
| `durationMs` / `totalMs` | `Int` | Elapsed / target collection time (offline-mode countdown derived via `remainingSec`) |
| `serialAvailable` | `Boolean` | Whether the PC time-sync service (serial) is reachable; startSync/offsetTest are no-ops when false |
| `onlineSync` | `Boolean` | VR sync mode: true = online continuous correction (default) / false = offline 30s calibration lock |
| `error` | `String?` | Error description |

Convenience: `remainingSec` (seconds left), `isOfflineCollecting` / `isOnlineCollecting`.

**XrDevice (scan result)**: `mac`, `name`, `serialNumber`, `rssi`

### Handshank / wrist camera (E6+ES202)

```kotlin
client.setHandshankListener(object : XrHandshankListener {
    override fun onHandshankStatus(side: HandshankSide, state: HandshankSideState) {}
    override fun onHandshankBondError(side: HandshankSide, error: String) {}
    override fun onWristDetail(side: HandshankSide, detail: WristDetail?) {} // detail=null = side not ready
    override fun onWristImu(side: HandshankSide, imu: WristImuData) {}
})
client.getHandshankStatus()          // auto re-queried once after connect (non-E2)
client.bondHandshank(HandshankSide.LEFT)
client.unbondHandshank(HandshankSide.RIGHT)
client.getWristDetail(HandshankSide.LEFT)
client.setWristImuEnabled(true)
client.setWristSurfaces(leftSurface, rightSurface) // render targets for the wrist group
client.updateVideoSurface(surface)   // re-deliver after the main-stream Surface is recreated
// StateFlow: handshankLeft / handshankRight / wristStreams / wristPlayback
client.switchGroup(CameraGroup.WRIST) // E6 wrist group: drop main stream → connect left/right wrist :8080 directly
```

- Capability flag `deviceInfo.wristSupported`: OR of the d303d_* bit field from get_device_info
  and wrist_supported from get_handshank_status; null = unknown.
- Capability flag `deviceInfo.es202Handshank`: es202_handshank from get_handshank_status
  (platform-determined by displayId on the VR side); null = not sent by older firmware.
  **Semantically independent of `wristSupported`** — that is the E2 wrist-camera capability.
  A CS02 device without E2 cameras still has an ES202 handshank. Use `es202Handshank` for
  handshank-model-specific scenarios (e.g. bind prompts); fall back to `wristSupported`
  when null (older firmware), matching the Flutter panel.
- Older ES201(E2) firmware does not recognize handshank commands; the SDK guards by model and does not send them.

## Connection State Machine

```
DISCONNECTED ──startScan()──> SCANNING ──stop/timeout──> DISCONNECTED
DISCONNECTED ──connect()────> CONNECTING ──success+creds written──> CONNECTED
                                │                                   │
                                └──5 attempts failed──> FAILED      └──disconnect/unexpected──> DISCONNECTED
```

> `SCANNING` never transitions directly to `CONNECTING`: after scanning stops the state returns to `DISCONNECTED`, then a device from the results is passed to `connect()`.

## Version

The AAR is named `xr-camera-control-v<x>.aar`, where `<x>` is the `sdkVersion` in `xr-camera-control/build.gradle.kts`, matching the version printed in Logcat at runtime. To upgrade, replace the old file in `libs/` (the `fileTree` dependency is insensitive to the filename version).

### v1.4 (current)

- New E6+ES202 wrist-handshank support: `XrHandshankListener` (status / bond error / wrist detail / wrist IMU), `bondHandshank` / `unbondHandshank` / `getHandshankStatus` / `getWristDetail` / `setWristImuEnabled` commands, `handshankLeft` / `handshankRight` / `wristStreams` / `wristPlayback` StateFlows, `setWristSurfaces` dual-stream rendering, `switchGroup(CameraGroup.WRIST)` wrist-group switching; capability flag `DeviceInfo.wristSupported`; ES201(E2) is guarded by model and handshank commands are not sent to it

### v1.3

- Aligned with VR-side auto time-sync (2026-08-24): `startSync()`/`cancelSync()` now `@Deprecated` (VR collects automatically; calls only get an error response). Status reporting unaffected

### v1.2

- **Time-sync model rework**: `TimeSyncState` now derives 7 states (COLLECTING/CALIBRATED/STABLE/WARMING/NO_DATA/DISCONNECTED/ERROR) from `state` + `duration_ms`; adds `predictedOffsetMs`/`serialAvailable`/`onlineSync`; removes `countdownSeconds`/`driftRatePpm`/`totalRounds` (countdown derived via `remainingSec`) — **breaking change**, migrate if you used the removed fields
- New `startSync()` / `cancelSync()` to manually control time-sync collection (new protocol commands `start_sync`/`cancel_sync`)
- Fixed occasional "cannot preview / cannot stop recording": 10s WS connect timeout + idempotent connect (aligned with the VR :8080 keep-new policy: the old connection is closed when a new one takes over); stale GATT callback filtering on BLE retry
- Dropped handling of legacy pushes `time_sync_countdown` / `time_sync_regression_complete`
- Fixed recording-duration display being inflated by days: aligned with ef924bb, `recording_started`/`get_state` now prefer the VR-pushed `record_elapsed_ms` (boottime monotonic clock) to derive the local anchor `recordStartMs` (= localNow − elapsed), keeping the whole duration in the local clock domain and immune to clock skew; on older VR without the field, an existing valid anchor is kept, otherwise (e.g. joining mid-recording) it falls back to localNow counting from 0 (it does NOT fall back to the VR-epoch `record_start_ms`, which would reintroduce clock-skew inflation)
- New `getLanguage()` / `setLanguage()` to read/write the VR system language (zh/en, new protocol commands `get_language`/`set_language`, result into `DeviceInfo.language`)

### v1.1

Protocol alignment (chunk duration / upload URL / power / delete data), bilingual comments, demo UI rework.

## ProGuard

The SDK's release build **does not enable minification** (`isMinifyEnabled = false`); internal packages (`ble`/`video`/`command`/`internal`) are kept as-is in the AAR for third-party troubleshooting, and the api boundary is enforced by Kotlin's `internal` modifier.

The SDK ships `consumer-rules.pro`: when the **host app** enables R8, it automatically keeps the SDK's `api` package from being obfuscated, so the host needs no extra config.
