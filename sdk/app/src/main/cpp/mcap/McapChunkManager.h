#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../DatasetFormat.h"
#include "McapWriter.h"

namespace xr::mcap {
// "mcap 激活但无 current writer"限频告警节流（McapChunkManager 与四个传感器写入
// 路径共用）：每 300 条放行 1 条。计数器由调用方持有；必须 std::atomic——WriterThread
// 与 StopSession drain 线程可并发进入同一写入函数，非原子 static 自增是 data race (UB)。
inline bool noWriterWarnThrottled(std::atomic<uint32_t>& count) {
    return count.fetch_add(1, std::memory_order_relaxed) % 300 == 0;
}

// 单文件一个 McapWriter；current() 返回共享指针，end() 写 6 附件后释放引用
//（deleter 内 finish 幂等兜底）。
// 可移植（仅标准库）；Android 属性读取在 PersistDatasetFormatProvider.cpp。
class McapChunkManager {
public:
    // mFmt 读写全部经 mMtx：injectDatasetFormat 每次录制重建 provider（旧实例退役驻留、
    // 不 delete，防无锁读者 UAF），编码回调线程 isMcap() 与重注入并发时必须互斥。
    // 配合调用方"先 setFormatProvider 切换 mFmt、后退役旧 provider"的次序，
    // mFmt 任一时刻都指向存活 provider。
    void setFormatProvider(const IDatasetFormatProvider* p) { std::lock_guard<std::mutex> lock(mMtx); mFmt = p; }
    void setTimeOffsetPtr(const std::atomic<int64_t>* p) { std::lock_guard<std::mutex> lock(mMtx); mTimeOffsetPtr = p; }
    bool isMcap() const { std::lock_guard<std::mutex> lock(mMtx); return mFmt && mFmt->isMcap(); }
    // 线程安全返回当前写入目标：传感器通道（IMU/位姿/标定等，经 current() 取 writer）
    // 与视频帧写同一文件（恒返回 mCurrent）。
    std::shared_ptr<McapWriter> current() const;
    void begin(const std::string& dir);         // 建 <dir>/<basename(dir)>.mcap 并置 current
    void end();                                 // 写 6 附件 + mCurrent.reset()

    // ---- 视频路由 ----
    static bool isMcapGroup(const std::string& g);   // "_rgb_l" / "_rgb_r" / "_tracking_l" / "_ctrl_r" ...
    // log_time 内部由 ptsUs*1000+offset 直出（与帧 1:1 配对，无重复；曾是渲染侧邮箱注入，
    // 编码输出线程抖动致 13~17% 重复时间戳、Foxglove 大面积不显示，已移除）
    void onEncodedFrame(const char* group, const uint8_t* data, size_t size, int64_t ptsUs, bool isConfig);

    // ---- 静态标定缓存（/camera/<group>/<eye>/info + /tf_static）----
    // 相机组分时到达（rgb/tracking/ctrl 首帧各自触发 saveCameraParams）→ 缓存合并；
    // begin() 覆盖旧 writer 前 / end() 时把全量缓存写入 writer（finish() 前），文件
    // 带完整标定。log_time 统一 = sessionStartNs()（标定不随时间变化）。
    int64_t sessionStartNs() const { std::lock_guard<std::mutex> lock(mMtx); return mSessionStartNs; }
    // 每眼 CameraCalibration JSON（topic=/camera/<group>/<eye>/info，同 topic 覆盖）；
    // json 内 timestamp 由调用方用首帧曝光中点 UTC（t0Ns，与 onEncodedFrame 锚定的会话
    // 起点一致）生成——不用 sessionStartNs()（begin 时刻，比数据起点早 ~2.4s 门控延迟）。
    void cacheCameraInfo(const std::string& topic, std::string json);
    // 累积一路相机静态外参（原始 SVR/OpenXR 约定、无转换，parent 恒 "body"）；childFrameId 去重覆盖。
    void cacheTfTransform(const std::string& childFrameId,
                          const std::array<double,3>& t, const std::array<double,4>& q);

    // ---- 附件嵌入（audio/imu_calibration/camera_params → mcap attachment）----
    // camera_params 原始 JSON 缓存（saveCameraParams mcap 分支注入；group="rgb"/"tracking"/"ctrl"，
    // 同 key 覆盖 = 每次录制重存；标定静态，跨录制内容一致）。定稿时生成 camera_params_<group>.json。
    void cacheCameraParamsJson(const std::string& group, std::string json);
private:
    // "_<base>_<eye>" → base∈{rgb,tracking,ctrl}, eye∈{l,r}；非法返回 false
    static bool parseMcapGroup(const std::string& g, std::string& base, std::string& eye);
    // 建 <dir>/<basename(dir)>.mcap（含目录创建 + 全部 schema 注册）；失败返回 nullptr
    std::shared_ptr<McapWriter> makeWriter(const std::string& dir);
    // 锁内调用：标定缓存全量写入 w（writer finish() 前；channel 惰性建立并关联已注册 schema）
    void writeStaticMessagesLocked(const std::shared_ptr<McapWriter>& w) const;
    // 锁内调用：各视频通道等首个 IDR、metainfo frame_index 从 0 起
    void resetChunkFrameStateLocked();
    // 锁内调用：写会话标识 metadata record（begin 在 writer 就位后写一次；
    // sessionId=录制目录名。分析脚本按 sessionId 标识同一录制）
    void writeSessionMetadataLocked(const std::shared_ptr<McapWriter>& w);

    // 锁外调用：把侧文件 ×3 + camera_params 缓存写入 w，删除已成功 attach 的侧文件，
    // 最后显式 finish()（幂等；保证返回后文件定稿完整，可立即上传）。
    // 读取失败跳过 + warn（保留侧文件防丢唯一副本）。
    void finalizeOne(const std::shared_ptr<McapWriter>& w, const std::string& dir);

    std::string mCurrentDir;                             // begin 记录（mMtx 保护）
    std::map<std::string, std::string> mCameraParamsJson; // group -> 原始 JSON（mMtx 保护）

    std::shared_ptr<McapWriter> mCurrent;
    mutable std::mutex mMtx;
    const IDatasetFormatProvider* mFmt = nullptr;
    const std::atomic<int64_t>* mTimeOffsetPtr = nullptr;
    // 每路视频：参数集缓存 + 帧计数。
    // ⚠ 以下 map 跨线程访问（onEncodedFrame=编码回调线程，begin/end=录制
    //   控制线程），必须全部经 mMtx 加锁；onEncodedFrame 锁内仅做 map 读写 + current()，
    //   base64/JSON 组装放锁外。
    std::map<std::string, std::vector<uint8_t>> mParamSets;    // group -> AnnexB VPS/SPS/PPS
    std::map<std::string, uint64_t> mLeftFrameIdx;             // group(base) -> metainfo frame_index（左眼写入后自增）
    // begin 后置 true：requestKeyFrame 对在途帧（请求前已入 codec 输入队列）不生效，
    // 文件头部可能落 ≤1 个 P 帧（引用上一录制 IDR 不可解码）——onEncodedFrame 丢弃各通道
    // 首个 IDR 之前的帧（对齐 CameraEncoder 懒启动"只在第一个 IDR 帧 open 文件"语义）。
    std::map<std::string, bool> mAwaitFirstIdr;                // group -> 是否等待首个 IDR
    std::atomic<uint32_t> mNoWriterWarnCount{0};               // 限频：mcap 激活但 current 为空的告警计数（每 300 帧一条）
    // 静态标定缓存（mMtx 保护，跨录制存活；同 key 覆盖实现"每次录制重存"）
    int64_t mSessionStartNs = 0;                               // begin() 捕获：boottime+offset ≈ 会话开始 UTC ns
    std::string mSessionId;                                    // begin() 捕获：录制目录基名（YYYYMMDD_HHMMSS，人类可读）
    bool mSessionAnchored = false;                             // 会话起点是否已重锚到首帧（onEncodedFrame 首帧写入时置位）
    std::map<std::string, std::string> mCameraInfoJson;        // topic -> CameraCalibration JSON
    std::map<std::string, std::pair<std::array<double,3>, std::array<double,4>>> mTfTransforms; // child frame -> (t, q)
};
} // namespace xr::mcap
