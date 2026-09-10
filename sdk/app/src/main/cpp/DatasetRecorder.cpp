#include "DatasetRecorder.h"

#include "RecordingGatekeeper.h"
#include "mcap/McapChunkManager.h"
#include "mcap/CdrWriter.h"

#include <android/log.h>
#include <sys/stat.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#define LOG_TAG "DatasetRecorder"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

DatasetRecorder::DatasetRecorder() = default;

DatasetRecorder::~DatasetRecorder() {
    stop();
}

void DatasetRecorder::init(const std::string& basePath) {
    mBasePath = basePath;
    LOGI("DatasetRecorder initialized with basePath: %s", basePath.c_str());
}

std::string DatasetRecorder::generateDatasetDirName() {
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_val, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return ss.str();
}

bool DatasetRecorder::start() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mRecording.load()) {
        LOGW("Already recording");
        return false;
    }

    std::string datasetBase = mBasePath + "/dataset";
    mkdir(datasetBase.c_str(), 0777);

    std::string dirName = generateDatasetDirName();
    mDatasetDir = datasetBase + "/" + dirName;
    if (mkdir(mDatasetDir.c_str(), 0777) != 0) {
        LOGE("Failed to create dataset directory: %s", mDatasetDir.c_str());
        return false;
    }

    LOGI("Dataset recording started: %s", mDatasetDir.c_str());

    // Capture BOOTTIME→REALTIME offset once at recording start
    {
        struct timespec bt, rt;
        clock_gettime(CLOCK_BOOTTIME, &bt);
        clock_gettime(CLOCK_REALTIME, &rt);
        int64_t boottimeNs = (int64_t)bt.tv_sec * 1000000000LL + bt.tv_nsec;
        int64_t realtimeNs = (int64_t)rt.tv_sec * 1000000000LL + rt.tv_nsec;
        mBoottimeToRealtimeOffsetNs = realtimeNs - boottimeNs;
    }
    LOGI("BOOTTIME→REALTIME offset: %ld ns", (long)mBoottimeToRealtimeOffsetNs);

    // Start audio encoder (set offset BEFORE start to avoid race window)
    std::string audioPath = mDatasetDir + "/audio.m4a";
    mAudioEncoder.setTimeOffset(mBoottimeToRealtimeOffsetNs);
    if (!mAudioEncoder.start(audioPath)) {
        LOGE("Failed to start audio encoder");
    }

    // Start IMU collector (set offset BEFORE start to avoid race window)
    std::string accelPath = mDatasetDir + "/accel.csv";
    std::string gyroPath = mDatasetDir + "/gyro.csv";
    mImuCollector.setTimeOffset(mBoottimeToRealtimeOffsetNs);
    if (!mImuCollector.start(accelPath, gyroPath)) {
        LOGE("Failed to start IMU collector");
    }

    // Start head pose writer (head_pose.csv；mcap 模式改写 /head_pose 通道，不开 CSV)
    mPoseCount = 0;
    mMaxSeenPoseTimestamp = 0;
    mPoseMap.clear();
    mPathPoses.clear();
    mPathStartUtcNs = 0;
    mLastPathSnapshotNs = 0;
    const bool mcapMode = isMcapMode();
    bool poseOutputReady = true;
    if (!mcapMode) {
        mPoseFile.open(mDatasetDir + "/head_pose.csv", std::ios::out | std::ios::trunc);
        if (mPoseFile.is_open()) {
            mPoseFile << "timestamp_ns,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w\n";
            mPoseFile.flush();
        } else {
            LOGE("Failed to open head_pose.csv");
            poseOutputReady = false;
        }
    }
    if (poseOutputReady) {
        mPoseWriterRunning = true;
        mPoseWriterThread = std::thread(&DatasetRecorder::poseWriterThreadFunc, this);
    }

    mRecording = true;
    return true;
}

void DatasetRecorder::stop() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (!mRecording.load()) return;

    LOGI("Dataset recording stopping: %s", mDatasetDir.c_str());

    mAudioEncoder.stop();
    mImuCollector.stop();

    // Stop pose writer
    mPoseWriterRunning = false;
    mPoseCV.notify_all();
    if (mPoseWriterThread.joinable()) mPoseWriterThread.join();

    // Flush remaining pose entries (already sorted by timestamp)
    {
        std::lock_guard<std::mutex> plock(mPoseMutex);
        for (const auto& [ts, e] : mPoseMap) {
            // drop poses outside [gate-open, end-cutoff]
            if (!recordingGateIsOpen(e.timestamp) || !recordingGateBeforeStop(e.timestamp)) continue;
            int64_t utcNs = e.timestamp + mBoottimeToRealtimeOffsetNs;
            writePoseRow(utcNs, e.pos, e.quat);
        }
        mPoseMap.clear();
    }

    if (mPoseFile.is_open()) { mPoseFile.flush(); mPoseFile.close(); }

    mRecording = false;
    LOGI("Dataset recording stopped. Poses: %lu", (unsigned long)mPoseCount.load());
}

std::string DatasetRecorder::getHandTrackingCsvPath() const {
    if (mDatasetDir.empty()) return "";
    return mDatasetDir + "/hand_tracking.csv";
}

std::string DatasetRecorder::getControllerPoseCsvPath() const {
    if (mDatasetDir.empty()) return "";
    return mDatasetDir + "/controller_poses.csv";
}

std::string DatasetRecorder::getAudioPath() const {
    if (mDatasetDir.empty()) return "";
    return mDatasetDir + "/audio.m4a";
}

void DatasetRecorder::saveHeadPose(int64_t boottimeNs, const XrPosef& pose) {
    if (!mRecording.load() || !mPoseWriterRunning.load()) return;

    PoseEntry entry{};
    entry.timestamp = boottimeNs;
    entry.pos[0] = pose.position.x;
    entry.pos[1] = pose.position.y;
    entry.pos[2] = pose.position.z;
    entry.quat[0] = pose.orientation.x;
    entry.quat[1] = pose.orientation.y;
    entry.quat[2] = pose.orientation.z;
    entry.quat[3] = pose.orientation.w;

    {
        std::lock_guard<std::mutex> lock(mPoseMutex);
        mPoseMap[entry.timestamp] = entry;
        if (entry.timestamp > mMaxSeenPoseTimestamp) {
            mMaxSeenPoseTimestamp = entry.timestamp;
        }
    }
    mPoseCV.notify_one();
}

void DatasetRecorder::poseWriterThreadFunc() {
    LOGI("Pose writer thread started");

    while (true) {
        std::unique_lock<std::mutex> lock(mPoseMutex);

        // Exit once shutdown is signalled and everything is drained.
        if (!mPoseWriterRunning.load() && mPoseMap.empty()) break;

        // Flush entries older than the reorder window (all of them on shutdown).
        const int64_t safeThreshold = mPoseWriterRunning.load()
            ? (mMaxSeenPoseTimestamp - REORDER_WINDOW_NS)
            : INT64_MAX;
        int written = 0;
        for (auto it = mPoseMap.begin(); it != mPoseMap.end() && it->first <= safeThreshold; ) {
            const auto& e = it->second;
            if (!recordingGateIsOpen(e.timestamp) || !recordingGateBeforeStop(e.timestamp)) {
                it = mPoseMap.erase(it);  // drop pose outside [gate-open, end-cutoff]: no write, no count
                continue;
            }
            const int64_t utcNs = e.timestamp + mBoottimeToRealtimeOffsetNs;
            writePoseRow(utcNs, e.pos, e.quat);
            ++mPoseCount;
            it = mPoseMap.erase(it);
            ++written;
        }

        if (written > 0) {
            // Flushed something — re-scan immediately in case more are now old enough.
            continue;
        }

        // Nothing writable (remaining entries are still inside the reorder window).
        // Wait for a new pose to arrive (notify) or the 50ms timer — 50ms < the
        // 100ms window so entries still flush promptly. Without this wait the loop
        // busy-spins at ~100% CPU, which was the power regression.
        mPoseCV.wait_for(lock, std::chrono::milliseconds(50));
    }

    if (mPoseFile.is_open()) mPoseFile.flush();
    LOGI("Pose writer thread exited, total: %lu", (unsigned long)mPoseCount.load());
}

bool DatasetRecorder::isMcapMode() const { return mSink.isMcap(); }

// head_pose 单条写出：mcap 通道或 CSV 行
void DatasetRecorder::writePoseRow(int64_t utcNs, const float pos[3], const float quat[4]) {
    auto* mcapMgr = mSink.mgr.load();
    if (isMcapMode() && mcapMgr) {
        writePoseMcap(utcNs, pos, quat);
    } else {
        mPoseFile << utcNs
            << "," << pos[0] << "," << pos[1] << "," << pos[2]
            << "," << quat[0] << "," << quat[1] << "," << quat[2] << "," << quat[3] << "\n";
    }
}

// mcap 分支：/head_pose(PoseStamped CDR) + /tf(world→body TFMessage CDR) +
// 累积 path 缓存 + 有界快照。log_time 直接用 head_pose 的 UTC ns。
void DatasetRecorder::writePoseMcap(int64_t utcNs, const float pos[3], const float quat[4]) {
    auto* m = mSink.mgr.load();
    auto w = m ? m->current() : nullptr;
    if (!w) {
        // 无 mcap 分片（录制收尾 end() 后或 begin() 失败）——丢弃尾部，与 reference 尾部语义一致；
        // 限频告警便于发现 begin 失败的静默降级（共用 noWriterWarnThrottled 节流）
        static std::atomic<uint32_t> sNoWriterWarnCount{0};
        if (xr::mcap::noWriterWarnThrottled(sNoWriterWarnCount)) {
            LOGW("writePoseMcap: mcap active but no current writer (mcap create failed?); poses dropped");
        }
        return;
    }
    const double px = pos[0], py = pos[1], pz = pos[2];
    const double qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
    const auto ps = xr::mcap::CdrWriter::poseStamped(utcNs, "world", px, py, pz, qx, qy, qz, qw);
    w->writeCdr("/head_pose", utcNs, ps.data(), ps.size());
    const auto tf = xr::mcap::CdrWriter::tfMessage(
            {{"world", "body", {px, py, pz}, {qx, qy, qz, qw}}}, utcNs);
    w->writeCdr("/tf", utcNs, tf.data(), tf.size());
    mPathPoses.push_back({px, py, pz, qx, qy, qz, qw});
    maybeWritePathSnapshot(w, utcNs);
}

void DatasetRecorder::maybeWritePathSnapshot(const std::shared_ptr<xr::mcap::McapWriter>& w,
                                             int64_t utcNs) {
    if (mPathStartUtcNs == 0) mPathStartUtcNs = utcNs;
    // 快照间隔 = max(500ms, 已录制时长/200)（快照密度随时长有界下降）
    const int64_t intervalNs =
            std::max<int64_t>(500000000LL, (utcNs - mPathStartUtcNs) / 200);
    if (utcNs - mLastPathSnapshotNs < intervalNs) return;
    mLastPathSnapshotNs = utcNs;
    // 每快照历史点 ≤3000：超限均匀抽稀（i=kPathMaxPoints-1 时 i*(n-1)/(kPathMaxPoints-1)==n-1，
    // 末次迭代即最新点 back()）；缓存同步抽稀防内存随时长增长
    if (mPathPoses.size() > kPathMaxPoints) {
        const size_t n = mPathPoses.size();
        std::vector<std::array<double, 7>> dec;
        dec.reserve(kPathMaxPoints);
        for (size_t i = 0; i < kPathMaxPoints; ++i) {
            dec.push_back(mPathPoses[i * (n - 1) / (kPathMaxPoints - 1)]);
        }
        mPathPoses.swap(dec);
    }
    const auto path = xr::mcap::CdrWriter::pathMessage(utcNs, "world", mPathPoses);
    w->writeCdr("/head_pose/path", utcNs, path.data(), path.size());
}
