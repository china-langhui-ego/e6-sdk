#include "ImuPoseCollector.h"
#include "RecordingGatekeeper.h"
#include "mcap/McapChunkManager.h"
#include "mcap/McapSchemas.h"
#include <android/log.h>
#include <cstring>

#define LOG_TAG "ImuPoseCollector"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

ImuPoseCollector::ImuPoseCollector() = default;

ImuPoseCollector::~ImuPoseCollector() {
    stop();
}

bool ImuPoseCollector::start(const std::string& accelCsvPath, const std::string& gyroCsvPath) {
    if (mRunning.load()) {
        LOGW("Already running");
        return false;
    }

    mAccelCount = 0;
    mGyroCount = 0;
    mAccelReadyNotified = false;
    mGyroReadyNotified = false;
    mLastAccelUtcNs = 0;
    mLastGyroUtcNs = 0;
    mAccelClampCount = 0;
    mGyroClampCount = 0;

    // mcap 模式不开 CSV（写 /imu/accel /imu/gyro 通道替代）
    const bool mcapMode = isMcapMode();
    if (!mcapMode) {
        mAccelFile.open(accelCsvPath, std::ios::out | std::ios::trunc);
        if (!mAccelFile.is_open()) {
            LOGE("Failed to open accel CSV: %s", accelCsvPath.c_str());
            return false;
        }
        mAccelFile << "timestamp_ns,x,y,z\n";
        mAccelFile.flush();

        mGyroFile.open(gyroCsvPath, std::ios::out | std::ios::trunc);
        if (!mGyroFile.is_open()) {
            LOGE("Failed to open gyro CSV: %s", gyroCsvPath.c_str());
            mAccelFile.close();
            return false;
        }
        mGyroFile << "timestamp_ns,x,y,z\n";
        mGyroFile.flush();
    }

    mSensorManager = ASensorManager_getInstanceForPackage("com.ssnwt.helloxr");
    if (!mSensorManager) {
        LOGE("Failed to get ASensorManager");
        mAccelFile.close();
        mGyroFile.close();
        return false;
    }

    mWriterRunning = true;
    mWriterThread = std::thread(&ImuPoseCollector::writerThreadFunc, this);

    mRunning = true;
    mSensorThread = std::thread(&ImuPoseCollector::sensorThreadFunc, this);

    LOGI("ImuPoseCollector started: %s, %s", accelCsvPath.c_str(), gyroCsvPath.c_str());
    return true;
}

bool ImuPoseCollector::isMcapMode() const { return mSink.isMcap(); }

// mcap 通道或 CSV 文件写入一条 IMU 样本（计数与周期 flush 由调用方负责；
// mcap 模式下 mAccelFile/mGyroFile 未打开，调用方周期 flush 为空操作）
void ImuPoseCollector::writeSample(bool isAccel, int64_t utcNs, float x, float y, float z) {
    // IMU 时间戳单调钳制：offset 采样污染致单点负增长时逐点钳制；
    // 统一对 mcap 通道与 CSV 生效（mcap 时间戳同样要求单调）。
    if (isAccel) {
        if (utcNs <= mLastAccelUtcNs) { utcNs = mLastAccelUtcNs + 1; mAccelClampCount++; }
        mLastAccelUtcNs = utcNs;
    } else {
        if (utcNs <= mLastGyroUtcNs) { utcNs = mLastGyroUtcNs + 1; mGyroClampCount++; }
        mLastGyroUtcNs = utcNs;
    }
    auto* mcapMgr = mSink.mgr.load();
    if (isMcapMode() && mcapMgr) {
        if (auto w = mcapMgr->current()) {
            w->writeJson(isAccel ? "/imu/accel" : "/imu/gyro", utcNs,
                              isAccel ? xr::mcap::accelJson(utcNs, x, y, z)
                                      : xr::mcap::gyroJson(utcNs, x, y, z));
        } else {
            // begin() 失败后 current() 恒 null：限频告警避免静默丢弃（共用节流 helper）
            static std::atomic<uint32_t> sNoWriterWarnCount{0};
            if (xr::mcap::noWriterWarnThrottled(sNoWriterWarnCount)) {
                LOGW("writeSample: mcap active but no current writer (mcap create failed?); samples dropped");
            }
        }
    } else if (isAccel) {
        mAccelFile << utcNs << "," << x << "," << y << "," << z << "\n";
    } else {
        mGyroFile << utcNs << "," << x << "," << y << "," << z << "\n";
    }
}

void ImuPoseCollector::stop() {
    if (!mRunning.load()) return;

    mRunning = false;
    if (mLooper) ALooper_wake(mLooper);
    if (mSensorThread.joinable()) mSensorThread.join();

    mWriterRunning = false;
    mQueueCV.notify_all();
    if (mWriterThread.joinable()) mWriterThread.join();

    // Flush remaining (gated on both the start gate and the end cutoff, so a
    // stop before/after the gate window writes nothing outside it).
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        while (!mWriteQueue.empty()) {
            auto& e = mWriteQueue.front();
            if (!recordingGateIsOpen(e.timestamp) || !recordingGateBeforeStop(e.timestamp)) {
                mWriteQueue.pop();
                continue;
            }
            int64_t utcNs = e.timestamp + mTimeOffsetNs;
            writeSample(e.isAccel, utcNs, e.x, e.y, e.z);
            mWriteQueue.pop();
        }
    }

    if (mAccelFile.is_open()) { mAccelFile.flush(); mAccelFile.close(); }
    if (mGyroFile.is_open()) { mGyroFile.flush(); mGyroFile.close(); }

    LOGI("ImuPoseCollector stopped. Accel: %lu, Gyro: %lu",
         (unsigned long)mAccelCount.load(), (unsigned long)mGyroCount.load());
    if (mAccelClampCount > 0 || mGyroClampCount > 0) {
        LOGW("IMU monotonic clamp triggered: accel=%lu gyro=%lu (offset sampling pollution)",
             (unsigned long)mAccelClampCount, (unsigned long)mGyroClampCount);
    }
}

void ImuPoseCollector::sensorThreadFunc() {
    mLooper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    mEventQueue = ASensorManager_createEventQueue(mSensorManager, mLooper, 100, nullptr, nullptr);
    if (!mEventQueue) {
        LOGE("Failed to create sensor event queue");
        mRunning = false;
        return;
    }

    const ASensor* accelSensor = ASensorManager_getDefaultSensor(mSensorManager, ASENSOR_TYPE_ACCELEROMETER);
    if (accelSensor) {
        int ret = ASensorEventQueue_enableSensor(mEventQueue, accelSensor);
        if (ret < 0) {
            LOGE("Failed to enable accelerometer: %d", ret);
        } else {
            ASensorEventQueue_setEventRate(mEventQueue, accelSensor, 0);
            LOGI("Accelerometer enabled at FASTEST rate");
        }
    } else {
        LOGW("No accelerometer found");
    }

    const ASensor* gyroSensor = ASensorManager_getDefaultSensor(mSensorManager, ASENSOR_TYPE_GYROSCOPE);
    if (gyroSensor) {
        int ret = ASensorEventQueue_enableSensor(mEventQueue, gyroSensor);
        if (ret < 0) {
            LOGE("Failed to enable gyroscope: %d", ret);
        } else {
            ASensorEventQueue_setEventRate(mEventQueue, gyroSensor, 0);
            LOGI("Gyroscope enabled at FASTEST rate");
        }
    } else {
        LOGW("No gyroscope found");
    }

    while (mRunning.load()) {
        int ident = ALooper_pollAll(1, nullptr, nullptr, nullptr);
        if (ident == ALOOPER_POLL_TIMEOUT || ident == ALOOPER_POLL_WAKE) continue;

        ASensorEvent event;
        while (ASensorEventQueue_getEvents(mEventQueue, &event, 1) > 0) {
            ImuEvent ie{};
            ie.timestamp = event.timestamp;

            if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
                if (!mAccelReadyNotified) {
                    recordingGateNotifyReady(RecordingGatekeeper::Source::ACCEL);
                    mAccelReadyNotified = true;
                }
                ie.x = event.acceleration.x;
                ie.y = event.acceleration.y;
                ie.z = event.acceleration.z;
                ie.isAccel = true;
            } else if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                if (!mGyroReadyNotified) {
                    recordingGateNotifyReady(RecordingGatekeeper::Source::GYRO);
                    mGyroReadyNotified = true;
                }
                ie.x = event.data[0];
                ie.y = event.data[1];
                ie.z = event.data[2];
                ie.isAccel = false;
            } else {
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mQueueMutex);
                mWriteQueue.push(ie);
            }
            mQueueCV.notify_one();
        }
    }

    if (accelSensor) ASensorEventQueue_disableSensor(mEventQueue, accelSensor);
    if (gyroSensor) ASensorEventQueue_disableSensor(mEventQueue, gyroSensor);
    if (mEventQueue && mSensorManager) {
        ASensorManager_destroyEventQueue(mSensorManager, mEventQueue);
        mEventQueue = nullptr;
    }
    mLooper = nullptr;
    LOGI("Sensor thread exited");
}

void ImuPoseCollector::writerThreadFunc() {
    LOGI("Writer thread started");

    while (mWriterRunning.load() || !mWriteQueue.empty()) {
        ImuEvent event;
        bool hasEvent = false;

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mQueueCV.wait_for(lock, std::chrono::milliseconds(1), [this] {
                return !mWriteQueue.empty() || !mWriterRunning.load();
            });
            if (!mWriteQueue.empty()) {
                event = mWriteQueue.front();
                mWriteQueue.pop();
                hasEvent = true;
            }
        }

        if (hasEvent) {
            if (!recordingGateIsOpen(event.timestamp) || !recordingGateBeforeStop(event.timestamp)) {
                continue;  // drop until the synchronized start opens, and after the end cutoff
            }
            float x = event.x, y = event.y, z = event.z;

            int64_t utcNs = event.timestamp + mTimeOffsetNs;
            writeSample(event.isAccel, utcNs, x, y, z);
            if (event.isAccel) {
                mAccelCount++;
                if (mAccelCount % 200 == 0) mAccelFile.flush();
            } else {
                mGyroCount++;
                if (mGyroCount % 200 == 0) mGyroFile.flush();
            }
        }
    }

    if (mAccelFile.is_open()) mAccelFile.flush();
    if (mGyroFile.is_open()) mGyroFile.flush();
    LOGI("Writer thread exited");
}
