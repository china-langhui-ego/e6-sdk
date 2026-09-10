#include "ControllerPoseSaver.h"
#include "RecordingGatekeeper.h"
#include "mcap/McapChunkManager.h"
#include "mcap/McapSchemas.h"
#include <android/log.h>
#include <sys/stat.h>
#include <sstream>
#include <iomanip>
#include <cstring>

#define LOG_TAG "ControllerPoseSaver"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

ControllerPoseSaver::ControllerPoseSaver() = default;

ControllerPoseSaver::~ControllerPoseSaver() {
    Shutdown();
}

void ControllerPoseSaver::Init(const std::string& savePath) {
    if (m_running) return;
    m_savePath = savePath;
    m_running = true;
    m_paused = false;
    m_savedCount = 0;
    m_writerThread = std::thread(&ControllerPoseSaver::WriterThread, this);
    LOGI("ControllerPoseSaver initialized: %s", savePath.c_str());
}

void ControllerPoseSaver::Shutdown() {
    if (!m_running) return;
    m_running = false;
    m_queueCv.notify_all();
    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_csvMutex);
        if (m_csvFile.is_open()) {
            m_csvFile.flush();
            m_csvFile.close();
        }
    }
    LOGI("ControllerPoseSaver shutdown. Saved %u frames", m_savedCount.load());
}

bool ControllerPoseSaver::isMcapMode() const { return mSink.isMcap(); }

bool ControllerPoseSaver::StartSession(const std::string& csvPath) {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (m_csvFile.is_open()) {
        m_csvFile.flush();
        m_csvFile.close();
    }
    // mcap 模式不开 CSV（写 /controller_poses 通道替代），session 照常激活
    if (!isMcapMode()) {
        m_csvFile.open(csvPath, std::ios::out | std::ios::trunc);
        if (!m_csvFile.is_open()) {
            LOGE("Failed to open CSV: %s", csvPath.c_str());
            return false;
        }
        m_csvFile << GenerateHeader();
        m_csvFile.flush();
    }
    m_sessionActive = true;
    LOGI("ControllerPoseSaver session started: %s", csvPath.c_str());
    return true;
}

void ControllerPoseSaver::StopSession() {
    // drain 剩余队列（对齐 ImuPoseCollector::stop()）：WriterThread 与 m_running 解耦、
    // 不 join，且写前检查 m_sessionActive——只关会话标志不排空则尾部在途记录全部丢弃。
    // 与 WriterThread 互斥（m_queueMutex），swap 后 WriterThread 从空队列取不到记录，
    // 此处是剩余记录的唯一定稿者。
    if (m_sessionActive.exchange(false)) {
        std::queue<ControllerPoseRecord> drain;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            drain.swap(m_queue);
        }
        while (!drain.empty()) {
            writeRecord(drain.front());
            drain.pop();
        }
    }
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (m_csvFile.is_open()) {
        m_csvFile.flush();
        m_csvFile.close();
    }
    LOGI("ControllerPoseSaver session stopped");
}

// 单条写出：mcap 通道或 CSV 行。WriterThread（已过 gate 检查）与 StopSession drain 共用。
// 注意：本函数不做 gate 判断——drain 尾部直接写出（与 ImuPoseCollector::stop() 不同，
// 其 drain 保留 gate 过滤）；此处依赖 feed 侧 recordingGateWithinWindow 预过滤，
// 队列内记录基本都在窗口内，窗口外漏网者数量可忽略。
void ControllerPoseSaver::writeRecord(const ControllerPoseRecord& rec) {
    auto* mcapMgr = mSink.mgr.load();
    if (isMcapMode() && mcapMgr) {
        if (auto w = mcapMgr->current()) {
            const int64_t utcNs = rec.timestamp + m_timeOffsetNs;
            w->writeJson("/controller_poses", utcNs,
                              xr::mcap::controllerPosesJson(utcNs, RecordToJson(rec)));
        } else {
            // begin() 失败后 current() 恒 null：限频告警避免静默丢弃（共用节流 helper）
            static std::atomic<uint32_t> sNoWriterWarnCount{0};
            if (xr::mcap::noWriterWarnThrottled(sNoWriterWarnCount)) {
                LOGW("writeRecord: mcap active but no current writer (mcap create failed?); records dropped");
            }
        }
    } else {
        std::string line = RecordToCsv(rec);
        {
            std::lock_guard<std::mutex> lock(m_csvMutex);
            if (m_csvFile.is_open()) {
                m_csvFile << line;
                m_csvFile.flush();
            }
        }
    }
    m_savedCount++;
}

bool ControllerPoseSaver::SaveFrame(const ControllerPoseRecord& record) {
    if (!m_running || m_paused || !m_sessionActive.load()) return false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(record);
    }
    m_queueCv.notify_one();
    return true;
}

void ControllerPoseSaver::Pause() {
    m_paused = true;
}

void ControllerPoseSaver::Resume() {
    m_paused = false;
    m_queueCv.notify_all();
}

void ControllerPoseSaver::WriterThread() {
    while (m_running) {
        ControllerPoseRecord rec;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] {
                return !m_queue.empty() || !m_running;
            });
            if (!m_running) break;
            if (m_paused || m_queue.empty()) continue;
            rec = m_queue.front();
            m_queue.pop();
        }

        if (!m_sessionActive.load()) continue;

        if (!recordingGateIsOpen(rec.timestamp) || !recordingGateBeforeStop(rec.timestamp)) {
            continue;  // drop controller frame outside [gate-open, end-cutoff]
        }

        writeRecord(rec);
    }
}

std::string ControllerPoseSaver::GenerateHeader() {
    return "frame_number,timestamp_ns,left_active,"
           "left_px,left_py,left_pz,left_qx,left_qy,left_qz,left_qw,"
           "right_active,"
           "right_px,right_py,right_pz,right_qx,right_qy,right_qz,right_qw\n";
}

std::string ControllerPoseSaver::RecordToCsv(const ControllerPoseRecord& rec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << rec.frameNumber << ","
        << (rec.timestamp + m_timeOffsetNs) << ","
        << (rec.leftActive ? 1 : 0) << ",";

    if (rec.leftActive) {
        float pos[3], quat[4];
        memcpy(pos, rec.leftPos, sizeof(pos));
        memcpy(quat, rec.leftQuat, sizeof(quat));
        oss << pos[0] << "," << pos[1] << "," << pos[2] << ","
            << quat[0] << "," << quat[1] << "," << quat[2] << "," << quat[3];
    } else {
        oss << "0.0,0.0,0.0,0.0,0.0,0.0,0.0";
    }
    oss << "," << (rec.rightActive ? 1 : 0) << ",";
    if (rec.rightActive) {
        float pos[3], quat[4];
        memcpy(pos, rec.rightPos, sizeof(pos));
        memcpy(quat, rec.rightQuat, sizeof(quat));
        oss << pos[0] << "," << pos[1] << "," << pos[2] << ","
            << quat[0] << "," << quat[1] << "," << quat[2] << "," << quat[3];
    } else {
        oss << "0.0,0.0,0.0,0.0,0.0,0.0,0.0";
    }
    oss << "\n";
    return oss.str();
}

// mcap row JSON：字段与 controller_poses.csv 列同构（frame_number,timestamp_ns,
// left_active,left_p*,left_q*,right_active,right_p*,right_q*）；inactive 侧与 CSV 一致填 0。
std::string ControllerPoseSaver::RecordToJson(const ControllerPoseRecord& rec) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << "{\"frame_number\":" << rec.frameNumber
        << ",\"timestamp_ns\":" << (rec.timestamp + m_timeOffsetNs);

    // left/right 两块完全同构，lambda 复用；inactive 侧与 CSV 一致输出 "0.0" 字面量。
    auto emitSide = [&oss](const char* prefix, bool active,
                           const float pos[3], const float quat[4]) {
        oss << ",\"" << prefix << "_active\":" << (active ? 1 : 0);
        if (active) {
            oss << ",\"" << prefix << "_px\":" << pos[0] << ",\"" << prefix << "_py\":" << pos[1]
                << ",\"" << prefix << "_pz\":" << pos[2]
                << ",\"" << prefix << "_qx\":" << quat[0] << ",\"" << prefix << "_qy\":" << quat[1]
                << ",\"" << prefix << "_qz\":" << quat[2] << ",\"" << prefix << "_qw\":" << quat[3];
        } else {
            oss << ",\"" << prefix << "_px\":0.0,\"" << prefix << "_py\":0.0,\"" << prefix << "_pz\":0.0"
                << ",\"" << prefix << "_qx\":0.0,\"" << prefix << "_qy\":0.0,\"" << prefix << "_qz\":0.0,\"" << prefix << "_qw\":0.0";
        }
    };
    emitSide("left", rec.leftActive, rec.leftPos, rec.leftQuat);
    emitSide("right", rec.rightActive, rec.rightPos, rec.rightQuat);
    oss << "}";
    return oss.str();
}
