#pragma once

#include <media/NdkMediaCodec.h>
#include <aaudio/AAudio.h>
#include <android/log.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <vector>
#include <cstdio>

#include "FMP4Writer.h"

#define AUDIO_ENCODER_TAG "AudioEncoder"
#define AE_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, AUDIO_ENCODER_TAG, __VA_ARGS__))
#define AE_LOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, AUDIO_ENCODER_TAG, __VA_ARGS__))
#define AE_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, AUDIO_ENCODER_TAG, __VA_ARGS__))

class AudioEncoder {
public:
    AudioEncoder(int sampleRate = 44100, int bitRate = 96000, int channelCount = 1);
    ~AudioEncoder();

    bool start(const std::string& outputPath);
    void stop();

    bool isRecording() const { return mRunning.load(); }

    // Set BOOTTIME→REALTIME offset for timestamp conversion (called once per recording session)
    void setTimeOffset(int64_t offsetNs) { mTimeOffsetNs = offsetNs; }

private:
    void initEncoder(const std::string& outputPath);
    void recordingLoop();   // AAudio read → PCM queue
    void inputLoop();       // PCM queue → AMediaCodec input
    void outputLoop();      // AMediaCodec output → FMP4Writer + CSV

    // Lazily start the FMP4Writer using the codec's current output format.
    // Pulls csd-0 (AudioSpecificConfig), sample_rate and channel_count.
    // Returns true on success (and sets mFmp4Started).
    bool startFmp4FromFormat();

    // Write one AAC output packet (fMP4 sample + CSV row), gated on the start
    // gate and the end cutoff. Called from both the main output loop and the
    // EOS drain loop. Returns without writing for pre-gate / post-cutoff packets.
    void writeGatedPacket(const uint8_t* outBuf, const AMediaCodecBufferInfo& info);

    struct PcmFrame {
        int64_t ptsUs;
        std::vector<uint8_t> data;
    };

    int mSampleRate;
    int mBitRate;
    int mChannelCount;

    int64_t mBoottimeBaseNs = 0;

    // First AAC sample's absolute PTS. Each recording zero-bases its muxer PTS
    // and CSV pts_us from its own first packet so the fMP4 audio timeline
    // starts at 0, matching CameraEncoder/video. Reset to -1 in initEncoder().
    int64_t mFirstPtsUs = -1;

    // Gate: drop AAC packets until the synchronized start opens.
    bool mGateNotified = false;

    AMediaCodec* mCodec = nullptr;
    AAudioStream* mRecordingStream = nullptr;

    // Fragmented-MP4 writer (replaces AMediaMuxer + in-band text track).
    SXR::FMP4Writer mFmp4;
    bool mFmp4Started = false;

    // Per-packet CSV (one row per AAC output sample).
    FILE* mAudioMetaFile = nullptr;
    uint64_t mPacketIndex = 0;

    // Audio format captured on first output (used for timescale conversion).
    int32_t mAudioSampleRate = 0;
    int32_t mAudioChannels = 0;

    std::thread mRecordingThread;
    std::thread mInputThread;
    std::thread mOutputThread;
    std::atomic<bool> mRunning{false};
    std::mutex mMutex;

    std::queue<PcmFrame> mPcmQueue;
    std::mutex mQueueMutex;
    std::condition_variable mQueueCV;
    std::atomic<bool> mRecordingDone{false};
    static constexpr size_t kMaxQueueFrames = 64;

    // Software gain to compensate for missing AGC in LOW_LATENCY mode.
    // MediaRecorder (MIC source) applies AGC; AAudio LOW_LATENCY bypasses it.
    // 2.0x = +6dB compensates for ~3-8dB observed shortfall.
    float mAudioGain = 2.0f;

    int64_t mLastPtsUs = 0;
    int64_t mTotalFramesRead = 0;
    int64_t mTotalFramesDropped = 0;
    std::string mOutputPath;

    // BOOTTIME→REALTIME offset for timestamp conversion
    int64_t mTimeOffsetNs = 0;
};
