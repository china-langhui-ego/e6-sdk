// app/src/main/cpp/RecordingGatekeeper.h
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <condition_variable>

// Coordinates a synchronized start ("gate") across all dataset recording
// sources. Sources keep running but discard data until the gate opens; the
// gate opens once every source has produced its first sample AND a 1.5s
// stable window has elapsed. All timestamps are CLOCK_BOOTTIME ns.
class RecordingGatekeeper {
public:
    enum class Source : uint32_t { RGB=0, TRACKING=1, CTRL=2, AUDIO=3, ACCEL=4, GYRO=5, COUNT=6 };
    enum class State { IDLE, ARMING, WAIT_STABLE, OPEN };

    RecordingGatekeeper() = default;
    ~RecordingGatekeeper();

    // Begin waiting for sources. Only valid from IDLE; otherwise a no-op.
    void arm();
    // Back to IDLE from any state; stops timers. Called when recording stops.
    void reset();
    // Mark a source as having produced its first sample (idempotent per source).
    void notifyReady(Source s);
    // Lock-free hot path: true once OPEN and the sample is at/after the gate.
    bool isOpen(int64_t tsNs) const {
        return mState.load(std::memory_order_acquire) == State::OPEN &&
               tsNs >= mGateOpenTsNs.load(std::memory_order_relaxed);
    }
    State state() const { return mState.load(std::memory_order_acquire); }
    int64_t gateOpenTsNs() const { return mGateOpenTsNs.load(std::memory_order_relaxed); }

    // A2: refine the gate to the first RGB frame's mid-exposure. At OPEN the
    // gate timestamp is provisional (wall clock at the stable-window expiry);
    // the RGB source calls this on its first post-gate swapBuffers to re-anchor
    // the gate to that frame's mid-exposure, so the dataset start == RGB first
    // frame. Idempotent (only the first call wins).
    void refineGateToRgbFirstFrame(int64_t rgbMidExposureNs);

private:
    static constexpr int64_t STABLE_WINDOW_NS = 1500000000LL;   // 1.5s
    static constexpr int64_t TIMEOUT_NS       = 5000000000LL;   // 5s
    static constexpr uint32_t ALL_READY = (1u << (uint32_t)Source::COUNT) - 1u; // 0b111111

    // 5s timeout while ARMING. On timeout the gate degrades gracefully to OPEN
    // instead of aborting: healthy sources keep recording, missing sources just
    // leave their files absent (pre-gate behavior). No data is ever deleted.
    void watchdogLoop();
    void stableLoop();    // 1.5s settle window in WAIT_STABLE

    std::atomic<State> mState{State::IDLE};
    std::atomic<uint32_t> mReadyMask{0};
    std::atomic<int64_t> mGateOpenTsNs{0};
    std::atomic<bool> mGateRefinedToRgb{false};  // A2: true once gate re-anchored to RGB first frame
    std::atomic<int64_t> mArmedAtNs{0};      // CLOCK_BOOTTIME when ARMING began
    std::atomic<int64_t> mStableAtNs{0};     // CLOCK_BOOTTIME when WAIT_STABLE began

    // Serializes assignment (arm/notifyReady, camera callback threads) against
    // join (reset, teardown thread) of the two std::thread members. Without
    // this, a join racing the assignment is a data race (UB) and can leave a
    // joinable thread unjoined — std::terminate on the next assignment.
    std::mutex mThreadMutex;
    std::thread mWatchdogThread;
    std::thread mStableThread;
    std::atomic<bool> mThreadsRunning{false};
    std::mutex mCvMutex;
    std::condition_variable mCv;
};

// Cross-translation-unit accessors for the engine's RecordingGatekeeper
// (g_engine is a `static engine*` inside main.cpp and `engine` is only
// defined there, so other TUs cannot reach `g_engine->mGate` directly).
// Defined in main.cpp after the engine struct.
bool recordingGateIsOpen(int64_t tsNs);
void recordingGateNotifyReady(RecordingGatekeeper::Source src);
// A2: forward to RecordingGatekeeper::refineGateToRgbFirstFrame (null-guarded).
void recordingGateRefineToRgbFirstFrame(int64_t rgbMidExposureNs);

// A1/A4: end-of-recording alignment. The stop entry point captures a single
// CLOCK_BOOTTIME "stop" timestamp; every source drops samples LATER than it so
// the dataset end aligns across streams. Default INT64_MAX disables filtering
// (A4: degrade to drain-and-flush, dropping nothing); recordingGateResetStop()
// returns to that default so a new recording starts un-filtered.
void recordingGateSetStopNs(int64_t stopBootNs);
bool recordingGateBeforeStop(int64_t tsNs);  // true => tsNs is within the recording (keep)
void recordingGateResetStop();

// isOpen && beforeStop 组合谓词（对齐源项目命名）：调用点预过滤用，与 WriterThread 内联检查等价
bool recordingGateWithinWindow(int64_t tsNs);
