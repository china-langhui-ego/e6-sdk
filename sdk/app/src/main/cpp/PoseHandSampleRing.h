#pragma once
// PoseHandSampleRing — time-alignment ring buffer for head+hand samples.
// Extracted verbatim from main.cpp so it can be unit-tested on host.
// Head pose: SLERP. Hand joints: LERP (pos/radius) + SLERP (orient) when
// both bracket samples active, else nearest-neighbor.
#include <mutex>
#include <cstdint>
#include <cstring>
#include <cmath>

struct PoseHandSampleRing {
    struct Sample {
        int64_t bootTimeNs = 0;
        float headPos[3] = {0, 0, 0};        // device/IMU position (from xrLocateSpace on viewSpace)
        float headQuat[4] = {0, 0, 0, 1};    // x,y,z,w
        bool poseValid = false;
        bool leftActive = false;
        float leftJoints[26][3] = {};
        float leftRadii[26] = {};
        float leftQuats[26][4] = {};
        bool rightActive = false;
        float rightJoints[26][3] = {};
        float rightRadii[26] = {};
        float rightQuats[26][4] = {};
    };

    static constexpr int CAPACITY = 64;

    mutable std::mutex mutex;
    Sample buffer[CAPACITY];
    int writeIdx = 0;
    int count = 0;

    void push(const Sample& s) {
        std::lock_guard<std::mutex> lock(mutex);
        buffer[writeIdx] = s;
        writeIdx = (writeIdx + 1) % CAPACITY;
        if (count < CAPACITY) count++;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        writeIdx = 0;
        count = 0;
    }

    struct SampleInfo {
        int curCount = 0;
        int64_t oldestNs = 0;
        int64_t newestNs = 0;
        double alpha = 0.0;       // 0..1 within bracket; 0 if clamped to oldest, 1 if to newest
        bool clampedLow = false;
        bool clampedHigh = false;
    };

    // Sample at boottime t. Returns false if the ring is empty.
    // Position: linear; orientation: slerp; hand joints: nearest neighbor.
    bool sample(int64_t t, Sample& out, SampleInfo* info = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex);
        if (info) { info->curCount = count; }
        if (count == 0) return false;

        int oldestIdx = (writeIdx - count + CAPACITY) % CAPACITY;
        int newestIdx = (writeIdx - 1 + CAPACITY) % CAPACITY;
        if (info) {
            info->oldestNs = buffer[oldestIdx].bootTimeNs;
            info->newestNs = buffer[newestIdx].bootTimeNs;
        }

        if (count == 1 || t <= buffer[oldestIdx].bootTimeNs) {
            if (count >= 2) {
                // Extrapolate backward using the two oldest samples.
                // At ~60 fps, samples are ~16.7 ms apart; clamp
                // extrapolation to at most half a frame interval backward
                // to limit error growth.
                int secondIdx = (oldestIdx + 1) % CAPACITY;
                const Sample& s0 = buffer[oldestIdx];
                const Sample& s1 = buffer[secondIdx];
                double dt = (double)(s1.bootTimeNs - s0.bootTimeNs);
                double alpha = dt > 0.0 ? (double)(t - s0.bootTimeNs) / dt : 0.0;
                // alpha is negative (extrapolating backward into the past);
                // clamp to prevent unbounded extrapolation error.
                if (alpha < -0.5) alpha = -0.5;
                if (info) { info->clampedLow = true; info->alpha = alpha; }

                out.bootTimeNs = t;
                out.poseValid = s0.poseValid && s1.poseValid;

                // Position: linear extrapolation (same LERP formula, alpha < 0)
                for (int i = 0; i < 3; i++) {
                    out.headPos[i] = (float)(s0.headPos[i] + alpha * (s1.headPos[i] - s0.headPos[i]));
                }

                // Orientation: slerp (same as below, tolerates alpha < 0)
                float q0[4] = { s0.headQuat[0], s0.headQuat[1], s0.headQuat[2], s0.headQuat[3] };
                float q1[4] = { s1.headQuat[0], s1.headQuat[1], s1.headQuat[2], s1.headQuat[3] };
                float dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
                if (dot < 0.0f) {
                    for (int i = 0; i < 4; i++) q1[i] = -q1[i];
                    dot = -dot;
                }
                float qr[4];
                if (dot > 0.9995f) {
                    for (int i = 0; i < 4; i++) qr[i] = q0[i] + (float)alpha * (q1[i] - q0[i]);
                } else {
                    float theta0 = acosf(dot);
                    float sinTheta0 = sinf(theta0);
                    float theta = theta0 * (float)alpha;
                    float s0c = cosf(theta) - dot * sinf(theta) / sinTheta0;
                    float s1c = sinf(theta) / sinTheta0;
                    for (int i = 0; i < 4; i++) qr[i] = s0c * q0[i] + s1c * q1[i];
                }
                float n = sqrtf(qr[0]*qr[0] + qr[1]*qr[1] + qr[2]*qr[2] + qr[3]*qr[3]);
                if (n > 0.0f) {
                    for (int i = 0; i < 4; i++) qr[i] /= n;
                }
                memcpy(out.headQuat, qr, sizeof(qr));

                // Hand joints: nearest-neighbor to the oldest sample (avoid
                // extrapolating joint positions where linear model is poor).
                out.leftActive = s0.leftActive;
                out.rightActive = s0.rightActive;
                memcpy(out.leftJoints, s0.leftJoints, sizeof(float) * 26 * 3);
                memcpy(out.leftRadii, s0.leftRadii, sizeof(float) * 26);
                memcpy(out.leftQuats, s0.leftQuats, sizeof(float) * 26 * 4);
                memcpy(out.rightJoints, s0.rightJoints, sizeof(float) * 26 * 3);
                memcpy(out.rightRadii, s0.rightRadii, sizeof(float) * 26);
                memcpy(out.rightQuats, s0.rightQuats, sizeof(float) * 26 * 4);
                return true;
            }
            out = buffer[oldestIdx];
            if (info) { info->clampedLow = true; info->alpha = 0.0; }
            return true;
        }
        if (t >= buffer[newestIdx].bootTimeNs) {
            out = buffer[newestIdx];
            if (info) { info->clampedHigh = true; info->alpha = 1.0; }
            return true;
        }

        // Find bracketing samples [a, b] s.t. a.t <= t < b.t.
        int aIdx = oldestIdx;
        int bIdx = newestIdx;
        for (int i = 1; i < count; i++) {
            int idx = (oldestIdx + i) % CAPACITY;
            if (buffer[idx].bootTimeNs > t) {
                bIdx = idx;
                aIdx = (oldestIdx + i - 1) % CAPACITY;
                break;
            }
        }
        const Sample& a = buffer[aIdx];
        const Sample& b = buffer[bIdx];

        double denom = (double)(b.bootTimeNs - a.bootTimeNs);
        double alpha = denom > 0.0 ? (double)(t - a.bootTimeNs) / denom : 0.0;
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) alpha = 1.0;
        if (info) { info->alpha = alpha; }

        out.bootTimeNs = t;
        out.poseValid = a.poseValid && b.poseValid;

        // Position: linear interpolation.
        for (int i = 0; i < 3; i++) {
            out.headPos[i] = (float)(a.headPos[i] + alpha * (b.headPos[i] - a.headPos[i]));
        }

        // Orientation: slerp (x, y, z, w convention).
        float q0[4] = { a.headQuat[0], a.headQuat[1], a.headQuat[2], a.headQuat[3] };
        float q1[4] = { b.headQuat[0], b.headQuat[1], b.headQuat[2], b.headQuat[3] };
        float dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
        if (dot < 0.0f) {
            for (int i = 0; i < 4; i++) q1[i] = -q1[i];
            dot = -dot;
        }
        float qr[4];
        if (dot > 0.9995f) {
            for (int i = 0; i < 4; i++) qr[i] = q0[i] + (float)alpha * (q1[i] - q0[i]);
        } else {
            float theta0 = acosf(dot);
            float sinTheta0 = sinf(theta0);
            float theta = theta0 * (float)alpha;
            float s0 = cosf(theta) - dot * sinf(theta) / sinTheta0;
            float s1 = sinf(theta) / sinTheta0;
            for (int i = 0; i < 4; i++) qr[i] = s0 * q0[i] + s1 * q1[i];
        }
        float n = sqrtf(qr[0]*qr[0] + qr[1]*qr[1] + qr[2]*qr[2] + qr[3]*qr[3]);
        if (n > 0.0f) {
            for (int i = 0; i < 4; i++) qr[i] /= n;
        }
        memcpy(out.headQuat, qr, sizeof(qr));

        // Hand joints: LERP for positions/radii, SLERP for orientations.
        // Interpolate when both bracket samples have the hand active;
        // fall back to the nearest sample when only one has it.
        float fa = (float)alpha;

        auto lerpHand = [fa](bool& outAct,
                              float outJ[26][3], float outR[26], float outQ[26][4],
                              bool aAct, const float aJ[26][3], const float aR[26], const float aQ[26][4],
                              bool bAct, const float bJ[26][3], const float bR[26], const float bQ[26][4]) {
            if (aAct && bAct) {
                outAct = true;
                for (int j = 0; j < 26; ++j) {
                    for (int k = 0; k < 3; ++k)
                        outJ[j][k] = aJ[j][k] + fa * (bJ[j][k] - aJ[j][k]);
                    outR[j] = aR[j] + fa * (bR[j] - aR[j]);
                    // SLERP (same logic as head pose above)
                    float q0[4] = { aQ[j][0], aQ[j][1], aQ[j][2], aQ[j][3] };
                    float q1[4] = { bQ[j][0], bQ[j][1], bQ[j][2], bQ[j][3] };
                    float d = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
                    if (d < 0.0f) { for (int kk = 0; kk < 4; ++kk) q1[kk] = -q1[kk]; d = -d; }
                    if (d > 0.9995f) {
                        for (int kk = 0; kk < 4; ++kk) outQ[j][kk] = q0[kk] + fa * (q1[kk] - q0[kk]);
                    } else {
                        float t0 = acosf(d), st0 = sinf(t0), t = t0 * fa;
                        float s0 = cosf(t) - d * sinf(t) / st0;
                        float s1 = sinf(t) / st0;
                        for (int kk = 0; kk < 4; ++kk) outQ[j][kk] = s0 * q0[kk] + s1 * q1[kk];
                    }
                    float n = sqrtf(outQ[j][0]*outQ[j][0] + outQ[j][1]*outQ[j][1] +
                                   outQ[j][2]*outQ[j][2] + outQ[j][3]*outQ[j][3]);
                    if (n > 0.0f) { for (int kk = 0; kk < 4; ++kk) outQ[j][kk] /= n; }
                }
            } else if (aAct) {
                outAct = true;
                memcpy(outJ, aJ, sizeof(float) * 26 * 3);
                memcpy(outR, aR, sizeof(float) * 26);
                memcpy(outQ, aQ, sizeof(float) * 26 * 4);
            } else if (bAct) {
                outAct = true;
                memcpy(outJ, bJ, sizeof(float) * 26 * 3);
                memcpy(outR, bR, sizeof(float) * 26);
                memcpy(outQ, bQ, sizeof(float) * 26 * 4);
            }
        };

        lerpHand(out.leftActive, out.leftJoints, out.leftRadii, out.leftQuats,
                 a.leftActive, a.leftJoints, a.leftRadii, a.leftQuats,
                 b.leftActive, b.leftJoints, b.leftRadii, b.leftQuats);
        lerpHand(out.rightActive, out.rightJoints, out.rightRadii, out.rightQuats,
                 a.rightActive, a.rightJoints, a.rightRadii, a.rightQuats,
                 b.rightActive, b.rightJoints, b.rightRadii, b.rightQuats);
        return true;
    }
};
