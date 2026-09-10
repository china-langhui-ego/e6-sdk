#include "RecordingEncodeWorker.h"

#include <android/log.h>
#include <chrono>

// EGL extension function pointer (defined in SharedTexture.cpp, loaded via
// eglGetProcAddress); same extern declaration pattern as main.cpp.
namespace glext {
    extern PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
}

#define LOG_TAG "RecEncodeWorker"

#define LOGI(...)  ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...)  ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...)  ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

namespace SXR {

RecordingEncodeWorker::RecordingEncodeWorker(const char* name, EGLDisplay display, size_t cap)
    : mName(name), mDisplay(display), mCap(cap) {}

RecordingEncodeWorker::~RecordingEncodeWorker() {
    stopAndJoin();
}

void RecordingEncodeWorker::start(ProcessFn fn) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mRunning) return;
    mFn = std::move(fn);
    mRunning = true;
    mThread = std::thread(&RecordingEncodeWorker::loop, this);
    LOGI("%s: started (cap=%zu)", mName.c_str(), mCap);
}

void RecordingEncodeWorker::resetCounters() {
    mDropped.store(0, std::memory_order_relaxed);
    mProcessed.store(0, std::memory_order_relaxed);
    mNotRunningDrop.store(0, std::memory_order_relaxed);
}

void RecordingEncodeWorker::enqueue(EncodeFrameItem item) {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mRunning) {
            // Worker not running: nothing will consume the item. Destroy any
            // owned images right away so camera HAL buffers are not leaked.
            // Throttled like the queue-full path (can hit once per frame in
            // the stop→close_group window).
            const uint32_t n = mNotRunningDrop.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || n % 16 == 0) {
                LOGW("%s: enqueue while not running, dropping frame %u (total=%u)",
                     mName.c_str(), item.frameId, n);
            }
            if (item.ownImages) destroyItemImages(item);
            return;
        }
        if (mQueue.size() >= mCap) {
            EncodeFrameItem old = std::move(mQueue.front());
            mQueue.pop_front();
            if (old.ownImages) destroyItemImages(old);
            const uint32_t dropped = mDropped.fetch_add(1, std::memory_order_relaxed) + 1;
            // Overflow must be visible: throttled like the sensor-align queue.
            if (dropped == 1 || dropped % 16 == 0) {
                LOGW("%s: queue full (cap=%zu), dropped total=%u", mName.c_str(), mCap, dropped);
            }
        }
        mQueue.push_back(std::move(item));
    }
    mCv.notify_one();
}

void RecordingEncodeWorker::stopAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mRunning) return;
        mRunning = false;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
    // Drain any items left after the loop exited (loop exits with an empty
    // queue; this is a belt-and-suspenders pass for the not-running enqueue).
    std::lock_guard<std::mutex> lock(mMutex);
    while (!mQueue.empty()) {
        EncodeFrameItem item = std::move(mQueue.front());
        mQueue.pop_front();
        if (item.ownImages) destroyItemImages(item);
    }
    while (!mPendingDestroy.empty()) {
        EncodeFrameItem item = std::move(mPendingDestroy.front());
        mPendingDestroy.pop_front();
        destroyItemImages(item);
    }
}

void RecordingEncodeWorker::destroyItemImages(EncodeFrameItem& item) {
    for (int i = 0; i < 2; i++) {
        if (item.img[i] != EGL_NO_IMAGE_KHR) {
            // EGL_KHR_image_base: destroy is deferred while any texture is
            // still bound to the image, so dropping a frame whose image is
            // currently bound in the GL pipeline is safe.
            glext::eglDestroyImageKHR(mDisplay, item.img[i]);
            item.img[i] = EGL_NO_IMAGE_KHR;
        }
    }
}

void RecordingEncodeWorker::loop() {
    // Destroy an owned image only after kDestroyDelay newer images have been
    // processed: gives the GPU command stream referencing the buffer time to
    // complete before the buffer returns to the camera HAL (see the
    // mPendingDestroy note in the header).
    constexpr size_t kDestroyDelay = 8;
    // Idle flush: once the queue has been empty this long, the GPU is certainly
    // done with the pending images (8 frames ≪ timeout), so release them back to
    // the camera HAL instead of holding until the next enqueue — the RGB worker
    // gets no new items once recording stops, and pinned gralloc buffers risk
    // starving the HAL buffer pool during long preview-only stretches.
    constexpr auto kIdleFlush = std::chrono::milliseconds(500);
    for (;;) {
        EncodeFrameItem item;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mPendingDestroy.empty()) {
                mCv.wait(lock, [this] { return !mQueue.empty() || !mRunning; });
            } else {
                mCv.wait_for(lock, kIdleFlush,
                             [this] { return !mQueue.empty() || !mRunning; });
            }
            if (mQueue.empty()) {
                if (!mRunning) break;
                // Idle timeout with pending destroys: flush them now.
                while (!mPendingDestroy.empty()) {
                    EncodeFrameItem old = std::move(mPendingDestroy.front());
                    mPendingDestroy.pop_front();
                    destroyItemImages(old);
                }
                continue;
            }
            item = std::move(mQueue.front());
            mQueue.pop_front();
        }

        if (mFn) mFn(item);

        if (item.ownImages) {
            mPendingDestroy.push_back(std::move(item));
            while (mPendingDestroy.size() > kDestroyDelay) {
                EncodeFrameItem old = std::move(mPendingDestroy.front());
                mPendingDestroy.pop_front();
                destroyItemImages(old);
            }
        }

        const uint32_t processed = mProcessed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (processed % 300 == 0) {
            std::lock_guard<std::mutex> lock(mMutex);
            LOGI("%s: processed=%u qdepth=%zu dropped=%u",
                 mName.c_str(), processed, mQueue.size(), mDropped.load());
        }
    }
}

} // namespace SXR
