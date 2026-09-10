# Changelog


## 1.8.2（2026-09-03）

### 新增
- **数据集格式版本号**：`camera_params_*.json` / `imu_calibration.json` 首字段新增 `version`（当前 `2`）；mcap `session` metadata 新增 `datasetFormatVersion`——数据集可程序化识别格式/坐标约定代次，缺失即历史格式（4f38243）
- **SDK 发布版本可追溯**：仓库根新增 `VERSION` 文件（打 tag 时同步更新、随同一提交打 tag）；构建注入 `SDK_VERSION`，启动 logcat 打印 `SkyEgoSenseSDK <ver> (dataset format v2)`；mcap `session` metadata 新增 `sdkVersion` 键（ad20121）

### 性能
- **录制编码链路 worker 化**（自 XRCameraControl 移植）：相机回调内的 blit + 编码提交移入新增的 `RecordingEncodeWorker` 专用线程（FIFO 队列 cap=8 满丢最旧、EGLImage 延迟 8 帧销毁防 GPU fence 死等、空闲 500ms 冲刷防 HAL buffer 滞留），mcap 六路每眼编码器预创建收养消除录制首帧尖刺（原 66~132ms）——实测相机回调 RGB 5.7→1.7ms、灰度 3.0→0.9ms，3min 录制（mp4/mcap）零源丢帧、零 queue-full（5668699）
- **灰度编码器锁拆分**：tracking / ctrl 各持一把锁，消除共用单锁互相阻塞（5668699）

### 修复
- **mcap RGB 每眼保存画面上下颠倒**：rgbWorkerVBO 顶点布局与 encoderVBO 对齐（shader 自带 1-v 翻转，负负得反）（5668699）
- **录制链路 3 处资源泄漏**：worker 自有 GL 对象（pause 时显式删除，防每轮 pause/resume 累积）、滞留 gralloc buffer、eyeEglImage（5668699）

### 文档
- OpenCV Body 转换补全相机系换基配方：`R_cv = F·R_svr·Hᵀ`、`t_cv = F·t_svr`（`[x,-y,-z]` 仅覆盖位姿/IMU，外参还差 90° 相机系换基）；外参 convention 1 语义与 tag 边界说明（tag 1.7.0 起新约定，≤ 1.6.0 为 convention 0 共轭输出）；版本映射表对齐远程 tag（4f38243、6a22e83）

## 1.8.1（2026-08-31）

### 修复
- **录制启动期丢帧**：3 路编码器 `create/configure/start`（实测 25-37ms/路）移出相机回调线程，改为录制开始时后台预创建（回调内初始化 39ms→2.6ms）；预创建失败回退原内联路径（e580f30）
- **手柄投影残余偏移**：世界系常量修正 (0,0,-0.042) 改为 view 系平移经头部姿态旋转——转头 90° 时旧方案残余误差约 6cm，新方案各朝向均贴合物理手柄（2aeac27）

### 变更
- 3rdlibs/libcamera-release.aar 同步 qxr：CV 顺序补帧替代追帧快进，消除源端丢帧（e580f30）
- 新增 FrameDropMonitor 实时丢帧监控（源端跳号 / 门控拒绝 / 5s 汇总，logcat `grep FrameDrop`）；5 轮真机长录制（最长 34min）三路零丢帧（e580f30）

## 1.8.0（2026-08-27）

### 新增
- **MCAP 单文件数据集格式**：`persist.xr.dataset_type=mcap` 切换；vendor 官方 mcap v2.1.3，jsonschema + CDR 编码，6 附件嵌入（audio / imu_calibration / camera_params×3），`/camera/*/info` + `/tf_static` 标定通道，session metadata；topic/schema 与 XRCameraControl 下游约定对齐，MP4 路径零行为变化（8381b15）
- **录制多源同步起停（RecordingGatekeeper）**：6 源（3 视频+音频+accel+gyro）就绪后统一开门，门前冷启动样本丢弃；视频首帧强制 IDR、PTS 归零；停录统一截止。实测各流起始偏差 16-20ms（此前 >1s）（2ef838d）
- IMU 时间戳单调钳制扩展到 MP4 CSV 路径：`accel.csv` / `gyro.csv` 不再可能出现时间戳回退行（8381b15，MP4 可观察变更）

### 修复
- **手柄投影偏移 ~50-70px**：aim pose 原点改绑 `khr/simple_controller` profile（oculus/touch 的 aim 原点与 SCTRL body pose 相差数厘米，世界系补偿无法各朝向成立）；录制切换按键 Right B → Right A（494be08）
- **启动 ANR**：首启资产拷贝改为成功后才置 FIRST_TIME_TAG，失败下次启动重试（原失败即永久缺 shader → 主线程挂死）（19d4b98）
- **手柄 pose 时间戳**：改用 `xrConvertTimeToTimespecTimeKHR` 由 predictedDisplayTime 精确转换 boottime，取代与 pose 时刻不符的 sensor 时刻（3a6fa06）
- **手柄坐标轴投影恢复 3D 旋转**：轴向量经手柄四元数旋转后过 KB 鱼眼投影，投影长度钳制 [8,40]px（bd4f5a2）；tracking.mp4 不再投手柄坐标轴（仅 rgb.mp4 保留）（a879f7b）

### 性能
- **poseWriter 空转**：录制期间 ~100% CPU（~0.86W）——重排窗口内"非空但不可写"状态改为 `wait_for(50ms)`，消除单核占满（c4f0524）
- **编码输出线程空转**：dequeue 超时 5ms→50ms + 删外层 sleep，稳态唤醒 ~200 次/秒 → ~20 次/秒；功率对比无回归（1b56f1b）

### 其他
- `svr_plugin_android_api.aar` 更新以兼容 E8 设备（f3e1cb5）

### 文档
- 新增英文文档 `Readme_en.md`（dc6d189）；MCAP 格式文档 `mcap-dataset-format.md` + Readme 属性表 `persist.xr.dataset_type`（7b3f9d7）；中英文档与 HTML 同步（c25f97e、85c2991）

## 1.7.0（2026-07-07）

### 修复
- **息屏后灰度相机失效**：resume() 改为 `sxr_camera_destroy` + `sxr_camera_create` 重建整个 QVR 客户端会话（close_group+open_group 不重置 QVR 会话，回调永不恢复）；另加两道 PTS 防御钳制；10 轮息屏循环验证（944bb6e）

### 变更（数据集可观察）
- **坐标约定回归 raw SVR/OpenXR Body**：移除全部保存路径的 OpenCV-body 转换（`posBodyToOpencv` 等），数据集原样存储 API 输出（主干上的 convention 1 切换与 OpenCV-body 落盘 6bd1de7 只存在于 1.7.0 开发期内、发布前已被本提交回退，**从未随任何 tag 发布**；同类改动 bfe7047 仅在未合入的 main-dev 分支）；同步重构出宿主可测的 `PoseHandSampleRing.h`（f9f7cc9）
- **全视频流强制 CBR**：RGB 8Mbps / 灰度 4Mbps 恒定码率，替代 Android 默认 VBR（静态场景码率塌缩）；实测均值贴合目标、GOP 码率波动收窄（3331ac0）
- **RGB GOP 统一 1s**：rgbEncoder `KEY_FRAME_RATE` 与实际 fps 同步（原 60fps 下误配 30 → 0.5s GOP），三路 GOP 均匀 1s（6f9eda9）

### 性能
- **fMP4 写盘**：分片内存缓冲周期刷盘（~500ms/2MB，崩溃安全边界不变，异常终止最多丢一个刷盘间隔）（58a00e1）；单帧 5 次 write 合并为一次 writev，~5x 系统调用减少、落盘字节不变（994fe1b）

### 新增
- 数据集哨兵测试：`test_gop.py`（每流恒定帧数 GOP、无 B 帧）等 P-GOP/P-SCHEMA/P-CV/P-TS 系列（03ff2a8、f9f7cc9）

## 1.6.0（2026-06-23）

范围 1.5.0..1.6.0，52 提交（2026-06-09 ~ 06-23），数据集格式的奠基版本。

### 新增（数据集格式重构）
- **自研 fragmented MP4 写入器 `FMP4Writer`**：ftyp+moov 骨架、per-sample moof/mdat、P 帧非同步 flag + PTS 单调守卫、AAC 音轨（esds/mp4a）、截断安全（异常终止可播放到最后完整分片）+ 宿主测试骨架（5747786、b0a291e、5a193f3、d26e5cc、3b0e2a3）
- **视频/音频编码器切换 FMP4Writer**：HEVC `max-bframes=0`（解码序 == 呈现序）；Annex-B HEVC 转 length-prefixed 修复解码（f144e6c、ef6069b、8ca31b2）
- **逐帧元数据 CSV**：`*_metainfo.csv`（帧级时间戳/曝光/增益）与 `audio_metainfo.csv` 取代 mett 时间戳文本轨与 `time_offset.json`；视频/音频 PTS 零基（fe99efa、ffc443d、ef6069b、943f88e、3da86c8、f0c77a0）
- **IMU 标定 sidecar `imu_calibration.json`**：bias / scale_factor / nonorthogonality + 每相机 time_alignment；libcamera AAR 重建带 `get_imu_calibration` 并镜像 `SxrImuCalibration` ABI；移除 source 字段、9 位小数精度（072141e、d9a5362、908d7d6、c4b15b9、fe38660、c4d42d4、41a344b、10c0007）
- **手柄坐标轴投影**：`persist.xr.project_controller=1` 时 RGB 编码视频叠加 RGB 三轴坐标框（808c2c4、1c8e458）
- **编码帧输出监听**：`IEncoderOutputListener` + 空 baseDir 纯流式预览模式（31542ad）
- fMP4 moov 回填真实平均帧间隔/时长——Windows 资源管理器属性页不再恒显 30fps（a8ba490）；`persist.sxr.cam.rgb.fps` 属性文档（2be48f3）

### 时间戳体系
- 全数据集时间戳 CLOCK_BOOTTIME → **Unix UTC ns**（aeb3e76）；head_pose.csv 排序缓冲重排 + 5 项平滑修复（b61373a、9f2cf31）；时间戳域文档修正（全流 boottime→UTC）与厂商命名脱敏（4bf7b5a、5a186a5、c06463b、844051f）

### 修复
- shader crash / head-pose 录制 / 手柄位姿对齐（d685025）
- 坐标轴投影空指针崩溃（`projectPointBothEyes` nullptr 解引用）→ 固定 25px 轴臂（3f444a4）
- RGB 编码器 surface 上下文切换后 shader 状态重绑（8924693）
- 录制健壮性：剩余空间 <1GiB 拒录（69b8c6e）；12h 自动停录防 AAudio int32 帧计数溢出（202f96c）；AAudio 冷启动毛刺消除 + 低音量补偿（416eab0）；录制期持有 wake lock 防息屏导致会话失焦（798265b）

### 性能
- RGB 编码改相机回调内直渲编码器 surface（单 pass）+ shader location 缓存 + 持久眼纹理 + 去 SBS 清屏（b36ffc4、a0134fb）；删除死代码 SBS 拷贝管线 −222 行（a58e150）
- 手部叠加渲染线程安全（renderMutex + 双缓冲投影）+ `computeProjection` 移至 sensorAlignWorker 异步线程，解耦相机回调线程（a522c5c）
