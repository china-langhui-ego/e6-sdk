// McapChunkManager：mcap 单文件生命周期 + 视频帧路由（可移植 C++17，仅标准库，无 Android 依赖）。
// 线程安全：mCurrent / mParamSets / mLeftFrameIdx 全部经 mMtx 保护（onEncodedFrame=编码
// 回调线程，begin/end=录制控制线程）；base64/JSON 组装放锁外（McapWriter 自带内部锁），
// 避免长持 mMtx 阻塞其他通道。log_time 直接用 ptsUs（Surface 编码器 presentationTimeUs
// = 曝光中点 BOOTTIME）+ 全局 offset，与帧 1:1 配对，无需渲染侧注入。
#include "McapChunkManager.h"
#include "McapSchemas.h"
#include "CdrWriter.h"
#include "../AnnexBConverter.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef __ANDROID__
#include <android/log.h>
#define MCAP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "McapChunkManager", __VA_ARGS__)
#define MCAP_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "McapChunkManager", __VA_ARGS__)
#else
#define MCAP_LOGE(...) do { \
        fprintf(stderr, "McapChunkManager: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } while (0)
#define MCAP_LOGW(...) do { \
        fprintf(stderr, "McapChunkManager: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } while (0)
#endif

namespace xr::mcap {

namespace {

// 旧 writer 引用归零时调用：finish() 幂等收尾后 delete。
// 标定 /camera/<group>/<eye>/info + /tf_static 不在此处写——deleter 触发时机取决于最后
// 一个 shared_ptr 持有者（可能在任意线程），无法安全访问 mMtx 保护的缓存；改为
// begin()/end() 在锁内 swap/reset 前由 writeStaticMessagesLocked 写入。
struct McapWriterDeleter {
    void operator()(McapWriter* w) const {
        if (w == nullptr) return;
        w->finish();
        delete w;
    }
};

// 全部 mcap 视频组（每眼）：begin() 的 IDR 守卫用
constexpr const char* kMcapGroups[] = {
    "_rgb_l", "_rgb_r", "_tracking_l", "_tracking_r", "_ctrl_l", "_ctrl_r"
};

// steady_clock（Android 即 CLOCK_MONOTONIC）当前时刻 ns
static int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 预注册 schema/channel 表：注册顺序即 schema id 分配顺序（writeJson/writeCdr 按 schema 名
// 解析），表内顺序与旧逐行调用一致，产物逐字节不变。
struct SchemaDef { const char* name; const char* encoding; const char* data; };
static const SchemaDef kSchemas[] = {
    {"foxglove.CompressedVideo", kJsonEncoding, kSchemaCompressedVideo},
    {"foxglove.CameraCalibration", kJsonEncoding, kSchemaCameraCalibration},
    {"gyro", kJsonEncoding, kSchemaGyro},
    {"accel", kJsonEncoding, kSchemaAccel},
    {"hand_tracking", kJsonEncoding, kSchemaHandTracking},
    {"controller_poses", kJsonEncoding, kSchemaControllerPoses},
    {"metainfo", kJsonEncoding, kSchemaMetainfo},
    {"geometry_msgs/PoseStamped", kRos2MsgEncoding, kMsgdefPoseStamped},
    {"nav_msgs/Path", kRos2MsgEncoding, kMsgdefPath},
    {"tf2_msgs/msg/TFMessage", kRos2MsgEncoding, kMsgdefTfMessage},
};
struct ChannelDef { const char* topic; const char* msgEncoding; const char* schemaName; };
static const ChannelDef kChannels[] = {
    {"/imu/gyro", "json", "gyro"},
    {"/imu/accel", "json", "accel"},
    {"/hand_tracking", "json", "hand_tracking"},
    {"/controller_poses", "json", "controller_poses"},
    {"/head_pose", kCdrMessageEncoding, "geometry_msgs/PoseStamped"},
    {"/head_pose/path", kCdrMessageEncoding, "nav_msgs/Path"},
    {"/tf", kCdrMessageEncoding, "tf2_msgs/msg/TFMessage"},
};

} // namespace

bool McapChunkManager::isMcapGroup(const std::string& g) {
    std::string base, eye;
    return parseMcapGroup(g, base, eye);
}

bool McapChunkManager::parseMcapGroup(const std::string& g, std::string& base, std::string& eye) {
    // 形式："_<base>_<eye>"，base∈{rgb,tracking,ctrl}，eye∈{l,r}
    // rfind('_') 切分依赖 base 自身不含 '_'（当前集合满足；新增 base 必须遵守）
    if (g.size() < 2 || g[0] != '_') return false;
    const size_t sep = g.rfind('_');
    if (sep == 0 || sep == std::string::npos || sep + 1 >= g.size()) return false;
    base = g.substr(1, sep - 1);
    eye = g.substr(sep + 1);
    if (base != "rgb" && base != "tracking" && base != "ctrl") return false;
    if (eye != "l" && eye != "r") return false;
    return true;
}

std::shared_ptr<McapWriter> McapChunkManager::current() const {
    std::lock_guard<std::mutex> lock(mMtx);
    // 传感器通道（IMU/位姿/标定等，均经 current() 取 writer）与视频帧写同一文件
    //（恒返回 mCurrent）。视频帧由 onEncodedFrame 写入，也不经此函数。
    return mCurrent;
}

// <dir>/<basename(dir)>.mcap — 文件名 = 目录基名（YYYYMMDD_HHMMSS）
static std::string chunkFilePath(const std::string& dir) {
    return dir + "/" + dir.substr(dir.find_last_of('/') + 1) + ".mcap";
}

std::shared_ptr<McapWriter> McapChunkManager::makeWriter(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // 生产上 engine 预建录制目录，此处兜底
    const std::string path = chunkFilePath(dir);
    std::shared_ptr<McapWriter> w(new McapWriter, McapWriterDeleter());
    if (!w->open(path)) {
        MCAP_LOGE("makeWriter: open failed: %s", path.c_str());
        return nullptr;
    }
    // 预注册全部 schema/channel（表内顺序即 schema id 分配顺序，与旧逐行调用一致）
    for (const auto& s : kSchemas) {
        w->registerSchema(s.name, s.encoding, s.data);
    }
    // 自动通道类 topic 预建并关联 schema：写入方（ImuPoseCollector/DatasetRecorder/
    // RawDateSave/ControllerPoseSaver）走 writeJson/writeCdr 自动通道，不预建则
    // channel schema_id=0，Foxglove 无法按 schema 解析。ROS2 通道 message_encoding
    // 用 "cdr"（XCDR1，CdrWriter 输出），schema_encoding 用 "ros2msg"（.msg 文本定义）。
    for (const auto& c : kChannels) {
        w->registerChannel(c.topic, c.msgEncoding, w->schemaIdOf(c.schemaName));
    }
    return w;
}

void McapChunkManager::resetChunkFrameStateLocked() {
    // 各视频通道等首个 IDR（requestKeyFrame 对在途帧不生效，文件头部可能落
    // ≤1 个 P 帧，onEncodedFrame 丢弃各通道首个 IDR 之前的帧）；metainfo frame_index
    // 从 0 起（对齐 mp4 frame_index 从 0 开始）。
    for (const char* g : kMcapGroups) {
        mAwaitFirstIdr[g] = true;
    }
    mLeftFrameIdx.clear();
}

void McapChunkManager::writeSessionMetadataLocked(const std::shared_ptr<McapWriter>& w) {
    if (!w) return;
    // SDK_VERSION 由 CMake 从仓库根 VERSION 注入；宿主测试等无注入场景回退 "dev"
#ifndef SDK_VERSION
#define SDK_VERSION "dev"
#endif
    w->writeMetadata("session", {{"sessionId", mSessionId},
                                 {"datasetType", "mcap"},
                                 {"datasetFormatVersion", std::to_string(kDatasetFormatVersion)},
                                 {"sdkVersion", SDK_VERSION}});
}

void McapChunkManager::begin(const std::string& dir) {
    std::shared_ptr<McapWriter> w = makeWriter(dir);
    if (!w) {
        MCAP_LOGE("begin: mcap create failed (%s), this recording will have no output",
                  chunkFilePath(dir).c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(mMtx);
    const int64_t off = (mTimeOffsetPtr != nullptr) ? mTimeOffsetPtr->load() : 0;
    writeStaticMessagesLocked(mCurrent);   // 防御：覆盖旧 writer 前补写标定
    mSessionStartNs = steadyNowNs() + off;
    mSessionAnchored = false;              // 首帧写入时重锚（onEncodedFrame）
    mSessionId = dir.substr(dir.find_last_of('/') + 1);
    resetChunkFrameStateLocked();          // 各组等首个 IDR、metainfo frame_index 从 0 起
    mParamSets.clear();                    // 参数集依赖新编码器，残留旧录制的注入会失真
    mCurrent = std::move(w);
    mCurrentDir = dir;
    writeSessionMetadataLocked(mCurrent);
}

void McapChunkManager::end() {
    std::shared_ptr<McapWriter> cur;
    std::string curDir;
    {
        std::lock_guard<std::mutex> lock(mMtx);
        writeStaticMessagesLocked(mCurrent);  // 单文件同样带完整标定（finish 前写入）
        cur = mCurrent;
        curDir = mCurrentDir;
        mCurrent.reset();
        mCurrentDir.clear();
    }
    // 写 6 附件；cur 离开作用域引用归零 → deleter 内 finish()
    if (cur) finalizeOne(cur, curDir);
}

void McapChunkManager::cacheCameraInfo(const std::string& topic, std::string json) {
    std::lock_guard<std::mutex> lock(mMtx);
    mCameraInfoJson[topic] = std::move(json);
}

void McapChunkManager::cacheTfTransform(const std::string& childFrameId,
                                        const std::array<double,3>& t,
                                        const std::array<double,4>& q) {
    std::lock_guard<std::mutex> lock(mMtx);
    mTfTransforms[childFrameId] = {t, q};
}

void McapChunkManager::cacheCameraParamsJson(const std::string& group, std::string json) {
    std::lock_guard<std::mutex> lock(mMtx);
    mCameraParamsJson[group] = std::move(json);
}

void McapChunkManager::finalizeOne(const std::shared_ptr<McapWriter>& w, const std::string& dir) {
    if (!w) return;
    // 锁内快照 offset 指针与 camera_params 缓存（避免定稿 IO 期间二次持锁）
    const std::atomic<int64_t>* offPtr;
    std::map<std::string, std::string> params;
    {
        std::lock_guard<std::mutex> lock(mMtx);
        offPtr = mTimeOffsetPtr;
        params = mCameraParamsJson;
    }
    // 附件 log_time = 定稿时刻 UTC（steady_clock+offset，与 begin() 的 mSessionStartNs 同源）
    const int64_t off = (offPtr != nullptr) ? offPtr->load() : 0;
    const int64_t nowNs = steadyNowNs() + off;
    // 1) 侧文件附件（成功写入才删侧文件；读取/写入失败保留 + warn）
    static const struct { const char* file; const char* media; } kSideFiles[] = {
        {"audio.m4a", "audio/mp4"},
        {"audio_metainfo.csv", "text/csv"},
        {"imu_calibration.json", "application/json"},
    };
    for (const auto& sf : kSideFiles) {
        const std::string path = dir + "/" + sf.file;
        // 附件整文件一次性读入内存（官方 writeAttachment 需连续缓冲）：12h 自动停录的
        // audio.m4a ≈500MB，叠加库内 chunk 拷贝瞬时峰值可达 ~1GB，移动端 OOM 风险，
        // 且失败发生在 end() 阶段会致 mcap 无 footer 整文件报废。超上限降级保留侧文件。
        constexpr uintmax_t kMaxAttachmentBytes = 256u * 1024u * 1024u;
        std::error_code sizeEc;
        const uintmax_t fsize = std::filesystem::file_size(path, sizeEc);
        if (sizeEc) {
            MCAP_LOGW("finalizeOne: skip missing %s", path.c_str());
            continue;
        }
        if (fsize > kMaxAttachmentBytes) {
            MCAP_LOGE("finalizeOne: %s too large (%llu bytes > %llu), side file kept (not embedded)",
                      path.c_str(), (unsigned long long)fsize,
                      (unsigned long long)kMaxAttachmentBytes);
            continue;
        }
        std::vector<uint8_t> bytes;
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs) {
                MCAP_LOGW("finalizeOne: skip missing %s", path.c_str());
                continue;
            }
            bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }  // ifs 在此销毁，释放文件句柄（Windows 上 remove 前必须关，否则被占用失败）
        if (!w->writeAttachment(sf.file, sf.media, nowNs, bytes.data(), bytes.size())) {
            MCAP_LOGW("finalizeOne: attach failed %s (side file kept)", path.c_str());
            continue;
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) MCAP_LOGW("finalizeOne: remove failed %s: %s", path.c_str(), ec.message().c_str());
    }
    // 2) camera_params 缓存（内存，无侧文件可删）
    for (const auto& [group, json] : params) {
        const std::string name = "camera_params_" + group + ".json";
        if (!w->writeAttachment(name, "application/json", nowNs,
                                reinterpret_cast<const uint8_t*>(json.data()), json.size())) {
            MCAP_LOGW("finalizeOne: attach failed %s", name.c_str());
        }
    }
    // 3) 显式 finish（幂等、McapWriter 内部自锁）：finish 原由 deleter 在引用归零时触发，
    // 传感器线程经 current() 持 writer 引用会把 finish 推迟到其写完之后——而调用方
    //（end()）返回后随即排水上传，可能读到未写 footer/summary 的文件。此处定稿即
    // finish，上传恒读完整文件；finish 后迟到的传感器写入由 McapWriter 返回 false
    // 丢弃（单条消息级丢失，无文件损坏）。
    w->finish();
}

void McapChunkManager::writeStaticMessagesLocked(const std::shared_ptr<McapWriter>& w) const {
    if (!w) return;
    // 每眼一条 CameraCalibration（log_time=会话开始，标定静态）
    for (const auto& [topic, json] : mCameraInfoJson) {
        const auto ch = w->registerChannel(topic, "json", w->schemaIdOf("foxglove.CameraCalibration"));
        w->write(ch, mSessionStartNs, reinterpret_cast<const uint8_t*>(json.data()), json.size());
    }
    // 全部相机外参合并为一条 /tf_static TFMessage（parent=body，map 按 child frame 字典序）
    if (!mTfTransforms.empty()) {
        std::vector<std::tuple<std::string, std::string, std::array<double,3>, std::array<double,4>>> trs;
        trs.reserve(mTfTransforms.size());
        for (const auto& [child, tq] : mTfTransforms) {
            trs.emplace_back("body", child, tq.first, tq.second);
        }
        const auto bytes = CdrWriter::tfMessage(trs, mSessionStartNs);
        const auto ch = w->registerChannel("/tf_static", kCdrMessageEncoding,
                                           w->schemaIdOf("tf2_msgs/msg/TFMessage"));
        w->write(ch, mSessionStartNs, bytes.data(), bytes.size());
    }
}

void McapChunkManager::onEncodedFrame(const char* group, const uint8_t* data, size_t size,
                                      int64_t ptsUs, bool isConfig) {
    if (!isMcap() || group == nullptr) return;
    std::string base, eye;
    if (!parseMcapGroup(group, base, eye)) return;
    const std::string g(group);

    // AVCC → AnnexB（纯输入函数，锁外完成；已是 AnnexB 则 convert 内部直接拷贝）
    std::vector<uint8_t> annexb;
    if (!AnnexBConverter::convert(data, size, annexb)) {
        MCAP_LOGE("onEncodedFrame: Annex-B convert failed, group=%s size=%zu", g.c_str(), size);
        return;
    }
    // AU 判型遍历全部 NAL：混合 AU（参数集+IDR 同帧，编码器 prepend 模式）不会
    // 被误判为纯参数集而整帧吞掉（只判首 NAL 曾致视频通道零帧的隐患）
    const auto au = AnnexBConverter::scanAccessUnit(annexb.data(), annexb.size());
    const bool isParamSet = isConfig || (au.hasParamSet && !au.hasVcl);
    const bool isKey = !isParamSet && au.hasKey;

    // ---- 锁内：仅 map 读写 + current() ----
    std::shared_ptr<McapWriter> w;
    std::vector<uint8_t> payload;
    int64_t logTime = 0;
    bool leftMeta = false;
    uint64_t frameIdx = 0;
    {
        std::lock_guard<std::mutex> lock(mMtx);
        w = mCurrent;
        if (!w) {
            // mcap 激活但 writer 为 null：文件创建失败后的静默降级，限频告警便于事后发现
            if (noWriterWarnThrottled(mNoWriterWarnCount)) {
                MCAP_LOGW("onEncodedFrame: mcap active but no current writer (mcap create failed?), "
                          "group=%s; frames dropped", g.c_str());
            }
            return;
        }
        if (isParamSet) {
            // 纯参数集 AU（isConfig || hasParamSet&&!hasVcl，对齐 CaptureManager.cpp:110
            // 防漏缓存）：只缓存 AnnexB 参数集，不单独写消息；关键帧前注入
            mParamSets[g] = std::move(annexb);
            return;
        }
        // 文件头部：丢弃首个 IDR 之前的 P 帧（引用上一录制 IDR，在新文件不可解码）
        auto ait = mAwaitFirstIdr.find(g);
        if (ait != mAwaitFirstIdr.end() && ait->second) {
            if (!isKey) return;
            ait->second = false;
        }
        payload = std::move(annexb);  // annexb 此后不再使用，移动消除每帧全量拷贝
        if (isKey) {
            if (au.paramSetPrefixLen > 0) {
                // 混合 AU：payload 自带参数集（无需前置注入）；缓存头部参数集前缀
                // 供后续无参数集的关键帧注入
                mParamSets[g].assign(payload.begin(),
                                     payload.begin() + static_cast<ptrdiff_t>(au.paramSetPrefixLen));
            } else {
                auto it = mParamSets.find(g);
                if (it != mParamSets.end() && !it->second.empty()) {
                    payload.reserve(it->second.size() + payload.size());
                    payload.insert(payload.begin(), it->second.begin(), it->second.end());
                }
            }
        }
        // log_time = ptsUs*1000+offset：Surface 模式编码器 info.presentationTimeUs 即
        // 渲染侧 setPresentationTime(midExpNs) 的绝对 BOOTTIME（CameraEncoder.cpp 注释），
        // 与编码帧 1:1 严格配对、天然无重复。旧「邮箱」语义（setLastExposureUtcNs 渲染
        // 线程注入最新值、本线程取后保留）在编码输出线程抖动时两个连续回调读到同一
        // 注入值 → 13~17% 帧 log_time 与前帧重复（实测 150118：RGB 零重复 Foxglove
        // 播放正常，灰度组重复率高则大部分帧不显示）。
        const int64_t off = (mTimeOffsetPtr != nullptr) ? mTimeOffsetPtr->load() : 0;
        logTime = ptsUs * 1000 + off;
        // 会话起点重锚（仅首帧）：数据帧被 RecordingGatekeeper 门控（等 6 源就绪 +
        // WAIT_STABLE 1.5s + 首 IDR），begin() 时 mSessionStartNs 比首帧早约 2.4s；
        // /info、/tf_static 等静态消息统一用 mSessionStartNs 打时间戳，不重锚则 Foxglove
        // 时间轴前段 ~2.4s 只有标定无数据。首帧写入时把会话起点设为该帧 logTime。
        if (!mSessionAnchored) {
            mSessionStartNs = logTime;
            mSessionAnchored = true;
        }
        if (eye == "l") {
            leftMeta = true;
            frameIdx = mLeftFrameIdx[base]++;
        }
    }

    // ---- 锁外：base64/JSON 组装 + 写文件（McapWriter 内部自锁） ----
    const std::string eyeName = (eye == "l") ? "left" : "right";
    const std::string frameId = "camera_" + base + "_" + eyeName + "_optical_frame";
    const std::string videoTopic = "/camera/" + base + "/" + eyeName;
    const auto vch = w->registerChannel(videoTopic, "json", w->schemaIdOf("foxglove.CompressedVideo"));
    const std::string vjson = compressedVideoJson(logTime, frameId, payload.data(), payload.size());
    w->write(vch, logTime, reinterpret_cast<const uint8_t*>(vjson.data()), vjson.size());
    if (leftMeta) {
        const std::string metaTopic = "/camera/" + base + "/metainfo";
        const auto mch = w->registerChannel(metaTopic, "json", w->schemaIdOf("metainfo"));
        const std::string mjson = metainfoJson(frameIdx, logTime);
        w->write(mch, logTime, reinterpret_cast<const uint8_t*>(mjson.data()), mjson.size());
    }
}

} // namespace xr::mcap
