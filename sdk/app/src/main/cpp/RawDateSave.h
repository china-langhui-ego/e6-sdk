/****************************************************************
 * Copyright (c) 2026
 * All Rights Reserved.
 ****************************************************************/

#ifndef HELLOXR_RAWDATESAVE_H
#define HELLOXR_RAWDATESAVE_H

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <fstream>

#include "openxr/openxr.h"
#include "DatasetFormat.h"

// 手势关节位置数据
struct HandJointPosition {
    // 关节位置 (X, Y, Z)
    float position[3];

    // 关节旋转 (四元数: X, Y, Z, W)
    float orientation[4];

    // 关节半径
    float radius;
};

// 单帧手势数据
struct HandFrameData {

    // 手部是否活跃
    bool isActive;

    // 关节数量 (XR_HAND_JOINT_COUNT_EXT = 26)
    uint32_t jointCount;

    // 关节位置数据数组
    HandJointPosition joints[26];
};

// 用于存储单帧左右手数据
struct FrameData {
    // 帧编号
    uint32_t frameNumber;

    // 时间戳
    XrTime timestamp;

    // 左手数据
    HandFrameData leftHand;

    // 右手数据
    HandFrameData rightHand;

    // 是否有左手数据
    bool hasLeftHand;

    // 是否有右手数据
    bool hasRightHand;
};

class RawDateSave {
public:
    RawDateSave();
    ~RawDateSave();

    // 初始化保存器
    bool Init(const std::string& savePath);

    // 关闭保存器
    void Shutdown();

    // 保存单帧数据（异步）
    bool SaveFrame(const FrameData& frameData);

    // 暂停保存
    void Pause();

    // 恢复保存
    void Resume();

    // Start a new recording session to a specific CSV path
    bool StartNewSession(const std::string& csvPath);

    // Stop the current recording session (flush and close CSV)
    void StopSession();

    // Check if a recording session is active
    bool IsSessionActive() const { return m_sessionActive.load(); }

    // 获取已保存的帧数
    uint32_t GetSavedFrameCount() const { return m_savedFrameCount.load(); }

    // 获取当前队列大小
    size_t GetQueueSize() const;

    // 设置是否正在保存
    bool IsSaving() const { return m_isSaving.load(); }

    // Set BOOTTIME→REALTIME offset for timestamp conversion (called once per recording session)
    void SetTimeOffset(int64_t offsetNs) { m_timeOffsetNs = offsetNs; }

    // MCAP 数据集：mcap 模式写 /hand_tracking 通道替代 CSV。
    // 每次录制由 beginRecordingCommon 统一重注入（provider 每次重建，防悬垂）。
    void setMcapManager(xr::mcap::McapChunkManager* m) { mSink.setMcapManager(m); }
    void setFormatProvider(const xr::IDatasetFormatProvider* p) { mSink.setFormatProvider(p); }

    // 重置帧计数器
    void ResetFrameCounter() { m_frameCounter = 0; }

private:
    // 保存工作线程函数
    void SaveWorkerThread();
    // 单条写出（mcap 通道或 CSV 行）：SaveWorkerThread 与 StopSession drain 共用
    void writeFrame(const FrameData& frameData);

    // 将帧数据转换为CSV格式字符串（一帧一行）
    std::string FrameToCsv(const FrameData& frameData);

    // mcap row：与 hand_tracking.csv 行同构的 JSON 对象
    std::string FrameToJson(const FrameData& frameData);

    // 生成CSV表头
    std::string GenerateCsvHeader();

    // 保存数据到文件
    bool SaveToFile(const std::string& filename, const std::string& content);

    // 确保目录存在
    bool EnsureDirectoryExists(const std::string& path);

private:
    // 保存路径
    std::string m_savePath;

    // 帧数据队列
    std::queue<FrameData> m_frameQueue;

    // 队列互斥锁
    mutable std::mutex m_queueMutex;

    // 条件变量
    std::condition_variable m_queueCondition;

    // 保存线程
    std::thread m_saveThread;

    // 是否正在运行
    std::atomic<bool> m_isRunning;

    // 是否暂停
    std::atomic<bool> m_isPaused;

    // 是否正在保存
    std::atomic<bool> m_isSaving;

    // 已保存的帧数
    std::atomic<uint32_t> m_savedFrameCount;

    // 当前帧计数器
    std::atomic<uint32_t> m_frameCounter;

    // CSV 文件流 (追加模式)
    std::ofstream m_csvFile;

    // CSV文件互斥锁
    std::mutex m_csvMutex;

    // Whether a recording session is active
    std::atomic<bool> m_sessionActive{false};

    // BOOTTIME→REALTIME offset for timestamp conversion
    int64_t m_timeOffsetNs{0};

    // MCAP：组合体持 mcap manager 裸指针 + atomic 格式 provider（写入线程读、录制启动线程写）
    xr::McapSinkCtx mSink;

    bool isMcapMode() const;  // 定义在 .cpp（委托 mSink.isMcap()）
};

#endif //HELLOXR_RAWDATESAVE_H
