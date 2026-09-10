/****************************************************************
 * Copyright (c) 2026
 * All Rights Reserved.
 ****************************************************************/

#include "RawDateSave.h"
#include "RecordingGatekeeper.h"
#include "mcap/McapChunkManager.h"
#include "mcap/McapSchemas.h"
#include "xr_logger.h"
#include <android/log.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sstream>
#include <iomanip>
#include <cstring>

#define LOG_TAG "RawDateSave"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

// 手势关节名称映射 (XR_HAND_JOINT_26个关节)
static const char* JOINT_NAMES[26] = {
    "PALM",             // 0
    "WRIST",            // 1
    "THUMB_METACARPAL",  // 2
    "THUMB_PROXIMAL",   // 3
    "THUMB_DISTAL",     // 4
    "THUMB_TIP",        // 5
    "INDEX_METACARPAL",  // 6
    "INDEX_PROXIMAL",   // 7
    "INDEX_INTERMEDIATE", // 8
    "INDEX_DISTAL",     // 9
    "INDEX_TIP",        // 10
    "MIDDLE_METACARPAL", // 11
    "MIDDLE_PROXIMAL",  // 12
    "MIDDLE_INTERMEDIATE", // 13
    "MIDDLE_DISTAL",    // 14
    "MIDDLE_TIP",       // 15
    "RING_METACARPAL",  // 16
    "RING_PROXIMAL",   // 17
    "RING_INTERMEDIATE", // 18
    "RING_DISTAL",      // 19
    "RING_TIP",         // 20
    "LITTLE_METACARPAL", // 21
    "LITTLE_PROXIMAL",  // 22
    "LITTLE_INTERMEDIATE", // 23
    "LITTLE_DISTAL",     // 24
    "LITTLE_TIP"        // 25
};

RawDateSave::RawDateSave()
    : m_isRunning(false)
    , m_isPaused(false)
    , m_isSaving(false)
    , m_savedFrameCount(0)
    , m_frameCounter(0)
{
}

RawDateSave::~RawDateSave() {
    Shutdown();
}

bool RawDateSave::Init(const std::string& savePath) {
    m_savePath = savePath;

    // 确保目录存在
    if (!EnsureDirectoryExists(m_savePath)) {
        LOGE("Failed to create directory: %s", m_savePath.c_str());
        return false;
    }

    // CSV file will be created by StartNewSession() when recording starts

    // 启动保存线程
    m_isRunning = true;
    m_isPaused = false;
    m_frameCounter = 0;
    m_savedFrameCount = 0;

    m_saveThread = std::thread(&RawDateSave::SaveWorkerThread, this);

    LOGI("RawDateSave initialized with path: %s", m_savePath.c_str());
    return true;
}

bool RawDateSave::isMcapMode() const { return mSink.isMcap(); }

bool RawDateSave::StartNewSession(const std::string& csvPath) {
    std::lock_guard<std::mutex> lock(m_csvMutex);

    // Close existing file if open
    if (m_csvFile.is_open()) {
        m_csvFile.flush();
        m_csvFile.close();
    }

    // mcap 模式不开 CSV（写 /hand_tracking 通道替代），session 照常激活
    if (!isMcapMode()) {
        m_csvFile.open(csvPath, std::ios::out | std::ios::trunc);
        if (!m_csvFile.is_open()) {
            LOGE("Failed to open session CSV: %s", csvPath.c_str());
            return false;
        }

        m_csvFile << GenerateCsvHeader();
        m_csvFile.flush();
    }
    m_sessionActive = true;

    LOGI("RawDateSave session started: %s", csvPath.c_str());
    return true;
}

void RawDateSave::StopSession() {
    // drain 剩余队列（对齐 ImuPoseCollector::stop()）：SaveWorkerThread 与 m_isRunning 解耦、
    // 不 join，且写前检查 m_sessionActive——只关会话标志不排空则尾部在途帧全部丢弃。
    // 与 SaveWorkerThread 互斥（m_queueMutex），swap 后 SaveWorkerThread 从空队列取不到
    // 数据，此处是剩余帧的唯一定稿者。
    if (m_sessionActive.exchange(false)) {
        std::queue<FrameData> drain;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            drain.swap(m_frameQueue);
        }
        while (!drain.empty()) {
            writeFrame(drain.front());
            drain.pop();
        }
    }

    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (m_csvFile.is_open()) {
        m_csvFile.flush();
        m_csvFile.close();
    }

    LOGI("RawDateSave session stopped");
}

// 单条写出：mcap 通道或 CSV 行。SaveWorkerThread（已过 gate 检查）与 StopSession drain 共用。
// 注意：本函数不做 gate 判断——drain 尾部直接写出（与 ImuPoseCollector::stop() 不同，
// 其 drain 保留 gate 过滤）；此处依赖 feed 侧 recordingGateWithinWindow 预过滤，
// 队列内记录基本都在窗口内，窗口外漏网者数量可忽略。
void RawDateSave::writeFrame(const FrameData& frameData) {
    auto* mcapMgr = mSink.mgr.load();
    if (isMcapMode() && mcapMgr) {
        if (auto w = mcapMgr->current()) {
            const int64_t utcNs = frameData.timestamp + m_timeOffsetNs;
            w->writeJson("/hand_tracking", utcNs,
                              xr::mcap::handTrackingJson(utcNs, FrameToJson(frameData)));
        } else {
            // begin() 失败后 current() 恒 null：限频告警避免静默丢弃（共用节流 helper）
            static std::atomic<uint32_t> sNoWriterWarnCount{0};
            if (xr::mcap::noWriterWarnThrottled(sNoWriterWarnCount)) {
                LOGW("writeFrame: mcap active but no current writer (mcap create failed?); frames dropped");
            }
        }
    } else {
        std::string csvContent = FrameToCsv(frameData);
        {
            std::lock_guard<std::mutex> lock(m_csvMutex);
            if (m_csvFile.is_open()) {
                m_csvFile << csvContent;
                m_csvFile.flush();
            }
        }
    }
    m_savedFrameCount++;
}

void RawDateSave::Shutdown() {
    if (!m_isRunning) {
        return;
    }

    // 停止保存线程
    m_isRunning = false;
    m_isPaused = false;

    // 唤醒线程
    m_queueCondition.notify_all();

    // 等待线程结束
    if (m_saveThread.joinable()) {
        m_saveThread.join();
    }

    // 关闭CSV文件
    {
        std::lock_guard<std::mutex> lock(m_csvMutex);
        if (m_csvFile.is_open()) {
            m_csvFile.flush();
            m_csvFile.close();
        }
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<FrameData> emptyQueue;
        m_frameQueue.swap(emptyQueue);
    }

    LOGI("RawDateSave shutdown. Total frames saved: %u", m_savedFrameCount.load());
}

bool RawDateSave::SaveFrame(const FrameData& frameData) {
    if (!m_isRunning || m_isPaused) {
        return false;
    }
    // Only save if a recording session is active
    if (!m_sessionActive.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_frameQueue.push(frameData);
    }

    m_queueCondition.notify_one();
    return true;
}

void RawDateSave::Pause() {
    m_isPaused = true;
    LOGI("RawDateSave paused");
}

void RawDateSave::Resume() {
    m_isPaused = false;
    m_queueCondition.notify_all();
    LOGI("RawDateSave resumed");
}

size_t RawDateSave::GetQueueSize() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_frameQueue.size();
}

void RawDateSave::SaveWorkerThread() {
    LOGI("SaveWorkerThread started");

    while (m_isRunning) {
        FrameData frameData;

        // 从队列中获取数据
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] {
                return !m_frameQueue.empty() || !m_isRunning;
            });

            if (!m_isRunning) {
                break;
            }

            if (m_isPaused) {
                continue;
            }

            if (m_frameQueue.empty()) {
                continue;
            }

            frameData = m_frameQueue.front();
            m_frameQueue.pop();
        }

        // 保存数据
        m_isSaving = true;

        if (!m_sessionActive.load()) {
            m_isSaving = false;
            continue;
        }

        if (!recordingGateIsOpen((int64_t)frameData.timestamp) ||
            !recordingGateBeforeStop((int64_t)frameData.timestamp)) {
            m_isSaving = false;
            continue;  // drop hand frame outside [gate-open, end-cutoff]
        }

        writeFrame(frameData);

        m_isSaving = false;
    }

    LOGI("SaveWorkerThread ended");
}

std::string RawDateSave::GenerateCsvHeader() {
    std::ostringstream oss;

    oss << "frame_number,timestamp,left_active,right_active";

    // 左手26个关节: id, name,radius, pos_x, pos_y, pos_z
    for (uint32_t i = 0; i < 26; ++i) {
        oss << ",left_joint" << i << "_id,left_joint" << i << "_name,left_joint" << i << "_radius,left_joint" << i << "_pos_x,left_joint" << i << "_pos_y,left_joint" << i << "_pos_z,left_joint"
                << i << "_orientation_x,left_joint" << i << "_orientation_y,left_joint" << i << "_orientation_z,left_joint" << i << "_orientation_w";
    }

    // 右手26个关节: id, name,radius, pos_x, pos_y, pos_z
    for (uint32_t i = 0; i < 26; ++i) {
        oss << ",right_joint" << i << "_id,right_joint" << i << "_name,right_joint" << i << "_radius,right_joint" << i << "_pos_x,right_joint" << i << "_pos_y,right_joint" << i << "_pos_z,right_joint"
                << i << "_orientation_x,right_joint" << i << "_orientation_y,right_joint" << i << "_orientation_z,right_joint" << i << "_orientation_w";
    }

    oss << "\n";
    return oss.str();
}

std::string RawDateSave::FrameToCsv(const FrameData& frameData) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);

    // 基础信息
    oss << frameData.frameNumber << ","
        << (frameData.timestamp + m_timeOffsetNs) << ","
        << (frameData.hasLeftHand && frameData.leftHand.isActive ? 1 : 0) << ","
        << (frameData.hasRightHand && frameData.rightHand.isActive ? 1 : 0);

    // 左手26个关节数据: id, name,radius, pos_x, pos_y, pos_z
    for (uint32_t i = 0; i < 26; ++i) {
        if (frameData.hasLeftHand && frameData.leftHand.isActive) {
            const HandJointPosition& joint = frameData.leftHand.joints[i];
            float pos[3], orient[4];
            memcpy(pos, joint.position, sizeof(pos));
            memcpy(orient, joint.orientation, sizeof(orient));
            oss << "," << i
                << "," << JOINT_NAMES[i]
                << "," << joint.radius
                << "," << pos[0]
                << "," << pos[1]
                << "," << pos[2]
                << "," << orient[0]
                << "," << orient[1]
                << "," << orient[2]
                << "," << orient[3];
        } else {
            // 手部不活跃时填充空值: id, name, radius, pos_x, pos_y, pos_z, orientation_x, orientation_y, orientation_z, orientation_w
            oss << ",-1,\"\",0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0";
        }
    }

    // 右手26个关节数据: id, name, radius, pos_x, pos_y, pos_z
    for (uint32_t i = 0; i < 26; ++i) {
        if (frameData.hasRightHand && frameData.rightHand.isActive) {
            const HandJointPosition& joint = frameData.rightHand.joints[i];
            float pos[3], orient[4];
            memcpy(pos, joint.position, sizeof(pos));
            memcpy(orient, joint.orientation, sizeof(orient));
            oss << "," << i
                << "," << JOINT_NAMES[i]
                << "," << joint.radius
                << "," << pos[0]
                << "," << pos[1]
                << "," << pos[2]
                << "," << orient[0]
                << "," << orient[1]
                << "," << orient[2]
                << "," << orient[3];
        } else {
            // 手部不活跃时填充空值: id, name, radius, pos_x, pos_y, pos_z, orientation_x, orientation_y, orientation_z, orientation_w
            oss << ",-1,\"\",0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0";
        }
    }

    oss << "\n";
    return oss.str();
}

// mcap row JSON：字段与 hand_tracking.csv 列同构（frame_number,timestamp,left_active,
// right_active,left/right_jointN_{id,name,radius,pos_*,orientation_*}）；
// 手不活跃侧与 CSV 一致填 id=-1/name=""/数值 0。
std::string RawDateSave::FrameToJson(const FrameData& frameData) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);

    // active 条件先各求值一次，供 active 字段与后续循环共用
    const bool leftActive = frameData.hasLeftHand && frameData.leftHand.isActive;
    const bool rightActive = frameData.hasRightHand && frameData.rightHand.isActive;
    oss << "{\"frame_number\":" << frameData.frameNumber
        << ",\"timestamp\":" << (frameData.timestamp + m_timeOffsetNs)
        << ",\"left_active\":" << (leftActive ? 1 : 0)
        << ",\"right_active\":" << (rightActive ? 1 : 0);
    for (int side = 0; side < 2; ++side) {
        const char* prefix = (side == 0) ? "left" : "right";
        const bool active = (side == 0) ? leftActive : rightActive;
        const HandFrameData& hand = (side == 0) ? frameData.leftHand : frameData.rightHand;
        for (uint32_t i = 0; i < 26; ++i) {
            // 关节前缀一次拼好，避免 16 次重复流拼接（"left_joint0_" 等）
            const std::string jp = std::string(prefix) + "_joint" + std::to_string(i) + "_";
            oss << ",\"" << jp << "id\":" << (active ? (int)i : -1)
                << ",\"" << jp << "name\":\""
                << (active ? JOINT_NAMES[i] : "") << "\"";
            if (active) {
                const HandJointPosition& joint = hand.joints[i];
                float pos[3], orient[4];
                memcpy(pos, joint.position, sizeof(pos));
                memcpy(orient, joint.orientation, sizeof(orient));
                oss << ",\"" << jp << "radius\":" << joint.radius
                    << ",\"" << jp << "pos_x\":" << pos[0]
                    << ",\"" << jp << "pos_y\":" << pos[1]
                    << ",\"" << jp << "pos_z\":" << pos[2]
                    << ",\"" << jp << "orientation_x\":" << orient[0]
                    << ",\"" << jp << "orientation_y\":" << orient[1]
                    << ",\"" << jp << "orientation_z\":" << orient[2]
                    << ",\"" << jp << "orientation_w\":" << orient[3];
            } else {
                oss << ",\"" << jp << "radius\":0.0"
                    << ",\"" << jp << "pos_x\":0.0"
                    << ",\"" << jp << "pos_y\":0.0"
                    << ",\"" << jp << "pos_z\":0.0"
                    << ",\"" << jp << "orientation_x\":0.0"
                    << ",\"" << jp << "orientation_y\":0.0"
                    << ",\"" << jp << "orientation_z\":0.0"
                    << ",\"" << jp << "orientation_w\":0.0";
            }
        }
    }

    oss << "}";
    return oss.str();
}

bool RawDateSave::EnsureDirectoryExists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return true;
        }
        return false;
    }

    // 创建目录
    if (mkdir(path.c_str(), 0777) != 0) {
        return false;
    }

    return true;
}
