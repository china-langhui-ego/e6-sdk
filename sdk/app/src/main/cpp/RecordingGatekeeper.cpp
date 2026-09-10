// app/src/main/cpp/RecordingGatekeeper.cpp
#include "RecordingGatekeeper.h"
#include <android/log.h>

#define LOG_TAG "RecordingGatekeeper"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))

static int64_t nowBoottimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

RecordingGatekeeper::~RecordingGatekeeper() { reset(); }

void RecordingGatekeeper::arm() {
    State expected = State::IDLE;
    if (!mState.compare_exchange_strong(expected, State::ARMING,
                                        std::memory_order_acq_rel)) {
        return;  // already armed/open — idempotent
    }
    mReadyMask.store(0, std::memory_order_relaxed);
    mGateOpenTsNs.store(0, std::memory_order_relaxed);
    mArmedAtNs.store(nowBoottimeNs(), std::memory_order_relaxed);
    mThreadsRunning.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mThreadMutex);
        mWatchdogThread = std::thread(&RecordingGatekeeper::watchdogLoop, this);
    }
    LOGI("ARMING (waiting for %u sources)", (unsigned)Source::COUNT);
}

void RecordingGatekeeper::reset() {
    mState.store(State::IDLE, std::memory_order_release);
    mReadyMask.store(0, std::memory_order_relaxed);
    mGateRefinedToRgb.store(false, std::memory_order_relaxed);
    mThreadsRunning.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mCvMutex);
        mCv.notify_all();
    }
    // Join under mThreadMutex so a concurrent arm()/notifyReady() assignment
    // is serialized against the joinable() checks (no data race, no
    // joinable-thread-left-unjoined).
    {
        std::lock_guard<std::mutex> lk(mThreadMutex);
        if (mWatchdogThread.joinable()) mWatchdogThread.join();
        if (mStableThread.joinable()) mStableThread.join();
    }
}

void RecordingGatekeeper::refineGateToRgbFirstFrame(int64_t rgbMidExposureNs) {
    if (mState.load(std::memory_order_acquire) != State::OPEN) return;
    bool expected = false;
    if (mGateRefinedToRgb.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
        mGateOpenTsNs.store(rgbMidExposureNs, std::memory_order_relaxed);
        LOGI("gate refined to RGB first frame: gateOpenTsNs=%lld", (long long)rgbMidExposureNs);
    }
}

void RecordingGatekeeper::notifyReady(Source s) {
    if (mState.load(std::memory_order_acquire) != State::ARMING) return;
    const uint32_t bit = 1u << (uint32_t)s;
    const uint32_t prev = mReadyMask.fetch_or(bit, std::memory_order_acq_rel);
    if (prev & bit) return;  // already reported
    const uint32_t now = prev | bit;
    LOGI("ready: source=%u mask=0b%06u", (unsigned)s, now);

    if (now == ALL_READY) {
        // Hold mThreadMutex across the state CAS and the thread construction:
        // reset() joins under the same lock, so either the thread is fully
        // assigned before reset's joinable() check (and gets joined), or reset
        // has already stored IDLE and this CAS fails — no unjoined thread, no
        // data race on the std::thread member.
        std::lock_guard<std::mutex> lk(mThreadMutex);
        State e = State::ARMING;
        if (mState.compare_exchange_strong(e, State::WAIT_STABLE,
                                           std::memory_order_acq_rel)) {
            mStableAtNs.store(nowBoottimeNs(), std::memory_order_relaxed);
            mStableThread = std::thread(&RecordingGatekeeper::stableLoop, this);
            LOGI("all sources ready -> WAIT_STABLE (1.5s)");
        }
    }
}

void RecordingGatekeeper::stableLoop() {
    std::unique_lock<std::mutex> lk(mCvMutex);
    const int64_t deadline = mStableAtNs.load(std::memory_order_relaxed) + STABLE_WINDOW_NS;
    while (mThreadsRunning.load(std::memory_order_acquire) &&
           mState.load(std::memory_order_acquire) == State::WAIT_STABLE) {
        const int64_t remainNs = deadline - nowBoottimeNs();
        if (remainNs <= 0) break;
        mCv.wait_for(lk, std::chrono::nanoseconds(remainNs));
    }
    {
        State e = State::WAIT_STABLE;
        if (mState.load(std::memory_order_acquire) == State::WAIT_STABLE) {
            // A2: provisional gate = wall clock at stable-window expiry. The RGB
            // source re-anchors it to its first post-gate frame's mid-exposure
            // (refineGateToRgbFirstFrame) so the dataset start == RGB first frame.
            mGateOpenTsNs.store(nowBoottimeNs(), std::memory_order_relaxed);
            if (mState.compare_exchange_strong(e, State::OPEN,
                                               std::memory_order_acq_rel)) {
                LOGI("OPEN gateOpenTsNs=%lld (provisional)", (long long)mGateOpenTsNs.load());
            }
        }
    }
}

void RecordingGatekeeper::watchdogLoop() {
    std::unique_lock<std::mutex> lk(mCvMutex);
    const int64_t deadline = mArmedAtNs.load(std::memory_order_relaxed) + TIMEOUT_NS;
    while (mThreadsRunning.load(std::memory_order_acquire) &&
           mState.load(std::memory_order_acquire) == State::ARMING) {
        const int64_t remainNs = deadline - nowBoottimeNs();
        if (remainNs <= 0) break;
        mCv.wait_for(lk, std::chrono::nanoseconds(remainNs));
    }
    // Timed out still ARMING -> degrade gracefully: open the gate so healthy
    // sources keep recording; missing sources just leave their files absent.
    // Symmetric with stableLoop: store the provisional gate timestamp first,
    // then CAS to OPEN. The CAS can only win from ARMING (stableLoop's wins
    // from WAIT_STABLE), so the two paths never both succeed.
    {
        State e = State::ARMING;
        if (mState.load(std::memory_order_acquire) == State::ARMING) {
            mGateOpenTsNs.store(nowBoottimeNs(), std::memory_order_relaxed);
            if (mState.compare_exchange_strong(e, State::OPEN,
                                               std::memory_order_acq_rel)) {
                LOGW("OPEN (degraded): timeout waiting for sources, ready=0b%06u missing=0b%06u",
                     mReadyMask.load(std::memory_order_relaxed),
                     ALL_READY ^ mReadyMask.load(std::memory_order_relaxed));
            }
        }
    }
}

