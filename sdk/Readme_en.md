# LANGHUI EGO SDK User Guide

XR device multi-sensor data collection SDK: captures RGB / grayscale cameras, IMU, audio, poses, and hand tracking via OpenXR + camera extensions, recording them synchronously into a timestamped dataset.

## Table of Contents

- [1. Quick Start](#1-quick-start) — System properties / ADB commands / buttons / paths
- [2. Dataset](#2-dataset) — Directory structure / file formats / timestamp system / recording flow
- [3. Cameras & Coordinate Systems](#3-cameras--coordinate-systems) — Conventions / intrinsics & extrinsics / API→dataset conversion / pose transforms
- [4. Encoding Pipeline](#4-encoding-pipeline) — Hardware encoding / SBS / timestamps / formats
- [5. Developer Reference](#5-developer-reference) — SxrCameraApi / core structures / hand tracking
- [Appendix](#appendix) — Device hardware / screenshots & recording / MP4 analysis / notes

---

# 1. Quick Start

### System Properties

| Property | Values | Description | Takes Effect |
|------|------|------|------|
| `persist.xr.project_hand` | `1`/`0` | Project hand skeleton onto encoded video (does not affect headset preview) | After reboot |
| `persist.xr.project_controller` | `1`/`0` | Project controller coordinate axes onto encoded video (does not affect headset preview) | After reboot |
| `persist.sxr.cam.rgb.fps` | `30`/`60` | RGB camera frame rate (default 30) | After app restart |
| `persist.xr.dataset_type` | `mp4`/`mcap` | Dataset format (default mp4; `mcap` is a single-file MCAP, see [mcap-dataset-format.md](mcap-dataset-format.md) for details) | Next recording |

```bash
adb shell setprop persist.xr.project_hand 1        # Enable skeleton projection
adb shell setprop persist.xr.project_controller 1   # Enable axes projection
adb shell setprop persist.sxr.cam.rgb.fps 60       # Switch RGB to 60fps
adb shell setprop persist.xr.dataset_type mcap     # Switch dataset to single-file MCAP
```

### ADB Remote Commands

```bash
# Screenshot (all cameras → .../files/images/)
adb shell am broadcast -a com.ssnwt.helloxr.SAVE_IMAGE

# Dataset recording (→ .../files/dataset/<YYYYMMDD_HHMMSS>/)
adb shell am broadcast -a com.ssnwt.helloxr.START_RECORDING
adb shell am broadcast -a com.ssnwt.helloxr.STOP_RECORDING

# Pull dataset
adb pull /sdcard/Android/data/com.ssnwt.helloxr/files/dataset/ ./dataset/

# Restart app / view logs
adb shell am force-stop com.ssnwt.helloxr && sleep 1 \
  && adb shell am start -n com.ssnwt.helloxr/com.ssnwt.helloxr.VrNativeActivity
adb logcat | grep -E "HelloXr|sxrcam|DatasetRecorder|Encoder"
```

### Controller Buttons

| Button | Function |
|------|------|
| VOLUME_UP | Screenshot (all cameras) |
| Right A (select/click) / DPAD_CENTER | Toggle recording start/stop |

### Data Paths

```
/sdcard/Android/data/com.ssnwt.helloxr/files/
├── dataset/<YYYYMMDD_HHMMSS>/   # Dataset
└── images/                      # Screenshots (triggered by VOLUME_UP)
```

---

# 2. Dataset

> This section describes the default **multi-file MP4 format**. If `persist.xr.dataset_type=mcap`, the directory contains only a single `<YYYYMMDD_HHMMSS>.mcap` file (all channels + attachments embedded in one file); see [mcap-dataset-format.md](mcap-dataset-format.md) for the format and downstream consumption.

## 2.1 Directory Structure

```
dataset/<YYYYMMDD_HHMMSS>/
├── rgb.mp4 / rgb_metainfo.csv           # RGB SBS video (2W×H, H.265, 30fps) + per-frame metadata
├── tracking.mp4 / tracking_metainfo.csv # Tracking grayscale (W×H, H.265, 60fps) + metadata
├── ctrl.mp4 / ctrl_metainfo.csv         # Ctrl grayscale (W×H, H.265, 60fps) + metadata
├── audio.m4a / audio_metainfo.csv       # AAC audio (44.1kHz, mono, 96kbps) + metadata
├── accel.csv / gyro.csv                 # IMU accelerometer / gyroscope
├── head_pose.csv                        # 6DOF head pose
├── hand_tracking.csv                    # Hand joints (always present; only a header blank row when no hands)
├── controller_poses.csv                 # Controller poses (always present; only a header blank row when no controllers)
├── imu_calibration.json                 # IMU calibration sidecar
└── camera_params_{rgb,tracking,ctrl}.json  # Camera intrinsics / extrinsics
```

**Format version `version`** — the first field of `camera_params_*.json` / `imu_calibration.json` and the `datasetFormatVersion` key of the mcap `session` metadata carry the same dataset format version, incremented whenever the field structure or coordinate conventions change. The extrinsics convention switched to **convention 1 in tag 1.7.0** (after 1.6.0; tag ≤ 1.6.0 used the deprecated convention 0 conjugated output):

| version | Applies to | Meaning |
|---|---|---|
| absent | tag ≤ 1.6.0 | Historical: legacy extrinsics convention (convention 0 conjugated output, deprecated) |
| absent | tag 1.7.0 – 1.8.1 | Historical: new convention (convention 1), same semantics as v2, only without the version field |
| `2` | tag ≥ 1.8.2 | Current format: raw OpenXR Body + stored-image optical camera frame (X image-right/Y image-down/Z optical-forward) + extrinsics convention 1 + raw sensor-frame IMU bias |

The SDK's own release version lives in the repo-root `VERSION` file (single line, e.g. `1.8.2`) — update it when tagging and tag the same commit; the build injects it as the `SDK_VERSION` macro, visible in startup logcat (`adb logcat | grep HelloXr`) and in the mcap `session` metadata `sdkVersion` key.

**File size reference (~10 seconds)**: rgb.mp4 ~10MB · tracking/ctrl.mp4 ~5MB · audio.m4a ~120KB · accel/gyro.csv ~300KB · head_pose.csv ~50KB · hand_tracking.csv ~100KB.

## 2.2 File Formats

**Media + metadata** — Video/audio are **fragmented MP4** (`FMP4Writer`, crash-safe). Per-frame timestamps/exposure/gain are not stored in the media files; they are written to `*_metainfo.csv` (header row first, appended per sample):

| File | Schema |
|------|--------|
| `rgb/tracking/ctrl_metainfo.csv` | `frame_index,frame_id,pts_us,exposure_start_utc_ns,exposure_duration_ns,gain,mid_exposure_utc_ns` |
| `audio_metainfo.csv` | `packet_index,pts_us,capture_utc_ns` |

- `pts_us`: zero-based microseconds, matching the mp4 sample presentation time.
- `*_utc_ns`: absolute UTC (see [2.3 Timestamp System](#23-timestamp-system)). `mid_exposure_utc_ns` = exposure center, recommended as the image sampling time reference.

**IMU** — `accel.csv` / `gyro.csv`: `timestamp_ns,x,y,z` (accelerometer m/s², gyroscope rad/s; timestamp is UTC ns, see 2.3).

**Poses**:
- `head_pose.csv`: `timestamp_ns,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w` (pose of the Body frame in the World frame).
- `hand_tracking.csv`: `frame_number,timestamp,left_active,right_active,{left|right}_joint{i}_{id,name,radius,pos_x,pos_y,pos_z,orientation_x,y,z,w}` (26 joints per hand, 10 fields per joint; Root Space, meters). Always written; when no hands are present, only the first invalid row (active=0) is saved.
- `controller_poses.csv`: `frame_number,timestamp_ns,left_active,left_px..qw,right_active,right_px..qw`. Always written; when no controllers are present, only the first invalid row (active=0) is saved.

**IMU calibration `imu_calibration.json`** — Written at recording start; records the device IMU calibration parameters (raw accel/gyro in CSV; camera extrinsics in `camera_params_*.json`). The first field `version` is the dataset format version (see 2.1). Strict JSON.

| Field | Meaning | Unit |
|------|------|------|
| `device_uid` | Unique device ID | — |
| `imu.imu_id` / `imu.is_primary` | IMU index / whether it is the primary IMU | — / bool |
| `imu.bias.accelerometer_mps2` | Accelerometer tri-axis bias (constant offset) | m/s² |
| `imu.bias.gyroscope_rads` | Gyroscope tri-axis bias | rad/s |
| `imu.scale_factor.accelerometer` / `.gyroscope` | Tri-axis scale factor (deviation from 1.0) | dimensionless |
| `imu.nonorthogonality.accelerometer` / `.gyroscope` | Tri-axis non-orthogonality (cross-axis coupling) | dimensionless |
| `imu.time_alignment_s.imu_to_pose` | IMU↔pose (tracking) time offset, from calibration file `<Stateinit delta>` (second-level relative offset; add directly when both timestamps share the same domain) | s |
| `imu.time_alignment_s.cameras.<cam_name>` | Per-camera IMU↔camera time offset (`trackingA`/`trackingB`/`ctrl-trackingA`/`ctrl-trackingB`/`rgb-left`/`rgb-right`) | s |
| `imu.time_alignment_s.accel` | Additional accelerometer time offset relative to gyroscope, from `<Stateinit accelDelta>` | s |
| `noise.accel_noise_std_mps2` | Accelerometer measurement noise std σ_a | m/s² |
| `noise.gyro_noise_std_rads` | Gyroscope measurement noise std σ_g | rad/s |
| `noise.accel_bias_std_mps2` | Accelerometer bias random-walk noise σ_ba (empirical default; not provided by calibration file) | m/s² |
| `noise.gyro_bias_std_rads` | Gyroscope bias random-walk noise σ_bg (empirical default; not provided by calibration file) | rad/s |

**Camera parameters `camera_params_*.json`** — Saved on the first valid frame; static parameters:

```json
{
  "version": 2,
  "group": "rgb",
  "cameras": [
    { "eye": "left", "width": 2328, "height": 1748,
      "intrinsics": {
        "focalX": 1374.56, "focalY": 1369.12,
        "centerX": 1159.37, "centerY": 878.45,
        "radialDistortion": [0.0471,-0.0213,0.0038,-0.0002, 0,0,0,0]
      },
      "extrinsics": {
        "position": [0.032,-0.015,0.002],   // Camera optical center in Body frame (m)
        "rotation": [0.0,0.0,0.0,1.0]        // Camera→Body quaternion [x,y,z,w]
      }
    },
    { "eye": "right", /* ... */ }
  ]
}
```

All pose data uses the **OpenXR Body coordinate system** (see 3.1); the SDK outputs it as-is, with no coordinate conversion.

## 2.3 Timestamp System

**All data streams share a single absolute UTC timeline and are mutually consistent.** At recording start, a one-shot `BOOTTIME→REALTIME` offset is sampled (`offset = CLOCK_REALTIME - CLOCK_BOOTTIME`); every stream's raw `CLOCK_BOOTTIME` timestamp has this offset added before being written:

- **Video** (`*_metainfo.csv` `exposure_start_utc_ns` / `mid_exposure_utc_ns`): raw source is the camera frame `start_of_exposure_ts` (kernel `CLOCK_BOOTTIME`), `+ offset → UTC`.
- **Audio** (`capture_utc_ns`): reconstructed from a `CLOCK_BOOTTIME` base + codec PTS, `+ offset → UTC`.
- **IMU** (`accel/gyro.csv` `timestamp_ns`): Android `ASensorEvent.timestamp` (`CLOCK_BOOTTIME`), `+ offset → UTC`.
- **Poses** (`head_pose.csv` etc. `timestamp_ns`): `CLOCK_BOOTTIME`, `+ offset → UTC`.

**Alignment to RGB**: pose / hand / controller are queried directly from the OpenXR historical pose (`xrLocateSpace(camXrTime)`) at the RGB **mid-exposure** instant inside the RGB frame callback, and re-stamped to the RGB mid-exposure — measured residual is sub-microsecond. The IMU is an independent high-rate stream (~1 kHz, per-sample UTC); align it in post-processing by interpolating against `mid_exposure_utc_ns`. tracking/ctrl are independent 60 fps streams (RGB is 30 fps); match timestamps by their respective mid-exposures.

```bash
# Quantify per-frame alignment error (nearest-neighbor pose/hand, IMU residual, cross-camera sync)
python3 dataset_analysis/analyze_alignment.py <dataset_dir>
```

## 2.4 Recording Flow

```
START_RECORDING (button/Intent)
  └─ DatasetRecorder.start()
       ├─ Create dataset/<YYYYMMDD_HHMMSS>/
       ├─ AudioEncoder → audio.m4a ; ImuPoseCollector → accel/gyro.csv
       ├─ ControllerPoseSaver.StartSession() → controller_poses.csv
       ├─ RawDateSave.StartNewSession() → hand_tracking.csv
       ├─ Write imu_calibration.json (calls qxr's IMU calibration getter)
       └─ head_pose writer thread → head_pose.csv

Per-frame render loop:
  ├─ saveHeadPose(devicePose)           # xrLocateSpace(viewSpace) → boottime → head_pose.csv
  ├─ Camera callback → encoder → rgb/tracking/ctrl.mp4
  ├─ Camera callback → saveAlignedSensorData()  # pose/hand/controller written aligned to RGB mid-exposure
  ├─ Camera callback → saveCameraParams()       # first valid frame → camera_params_*.json
  ├─ ControllerPoseSaver.SaveFrame()      # always written (blank first row if no controller)
  └─ RawDateSave.SaveFrame()              # always written (blank first row if no hand)

STOP_RECORDING → flush all encoders/collectors → stopEncoder() (async)
```

Core components (see source): `DatasetRecorder` (central coordinator), `ImuPoseCollector` (IMU, sensor thread + writer thread), `AudioEncoder` (AAudio→AMediaCodec AAC), `ControllerPoseSaver` / `RawDateSave` (async thread writing CSV).

---

# 3. Cameras & Coordinate Systems

## 3.1 Coordinate Conventions

**API interfaces** (libcamera callbacks, OpenXR) use the **OpenXR Body coordinate system**.

```
OpenXR Body coordinate system (API returns): X right, Y up, Z backward (toward behind the user)

        Y (up)
        ↑
        |
        o————► X (right)
         \
          ↘ Z (backward)
```

**Dataset**: all pose and IMU data **as-is** uses the **OpenXR Body coordinate system** returned by the API (same as the API coordinate system above); the SDK performs no coordinate conversion.

If downstream needs OpenCV Body (X right, Y down, Z forward), convert poses / IMU yourself via `[x, -y, -z]` (flip Y and Z): position `(x, y, z) → (x, -y, -z)`, quaternion `(qx, qy, qz, qw) → (qx, -qy, -qz, qw)`.

> **Note**: in the current format (convention 1, tag ≥ 1.7.0) the camera side of the `camera_params` extrinsics is already the **stored-image optical frame** (X image-right / Y image-down / Z optical-forward; the 90° portrait-mounting roll of the tracking cameras is included in R). To use the extrinsics with OpenCV, only the Body flip `[x,-y,-z]` is needed — see 3.3. Only legacy convention 0 data (tag ≤ 1.6.0) has the SVR camera frame (X up / Y right / Z forward) on the camera side, which needs an additional right-multiplied basis change (see the end of 3.3).

- Pitch around X, Yaw around Y, Roll around Z.
- Pose unit: meters; quaternion order `(x, y, z, w)`.

## 3.2 Intrinsics

OpenCV standard convention, directly usable as the K matrix; distortion is Kannala-Brandt fisheye (first 4 coefficients).

```python
import numpy as np, cv2
K = np.array([[focalX,0,centerX],[0,focalY,centerY],[0,0,1]], dtype=np.float64)
D = np.array(radialDistortion[:4], dtype=np.float64).reshape(1,4)
map1,map2 = cv2.fisheye.initUndistortRectifyMap(K,D,np.eye(3),K,(w,h),cv2.CV_32FC1)
undistorted = cv2.remap(img,map1,map2,cv2.INTER_LINEAR)
```

## 3.3 Coordinate Systems (API → Dataset)

The extrinsics returned by the libcamera API callback (`FrameInfo.position / rotation`) use the **OpenXR Body coordinate system** (SVR convention, X right, Y up, Z backward). When writing the dataset, the SDK **stores them as-is**, with no coordinate conversion — `head_pose` / `hand_tracking` / `controller_poses` / `accel` / `gyro` / `camera_params` all stay in OpenXR Body. If downstream needs OpenCV Body, convert poses / IMU yourself via `[x, -y, -z]` (flip Y and Z; see 3.1).

Since **tag 1.7.0** the extrinsics are produced by libcamera in **extrinsics convention 1** (`sxr_camera_set_extrinsics_convention(&api, 1)`, which left-multiplies `Rz(90°)` on the rotation side relative to convention 0, converting the camera side from the SVR camera frame to the stored-image optical frame; tag ≤ 1.6.0 used the deprecated convention 0 conjugated output): Body side = OpenXR Body, camera side = **stored-image optical frame (X image-right / Y image-down / Z optical-forward)**, i.e. the `camera_params` extrinsics are Camera(stored-image optical frame) → Body(OpenXR). The tracking cameras are portrait-mounted, so the stored image is rotated 90° relative to the scene; this roll is included in R (decode tracking.mp4 to see the content lying sideways: scene-up = image-left).

| Layer | Coordinate System | Notes |
|---|---|---|
| libcamera API callback | OpenXR Body (SVR) | Raw output of `FrameInfo.position/rotation` |
| `head_pose.csv` / `hand_tracking.csv` / `controller_poses.csv` | OpenXR Body | Written as-is, no conversion |
| `accel.csv` / `gyro.csv` | OpenXR Body | Android sensor frame = OpenXR Body, written as-is, no conversion |
| `camera_params_*.json` | OpenXR Body | Extrinsics written as-is, no conversion (convention 1: Body = OpenXR, camera frame = stored-image optical: X image-right / Y image-down / Z optical-forward) |

**Write chain** (SDK passes through as-is, no coordinate conversion):

```
libcamera API
  │ FrameInfo.position/rotation (OpenXR Body / SVR)
  │
  ├─► saveCameraParams():  as-is → camera_params_*.json
  ├─► DatasetRecorder:     as-is → head_pose.csv
  ├─► ImuPoseCollector:    as-is → accel.csv / gyro.csv
  ├─► RawDateSave:         as-is → hand_tracking.csv
  ├─► ControllerPoseSaver: as-is → controller_poses.csv
  │
  └─► feedOverlayCameraParams:  SVR→conj(Rz(90°)) → HandOverlayRenderer
                                 (projection compensates for the RGB image's 90° mounting angle;
                                  unrelated to the dataset coordinate system)
```

**Extrinsics** represent **Camera → Body** (Tbc): `X_body = R @ X_camera + t`. `position` = camera optical center position in the Body frame (m), `rotation` = Camera→Body quaternion `(x,y,z,w)`.

Intrinsics follow the OpenCV standard camera model (K + KB fisheye distortion), directly usable with `cv2.fisheye`. The Body side of the extrinsics position/rotation is raw **OpenXR Body**; when consuming downstream as OpenCV Body, poses / IMU only need the Y/Z flip (see 3.1), and the extrinsics likewise **only need the Body flip** (the camera side is already the stored-image optical frame — no basis change):

```python
import numpy as np

F = np.diag([1.0, -1.0, -1.0])  # OpenXR Body → OpenCV Body ([x,-y,-z], 180° about X)

R_cv = F @ R_svr   # rotation: body flip only
t_cv = F @ t_svr   # translation: just [x,-y,-z]
```

Sanity check (measured on a tag 1.8.2 recording, rgb-left): `R_svr ≈ diag(1,-1,-1)` → `R_cv ≈ I` (only ±2° mounting tilt), i.e. the forward RGB camera's optical axes coincide with the OpenCV Body axes (X right / Y down / Z forward); decoding rgb.mp4 shows the scene upright (image-right = scene-right), consistent with this.

> **Legacy data (tag ≤ 1.6.0, convention 0)**: the camera side is the SVR camera frame (X up / Y right / Z forward) and needs an additional right-multiplied basis change `H.T` (`H = [[0,1,0],[-1,0,0],[0,0,1]]`), i.e. `R_cv = F @ R_svr @ H.T`; the measured rgb-left value back then was `R_svr ≈ [[0,1,0],[1,0,0],[0,0,-1]]`, which also yields `R_cv ≈ I` under this recipe. `load_cameras()` in `camera_analysis.py` has been updated accordingly: it uses the extrinsics as-is (the analysis only relies on camera-to-camera relative geometry, which is invariant to any global Body-side rotation), valid for convention 1 data; for convention 0 legacy data, right-multiply `Hᵀ` yourself.

```python
import json, numpy as np

def quat_to_rotmat(q):  # q = [x,y,z,w]
    x,y,z,w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-w*z),   2*(x*z+w*y)],
        [2*(x*y+w*z),   1-2*(x*x+z*z), 2*(y*z-w*x)],
        [2*(x*z-w*y),   2*(y*z+w*x),   1-2*(x*x+y*y)]])

def load_camera_params(json_path):
    with open(json_path) as f: data = json.load(f)
    out = {}
    for cam in data['cameras']:
        intr, extr = cam['intrinsics'], cam['extrinsics']
        K = np.array([[intr['focalX'],0,intr['centerX']],
                      [0,intr['focalY'],intr['centerY']],
                      [0,0,1]], dtype=np.float64)
        D = np.array(intr['radialDistortion'][:4], dtype=np.float64).reshape(1,4)
        R = quat_to_rotmat(extr['rotation'])
        t = np.array(extr['position'], dtype=np.float64)
        out[f"{data['group']}-{cam['eye']}"] = {'width':cam['width'],'height':cam['height'],'K':K,'D':D,'R':R,'t':t}
    return out
```

**Camera-pair relative transform** (A→B, both in Body frame): `R_BA = R_Bᵀ @ R_A`, `t_BA = R_Bᵀ @ (t_A - t_B)`.

## 3.4 Pose Transform (World → Body → Camera)

To compute the camera's pose in the World frame, the transform chain is uniformly **World → Body → Camera** (`xrViewSpace` = Body/head frame):

```
World (W) ──T_WB──► Body (B) ──T_BC──► Camera (C)      T_WC = T_WB × T_BC
```

```cpp
// 1. Body pose in World T_WB (OpenXR view space = body/head frame)
xrLocateSpace(xrViewSpace, xrRootSpace, timestamp, &location);
glm::quat q_wb(location.pose.orientation.w, location.pose.orientation.x,
               location.pose.orientation.y, location.pose.orientation.z);
glm::vec3 p_wb(location.pose.position.x, location.pose.position.y, location.pose.position.z);

// 2. Extrinsics T_BC: Camera pose in Body (OpenXR Body coordinate system, raw API output)
glm::quat q_bc(frameInfo.rotation[3], frameInfo.rotation[0],
               frameInfo.rotation[1], frameInfo.rotation[2]);
glm::vec3  p_bc(frameInfo.position[0], frameInfo.position[1], frameInfo.position[2]);

// 3. Compose T_WC
glm::quat q_wc = q_wb * q_bc;
glm::vec3  p_wc = glm::rotate(q_wb, p_bc) + p_wb;
```

Grayscale cameras (4: GRAY_LEFT/RIGHT/LEFT_UP/RIGHT_UP) also get their extrinsics from `SXR::FrameInfo.position/rotation` (Body frame).

## 3.5 Verification Tools

```bash
python3 camera_analysis.py <dataset_dir>   # Outputs camera_analysis_report.html
python3 visualize_imu_camera_positions.py <dataset_dir> [--opencv]   # Outputs imu_camera_positions.html
```

Analysis: pairwise reprojection error across 6 cameras, stereo rectification epipolar error, extrinsic accuracy (E_sim), RGB stereo baseline.

`visualize_imu_camera_positions.py`: interactive 3D visualization (plotly HTML) of the IMU–camera spatial layout — IMU at the origin, with camera centers, IMU→camera distances (mm), and per-group stereo baseline annotations. Displayed in the OpenXR Body frame (X right/Y up/Z backward) by default; `--opencv` switches to the OpenCV Body frame (X right/Y down/Z forward, Body flip `[x,-y,-z]` only — see 3.3).

---

# 4. Encoding Pipeline

## 4.1 Architecture

Hardware-accelerated **Surface mode** encoding (zero-copy). RGB stitches left/right eyes via an FBO into **SBS** and encodes it as a single mp4; grayscale cameras go through a YUV→grayscale shader before encoding. Each camera group uses an **independent EGL context** (no lock contention).

```
Camera callback ──► handleRGBFrame: AHardwareBuffer×2 → GL_TEXTURE_EXTERNAL_OES
            → SBS Stitch FBO (left viewport=0, right=W) → encoder surface
            → eglPresentationTimeANDROID → MediaCodec → rgb.mp4 (2W×H SBS)
          └► handleCVFrame: AHardwareBuffer → GL Texture → YUV→grayscale shader
            → encoder surface → MediaCodec → tracking.mp4 / ctrl.mp4
```

Timestamp units: camera frames ns, MediaCodec µs, OpenXR XrTime ns (`us = ns/1000`). `CameraEncoder::submitFrameMeta()` writes per-frame metadata to `*_metainfo.csv`.

## 4.2 Fragmented MP4 + Timestamps

`FMP4Writer` (pure POSIX file I/O, shared by `CameraEncoder`/`AudioEncoder`): `open()` → `setVideoTrack/setAudioTrack` → `start()` writes out `ftyp + moov` (up front); afterwards each `writeSample(data,size,ptsUs,isSync)` appends one `styp+moof+mdat` fragment (one per sample).

```
ftyp + moov (up front: track config + SPS/PPS/VPS or AAC csd)
styp + moof + mdat  ← sample 0 (first frame is an I-frame, PTS=0)
styp + moof + mdat  ← sample 1 ...
```

- **Crash-safe**: `moov` up front + independent fragment per sample; even on truncation or force-kill, playback works up to the last complete fragment.
- Video PTS is **zero-based** (first frame = 0); HEVC `max-bframes=0` (I/P frames only), so decode order == presentation order, and PTS is strictly monotonic.

Surface-mode PTS setup (call `eglPresentationTimeANDROID` before swapBuffers; the encoder internally converts to µs for PTS).

## 4.3 Encoding Formats

| Stream | Codec | Resolution | Frame Rate | Bitrate |
|----|------|--------|------|------|
| RGB (SBS) | H.265 | 2W×H | 30 (`persist.sxr.cam.rgb.fps` can set 60) | 8 Mbps |
| Tracking / Ctrl | H.265 | W×H | 60 | 4 Mbps |
| Audio | AAC-LC | — | 44.1 kHz mono | 96 kbps |

H.264/H.265 is switchable (see the `CameraEncoder` constructor; default is H.265). Key technical details (EGL context sharing, AHardwareBuffer→GL Texture, VAO across contexts, texture coordinate flipping, grayscale shader, SBS rendering pipeline) are implemented in `app/src/main/cpp/` source code.

---

# 5. Developer Reference

## 5.1 SxrCameraApi (Dynamic Loading)

Loaded via `dlopen("libsxr_camera_client.so")`; function-pointer table:

```cpp
struct SxrCameraApi {
    void* libHandle;
    SxrCameraCreateFunc         create;
    SxrCameraDestroyFunc        destroy;
    SxrCameraOpenGroupFunc      open_group;
    SxrCameraCloseGroupFunc     close_group;
    SxrCameraIsGroupOpenFunc    is_group_open;
    SxrCameraGetGroupInfoFunc   get_group_info;
    SxrCameraGetImuCalibrationFunc get_imu_calibration;  // optional symbol; returns -1 if missing
};
int  sxr_camera_api_init(SxrCameraApi* api, const char* libPath);  // NULL uses default lib
bool sxr_camera_api_is_valid(const SxrCameraApi* api);
```

## 5.2 Core Structures

```cpp
enum class CameraGroup : uint8_t { TRACKING=0, CTRL, RGB, DEPTH };

struct FrameInfo {
    uint32_t frameId; uint64_t timestamp;   // start_of_exposure_ts (CLOCK_BOOTTIME, ns)
    uint32_t exposure, gain;
    uint32_t width, height, stride, format, cropX, cropY;
    float focalX, focalY, centerX, centerY;            // OpenCV standard intrinsics
    float radialDistortion[8];                          // KB fisheye (first 4 non-zero)
    float position[3];   // Camera optical center in Body frame (m), OpenXR Body, consistent between API and dataset (no conversion)
    float rotation[4];   // Camera→Body quaternion (x,y,z,w), OpenXR Body, consistent between API and dataset (no conversion)
};
struct FrameData { CameraGroup group; FrameInfo frames[2]; /* [0]=left, [1]=right */
                   uint8_t hwBufferCount; AHardwareBuffer* hwBuffer[2]; };
typedef void (*FrameCallback)(void* userData, const FrameData* data);
```

## 5.3 Hand Tracking

Depends on OpenXR extensions such as `XR_EXT_hand_tracking` (API layer `XR_APILAYER_QCOM_handtracking`). 26 joints per hand (PALM/WRIST/THUMB_*…/LITTLE_TIP); data is in **Root Space** (global SLAM coordinate system; the app tries `xrCreateRootSpaceQCOM` at startup, falling back to `LOCAL` space on failure).

Core components: `HandTrackerLogic` (left/right hand trackers + joint localization), `Input` (OpenXR action input), `RawDateSave` (async thread writing `hand_tracking.csv`, session-based). Runs alongside `ControllerPoseSaver` (dual-track mode), with no `useController` property switching needed.

## 5.4 Overlay Projection onto RGB

Both hand skeleton (`persist.xr.project_hand=1`) and controller coordinate axes (`persist.xr.project_controller=1`) can be overlaid onto the encoded video. Neither affects the headset preview; they can be enabled independently or together, and require an app restart to take effect.

**Hand skeleton chain**:

```
HandJoint(RootSpace) → World → Camera → KB fisheye projection → 2D pixel → 90° clockwise about center → final pixel
```

```cpp
// World→Camera: extrinsics + head pose quaternion chain (body→world + camera→body)
R_WC = conj(extQuat) * conj(headQuat);
cam   = quatRotate(R_WC, joint - wcPos);        // wcPos = devicePos + quatRotate(deviceQuat, extPos)
// 90° clockwise rotation after KB projection (matches the Python reference implementation)
u_rot = centerX + (v - centerY);
v_rot = centerY - (u - centerX);
```

**Controller axes chain**:

```
ControllerPose(RootSpace) → KB fisheye origin projection → fixed-length pixel axis segments (25px)
    Red X right →, Green Y up ↑, Blue Z lower-right ↘ (screen pixel space)
```

The controller origin is projected through World→Camera→KB fisheye to a 2D pixel coordinate; the axis arms are fixed-length 25 px segments (not scaled by distance), drawn directly on the encoded frame. The projection logic reuses `HandOverlayRenderer`'s KB projection + 90° CW rotation to ensure consistency with the hand projection coordinates.

---

# Appendix

## A. Device Hardware Features

Integrated hardware test capabilities (`svr_plugin_android_api.aar`):

- **LED**: `DeviceUtils.flashLed(type)` / `blinkLed(type,on,off)` (1=red, 2=green, 3=blue).
- **Audio recording**: `MediaRecorder` (MIC → AAC/M4A, 44.1 kHz/96 kbps).
- **Battery**: `BroadcastReceiver`(ACTION_BATTERY_CHANGED) + `BatteryManager` (level/voltage/temperature/capacity/current, etc.).
- **System events**: `SystemEventUtils.Listener` (onStartHome / onRecenter / onKeyEvent, etc.).
- **AndroidInterface**: initialize via `AndroidInterface.getInstance().init(...)`.

## B. Screenshots & Recording

- Trigger: `VOLUME_UP` for screenshot; `Right A`/`DPAD_CENTER` to toggle recording; also via `am broadcast` (see [1. ADB](#adb-remote-commands)).
- Screenshots saved to `.../files/images/`: `rgb_*.png` (RGB left/right eye stitched), `tracking_*.png`, `ctrl_*.png`.
- Recording parameters in [4.3](#43-encoding-formats). Voice prompts (TTS) + Intent remote control (including WiFi ADB cross-device).

## C. MP4 Analysis Commands

```bash
ffprobe -v quiet -show_format -show_streams rgb.mp4                      # Structure (fragmented, single media track)
ffprobe -v quiet -select_streams 0 -count_packets -show_entries stream=nb_read_packets -of csv=p=0 rgb.mp4  # Frame count
ffprobe -v quiet -select_streams v -show_entries packet=pts_time -of csv=p=0 rgb.mp4 | head                # PTS (first frame 0.0)
# Cross-check *_metainfo.csv row count against mp4 packet count
```

## D. Notes

- **Permissions**: `CAMERA` / `RECORD_AUDIO` / `WRITE_EXTERNAL_STORAGE`. Uninstalling and reinstalling clears permissions and datasets.
- **Storage**: long recordings produce large amounts of data; watch available space.
- **OpenXR runtime**: requires `com.qualcomm.qti.openxrruntime` / `spaces.services`; projection toggles require a reboot to take effect.
- **Stopping recording**: `stopEncoder` is asynchronous; confirm it has completed before recording again (`stopInProgress` flag).
- **Hand/controller projection**: independently controlled by `persist.xr.project_hand` / `persist.xr.project_controller`; affects only the encoded video, not drawn in the headset preview.

## Related Files

```
app/src/main/cpp/
├── main.cpp                    # App main logic, camera callbacks, recording orchestration, alignment
├── DatasetRecorder.{h,cpp}     # Dataset recording coordinator
├── CameraEncoder.{h,cpp}       # MediaCodec video encoding (Surface mode)
├── AudioEncoder.{h,cpp}        # AAC audio encoding
├── FMP4Writer.{h,cpp}          # Fragmented MP4 writer
├── ImuPoseCollector.{h,cpp}    # IMU collection
├── HandOverlayRenderer.{h,cpp} # Hand skeleton + controller axes projection
├── sxr_camera.h / sxr_common.h # Camera API (dynamic loading) + shared structures
└── camera_analysis.py          # Intrinsics/extrinsics verification (generates HTML report)
dataset_analysis/
├── analyze_alignment.py        # Timestamp alignment quantification
└── analyze_all_timestamps.py   # Timestamp analysis
```
