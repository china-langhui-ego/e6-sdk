# DatasetServer — XR 数据集分片上传服务

接收 VR 头显通过 HTTP multipart 上传的录制分片数据。

## 系统工作流

```
VR 头显录制分片 → 分片完成自动上传 → PC 端 DatasetServer (:9000) → dataset/<chunk_name>/
                    ↑
            Flutter 控制面板 BLE 设置 URL / 切片时长
```

VR 端在录制过程中按切片时长自动切分数据集为独立子目录，每个分片完成后自动通过 HTTP multipart 上传至本服务。切片时长和上传地址可通过 Flutter 控制面板"更多"设置页面配置。

## 环境要求

- **Python 3.6+**（无需第三方依赖，仅使用标准库）
- Windows / Linux / macOS

Windows / macOS 上数据集接收服务已集成进 EgoVision PC 端，无需单独启动脚本（App 内开关控制，默认关闭）；Linux 使用系统 Python 运行本目录脚本。

## 快速启动

### Windows / macOS

已集成进 EgoVision PC 端（无需单独启动脚本）。

### Linux
```bash
chmod +x start_server.sh
./start_server.sh --port 9000
```

### Python 直接运行
```bash
python3 server.py --port 9000 --dir ./received_chunks
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | `9000` | HTTP 监听端口 |
| `--dir` | `dataset/`（脚本所在目录下） | 分片数据保存目录 |
| `--timeout` | `60` | socket 空闲超时秒数，用于中断死连接上传 |

启动后终端会打印本机所有可访问的 URL，将地址填入 Flutter 控制面板"更多"设置中的"分片上传地址"即可。

---

## API 文档

### 基础信息

- **协议**: HTTP/1.1
- **默认端口**: 9000
- **数据格式**: multipart/form-data
- **请求体上限**: 5 GB（仅对固定 `Content-Length` 的请求生效；VR 端实际使用 chunked 编码上传，不受该上限约束，chunked 请求体整体读入内存）
- **CORS**: 已启用

---

### 1. 欢迎页 / API 文档

**`GET /`**

返回纯文本格式的 API 文档和本机地址列表。可直接在浏览器打开查看。

---

### 2. 健康检查

**`GET /api/health`**

响应:
```json
{
  "status": "ok",
  "received_chunks": 5,
  "uptime_seconds": 3600
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | 固定 `"ok"` |
| `received_chunks` | int | 启动以来收到的分片总数 |
| `uptime_seconds` | int | 服务器运行秒数 |

---

### 3. 上传分片

**`POST /api/upload`**

将 VR 端录制的一个分片子目录上传到服务器。

#### 请求

- **Content-Type**: `multipart/form-data`
- **字段:**

| 字段名 | 类型 | 必填 | 说明 |
|--------|------|------|------|
| `chunk_name` | string | 是 | 分片目录名，格式 `<SN>_YYYYMMDD_HHMMSS`（`SN` 为设备序列号前缀，用于区分不同设备；SN 缺失时为 `YYYYMMDD_HHMMSS`） |
| `files` | file | 是 | 分片内的文件，可重复多次（每个文件一个 field） |

#### 分片内容

每个分片包含以下文件（由 VR 端录制生成）：

| 文件 | 说明 |
|------|------|
| `rgb.mp4` | RGB 摄像头视频（H.265） |
| `tracking.mp4` | Tracking 摄像头视频（H.265） |
| `ctrl.mp4` | Ctrl 摄像头视频（H.265） |
| `audio.m4a` | 音频文件 |
| `rgb_metainfo.csv` | RGB 帧元信息（时间戳等） |
| `tracking_metainfo.csv` | Tracking 帧元信息 |
| `ctrl_metainfo.csv` | Ctrl 帧元信息 |
| `audio_metainfo.csv` | 音频帧元信息 |
| `accel.csv` | 加速度计数据 |
| `gyro.csv` | 陀螺仪数据 |
| `head_pose.csv` | 头部姿态数据 |
| `hand_tracking.csv` | 手部追踪数据 |
| `controller_poses.csv` | 控制器姿态数据 |
| `camera_params_rgb.json` | RGB 摄像头内参/外参 |
| `camera_params_tracking.json` | Tracking 摄像头内参/外参 |
| `camera_params_ctrl.json` | Ctrl 摄像头内参/外参 |
| `imu_calibration.json` | IMU 标定（零偏/比例因子/非正交性/时间对齐/噪声） |

#### 示例 (curl)

```bash
curl -F "chunk_name=SN_20260630_143000" \
     -F "files=@rgb.mp4;type=application/octet-stream" \
     -F "files=@tracking.mp4;type=application/octet-stream" \
     -F "files=@accel.csv;type=application/octet-stream" \
     http://192.168.1.100:9000/api/upload
```

#### 原始 HTTP 请求格式

```
POST /api/upload HTTP/1.1
Host: 192.168.1.100:9000
Content-Type: multipart/form-data; boundary=----ChunkUpload

------ChunkUpload
Content-Disposition: form-data; name="chunk_name"

20260630_143000
------ChunkUpload
Content-Disposition: form-data; name="files"; filename="rgb.mp4"
Content-Type: application/octet-stream

<rgb.mp4 的二进制数据>
------ChunkUpload
Content-Disposition: form-data; name="files"; filename="tracking.mp4"
Content-Type: application/octet-stream

<tracking.mp4 的二进制数据>
------ChunkUpload
Content-Disposition: form-data; name="files"; filename="accel.csv"

<accel.csv 的文本内容>
------ChunkUpload--
```

#### 响应 (成功)

```json
{
  "status": "ok",
  "chunk_name": "SN_20260630_143000",
  "file_count": 17,
  "total_bytes": 123456789
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` 表示成功 |
| `chunk_name` | string | 分片目录名 |
| `file_count` | int | 成功保存的文件数 |
| `total_bytes` | int | 总字节数 |

#### 错误响应

```json
{
  "error": "missing chunk_name field"
}
```

#### 状态码

| 状态码 | 含义 |
|--------|------|
| 200 | 上传成功 |
| 400 | 请求格式错误（缺少字段、非 multipart 等） |
| 404 | 接口路径不存在 |
| 409 | 并发上传冲突（仅 EgoVision PC 内置版，同一时刻只处理一个上传） |
| 413 | 请求体过大（>5 GB，仅固定 `Content-Length` 请求；chunked 上传不触发） |

上传过程中，终端会实时显示进度百分比、传输速度、预计剩余时间（仅固定 `Content-Length` 请求；VR 端的 chunked 上传只打印「接收中」与完成后的汇总行）。

---

## 存储结构

上传的分片保存在 `--dir` 指定的目录（默认脚本所在目录下 `dataset/`），按 `chunk_name` 建子目录：

```
dataset/
├── SN_20260630_143000/
│   ├── rgb.mp4
│   ├── tracking.mp4
│   ├── ctrl.mp4
│   ├── audio.m4a
│   ├── rgb_metainfo.csv
│   ├── tracking_metainfo.csv
│   ├── ctrl_metainfo.csv
│   ├── audio_metainfo.csv
│   ├── accel.csv
│   ├── gyro.csv
│   ├── head_pose.csv
│   ├── hand_tracking.csv
│   ├── controller_poses.csv
│   ├── camera_params_rgb.json
│   ├── camera_params_tracking.json
│   ├── camera_params_ctrl.json
│   └── imu_calibration.json
└── SN_20260630_150000/
    └── ...
```

> EgoVision PC 内置版的存储布局不同：按设备 SN 分两级目录 `<文档目录>/XRCameraControl/dataset/<sn>/<YYYYMMDD_HHMMSS>/`（SN 前缀从 chunk_name 剥离）。

---

## EgoVision PC 内置版差异

EgoVision PC 端（Windows / macOS）内置的上传服务与本 Python 版协议兼容，但行为有差异：

- `GET /api/health` 只返回 `{"status": "ok"}`（无 `received_chunks` / `uptime_seconds`）
- 上传成功响应额外返回 `delete_after_upload` 字段，VR 端据此决定是否自删本地分片
- 存储布局为 `dataset/<sn>/<时间戳>/` 两级目录（见上）
- 并发上传返回 409；流式落盘（无 5GB 上限、chunked 不全量读内存）

---

## 安全说明

- `chunk_name` 经 `os.path.basename` 过滤；文件名统一分隔符后剥离前导 `/`、剔除 `.`/`..`/空段（保留相对子目录层级），落盘后再做绝对路径前缀复查，双重防路径遍历
- 请求体上限 5 GB（仅固定 `Content-Length` 请求；chunked 上传不受限，整体读入内存）
- 本服务设计用于**局域网内部**使用，不提供认证/TLS
- 如需公网部署，请在反向代理层添加认证和 TLS

---

## 自行实现参考

如需用自己的语言/框架实现 DatasetServer，只需实现上述 `POST /api/upload` 接口即可。核心逻辑：

1. 解析 `multipart/form-data` 请求体
2. 提取 `chunk_name` 字段作为子目录名
3. 提取每个 `files` 字段，用其 `filename` 作为文件名保存到子目录
4. 返回 JSON 格式的成功/失败响应
