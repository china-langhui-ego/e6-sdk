#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <fstream>

#include "DatasetFormat.h"

struct ControllerPoseRecord {
    uint32_t frameNumber;
    int64_t timestamp;

    bool leftActive;
    float leftPos[3];
    float leftQuat[4];

    bool rightActive;
    float rightPos[3];
    float rightQuat[4];
};

class ControllerPoseSaver {
public:
    ControllerPoseSaver();
    ~ControllerPoseSaver();

    void Init(const std::string& savePath);
    void Shutdown();

    // Set BOOTTIME→REALTIME offset for timestamp conversion (called once per recording session)
    void SetTimeOffset(int64_t offsetNs) { m_timeOffsetNs = offsetNs; }

    // MCAP 数据集：mcap 模式写 /controller_poses 通道替代 CSV。
    // 每次录制由 beginRecordingCommon 统一重注入（provider 每次重建，防悬垂）。
    void setMcapManager(xr::mcap::McapChunkManager* m) { mSink.setMcapManager(m); }
    void setFormatProvider(const xr::IDatasetFormatProvider* p) { mSink.setFormatProvider(p); }

    bool StartSession(const std::string& csvPath);
    void StopSession();
    bool IsSessionActive() const { return m_sessionActive.load(); }

    bool SaveFrame(const ControllerPoseRecord& record);
    void Pause();
    void Resume();

private:
    void WriterThread();
    // 单条写出（mcap 通道或 CSV 行）：WriterThread 与 StopSession drain 共用
    void writeRecord(const ControllerPoseRecord& rec);
    std::string GenerateHeader();
    std::string RecordToCsv(const ControllerPoseRecord& rec);
    std::string RecordToJson(const ControllerPoseRecord& rec);  // mcap row：与 CSV 行同构的 JSON 对象

    std::string m_savePath;

    // BOOTTIME→REALTIME offset for timestamp conversion
    int64_t m_timeOffsetNs{0};
    std::queue<ControllerPoseRecord> m_queue;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;

    std::thread m_writerThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_sessionActive{false};
    std::atomic<uint32_t> m_savedCount{0};

    std::ofstream m_csvFile;
    std::mutex m_csvMutex;

    // MCAP：组合体持 mcap manager 裸指针 + atomic 格式 provider（写入线程读、录制启动线程写）
    xr::McapSinkCtx mSink;

    bool isMcapMode() const;  // 定义在 .cpp（委托 mSink.isMcap()）
};
