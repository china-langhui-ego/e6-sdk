#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <array>
#include <vector>
#include <memory>
#include <condition_variable>
#include <fstream>

#include "ImuPoseCollector.h"
#include "AudioEncoder.h"
#include "openxr/openxr.h"
#include "DatasetFormat.h"

namespace xr::mcap { class McapWriter; }

class DatasetRecorder {
public:
    DatasetRecorder();
    ~DatasetRecorder();

    // Initialize with base storage path
    void init(const std::string& basePath);

    // Start recording: creates dataset_<timestamp> directory, starts collectors
    bool start();

    // Stop recording: stops all collectors, flushes data
    void stop();

    bool isRecording() const { return mRecording.load(); }
    const std::string& getDatasetDir() const { return mDatasetDir; }

    // Get paths for other collectors
    std::string getHandTrackingCsvPath() const;
    std::string getControllerPoseCsvPath() const;
    std::string getAudioPath() const;

    // Get BOOTTIME→REALTIME offset captured at recording start
    int64_t getTimeOffset() const { return mBoottimeToRealtimeOffsetNs; }

    // Save head pose from render thread (async, non-blocking)
    void saveHeadPose(int64_t boottimeNs, const XrPosef& pose);

    // MCAP 数据集：mcap 模式 pose 线程写 /head_pose + /tf + /head_pose/path
    // 通道替代 head_pose.csv。每次录制由 beginRecordingCommon 统一重注入（防悬垂）；
    // 转发给自有组件 ImuPoseCollector（AudioEncoder 无 mcap 通道，不转发——音频照常写
    // 侧文件由 finalizeOne 嵌入）。
    void setMcapManager(xr::mcap::McapChunkManager* m) {
        mSink.setMcapManager(m);
        mImuCollector.setMcapManager(m);
    }
    void setFormatProvider(const xr::IDatasetFormatProvider* p) {
        mSink.setFormatProvider(p);
        mImuCollector.setFormatProvider(p);
        // AudioEncoder 不转发：mcap 模式同样保留 audio_metainfo.csv，无需感知格式
    }

private:
    void poseWriterThreadFunc();
    // mcap 分支：写 /head_pose(CDR) + /tf(CDR) + 累积 path 缓存 + 有界快照
    void writePoseMcap(int64_t utcNs, const float pos[3], const float quat[4]);
    // head_pose 单条写出：mcap 通道或 CSV 行（stop 尾部 flush、有序写段共用）
    void writePoseRow(int64_t utcNs, const float pos[3], const float quat[4]);
    // 有界 path 快照：间隔 = max(500ms, 已录制时长/200)；历史点 ≤3000 均匀抽稀
    void maybeWritePathSnapshot(const std::shared_ptr<xr::mcap::McapWriter>& w, int64_t utcNs);

    std::string mBasePath;
    std::string mDatasetDir;
    std::atomic<bool> mRecording{false};
    std::mutex mMutex;

    // BOOTTIME→REALTIME offset captured once at recording start
    int64_t mBoottimeToRealtimeOffsetNs{0};

    // IMU collector
    ImuPoseCollector mImuCollector;

    // Audio encoder
    AudioEncoder mAudioEncoder;

    // Head pose async writer with reorder buffer.
    // Entries are stored in a sorted map keyed by timestamp so that the writer
    // thread always emits rows in monotonically increasing order, even when
    // camera frames arrive out-of-order in the callback.
    static constexpr int64_t REORDER_WINDOW_NS = 100000000LL;  // 100 ms
    struct PoseEntry {
        int64_t timestamp;
        float pos[3];
        float quat[4];
    };
    std::map<int64_t, PoseEntry> mPoseMap;
    int64_t mMaxSeenPoseTimestamp{0};
    std::mutex mPoseMutex;
    std::condition_variable mPoseCV;
    std::thread mPoseWriterThread;
    std::ofstream mPoseFile;
    std::atomic<bool> mPoseWriterRunning{false};
    std::atomic<uint64_t> mPoseCount{0};

    // MCAP：组合体持 mcap manager 裸指针 + atomic 格式 provider（写入线程读、录制启动线程写）
    xr::McapSinkCtx mSink;

    bool isMcapMode() const;  // 定义在 .cpp（委托 mSink.isMcap()）
    // 有界累积 path：{x,y,z,qx,qy,qz,qw} 全量缓存（超限在快照时均匀抽稀，保留最新点）
    static constexpr size_t kPathMaxPoints = 3000;
    std::vector<std::array<double, 7>> mPathPoses;
    int64_t mPathStartUtcNs{0};       // 首个 pose 的 UTC ns（"已录制时长"基准）
    int64_t mLastPathSnapshotNs{0};   // 上次快照 log_time

    static std::string generateDatasetDirName();
};
