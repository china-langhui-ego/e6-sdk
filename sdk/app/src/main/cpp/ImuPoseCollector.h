#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <fstream>

#include <android/sensor.h>
#include <android/looper.h>

#include "DatasetFormat.h"

class ImuPoseCollector {
public:
    ImuPoseCollector();
    ~ImuPoseCollector();

    // Start collecting IMU data to separate accel.csv and gyro.csv files.
    bool start(const std::string& accelCsvPath, const std::string& gyroCsvPath);

    void stop();

    bool isRunning() const { return mRunning.load(); }

    // Set BOOTTIME→REALTIME offset for timestamp conversion (called once per recording session)
    void setTimeOffset(int64_t offsetNs) { mTimeOffsetNs = offsetNs; }

    // MCAP 数据集：mcap 模式写 /imu/accel /imu/gyro 通道替代 CSV。
    // 每次录制由 beginRecordingCommon 统一重注入（provider 每次重建，防悬垂）。
    void setMcapManager(xr::mcap::McapChunkManager* m) { mSink.setMcapManager(m); }
    void setFormatProvider(const xr::IDatasetFormatProvider* p) { mSink.setFormatProvider(p); }

private:
    void sensorThreadFunc();
    void writerThreadFunc();

    // Write queue entry
    struct ImuEvent {
        int64_t timestamp;
        float x, y, z;
        bool isAccel;  // true=accelerometer, false=gyroscope
    };

    // Sensor state
    ASensorManager* mSensorManager{nullptr};
    ASensorEventQueue* mEventQueue{nullptr};
    ALooper* mLooper{nullptr};
    std::thread mSensorThread;
    std::atomic<bool> mRunning{false};

    // Write queue
    std::queue<ImuEvent> mWriteQueue;
    std::mutex mQueueMutex;
    std::condition_variable mQueueCV;
    std::thread mWriterThread;
    std::atomic<bool> mWriterRunning{false};

    // CSV output
    std::ofstream mAccelFile;
    std::ofstream mGyroFile;
    std::atomic<uint64_t> mAccelCount{0};
    std::atomic<uint64_t> mGyroCount{0};

    // BOOTTIME→REALTIME offset for timestamp conversion
    int64_t mTimeOffsetNs{0};

    // 单调钳制：offset 采样污染可能造成单点回退，钳制到 last+1ns 保时间戳单调。
    // 仅 writer 线程与 stop() flush（join writer 之后）访问，无并发。
    int64_t mLastAccelUtcNs{0};
    int64_t mLastGyroUtcNs{0};
    uint64_t mAccelClampCount{0};
    uint64_t mGyroClampCount{0};

    // Gate: discard samples until the synchronized start opens.
    bool mAccelReadyNotified = false;
    bool mGyroReadyNotified = false;

    // MCAP：组合体持 mcap manager 裸指针 + atomic 格式 provider（写入线程读、录制启动线程写）
    xr::McapSinkCtx mSink;

    bool isMcapMode() const;  // 定义在 .cpp（委托 mSink.isMcap()）

    // mcap 通道或 CSV 文件写入一条 IMU 样本（stop 尾部 flush 与 writerThreadFunc 共用）
    void writeSample(bool isAccel, int64_t utcNs, float x, float y, float z);
};
