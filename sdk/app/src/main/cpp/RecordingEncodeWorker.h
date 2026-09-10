#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace SXR {

/// One camera frame to be encoded off the camera-callback thread.
/// Producer (camera callback thread) fills the item and enqueues it; the
/// worker thread consumes items through the injected ProcessFn.
///
/// EGLImage ownership:
/// - ownImages=true (all current producers): ownership transfers to the item.
///   The queue destroys images when dropping the item (queue full) or after
///   processing; the producer must NOT destroy them after a successful enqueue.
/// - ownImages=false (reserved, unused): the worker never destroys img[];
///   ownership stays with the producer.
///
/// Timing fields mirror SXR::CameraEncoder::FrameMeta (BOOTTIME domain):
/// midExposureNs feeds mcap ptsUs (via EncoderSurface::setPresentationTime)
/// and the recording-gate refine anchor; exposureStartBootNs feeds gray SBS
/// setPts (exposure START there, not midpoint) and SBS FrameMeta.
struct EncodeFrameItem {
    EGLImageKHR img[2]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    bool ownImages{true};

    int64_t midExposureNs{0};
    int64_t exposureStartBootNs{0};
    uint32_t exposure{0};
    uint32_t gain{0};
    uint32_t frameId{0};

    // Per-eye geometry (worker computes SBS half-viewports from these)
    int eyeWidth{0};
    int eyeHeight{0};

    // Encoding flags evaluated at enqueue time (callback thread), so the
    // worker never re-reads racy gate/encoder state.
    bool doMcapEyes{false};    // mcap per-eye blit (both eyes)
    bool doSbs{false};         // SBS encoder submit (gray streaming preview)
};

/// FIFO queue + dedicated worker thread that executes an injected ProcessFn
/// per item, off the camera-callback thread. When the queue is full the
/// OLDEST item is dropped (its owned EGLImages destroyed immediately by the
/// producer) so the callback thread never blocks and back-pressure never
/// reaches the camera HAL — dropped frames stay an app-layer, per-group
/// event instead of stalling the whole camera subsystem.
///
/// The queue mutex only guards enqueue/dequeue; ProcessFn runs unlocked (it
/// takes the group's encoder mutex itself). One worker instance per camera
/// group; instances are fully independent (no shared GL objects between
/// groups — each ProcessFn uses its group's own textures/VBO).
///
/// Lifecycle: start() before the camera group is opened; stopAndJoin() must
/// complete BEFORE sxr_camera_close_group (same hard rule as the sensor-align
/// worker: in-flight items may touch group GL objects).
class RecordingEncodeWorker {
public:
    using ProcessFn = std::function<void(EncodeFrameItem&)>;

    RecordingEncodeWorker(const char* name, EGLDisplay display, size_t cap);
    ~RecordingEncodeWorker();  // safety net: calls stopAndJoin()

    RecordingEncodeWorker(const RecordingEncodeWorker&) = delete;
    RecordingEncodeWorker& operator=(const RecordingEncodeWorker&) = delete;

    // Start the worker thread. Idempotent while running.
    void start(ProcessFn fn);

    // Enqueue one frame. Never blocks: when full, drops the OLDEST item.
    // Ownership of item.img[] transfers to the worker when ownImages=true;
    // the caller must not touch item afterwards in that case.
    void enqueue(EncodeFrameItem item);

    // Stop, join, and drain (destroying any remaining owned images).
    // Idempotent.
    void stopAndJoin();

    uint32_t droppedCount() const { return mDropped.load(std::memory_order_relaxed); }
    // Reset per-session counters (mDropped/mProcessed/mNotRunningDrop). Called
    // between camera sessions (close→open) so per-session log/drop accounting
    // matches the sensor-align worker (which resets s_alignDropped per session).
    void resetCounters();

private:
    void loop();
    void destroyItemImages(EncodeFrameItem& item);

    std::string mName;
    EGLDisplay mDisplay{EGL_NO_DISPLAY};
    size_t mCap{4};

    ProcessFn mFn;

    std::deque<EncodeFrameItem> mQueue;
    // Processed items awaiting image destruction: the item's EGLImage may
    // still be referenced by an in-flight GPU command stream (swapBuffers
    // returns after the CPU-side queue, the GPU may lag). Destroying the
    // image immediately lets the HAL recycle the buffer mid-render and the
    // next swapBuffers blocks forever on a fence that can never signal (observed as
    // ioctl_kgsl_cmdstream_waittimestampevent hang). Delay by a few frames;
    // the loop also flushes this queue after a short idle timeout so buffers
    // are not pinned indefinitely when items stop arriving (e.g. RGB worker
    // after recording stops).
    std::deque<EncodeFrameItem> mPendingDestroy;
    mutable std::mutex mMutex;
    std::condition_variable mCv;
    std::thread mThread;
    bool mRunning{false};  // guarded by mMutex

    std::atomic<uint32_t> mDropped{0};
    std::atomic<uint32_t> mProcessed{0};
    // enqueue-while-not-running drop count (per-instance: keeps rgb/tracking/ctrl
    // logs independently attributable; reset per session via resetCounters()).
    std::atomic<uint32_t> mNotRunningDrop{0};
};

} // namespace SXR
