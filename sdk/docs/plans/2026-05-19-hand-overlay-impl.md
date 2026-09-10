# Hand Overlay Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Project hand tracking joints as 2D overlay (bone lines + joint dots) onto both headset display and encoded RGB SBS video, replacing 3D cube/line rendering. Toggled by `persist.xr.project_hand` system property.

**Architecture:** Add a new `HandOverlayRenderer` class (CPU KB projection + GL 2D drawing). Move SBS encoder submission from camera callback thread to render thread so both drawing targets share the same overlay code. Use `persist.xr.project_hand` property (default=1) to switch between 2D overlay and 3D rendering modes.

**Tech Stack:** C++, OpenGL ES 3.x, GLM, OpenXR, Android system properties, Kannala-Brandt fisheye projection

---

### Task 1: Add system property reader and engine flag

**Files:**
- Modify: `app/src/main/cpp/main.cpp:85-89` (after `readUseControllerProperty`)
- Modify: `app/src/main/cpp/main.cpp:2422` (engine struct, after `useControllerMode`)
- Modify: `app/src/main/cpp/main.cpp:4293` (render loop, after `useControllerMode` init)

**Step 1: Add property reader function**

Insert after line 89 (`readUseControllerProperty`):

```cpp
static bool readProjectHandProperty() {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.xr.project_hand", value);
    return strcmp(value, "0") != 0;
}
```

**Step 2: Add flag to engine struct**

Insert after line 2422 (`bool useControllerMode = false;`):

```cpp
bool useProjectHand = true;
```

**Step 3: Initialize flag at startup**

Find where `useControllerMode` is initialized (around line 4293):
```cpp
engine.useControllerMode = readUseControllerProperty();
```
Add after it:
```cpp
engine.useProjectHand = readProjectHandProperty();
```

**Step 4: Build and verify compilation**

Run: `./gradlew assembleDebug`
Expected: Successful build, no behavior change yet.

**Step 5: Commit**

```bash
git add app/src/main/cpp/main.cpp
git commit -m "feat: add persist.xr.project_hand system property flag"
```

---

### Task 2: Create HandOverlayRenderer header

**Files:**
- Create: `app/src/main/cpp/HandOverlayRenderer.h`

**Step 1: Write header file**

```cpp
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>

// KB fisheye projection + GL 2D overlay renderer for hand joints.
// Projects 3D hand joints (RootSpace) to 2D pixel coordinates using
// Kannala-Brandt fisheye model, then renders bone lines and joint dots
// as a 2D overlay on any target FBO.

struct AlignedSensorSnapshot;

class HandOverlayRenderer {
public:
    // Camera intrinsics + extrinsics for one eye
    struct EyeCameraParams {
        float focalX, focalY;
        float centerX, centerY;
        float distortion[8];
        float extPos[3];    // extrinsics position (in device/view space)
        float extQuat[4];   // extrinsics rotation (x,y,z,w)
        uint32_t width, height;
        bool valid = false;
    };

    // Projected hand: 2D pixel coordinates for 26 joints
    struct ProjectedHand {
        float joints[26][2]; // (u, v) in pixels, (-1, -1) = behind camera
        bool active = false;
    };

    // Per-frame projection result for both eyes
    struct FrameProjection {
        ProjectedHand leftHand[2];  // [0]=left eye, [1]=right eye
        ProjectedHand rightHand[2];
    };

    HandOverlayRenderer() = default;
    ~HandOverlayRenderer();

    // Compile shaders, create VBOs. Call once with a valid GL context.
    void init();

    // Update cached camera parameters from frame data (call when new params arrive)
    void updateCameraParams(int eyeIndex, float focalX, float focalY,
                            float centerX, float centerY,
                            const float distortion[8],
                            const float extPos[3], const float extQuat[4],
                            uint32_t width, uint32_t height);

    // Compute 2D projections for current frame (CPU, call once per frame)
    // headPos/headQuat: head pose in RootSpace (x,y,z,w quaternion)
    void computeProjection(const AlignedSensorSnapshot& snap,
                           const float headPos[3], const float headQuat[4]);

    // Draw 2D bone overlay on current FBO (full viewport, no depth test)
    // eyeIndex: 0=left, 1=right
    // offsetX, regionW: viewport offset/width for SBS half (0,halfW or halfW,halfW)
    void render(int eyeIndex, int offsetX, int regionW, int regionH) const;

    const FrameProjection& getProjection() const { return projection_; }

private:
    // KB projection: 3D point in camera space -> 2D pixel
    static bool projectKB(const float point3d[3],
                          float focalX, float focalY,
                          float centerX, float centerY,
                          const float distortion[8],
                          float& outU, float& outV);

    // Coordinate transforms (same as visualize_hand_on_rgb.py)
    static void worldToCamera(const float jointWorld[3],
                              const float wcPos[3],
                              const float wvQuat[4],
                              float outCam[3]);

    static void quatMultiply(const float a[4], const float b[4], float out[4]);
    static void quatConjugate(const float q[4], float out[4]);
    static void quatRotate(const float q[4], const float v[3], float out[3]);

    EyeCameraParams eyeParams_[2];
    FrameProjection projection_{};

    // GL resources
    GLuint shaderProgram_ = 0;
    GLuint lineVBO_ = 0;
    GLuint circleVBO_ = 0;
    GLuint vao_ = 0;
    bool initialized_ = false;
};
```

**Step 2: Verify header compiles (will be verified in Task 3)**

**Step 3: Commit**

```bash
git add app/src/main/cpp/HandOverlayRenderer.h
git commit -m "feat: add HandOverlayRenderer header with KB projection API"
```

---

### Task 3: Implement HandOverlayRenderer — math functions

**Files:**
- Create: `app/src/main/cpp/HandOverlayRenderer.cpp` (part 1: math)

**Step 1: Write math functions and init/destroy**

```cpp
#include "HandOverlayRenderer.h"
#include "RawDateSave.h"  // for AlignedSensorSnapshot
#include <cmath>
#include <cstring>
#include <android/log.h>

#define TAG "HandOverlay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// Bone connections: (start_joint, end_joint) — matches Python BONE_CONNECTIONS
static const int BONE_CONNECTIONS[][2] = {
    {1, 0},                                                     // WRIST → PALM
    {1, 2}, {2, 3}, {3, 4}, {4, 5},                            // Thumb
    {1, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10},                   // Index
    {1, 11}, {11, 12}, {12, 13}, {13, 14}, {14, 15},           // Middle
    {1, 16}, {16, 17}, {17, 18}, {18, 19}, {19, 20},           // Ring
    {1, 21}, {21, 22}, {22, 23}, {23, 24}, {24, 25},           // Pinky
};
static const int NUM_BONE_CONNECTIONS = sizeof(BONE_CONNECTIONS) / sizeof(BONE_CONNECTIONS[0]);

static constexpr int JOINT_COUNT = 26;

// ============================================================
// Quaternion math (x, y, z, w — OpenXR convention)
// ============================================================

void HandOverlayRenderer::quatMultiply(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

void HandOverlayRenderer::quatConjugate(const float q[4], float out[4]) {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
}

void HandOverlayRenderer::quatRotate(const float q[4], const float v[3], float out[3]) {
    float qv[4] = {v[0], v[1], v[2], 0.0f};
    float tmp[4], conj[4];
    quatMultiply(q, qv, tmp);
    quatConjugate(q, conj);
    float result[4];
    quatMultiply(tmp, conj, result);
    out[0] = result[0]; out[1] = result[1]; out[2] = result[2];
}

// ============================================================
// Kannala-Brandt fisheye projection
// ============================================================

bool HandOverlayRenderer::projectKB(const float point3d[3],
                                     float focalX, float focalY,
                                     float centerX, float centerY,
                                     const float distortion[8],
                                     float& outU, float& outV) {
    float x = point3d[0], y = point3d[1], z = point3d[2];
    if (z <= 1e-9f) return false;

    float xn = x / z, yn = y / z;
    float r2 = xn*xn + yn*yn;
    float r = sqrtf(r2);
    float theta = atan2f(r, 1.0f);

    float theta2 = theta * theta;
    float theta_d = theta;
    for (int i = 0; i < 4 && fabsf(distortion[i]) > 1e-12f; i++) {
        float term = distortion[i];
        for (int p = 0; p <= i; p++) term *= theta2;
        // term = distortion[i] * theta^(2*(i+1))
        theta_d += term;
    }

    float x_dist, y_dist;
    if (r < 1e-9f) {
        x_dist = 0.0f; y_dist = 0.0f;
    } else {
        float scale = theta_d / r;
        x_dist = scale * xn;
        y_dist = scale * yn;
    }

    outU = focalX * x_dist + centerX;
    outV = focalY * y_dist + centerY;
    return true;
}

// ============================================================
// World → Camera coordinate transform
// ============================================================

void HandOverlayRenderer::worldToCamera(const float jointWorld[3],
                                         const float wcPos[3],
                                         const float wvQuat[4],
                                         float outCam[3]) {
    float offset[3] = {
        jointWorld[0] - wcPos[0],
        jointWorld[1] - wcPos[1],
        jointWorld[2] - wcPos[2]
    };
    float conj[4];
    quatConjugate(wvQuat, conj);
    float view[3];
    quatRotate(conj, offset, view);
    // Z flip: Camera +Z = -View Z
    outCam[0] = view[0];
    outCam[1] = -view[1];
    outCam[2] = -view[2];
}
```

**Step 2: Build to verify math compiles**

Run: `./gradlew assembleDebug`
Expected: Build fails (incomplete class) or succeeds if CMake picks up the new file but linker is okay. Actually it will fail because the class isn't fully implemented. That's fine — we'll complete it in the next task.

**Step 3: Commit (together with Task 4)**

---

### Task 4: Implement HandOverlayRenderer — init, projection, render

**Files:**
- Modify: `app/src/main/cpp/HandOverlayRenderer.cpp` (complete the implementation)

**Step 1: Add init, updateCameraParams, computeProjection, render, destructor**

Continue the file from Task 3:

```cpp
// ============================================================
// GL initialization
// ============================================================

static const char* OVERLAY_VS = R"(
    #version 300 es
    layout(location = 0) in vec2 aPosition;
    layout(location = 1) in vec4 aColor;
    out vec4 vColor;
    uniform vec2 uResolution;
    void main() {
        vec2 ndc = (aPosition / uResolution) * 2.0 - 1.0;
        ndc.y = -ndc.y;  // flip Y: pixel top-down → GL bottom-up
        gl_Position = vec4(ndc, 0.0, 1.0);
        vColor = aColor;
        gl_PointSize = 8.0;
    }
)";

static const char* OVERLAY_FS = R"(
    #version 300 es
    in vec4 vColor;
    out highp vec4 fragColor;
    void main() {
        // Round point for joints
        vec2 coord = gl_PointCoord - vec2(0.5);
        if (length(coord) > 0.5) discard;
        fragColor = vColor;
    }
)";

HandOverlayRenderer::~HandOverlayRenderer() {
    if (shaderProgram_) glDeleteProgram(shaderProgram_);
    if (lineVBO_) glDeleteBuffers(1, &lineVBO_);
    if (circleVBO_) glDeleteBuffers(1, &circleVBO_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

void HandOverlayRenderer::init() {
    if (initialized_) return;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &OVERLAY_VS, nullptr);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &OVERLAY_FS, nullptr);
    glCompileShader(fs);

    shaderProgram_ = glCreateProgram();
    glAttachShader(shaderProgram_, vs);
    glAttachShader(shaderProgram_, fs);
    glLinkProgram(shaderProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &lineVBO_);
    glGenBuffers(1, &circleVBO_);

    // Pre-allocate line VBO: 25 bones * 2 verts * (2 pos + 4 color) floats
    glBindBuffer(GL_ARRAY_BUFFER, lineVBO_);
    glBufferData(GL_ARRAY_BUFFER, NUM_BONE_CONNECTIONS * 2 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // Pre-allocate circle VBO: 26 joints * (2 pos + 4 color) floats
    glBindBuffer(GL_ARRAY_BUFFER, circleVBO_);
    glBufferData(GL_ARRAY_BUFFER, JOINT_COUNT * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    initialized_ = true;
    LOGI("HandOverlayRenderer initialized");
}

void HandOverlayRenderer::updateCameraParams(int eyeIndex, float fX, float fY,
                                              float cX, float cY,
                                              const float dist[8],
                                              const float ePos[3],
                                              const float eQuat[4],
                                              uint32_t w, uint32_t h) {
    if (eyeIndex < 0 || eyeIndex > 1) return;
    auto& p = eyeParams_[eyeIndex];
    p.focalX = fX; p.focalY = fY;
    p.centerX = cX; p.centerY = cY;
    memcpy(p.distortion, dist, sizeof(p.distortion));
    memcpy(p.extPos, ePos, sizeof(p.extPos));
    memcpy(p.extQuat, eQuat, sizeof(p.extQuat));
    p.width = w; p.height = h;
    p.valid = true;
}

void HandOverlayRenderer::computeProjection(const AlignedSensorSnapshot& snap,
                                             const float headPos[3],
                                             const float headQuat[4]) {
    for (int eye = 0; eye < 2; eye++) {
        const auto& cam = eyeParams_[eye];
        if (!cam.valid) continue;

        // Camera world position: headPos + headQuat * extPos
        float wcPos[3];
        quatRotate(headQuat, cam.extPos, wcPos);
        wcPos[0] += headPos[0]; wcPos[1] += headPos[1]; wcPos[2] += headPos[2];

        // Left hand
        auto& lh = projection_.leftHand[eye];
        lh.active = snap.leftHand.active;
        if (lh.active) {
            for (int j = 0; j < JOINT_COUNT; j++) {
                float camPt[3];
                worldToCamera(snap.leftHand.joints[j], wcPos, headQuat, camPt);
                float u, v;
                if (projectKB(camPt, cam.focalX, cam.focalY, cam.centerX, cam.centerY,
                              cam.distortion, u, v)) {
                    if (u >= 0 && u < cam.width && v >= 0 && v < cam.height) {
                        lh.joints[j][0] = u;
                        lh.joints[j][1] = v;
                    } else {
                        lh.joints[j][0] = -1; lh.joints[j][1] = -1;
                    }
                } else {
                    lh.joints[j][0] = -1; lh.joints[j][1] = -1;
                }
            }
        }

        // Right hand
        auto& rh = projection_.rightHand[eye];
        rh.active = snap.rightHand.active;
        if (rh.active) {
            for (int j = 0; j < JOINT_COUNT; j++) {
                float camPt[3];
                worldToCamera(snap.rightHand.joints[j], wcPos, headQuat, camPt);
                float u, v;
                if (projectKB(camPt, cam.focalX, cam.focalY, cam.centerX, cam.centerY,
                              cam.distortion, u, v)) {
                    if (u >= 0 && u < cam.width && v >= 0 && v < cam.height) {
                        rh.joints[j][0] = u;
                        rh.joints[j][1] = v;
                    } else {
                        rh.joints[j][0] = -1; rh.joints[j][1] = -1;
                    }
                } else {
                    rh.joints[j][0] = -1; rh.joints[j][1] = -1;
                }
            }
        }
    }
}

void HandOverlayRenderer::render(int eyeIndex, int offsetX, int regionW, int regionH) const {
    if (!initialized_ || eyeIndex < 0 || eyeIndex > 1) return;

    const auto& lh = projection_.leftHand[eyeIndex];
    const auto& rh = projection_.rightHand[eyeIndex];

    glUseProgram(shaderProgram_);
    glUniform2f(glGetUniformLocation(shaderProgram_, "uResolution"),
                (float)regionW, (float)regionH);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // -- Draw bone lines for one hand --
    auto drawBones = [&](const ProjectedHand& hand, float r, float g, float b) {
        if (!hand.active) return;

        float lineData[NUM_BONE_CONNECTIONS * 2 * 6]; // 2 verts * (2 pos + 4 rgba) per connection
        int count = 0;
        for (int i = 0; i < NUM_BONE_CONNECTIONS; i++) {
            int a = BONE_CONNECTIONS[i][0], bIdx = BONE_CONNECTIONS[i][1];
            if (hand.joints[a][0] < 0 || hand.joints[bIdx][0] < 0) continue;
            // Vertex A
            lineData[count++] = hand.joints[a][0] + offsetX;
            lineData[count++] = hand.joints[a][1];
            lineData[count++] = r; lineData[count++] = g; lineData[count++] = b; lineData[count++] = 1.0f;
            // Vertex B
            lineData[count++] = hand.joints[bIdx][0] + offsetX;
            lineData[count++] = hand.joints[bIdx][1];
            lineData[count++] = r; lineData[count++] = g; lineData[count++] = b; lineData[count++] = 1.0f;
        }

        if (count > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, lineVBO_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(float), lineData);
            glBindVertexArray(vao_);
            // Position
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            // Color
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
            glLineWidth(2.0f);
            glDrawArrays(GL_LINES, 0, count / 6);
        }
    };

    // -- Draw joint dots for one hand --
    auto drawJoints = [&](const ProjectedHand& hand, float r, float g, float b) {
        if (!hand.active) return;

        float dotData[JOINT_COUNT * 6]; // (2 pos + 4 rgba) per joint
        int count = 0;
        for (int j = 0; j < JOINT_COUNT; j++) {
            if (hand.joints[j][0] < 0) continue;
            dotData[count++] = hand.joints[j][0] + offsetX;
            dotData[count++] = hand.joints[j][1];
            dotData[count++] = r; dotData[count++] = g; dotData[count++] = b; dotData[count++] = 1.0f;
        }

        if (count > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, circleVBO_);
            glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(float), dotData);
            glBindVertexArray(vao_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
            glDrawArrays(GL_POINTS, 0, count / 6);
        }
    };

    // Left hand: blue (0.0, 0.5, 1.0)
    drawBones(lh, 0.0f, 0.5f, 1.0f);
    drawJoints(lh, 0.0f, 0.5f, 1.0f);

    // Right hand: red-orange (1.0, 0.5, 0.0)
    drawBones(rh, 1.0f, 0.5f, 0.0f);
    drawJoints(rh, 1.0f, 0.5f, 0.0f);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
```

**Step 2: Build and verify compilation**

Run: `./gradlew assembleDebug`
Expected: Successful build. No behavior change yet (code not called).

**Step 3: Commit**

```bash
git add app/src/main/cpp/HandOverlayRenderer.cpp
git commit -m "feat: implement HandOverlayRenderer with KB projection and GL 2D overlay"
```

---

### Task 5: Add HandOverlayRenderer instance to engine and init at startup

**Files:**
- Modify: `app/src/main/cpp/main.cpp` — add include, add member to engine struct, init in render loop

**Step 1: Add include at top of main.cpp**

After the existing includes (around line 46 `#include "EncoderSurface.h"`), add:
```cpp
#include "HandOverlayRenderer.h"
```

**Step 2: Add member to engine struct**

After `HandTrackerLogic mHandTrackerLogic{this};` (line 2425), add:
```cpp
HandOverlayRenderer handOverlay;
```

**Step 3: Initialize overlay in render loop startup**

Find where hand line shader is initialized (around line 3975-4026 in `engine_init_scene_resources`). After the hand line VBO creation block, add:
```cpp
engine->handOverlay.init();
```

**Step 4: Build and verify**

Run: `./gradlew assembleDebug`
Expected: Successful build.

**Step 5: Commit**

```bash
git add app/src/main/cpp/main.cpp
git commit -m "feat: add HandOverlayRenderer instance to engine and init GL resources"
```

---

### Task 6: Feed camera params to HandOverlayRenderer

**Files:**
- Modify: `app/src/main/cpp/main.cpp` — in camera callback where camera params are saved

**Step 1: Update camera params in the RGB callback**

In `saveCameraParams` (line 1188-1235), the camera intrinsics/extrinsics are already extracted from `FrameData`. After the existing `saveCameraParams` call for RGB (around line 1434), add code to feed the params to the overlay renderer:

Find `saveCameraParams(data, "rgb", cameraParamsSavedRgb);` and add after it:
```cpp
// Feed camera params to hand overlay renderer
for (int i = 0; i < 2; i++) {
    const auto& f = data->frames[i];
    handOverlay.updateCameraParams(i, f.focalX, f.focalY,
                                    f.centerX, f.centerY,
                                    f.radialDistortion,
                                    f.position, f.rotation,
                                    f.width, f.height);
}
```

**Step 2: Build and verify**

Run: `./gradlew assembleDebug`
Expected: Successful build.

**Step 3: Commit**

```bash
git add app/src/main/cpp/main.cpp
git commit -m "feat: feed RGB camera params to HandOverlayRenderer"
```

---

### Task 7: Add pending frame signal and move SBS encoding to render thread

This is the core architectural change. We split the camera callback: keep texture import + SBS stitch, but move encoder surface submission to the render thread.

**Files:**
- Modify: `app/src/main/cpp/main.cpp`

**Step 1: Add atomic signal to CameraAccessExtension**

In the `CameraAccessExtension` struct (around line 597), add:
```cpp
std::atomic<uint64_t> pendingEncodeTimestamp{0};
```

**Step 2: Thin the RGB camera callback**

In the RGB camera callback (the section at lines 1534-1584 that handles SBS encoding), keep everything through the SBS FBO stitch, but replace the encoder surface submission with a signal:

Replace the block starting at `if (rgbSbsFBO != 0 && rgbEncoderSurface != nullptr && !encodersStopped.load())` (line 1534) through the `rgbEncoderSurface->swapBuffers()` and context restore — keep the SBS FBO stitch, but replace the encoder submission part:

**Keep:** Lines 1535-1546 (SBS FBO stitch: bind rgbSbsFBO, render each eye)
**Replace:** Lines 1548-1583 (encoder surface submission) with:
```cpp
// Signal render thread that a new SBS frame is ready for encoding
if (!encodersStopped.load()) {
    pendingEncodeTimestamp.store(data->frames[0].timestamp);
}
```
**Keep:** Line 1582 `saveAlignedSensorData(frameTimestampNs);` — move it to render thread later (Task 8)

Actually, let's keep `saveAlignedSensorData` in the callback for now — it saves sensor data to CSV and is independent of encoding. We only move the GL encoder submission.

Wait — looking at the design doc again, we decided to move `saveAlignedSensorData` to the render thread too. But that function writes CSV data and is not GL-related. Let's keep it in the callback for now to minimize risk. The key thing to move is just the GL encoder surface submission.

**Step 3: Add SBS encoding pass in render loop**

In the render loop (between `engine_draw_frame` calls and `xrEndFrame`), after line 4600 (after the swapchain loop), add:

```cpp
// Submit pending RGB SBS frame to encoder (moved from camera callback)
if (engine.mCameraAccessExtension.pendingEncodeTimestamp.load() != 0 &&
    engine.mCameraAccessExtension.rgbEncoderSurface != nullptr &&
    !engine.mCameraAccessExtension.encodersStopped.load()) {

    uint64_t ts = engine.mCameraAccessExtension.pendingEncodeTimestamp.exchange(0);

    auto* encSurf = engine.mCameraAccessExtension.rgbEncoderSurface;
    auto* encoder = engine.mCameraAccessExtension.rgbEncoder;

    encSurf->makeCurrent();
    int sbsW = engine.mCameraAccessExtension.rgbFrameWidths[0] * 2;
    int sbsH = engine.mCameraAccessExtension.rgbFrameHeights[0];
    glViewport(0, 0, sbsW, sbsH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw SBS texture fullscreen
    glUseProgram(engine.mCameraAccessExtension.sbsCopyShaderProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, engine.mCameraAccessExtension.rgbSbsTexture);
    glUniform1i(glGetUniformLocation(engine.mCameraAccessExtension.sbsCopyShaderProgram, "uTexture"), 0);

    glBindBuffer(GL_ARRAY_BUFFER, engine.mCameraAccessExtension.encoderVBO);
    GLint cpPosLoc = glGetAttribLocation(engine.mCameraAccessExtension.sbsCopyShaderProgram, "aPosition");
    GLint cpTexLoc = glGetAttribLocation(engine.mCameraAccessExtension.sbsCopyShaderProgram, "aTexCoord");
    glEnableVertexAttribArray(cpPosLoc);
    glVertexAttribPointer(cpPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(cpTexLoc);
    glVertexAttribPointer(cpTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(cpPosLoc);
    glDisableVertexAttribArray(cpTexLoc);

    // Hand overlay will be added in Task 8

    encSurf->setPresentationTime(ts);
    encoder->submitNsTimestamp(ts);
    encSurf->swapBuffers();

    // Restore main render context
    eglMakeCurrent(engine.display, engine.surface, engine.surface, engine.context);
}
```

Note: The context restore at the end is critical. After `encSurf->makeCurrent()`, the main render context is no longer current. We need to restore it before the next frame iteration. Use `eglMakeCurrent` with the engine's display/surface/context.

**Step 4: Build and verify**

Run: `./gradlew assembleDebug`
Expected: Successful build. Test on device: recording should still produce `rgb.mp4` with camera feed, no overlay yet.

**Step 5: Commit**

```bash
git add app/src/main/cpp/main.cpp
git commit -m "refactor: move SBS encoder submission from camera callback to render thread"
```

---

### Task 8: Wire up 2D overlay in engine_draw_frame and SBS encoding

**Files:**
- Modify: `app/src/main/cpp/main.cpp`

**Step 1: Replace 3D hand rendering with 2D overlay in engine_draw_frame**

In `engine_draw_frame` (lines 3399-3543), the hand rendering section does:
1. Lines 3399-3436: 3D joint cubes
2. Lines 3466-3549: 3D skeleton lines (point-based)

Wrap the entire hand rendering block with the mode switch. Find the line:
```cpp
// Render hand joints (use solid color, not texture)
engine->cubeShader->SetUniform1i("useTexture", 0);
```

Replace the entire block from there through the end of skeleton line rendering (line 3549 `}`) with:

```cpp
if (engine->useProjectHand) {
    // 2D overlay mode: draw projected hand bones on current FBO
    auto& proj = engine->handOverlay.getProjection();
    engine->handOverlay.render(viewIndex, 0, engine->width, engine->height);
} else {
    // 3D rendering mode: existing joint cubes + skeleton lines
    engine->cubeShader->SetUniform1i("useTexture", 0);

    // [keep existing 3D joint cube rendering code: lines 3402-3436]
    // [keep existing 3D skeleton line rendering code: lines 3466-3543]

    // Restore cube shader for subsequent rendering
    engine->cubeShader->Bind();
    engine->cubeShader->SetUniformMat4("projectionMatrix", eyeProjMat);
    engine->cubeShader->SetUniformMat4("viewMatrix", eyeViewMat);
    engine->cubeShader->SetUniformVec3("eyePos", eyePos);
}
```

**Step 2: Compute projection in render loop before engine_draw_frame**

In the render loop, after `engine.mHandTrackerLogic.Update(frameState)` (line 4371) and after the aligned snapshot update (around line 4450), compute the 2D projection once per frame:

```cpp
// Compute 2D hand overlay projection (once per frame, used by all views)
if (engine.useProjectHand) {
    float headPos[3] = {
        engine.state.m_views[0].pose.position.x,
        engine.state.m_views[0].pose.position.y,
        engine.state.m_views[0].pose.position.z
    };
    float headQuat[4] = {
        engine.state.m_views[0].pose.orientation.x,
        engine.state.m_views[0].pose.orientation.y,
        engine.state.m_views[0].pose.orientation.z,
        engine.state.m_views[0].pose.orientation.w
    };
    AlignedSensorSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(engine.alignedSnapshot.mutex);
        snap.copyFrom(engine.alignedSnapshot);
    }
    engine.handOverlay.computeProjection(snap, headPos, headQuat);
}
```

**Step 3: Add 2D overlay to SBS encoding pass**

In the SBS encoding pass added in Task 7 (Step 3), after the `glDrawArrays(GL_TRIANGLE_STRIP, ...)` that draws the SBS texture and before `encSurf->setPresentationTime(ts)`, add:

```cpp
// Draw 2D hand overlay on SBS frame (both eyes)
if (engine.useProjectHand) {
    int halfW = sbsW / 2;
    engine.handOverlay.render(0, 0, halfW, sbsH);       // left eye
    engine.handOverlay.render(1, halfW, halfW, sbsH);   // right eye
}
```

**Step 4: Build and verify**

Run: `./gradlew assembleDebug`
Expected: Successful build.

**Step 5: Test on device**

1. Flash the APK
2. Start recording
3. Move hands in front of camera
4. Verify: headset display shows 2D bone overlay on camera view
5. Stop recording, pull `rgb.mp4`, verify: video shows 2D bone overlay

**Step 6: Commit**

```bash
git add app/src/main/cpp/main.cpp
git commit -m "feat: wire up 2D hand overlay rendering on headset display and encoded video"
```

---

### Task 9: Test property toggle (persist.xr.project_hand=0 restores 3D mode)

**Files:**
- No code changes, device testing only

**Step 1: Test 2D overlay mode (default)**

1. Build and install APK
2. `adb shell getprop persist.xr.project_hand` → should be empty (default = enabled)
3. Start recording, verify 2D overlay appears on headset and in recorded video

**Step 2: Test 3D rendering mode (property=0)**

1. `adb shell setprop persist.xr.project_hand 0`
2. Restart the app
3. Start recording, verify 3D joint cubes + skeleton lines appear on headset
4. Stop recording, verify video does NOT have 2D overlay (only camera feed)

**Step 3: Restore default**

1. `adb shell setprop persist.xr.project_hand 1`
2. Restart app, verify 2D overlay is back

**Step 4: Commit any fixes found during testing**

```bash
git add -A
git commit -m "fix: adjustments from device testing of hand overlay toggle"
```

---

### Task 10: Final verification and cleanup

**Step 1: Verify both modes work correctly**

Run: `./gradlew assembleDebug`
Flash to device, test both modes.

**Step 2: Review code quality**

Check for:
- No memory leaks (GL resources freed in destructor)
- Thread safety (atomic signals between callback and render thread)
- Context restore after encoder surface `makeCurrent`
- Correct projection math (compare output with Python script on same dataset)

**Step 3: Final commit**

```bash
git add -A
git commit -m "feat: complete hand overlay implementation with persist.xr.project_hand toggle"
```
