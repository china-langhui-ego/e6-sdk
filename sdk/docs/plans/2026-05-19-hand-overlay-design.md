# Hand Overlay on RGB Video Design

## Goal

Project hand tracking joints onto the RGB video feed as 2D overlay (bone lines + joint dots), replacing the current 3D joint cube rendering. The overlay is drawn on both the headset display and the encoded RGB SBS video. Toggled by system property `persist.xr.project_hand` (default=1, enable overlay; =0, restore 3D rendering).

## Background

The current pipeline renders hand joints as 3D cubes in OpenXR swapchains (main.cpp:3399-3436). The Python script `visualize_hand_on_rgb.py` demonstrates the desired 2D overlay using Kannala-Brandt fisheye projection to map 3D joint positions to 2D pixel coordinates on the RGB image.

## Architecture Change

### Current Flow

```
Camera callback thread:
  AHardwareBuffer → GL textures → rgbSbsFBO (stitch L+R) → rgbEncoderSurface → encode

Render thread:
  engine_draw_frame → draw scene + 3D hand joints → swapchain → headset display
```

### New Flow

```
Camera callback thread:
  AHardwareBuffer → GL textures → rgbSbsTexture (stitch L+R, signal "new frame ready")

Render thread:
  engine_draw_frame:
    1. Draw scene
    2. If project_hand=1:
       a. CPU: compute 2D joint coordinates (KB projection, once per frame)
       b. GL: draw 2D bone overlay on swapchain (headset)
       c. GL: draw rgbSbsTexture + 2D bone overlay → rgbEncoderSurface (encode)
    3. If project_hand=0:
       a. Draw 3D joint cubes (existing behavior)
```

## Components

### 1. System Property

```cpp
// In main.cpp, alongside readUseControllerProperty()
static bool readProjectHandProperty() {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.xr.project_hand", value);
    // Default: enabled (1) when property is empty or "1"
    return strcmp(value, "0") != 0;
}
```

Called once at startup, stored in `engine.useProjectHand`.

### 2. KB Projection (CPU)

Port the projection logic from `visualize_hand_on_rgb.py` to C++:

```cpp
struct HandProjection {
    // Per-eye camera params (from FrameData, cached)
    struct EyeParams {
        float focalX, focalY, centerX, centerY;
        float distortion[8];
        float extPos[3];    // extrinsics position
        float extQuat[4];   // extrinsics rotation (x,y,z,w)
        bool valid = false;
    };
    EyeParams eyeParams[2]; // [0]=left, [1]=right

    // Cached 2D joint coordinates (computed once per frame)
    struct ProjectedHand {
        float joints[26][2]; // pixel (u,v), (-1,-1) = invalid
        bool active;
    };
    ProjectedHand left[2], right[2]; // [eye_index]

    void updateCameraParams(const SXR::FrameData* data);
    void project(const AlignedSensorSnapshot& snap, int eyeIndex);
};
```

Projection chain (same as Python script):
```
Joint(RootSpace) → T_WV(head pose) → ViewSpace → T_VC(extrinsics) → CameraSpace
  → KB distortion → 2D pixel coords
```

Key functions to port:
- `quat_multiply`, `quat_conjugate`, `quat_rotate`
- `project_kb` (Kannala-Brandt fisheye)
- `world_to_camera`, `camera_to_pixel`

### 3. 2D Bone Overlay Renderer (GL)

New shader + draw calls for rendering bone lines and joint dots as a 2D overlay:

**Vertex shader** (2D position + color):
```glsl
attribute vec2 aPosition;  // normalized [0,1] or pixel coords
attribute vec4 aColor;
varying vec4 vColor;
void main() {
    gl_Position = vec4(aPosition * 2.0 - 1.0, 0.0, 1.0);
    vColor = aColor;
}
```

**Fragment shader**:
```glsl
varying vec4 vColor;
void main() {
    gl_FragColor = vColor;
}
```

**Drawing**:
- Bone connections: `GL_LINES` for each bone pair (same connections as Python script's BONE_CONNECTIONS)
- Joint dots: `GL_TRIANGLE_FAN` circles at each joint position
- Left hand: blue (0.0, 0.0, 1.0), Right hand: red (1.0, 0.0, 0.0)
- Line width: 2-3px, dot radius: 4-5px
- Depth test disabled, blending enabled (for anti-aliasing)

### 4. Camera Callback Thinning

In the RGB camera callback (`processRGBCallback`), remove the encoder surface submission:

**Remove from callback:**
- `rgbEncoderSurface->makeCurrent()`
- `rgbEncoderSurface->setPresentationTime()`
- `rgbEncoder->submitNsTimestamp()`
- `rgbEncoderSurface->swapBuffers()`

**Keep in callback:**
- AHardwareBuffer → GL texture import
- Stitch to `rgbSbsFBO` / `rgbSbsTexture`
- Set `std::atomic<uint64_t> pendingEncodeFrameTimestamp` to signal new frame

### 5. Render Thread SBS Encoding

In the render loop (after `engine_draw_frame`, before `xrEndFrame`):

```cpp
if (pendingEncodeFrameTimestamp != 0 && rgbEncoderSurface) {
    uint64_t ts = pendingEncodeFrameTimestamp.exchange(0);

    if (useProjectHand) {
        // Render rgbSbsTexture to encoder surface, then overlay bones
        renderSbsToEncoderWithOverlay(ts);
    } else {
        // Render rgbSbsTexture to encoder surface (no overlay)
        renderSbsToEncoder(ts);
    }

    saveAlignedSensorData(ts);
}
```

`renderSbsToEncoderWithOverlay`:
1. `rgbEncoderSurface->makeCurrent()`
2. Draw `rgbSbsTexture` fullscreen (existing `sbsCopyShaderProgram`)
3. For each eye (left half, right half of SBS):
   - Draw bone lines and joint dots using HandOverlayRenderer
4. `rgbEncoderSurface->setPresentationTime(ts)`
5. `rgbEncoder->submitNsTimestamp(ts)`
6. `rgbEncoderSurface->swapBuffers()`

### 6. Headset Display Overlay

In `engine_draw_frame`, after scene rendering, before swapchain release:

```cpp
if (engine->useProjectHand) {
    // Skip 3D joint cube rendering
    // Instead: draw 2D bone overlay on current swapchain FBO
    handOverlay.render(swapchain.fbos[imgIndex], engine->width, engine->height,
                       projection.left[viewIndex], projection.right[viewIndex]);
} else {
    // Existing 3D joint cube rendering
}
```

## Thread Safety

- `rgbSbsTexture` is written by camera callback thread (GL operations on `rgbCtx`), read by render thread (GL operations on main context). Since `rgbSbsTexture` is a GL_TEXTURE_2D created under `rgbCtx`, the render thread cannot directly bind it. Options:
  - **Option A (Recommended)**: Use a shared EGL context. The render thread's main context shares with `rgbCtx` (already done via `eglCreateContext` with shared context). Verify that textures are shareable.
  - **Option B**: Use `rgbEncoderSurface`'s context (which already shares with `rgbCtx`) for the SBS encoding step in the render thread. This means temporarily switching EGL context during the render loop.

Given that `rgbEncoderSurface` was initialized with `rgbCtx`'s display and context (`main.cpp:1266`), Option B is simpler — the render thread uses `rgbEncoderSurface->makeCurrent()` which gives it access to `rgbSbsTexture`.

## Data Flow Summary

```
                    Camera Callback Thread
                    ┌──────────────────────┐
  AHardwareBuffer → │ import to GL texture  │
                    │ stitch to rgbSbsFBO   │
                    │ signal pendingEncodeTs │
                    └──────────┬───────────┘
                               │ (atomic signal)
                    ┌──────────▼───────────┐
                    │   Render Thread       │
                    │                       │
                    │  1. engine_draw_frame │
                    │     ├─ draw scene     │
                    │     └─ 2D overlay     │
                    │        on swapchain   │
                    │                       │
                    │  2. SBS encode pass   │
                    │     ├─ rgbSbsTexture  │
                    │     ├─ 2D overlay     │
                    │     └─ submit encode  │
                    └───────────────────────┘
```

## Files to Modify

1. **main.cpp** — Main changes: property reading, mode switching in engine_draw_frame, SBS encoding moved to render thread, camera callback thinning
2. **New: HandOverlay.h/cpp** — KB projection math + GL 2D overlay renderer (shader, VBO, draw calls)

## Bone Connections (from Python script)

```cpp
// (start_joint, end_joint) — OpenXR XR_HAND_JOINT_* indices
static const int BONE_CONNECTIONS[][2] = {
    {1, 0},                                                     // WRIST → PALM
    {1, 2}, {2, 3}, {3, 4}, {4, 5},                            // Thumb
    {1, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10},                   // Index
    {1, 11}, {11, 12}, {12, 13}, {13, 14}, {14, 15},           // Middle
    {1, 16}, {16, 17}, {17, 18}, {18, 19}, {19, 20},           // Ring
    {1, 21}, {21, 22}, {22, 23}, {23, 24}, {24, 25},           // Pinky
};
```

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Thread safety of rgbSbsTexture access | Use atomic frame counter; only read when callback is not writing (between frames) |
| GL context switching overhead in render thread | Only switch when encoding is active; measure impact |
| Projection accuracy vs Python script | Use identical math; validate with known test data |
| Performance impact of 2D overlay | Profile; overlay is lightweight (26 points + ~25 lines per hand per eye) |
