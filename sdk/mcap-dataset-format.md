# MCAP 数据集格式说明

> 适用：VR 端 `persist.xr.dataset_type=mcap` 录制产物。
> 读完本文应能回答：**MCAP 是什么、下游怎么适配消费**。
> 修订：2026-08-26（`/tf_static` 改为原始 SVR 外参不转换，child frame 更名 `*_svr_frame`）

---

## 1. MCAP 是什么

[MCAP](https://mcap.dev) 是 Foxglove 开源的通用机器人日志容器格式：单文件内按 **topic（通道）** 组织多模态消息（视频、IMU、位姿、TF、标定……），每条消息带 `log_time` 时间戳，支持 chunk 分块 + summary/index 索引，可随机 seek。可以把它理解为"可索引、可 seek 的单文件版 rosbag"，且不强依赖 ROS。

本项目 VR 端在 `persist.xr.dataset_type=mcap` 时，**一次录制产出一个 MCAP 文件**，视频/传感器/标定全部写入通道，音频等无法进通道的内容以 **附件（Attachment）** 形式嵌入同一文件——**数据集目录中只有这一个文件**：

```
dataset/<YYYYMMDD_HHMMSS>/
└── <YYYYMMDD_HHMMSS>.mcap   # 唯一产物：全部通道 + 6 个附件 + session metadata
```

- 容器库：官方 [foxglove/mcap](https://github.com/foxglove/mcap) C++ 库，pin [`releases/cpp/v2.1.3`](https://github.com/foxglove/mcap/tree/releases/cpp/v2.1.3/cpp/mcap)。
- 写入参数：**无压缩**（compression=None）、chunk + summary/index、每 500 条消息落盘一次。
- 文件首帧为 IDR（关键帧已注入 VPS/SPS/PPS 参数集），独立可解码。

---

## 2. Topic 一览（路径含义与主要数据）

所有 `log_time` 均为 **UTC ns**（`CLOCK_BOOTTIME + (REALTIME−BOOTTIME)` 偏移），与 MP4 数据集同一时间轴；视频帧取 **mid-exposure**（曝光中点）。`publishTime` 恒等于 `log_time`。

### 视频通道

| Topic | Schema（encoding） | 内容 |
|---|---|---|
| `/camera/<group>/left` `/camera/<group>/right` | `foxglove.CompressedVideo`（jsonschema / json） | 每眼一路 HEVC。`<group>` ∈ `rgb`（每眼 2328×1748@30fps）、`tracking`（灰度每眼 640×480@60fps）、`ctrl`（灰度每眼 640×480@60fps）。准确分辨率以 `/camera/<group>/<eye>/info` 中的 `width/height` 为准 |

消息 JSON：

```json
{
  "timestamp": {"sec": 1787055267, "nsec": 546000000},
  "frame_id": "camera_rgb_left_optical_frame",
  "format": "h265",
  "data": "<base64(HEVC AnnexB)>"
}
```

- `data` 为 AnnexB 裸流（start-code 分隔），**关键帧前已注入 VPS/SPS/PPS**，直接送 H.265 解码器即可。
- 注意 timestamp 字段名是 **`nsec`**（视频通道），其余通道用 `nanosec`。

| Topic | Schema | 内容 |
|---|---|---|
| `/camera/<group>/metainfo` | 自定义 `metainfo`（jsonschema / json） | `{"frame_index": n, "ts_ns": <UTC ns>}`，**仅左眼帧写入**（每组一条/帧对），`frame_index` 从 0 起，与该组视频帧一一对应 |

### 位姿与轨迹

| Topic | Schema（encoding） | 内容 |
|---|---|---|
| `/head_pose` | `geometry_msgs/PoseStamped`（ros2msg / cdr） | Body（头显）在 World 中的 6DoF 位姿，`frame_id="world"`，位置四元数 `(x,y,z,w)`。在 RGB 帧回调中按 RGB mid-exposure 时刻查 OpenXR 历史位姿对齐写入（~30Hz） |
| `/head_pose/path` | `nav_msgs/Path`（ros2msg / cdr） | **轨迹的有界累积快照**：每条消息是从录制起点到当前时刻的全部历史位姿点（非增量），快照间隔 = max(500ms, 已录时长/200)，单快照超 3000 点均匀抽稀（保留最新点）。Foxglove 3D 面板直接显示轨迹 |
| `/tf` | `tf2_msgs/msg/TFMessage`（ros2msg / cdr） | 动态 TF：`world → body`，与 `/head_pose` 同数据、同频率（每条 head_pose 伴随一条） |
| `/tf_static` | 同上 | 静态外参：**每文件 1 条**，合并全部相机 `body → camera_<group>_<eye>_svr_frame`（共 6 路：rgb/tracking/ctrl × left/right），**原始 SVR 外参、无坐标转换**（与附件 `camera_params_*.json` 的 `extrinsics` 同源），`log_time = 锚定后的会话起点（≈首帧时刻）` |

### IMU

| Topic | Schema（encoding） | 内容 |
|---|---|---|
| `/imu/gyro` | 自定义 `gyro`（jsonschema / json） | `{"timestamp":{"sec","nanosec"}, "angular_velocity":{"x","y","z"}}`，rad/s，高速独立流（~1kHz 量级） |
| `/imu/accel` | 自定义 `accel`（jsonschema / json） | 同上结构，字段 `linear_acceleration`，m/s² |

时间戳做了**单调钳制**（offset 采样污染致单点回退时逐点 +1ns 钳制），保证 mcap 时间戳单调。

### 手势与手柄

| Topic | Schema（encoding） | 内容 |
|---|---|---|
| `/hand_tracking` | 自定义 `hand_tracking`（jsonschema / json） | `{"timestamp": <UTC ns>, "row": {...}}`。row 内：`frame_number, timestamp, left_active, right_active, {left\|right}_joint{i}_{id,name,radius,pos_x,pos_y,pos_z,orientation_x,y,z,w}`（每只手 26 关节 × 10 字段，Root Space，米）。不活跃侧 `active=0` 且字段为 0/-1 |
| `/controller_poses` | 自定义 `controller_poses`（jsonschema / json） | 同 `{"timestamp","row"}` 包装。row 内：`frame_number, timestamp_ns, left_active, left_px..left_qw, right_active, right_px..right_qw`（手柄在 World 中的位姿，米 + 四元数 xyzw） |

### 相机标定

| Topic | Schema（encoding） | 内容 |
|---|---|---|
| `/camera/<group>/<eye>/info` | `foxglove.CameraCalibration`（jsonschema / json） | **每眼 1 条**（共 6 条），静态标定：`{timestamp, frame_id, width, height, distortion_model:"kannala_brandt", D:[k1..k4], K:[9], P:[12]}`。`log_time = 锚定后的会话起点（≈首帧时刻）` |

---

## 3. 附件（Attachment）与 Metadata

### 附件（正常每文件 6 个，录制停止时嵌入）

附件是 mcap 规范中**不入 chunk 的独立 record**（Foxglove 可展示/导出），`logTime = createTime = 定稿时刻 UTC`：

| name | mediaType | 内容 |
|---|---|---|
| `audio.m4a` | `audio/mp4` | AAC 音频（44.1kHz, mono, 96kbps，fragmented MP4）。mcap 无音频通道，音频的唯一载体 |
| `audio_metainfo.csv` | `text/csv` | 音频包级元数据：`packet_index,pts_us,capture_utc_ns`（`capture_utc_ns` 与 mcap log_time 同时间轴，用于音画对齐） |
| `imu_calibration.json` | `application/json` | IMU 标定（见下文「IMU 标定」） |
| `camera_params_rgb.json` / `camera_params_tracking.json` / `camera_params_ctrl.json` | `application/json` | 相机内参 + **原始 SVR 外参**（与 MP4 数据集同名文件逐字节一致，见下文「相机内参/外参」） |

写入语义：录制期间音频/标定先写为数据集目录内的临时侧文件，停止时嵌入附件，**attach 成功后删除侧文件**（读取/attach 失败则保留侧文件防丢唯一副本）——因此正常结束后目录中只剩 `.mcap` 一个文件。

> 降级分支：**音频旋转失败**（`audioRotated=false`，停止时音频仍在写入）时，`audio.m4a` / `audio_metainfo.csv` **跳过 attach 且侧文件保留**（防丢正在写入的唯一副本），其余 4 个附件照常嵌入——此时附件只有 4 个，目录中残留音频侧文件，属预期行为而非产物损坏。

### Metadata record（每文件 1 条）

name = `"session"`，录制会话标识：

| key | 含义 |
|---|---|
| `sessionId` | 数据集目录基名（`YYYYMMDD_HHMMSS`，人类可读的会话标识） |
| `chunkIndex` | 文件序号（单文件录制恒为 `"0"`） |
| `datasetType` | 恒 `"mcap"` |
| `datasetFormatVersion` | 数据集格式版本，与 `camera_params_*.json` / `imu_calibration.json` 首字段 `version` 同值（当前 `"2"`；外参约定自 tag 1.7.0 起 convention 1，完整版本映射表见 Readme 2.1） |
| `deviceSn` | 设备序列号（可能为空串 = 未获取到） |

---

## 4. 坐标系与标定

### 坐标系约定

全数据集涉及 4 个坐标系：

| 坐标系 | 轴向 | 角色 |
|---|---|---|
| **World** | Y 上、-Z 前（OpenXR 约定；应用启动时 `xrCreateRootSpaceQCOM`，失败回退 `LOCAL` space） | 位姿的参考系 |
| **Body**（头显本体） | X 右、Y 上、Z 后（OpenXR/SVR 约定） | IMU 读数与外参的参考系；IMU 系与 Body 重合 |
| **optical**（存储图像系） | X 图像右、Y 图像下、Z 光轴前（OpenCV 约定，相对**存储图像**；tracking 竖装 90° 滚转已含在外参 R 内） | 相机内参（K/D）、视频/info 消息的 `frame_id`，以及 **convention 1 外参的相机端**（`/tf_static` 与附件，实证与内参 optical 系一致，见下） |
| **SVR 相机系**（仅 convention 0 历史数据） | X 上、Y 右、Z 前 | tag ≤ 1.6.0 外参的相机端，已废弃 |

各通道保存时使用的坐标系：

| 数据 | 保存坐标系 | 说明 |
|---|---|---|
| `/head_pose`、`/tf`、`/head_pose/path`、`/hand_tracking`、`/controller_poses` | **World**（OpenXR 原始输出，无转换） | 位姿 = 物体在 World 中的位置与朝向 |
| `/imu/gyro`、`/imu/accel` | **Body**（原始值） | IMU 系与 Body 重合 |
| `/camera/*/*/info`（K/D） | **optical** | 内参 OpenCV 标准约定，可直接作 K 矩阵；畸变 Kannala-Brandt 鱼眼前 4 系数（`cv2.fisheye`） |
| `/tf_static`（body→camera） | **Body ← 存储图像 optical 系**（原始，无转换） | 与附件 `camera_params_*.json` 的 `extrinsics` **完全同源**（同一数据的两种载体），任选其一即可。child frame_id 的 `_svr_frame` 后缀为历史命名（convention 0 时代相机端确为 SVR 相机系）；convention 1 起相机端即存储图像 optical 系 |
| 附件 `camera_params_*.json` 的 `extrinsics` | **Body ← 存储图像 optical 系**（原始，无转换） | `position` = 相机光心在 Body 系位置(m)，`rotation` = Camera→Body 四元数 `(x,y,z,w)` |

> 下游如需统一为 OpenCV Body 约定（X 右 Y 下 Z 前）：位姿 / IMU 自行做 `T_opencv = R_x(180°) · T_openxr`（即 `[x, -y, -z]`，Y、Z 翻转）；**外参同样只需 Body 翻转**：`R_cv = F @ R_svr`、`t_cv = F @ t_svr`（`F = diag(1,-1,-1)`），相机侧无需换基。外参由 libcamera 以 extrinsics convention 1 输出（Body=OpenXR，相机系=存储图像 optical 系）；旧 convention 0 共轭模式已废弃，其相机侧才是 SVR 相机系（X 上/Y 右/Z 前），转换见下节。

**实证依据**（tag 1.8.2 录制）：RGB `R_svr ≈ diag(1,-1,-1)`（仅 ±2° 安装倾角），即存储图像轴与 Body 轴直接对齐（图像右=Body X 右、图像下=Body -Y 下、光轴=Body -Z 前），解码 rgb.mp4 场景正立（图像右 = 场景右）；tracking 相机竖装，解码 tracking.mp4 内容横躺（场景上 = 图像左），该 90° 滚转已含在 R 内（`R` 的图像右列 ≈ 场景下方向）。

**历史数据转换（仅 tag ≤ 1.6.0 / convention 0，当前数据无需此步骤）**：convention 0 外参相机端为 SVR 相机系（X 上/Y 右/Z 前），转 optical 只需相机端换基、**平移不动**：

```python
H = np.array([[0, 1, 0], [-1, 0, 0], [0, 0, 1]])  # Rz(-90°)
R_body_optical = R_svr @ H.T   # 右乘 Hᵀ；不要做成 H·R·Hᵀ 相似变换（那会连父系一起换掉）
t_body_optical = t_svr         # 平移不变
```

数值示例（convention 0 时代实测 rgb-left）：`R_svr ≈ [[0,1,0],[1,0,0],[0,0,-1]]`（四元数 ≈ `[0.711,0.703,0,0]`），`t = [-0.0512, 0.0241, -0.0112]`。变换后 `R_body_optical ≈ diag(1,-1,-1)`——正是前向相机 optical 系在 Body 系中的预期朝向（X 右、Y 下、Z 前），也正是 convention 1 外参直接给出的值（convention 1 即由 libcamera 左乘 `Rz(90°)` 把这一步并入输出）；双目基线 64mm 沿 Body X（水平），可自测验证。若错误地用相似变换 `H·R_svr·Hᵀ` 且旋转平移 `H·t`，等效把 parent 换成 X下/Y右/Z后 的非 Body 系，相机位姿与 `/tf` 链整体差 90°（2026-08 之前的 mcap 即此 bug，已修复）。

**四元数顺序**：数据集统一 `(x, y, z, w)`（`XrQuaternionf`/`SxrPose.rotation`/CDR 消息均是）；注意 `glm::quat` 构造序是 `(w,x,y,z)`。位姿单位：米。Pitch 绕 X、Yaw 绕 Y、Roll 绕 Z。

### 相机内参/外参（相机-IMU 标定）

- **内参**：`K = [[fx,0,cx],[0,fy,cy],[0,0,1]]`，畸变 Kannala-Brandt 鱼眼 `D=[k1,k2,k3,k4]`，去畸变：
  ```python
  map1, map2 = cv2.fisheye.initUndistortRectifyMap(K, D, np.eye(3), K, (w, h), cv2.CV_32FC1)
  undistorted = cv2.remap(img, map1, map2, cv2.INTER_LINEAR)
  ```
- **外参**：Camera→Body（Tbc）。相机在 World 中位姿 = `T_WC = T_WB × T_BC`（world→body 取 `/tf` 或 `/head_pose`，body→camera 取 `/tf_static` 或 camera_params 外参）。相机对相对变换（A→B）：`R_BA = R_Bᵀ @ R_A`，`t_BA = R_Bᵀ @ (t_A - t_B)`。
- 相机-IMU 的**时间**标定在 `imu_calibration.json`（见下）；空间上 IMU 与 body 系重合（外参即相机-IMU 外参）。

### IMU 标定（附件 `imu_calibration.json`）

录制开始时生成（严格 JSON），主要字段：

| 字段 | 含义 | 单位 |
|---|---|---|
| `device_uid` | 设备唯一 ID | — |
| `imu.bias.accelerometer_mps2` / `imu.bias.gyroscope_rads` | 加速度计/陀螺仪三轴零偏 | m/s² / rad/s |
| `imu.scale_factor.*` | 三轴比例因子（相对 1.0 偏差） | 无量纲 |
| `imu.nonorthogonality.*` | 三轴非正交性（轴间串扰） | 无量纲 |
| `imu.time_alignment_s.imu_to_pose` | IMU↔pose（tracking）时间偏移 | s |
| `imu.time_alignment_s.cameras.<cam>` | 每个相机各自的 IMU↔相机时间偏移（`trackingA`/`trackingB`/`ctrl-trackingA`/`ctrl-trackingB`/`rgb-left`/`rgb-right`） | s |
| `imu.time_alignment_s.accel` | 加速度计相对陀螺仪的额外偏移 | s |
| `noise.accel_noise_std_mps2` / `noise.gyro_noise_std_rads` | 测量噪声标准差 σ_a / σ_g | m/s² / rad/s |
| `noise.accel_bias_std_mps2` / `noise.gyro_bias_std_rads` | 零偏随机游走噪声（经验默认值） | m/s² / rad/s |

### 轨迹的含义

`/head_pose/path` 不是独立传感器，而是 `/head_pose` 平移分量的**累积快照**，专为 Foxglove 3D 面板轨迹渲染设计：

- 每条消息携带从录制起点到当前时刻的**全量**（抽稀后）位姿点，最后一条快照即完整轨迹；
- 有界：快照间隔随时长拉大（≥500ms）、单快照 ≤3000 点（超限均匀抽稀、保留最新点）；
- 后处理取轨迹时应**去重**：直接把所有 Path 消息拼接会大量重复，取每条快照的末点、或直接以 `/head_pose` 重建。

---

## 5. 官方工具与消费方式

### Foxglove 官方工具

| 工具 | 地址 | 说明 |
|---|---|---|
| MCAP 官网/规范 | <https://mcap.dev>（规范 <https://mcap.dev/spec>） | 格式定义、各语言 SDK 索引 |
| Foxglove Studio | <https://foxglove.dev>（桌面版下载；Web 版 <https://app.foxglove.dev>） | 官方可视化：直接拖入 .mcap，3D 面板看 TF/轨迹、Image 面板看 `foxglove.CompressedVideo` 视频、Plot 面板看 IMU 曲线，支持 seek |
