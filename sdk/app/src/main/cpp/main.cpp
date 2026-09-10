/****************************************************************
 * Copyright (c) 2020-2021 Qualcomm Technologies, Inc.
 * All Rights Reserved.
 * Confidential and Proprietary - Qualcomm Technologies, Inc.
 ****************************************************************/

// Define XR_USE_TIMESPEC before including OpenXR headers to enable timespec time conversion
#define XR_USE_TIMESPEC 1

#include <chrono>
#include <fstream>
#include <iomanip>
#include <unistd.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <mutex>
#include <condition_variable>

#include <android/log.h>
#include <android/looper.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <jni.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <map>

#include "AppCommon.h"
#include "Geometry.h"
#include "KtxLoader.h"
#include "Shader.h"
#include "CameraEncoder.h"
#include "EncoderSurface.h"
#include "RecordingEncodeWorker.h"
#include "HandOverlayRenderer.h"
#include "ImageSaver.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "xr_logger.h"
#include "openxr_qcom.h"
#include "pch.h"
#include "SharedTexture.h"
#include <EGL/eglext.h>

// EGL extension functions (defined in SharedTexture.cpp)
namespace glext {
    extern PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID;
    extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    extern PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    extern PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
}
#include "gfxHelper.h"
#include "sxr_camera.h"
#include "sxr_common.h"
#include "RootSpaceQCOM.h"
#include "RawDateSave.h"
#include "input.h"
#include "DatasetRecorder.h"
#include "RecordingGatekeeper.h"
#include "ControllerPoseSaver.h"
#include "DatasetFormat.h"
#include "mcap/McapChunkManager.h"
#include "mcap/McapSchemas.h"
#include <sys/system_properties.h>

#define LOGI(...)                                                              \
    ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGW(...)                                                              \
    ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define LOGE(...)                                                              \
    ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

#define EGL_SAMPLE_COUNT 4
#define CUBE_COUNT 3 

static int engine_init_xr_swapchains(struct engine *engine);

static bool readProjectHandProperty() {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.xr.project_hand", value);
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

static bool readProjectControllerProperty() {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.xr.project_controller", value);
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

// RGB camera fps is set by persist.sxr.cam.rgb.fps (default 30). The HEVC
// encoder derives the I-frame frame count from KEY_FRAME_RATE, so this hint
// MUST match the camera actual fps or the GOP drifts (e.g. 30 hint at a real
// 60 fps yields a 0.5 s GOP instead of 1 s).
static int readRgbFps() {
    char value[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.sxr.cam.rgb.fps", value);
    int fps = atoi(value);
    return (fps == 30 || fps == 60) ? fps : 30;
}

glm::vec3 CUBE_COLORS[CUBE_COUNT] = {{0.16f, 0.32f, 0.85f},
                                     {1.0f, 0.8f, 0.5f},
                                     {0.80f, 0.57f, 0.84f}};
const char* storagePath = "/storage/emulated/0/Android/data/com.ssnwt.helloxr/files";

// Minimum free space required to start or continue a dataset recording (1 GiB).
// Below this the recorder refuses to start and auto-stops an active recording.
static constexpr int64_t MIN_FREE_BYTES = 1024LL * 1024LL * 1024LL;

// Returns available bytes on the filesystem holding `path`, or -1 on error.
static int64_t getAvailableBytes(const char* path) {
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        LOGW("statvfs failed for %s", path);
        return -1;
    }
    return (int64_t)stat.f_bavail * (int64_t)stat.f_frsize;
}

// TTS JNI bridge
static JavaVM* g_javaVm = nullptr;
static jobject g_activity = nullptr;

// Time conversion function pointers (set by CameraAccessExtension::initTimeConversion)
static XrTime (*g_boottimeToXrTimeFn)(uint64_t) = nullptr;
static int64_t (*g_xrTimeToBoottimeFn)(XrTime) = nullptr;

void ttsSpeak(const char* text) {
    if (!g_javaVm || !g_activity) {
        LOGW("TTS not available: JVM/activity not set");
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    int ret = g_javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (ret != JNI_OK) {
        ret = g_javaVm->AttachCurrentThread(&env, nullptr);
        if (ret != JNI_OK) {
            LOGE("Failed to attach thread for TTS");
            return;
        }
        attached = true;
    }

    jclass cls = env->GetObjectClass(g_activity);
    if (cls) {
        jmethodID mid = env->GetMethodID(cls, "speak", "(Ljava/lang/String;)V");
        if (mid) {
            jstring jtext = env->NewStringUTF(text);
            env->CallVoidMethod(g_activity, mid, jtext);
            env->DeleteLocalRef(jtext);
        }
        env->DeleteLocalRef(cls);
    }

    if (attached) {
        g_javaVm->DetachCurrentThread();
    }
}

struct Swapchain : public AppCommon::Swapchain {
    std::vector<GLuint> fbos;
    std::vector<GLuint> dbos;
};

struct StereoSwapchain {
    std::vector<Swapchain> eyeSwapchain;
};
struct VertexLayoutPos3Uv2 {
    float position[3];
    float texCoord[2];
};
std::map<uint32_t, uint32_t> m_colorToDepthMap;
QtiGL::Geometry mNotificationMesh;
QtiGL::Shader *mNotificationShader;
GLuint quadTexture;
uint32_t quadTextureWidth, quadTextureHeight;

// ========== Text label rendering ==========
#include <cmath>
struct TextLabel {
    GLuint texture{0};
    QtiGL::Geometry geometry;
    uint32_t width{0};
    uint32_t height{0};
    std::string lastText;  // for dynamic update
};

// Simple 8x8 bitmap font (ASCII 32-127)
static const uint8_t FONT8X8[96][8] = {
    // Space (32)
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // ! (33)
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    // " (34)
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00},
    // # (35)
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    // $ (36)
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    // % (37)
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    // & (38)
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    // ' (39)
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    // ( (40)
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    // ) (41)
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    // * (42)
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    // + (43)
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    // , (44)
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    // - (45)
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    // . (46)
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    // / (47)
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    // 0-9 (48-57)
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    // : (58)
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    // ; (59)
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    // < (60)
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    // = (61)
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    // > (62)
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    // ? (63)
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    // @ (64)
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x78,0x00},
    // A-Z (65-90)
    {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    {0x3C,0x66,0x30,0x18,0x0C,0x66,0x3C,0x00},
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    {0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00},
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    // [ (91)
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    // \ (92)
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    // ] (93)
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    // ^ (94)
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    // _ (95)
    {0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00},
    // ` (96)
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    // a-z (97-122)
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C},
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00},
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0x7C},
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},
    // { (123)
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    // | (124)
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    // } (125)
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    // ~ (126)
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
    // DEL (127)
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// Generate a texture from text string using the bitmap font
static GLuint generateTextTexture(const std::string& text, uint32_t& outWidth, uint32_t& outHeight,
                                   int scale = 2, uint8_t fgR = 255, uint8_t fgG = 255, uint8_t fgB = 255) {
    int charW = 8 * scale;
    int charH = 8 * scale;
    outWidth = (uint32_t)(text.length() * charW);
    outHeight = (uint32_t)charH;
    if (outWidth == 0) outWidth = 1;

    std::vector<uint8_t> pixels(outWidth * outHeight * 4, 0);
    for (size_t ci = 0; ci < text.length(); ci++) {
        int ch = (unsigned char)text[ci];
        if (ch < 32 || ch > 127) ch = 32;
        const uint8_t* glyph = FONT8X8[ch - 32];
        int baseX = (int)ci * charW;
        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (row & (0x80 >> gx)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = baseX + gx * scale + sx;
                            int py = gy * scale + sy;
                            if (px < (int)outWidth && py < (int)outHeight) {
                                int idx = (py * outWidth + px) * 4;
                                pixels[idx + 0] = fgR;
                                pixels[idx + 1] = fgG;
                                pixels[idx + 2] = fgB;
                                pixels[idx + 3] = 255;
                            }
                        }
                    }
                }
            }
        }
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, outWidth, outHeight);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, outWidth, outHeight,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

// ========== Camera info panel (single texture, 6 lines, below cameras) ==========
struct engine;  // forward declaration

struct CameraInfoPanel {
    GLuint texture{0};
    uint32_t texWidth{0};
    uint32_t texHeight{0};
    QtiGL::Geometry* geometry{nullptr};
    std::string cachedText;
    float lastFps[6] = {-1.f};
    int frameCounter{0};

    // Generate multi-line text texture for all cameras
    void update(struct engine* engine);

    void render(QtiGL::Shader* shader) {
        if (!texture || !geometry) return;
        // Head-locked HUD: render in eye/view space. Setting viewMatrix to the
        // identity decouples the panel from head pose, so it stays fixed in
        // front of the eyes. engine_draw_frame resets viewMatrix to the eye
        // matrix at the start of each eye draw, so this is self-contained.
        // Tunables: z = distance, y = vertical offset, scale = size.
        glm::mat4 identityView(1.0f);  // SetUniformMat4 takes an lvalue reference
        shader->SetUniformMat4("viewMatrix", identityView);
        glm::mat4 mat = glm::translate(glm::vec3(0.0f, -0.25f, -1.8f))  // 1.8m ahead, slightly below center
                      * glm::scale(glm::vec3(0.35f));                    // tunable size
        shader->SetUniformMat4("modelMatrix", mat);
        shader->SetUniformSampler("srcTex", texture, GL_TEXTURE_2D, 0);
        geometry->Submit();
    }

    void cleanup() {
        if (texture) { glDeleteTextures(1, &texture); texture = 0; }
        if (geometry) { delete geometry; geometry = nullptr; }
    }
};
CameraInfoPanel gInfoPanel;


struct SpaceFrame{
    std::shared_ptr<SharedTexture> shareTexture{nullptr};
};

struct HandTrackerLogic{
    const AppCommon::base_engine* engine;
    XrHandTrackerEXT LeftHandTrackerHandle{XR_NULL_HANDLE};
    XrHandTrackerEXT RightHandTrackerHandle{XR_NULL_HANDLE};

    XrHandJointLocationEXT  LeftHandJointLocations[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT LeftHandLocations;
    XrHandJointLocationEXT  RightHandJointLocations[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT RightHandLocations;
    bool LeftHandIsActive = false;
    bool RightHandIsActive = false;

    PFN_xrCreateHandTrackerEXT pfnCreateHandTrackerEXT = nullptr;
    PFN_xrDestroyHandTrackerEXT pfnDestroyHandTrackerEXT = nullptr;
    PFN_xrLocateHandJointsEXT   pfnLocateHandJointsEXT = nullptr;
    PFN_xrCreateHandMeshSpaceMSFT pfnCreateHandMeshSpaceMSFT = nullptr;
    PFN_xrUpdateHandMeshMSFT      pfnUpdateHandMeshMSFT = nullptr;
    RawDateSave* rawDateSave;
    u_int64_t FrameCounter = 0;
    bool isResumed = false;

    ~HandTrackerLogic() {
        Release();
        delete rawDateSave;
        rawDateSave = nullptr;
    }

    // Release hand tracking resources. Call when switching to controller mode
    // to prevent OpenXR from wasting CPU/GPU cycles on tracking unused hands.
    void Release() {
        if (LeftHandTrackerHandle != XR_NULL_HANDLE && pfnDestroyHandTrackerEXT) {
            pfnDestroyHandTrackerEXT(LeftHandTrackerHandle);
            LeftHandTrackerHandle = XR_NULL_HANDLE;
            LOGI("Left hand tracker destroyed");
        }
        if (RightHandTrackerHandle != XR_NULL_HANDLE && pfnDestroyHandTrackerEXT) {
            pfnDestroyHandTrackerEXT(RightHandTrackerHandle);
            RightHandTrackerHandle = XR_NULL_HANDLE;
            LOGI("Right hand tracker destroyed");
        }
        LeftHandIsActive = false;
        RightHandIsActive = false;
        if (rawDateSave) {
            rawDateSave->StopSession();
        }
    }

    void Init(){
        XrResult res;
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrCreateHandTrackerEXT",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateHandTrackerEXT));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrCreateHandTrackerEXT function failed!");
        }
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrDestroyHandTrackerEXT",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyHandTrackerEXT));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrDestroyHandTrackerEXT function failed!");
        }
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrLocateHandJointsEXT",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnLocateHandJointsEXT));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrLocateHandJointsEXT function failed!");
        }
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrCreateHandMeshSpaceMSFT",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateHandMeshSpaceMSFT));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrCreateHandMeshSpaceMSFT function failed!");
        }
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrUpdateHandMeshMSFT",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&pfnUpdateHandMeshMSFT));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrUpdateHandMeshMSFT function failed!");
        }

        XrHandTrackerCreateInfoEXT HTCreateInfo{
                XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT,
                nullptr,
                XrHandEXT::XR_HAND_LEFT_EXT,
                XR_HAND_JOINT_SET_DEFAULT_EXT
        };
        res = pfnCreateHandTrackerEXT(engine->state.xrSession, &HTCreateInfo, &LeftHandTrackerHandle);
        if(res != XR_SUCCESS)
        {
            LOGE("create left hand tracker failed!");
        }

        HTCreateInfo.hand = XrHandEXT::XR_HAND_RIGHT_EXT;
        res = pfnCreateHandTrackerEXT(engine->state.xrSession, &HTCreateInfo, &RightHandTrackerHandle);
        if(res != XR_SUCCESS)
        {
            LOGE("create right hand tracker failed!");
        }

        LeftHandLocations = {
                .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
                .next = nullptr,
                .isActive = false,
                .jointCount = XR_HAND_JOINT_COUNT_EXT,
                .jointLocations = LeftHandJointLocations
        };

        RightHandLocations = {
                .type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT,
                .next = nullptr,
                .isActive = false,
                .jointCount = XR_HAND_JOINT_COUNT_EXT,
                .jointLocations = RightHandJointLocations
        };

        rawDateSave = new RawDateSave();
        rawDateSave->Init(storagePath);
        if(engine->state.Resumed) {
            rawDateSave->Resume();
            isResumed = true;
        }
    }

    void Update(XrTime atTime){
        UpdateLeftHand(atTime);
        UpdateRightHand(atTime);
        FrameCounter++;
    }
    private:
    void UpdateLeftHand(XrTime atTime){
        if(LeftHandTrackerHandle == nullptr)
            return;
        XrHandJointsLocateInfoEXT HandJointsLocateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};

        HandJointsLocateInfo.time = atTime;
        HandJointsLocateInfo.baseSpace = engine->useRootSpace ? engine->state.xrRootSpace : engine->state.xrLocalSpace;
        XrResult result = pfnLocateHandJointsEXT(LeftHandTrackerHandle, &HandJointsLocateInfo,
                                             &LeftHandLocations);

        if (result != XR_SUCCESS)
        {
            LOGW("left LocateHandJointsEXT failed %d time：%ld", result,atTime);
            LeftHandIsActive = false;
        }
        else
        {
            LOGI("left LocateHandJointsEXT isActive %d time：%ld", LeftHandLocations.isActive, atTime);
            LeftHandIsActive = LeftHandLocations.isActive;
        }

    }
    void UpdateRightHand(XrTime atTime){
        if(RightHandTrackerHandle == nullptr)
            return;
        XrHandJointsLocateInfoEXT HandJointsLocateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};

        HandJointsLocateInfo.time = atTime;
        HandJointsLocateInfo.baseSpace = engine->useRootSpace ? engine->state.xrRootSpace : engine->state.xrLocalSpace;
        XrResult result = pfnLocateHandJointsEXT(RightHandTrackerHandle, &HandJointsLocateInfo,
                                             &RightHandLocations);
        if (result != XR_SUCCESS)
        {
            LOGW("right LocateHandJointsEXT failed %d time：%ld", result,atTime);
            RightHandIsActive = false;
        }
        else
        {
            RightHandIsActive = RightHandLocations.isActive;
        }
    }
};

// Shared sensor snapshot for RGB-aligned timestamp saving.
// Render loop writes, RGB camera callback reads.
struct AlignedSensorSnapshot {
    std::mutex mutex;

    struct {
        float pos[3];
        float quat[4]; // x,y,z,w  — device/IMU pose in world (Root) space
        bool valid = false;
    } headPose;

    struct {
        bool active = false;
        float joints[26][3];  // positions
        float quats[26][4];   // orientations x,y,z,w
        float radii[26];
    } leftHand, rightHand;

    uint32_t rgbFrameCount = 0;

    // Copy data fields (excluding mutex) from another snapshot
    void copyFrom(const AlignedSensorSnapshot& other) {
        headPose = other.headPose;
        leftHand = other.leftHand;
        rightHand = other.rightHand;
        rgbFrameCount = other.rgbFrameCount;
    }
};

// Lightweight snapshot for overlay projection at encoder time.
// Captured in camera callback at RGB frame time, consumed in render loop encoder section.
struct OverlaySnapshot {
    float headPos[3] = {};
    float headQuat[4] = {};
    bool headValid = false;
    bool leftActive = false;
    bool rightActive = false;
    float leftJoints[26][3] = {};
    float rightJoints[26][3] = {};
    std::mutex mutex;

    void copyFrom(const AlignedSensorSnapshot& snap) {
        headValid = snap.headPose.valid;
        // Device/IMU pose — camera extrinsics are in device frame
        memcpy(headPos, snap.headPose.pos, sizeof(headPos));
        memcpy(headQuat, snap.headPose.quat, sizeof(headQuat));
        leftActive = snap.leftHand.active;
        rightActive = snap.rightHand.active;
        memcpy(leftJoints, snap.leftHand.joints, sizeof(leftJoints));
        memcpy(rightJoints, snap.rightHand.joints, sizeof(rightJoints));
    }

    // Controller pose for coordinate-axis projection (RootSpace).
    // Populated by saveAlignedSensorData in controller mode.
    bool ctrlLeftActive = false;
    float ctrlLeftPos[3] = {};
    float ctrlLeftQuat[4] = {0, 0, 0, 1};
    bool ctrlRightActive = false;
    float ctrlRightPos[3] = {};
    float ctrlRightQuat[4] = {0, 0, 0, 1};
};

#include "PoseHandSampleRing.h"

// View-frame correction for controller (aim-space) positions.
// The offset is constant in the head/view (device) frame, so it must be
// rotated by the time-aligned head orientation into world space before
// being added — a fixed world-frame translation only points the right way
// for one head yaw. Applies to both CSV rows and the encoded-video
// projection (applied at the consume site, not stored in the ring).
static const float kCtrlViewCorrection[3] = {0.0f, 0.0f, -0.042f};

// Rotate the view-frame correction into world space with the head
// (view->world) orientation and add it to an active controller position.
static void applyCtrlViewCorrection(float pos[3], const float headQuat[4]) {
    glm::quat headQ(headQuat[3], headQuat[0], headQuat[1], headQuat[2]);
    glm::vec3 corr = headQ * glm::vec3(kCtrlViewCorrection[0],
                                       kCtrlViewCorrection[1],
                                       kCtrlViewCorrection[2]);
    pos[0] += corr.x;
    pos[1] += corr.y;
    pos[2] += corr.z;
}

// Ring buffer of controller poses for time alignment with camera frames.
// Render thread pushes one sample per frame at current CLOCK_BOOTTIME.
// The RGB camera callback samples this ring at the frame's mid-exposure
// timestamp to align controller pose CSV rows with head_pose CSV rows.
struct ControllerPoseRing {
    struct Sample {
        int64_t bootTimeNs = 0;
        int frameNumber = 0;
        bool leftActive = false;
        float leftPos[3] = {0, 0, 0};
        float leftQuat[4] = {0, 0, 0, 1};
        bool rightActive = false;
        float rightPos[3] = {0, 0, 0};
        float rightQuat[4] = {0, 0, 0, 1};
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

    // Nearest-neighbor sample at boottime t. Returns false if empty.
    bool sample(int64_t t, Sample& out) const {
        std::lock_guard<std::mutex> lock(mutex);
        if (count == 0) return false;

        int oldestIdx = (writeIdx - count + CAPACITY) % CAPACITY;
        int newestIdx = (writeIdx - 1 + CAPACITY) % CAPACITY;

        if (count == 1 || t <= buffer[oldestIdx].bootTimeNs) {
            out = buffer[oldestIdx];
            return true;
        }
        if (t >= buffer[newestIdx].bootTimeNs) {
            out = buffer[newestIdx];
            return true;
        }

        // Find nearest sample
        int bestIdx = oldestIdx;
        int64_t bestDiff = std::abs(buffer[oldestIdx].bootTimeNs - t);
        for (int i = 1; i < count; i++) {
            int idx = (oldestIdx + i) % CAPACITY;
            int64_t diff = std::abs(buffer[idx].bootTimeNs - t);
            if (diff < bestDiff) {
                bestDiff = diff;
                bestIdx = idx;
            }
        }
        out = buffer[bestIdx];
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        writeIdx = 0;
        count = 0;
    }
};

struct engine;  // forward declaration for g_engine
static struct engine* g_engine = nullptr;  // global engine pointer (set in android_main)

// --- Sensor align worker thread (offloads OpenXR queries + KB projection from camera callback) ---
static std::atomic<int64_t>     s_alignTs{-1};
static std::mutex               s_alignMutex;
static std::condition_variable  s_alignCv;
static std::thread              s_alignThread;
static std::atomic<bool>        s_alignRun{false};

static void sensorAlignWorker();  // forward decl (defined after engine struct)
// --- end worker globals ---

// 限频告警助手：首次 + 每 every 次返回当次计数 n、其余返回 0（配合调用点各自的
// static atomic 计数器），用于防静默丢帧/降级无迹可查，同时不刷屏日志。
static int logThrottled(std::atomic<int>& cnt, int every) {
    int n = cnt.fetch_add(1, std::memory_order_relaxed) + 1;
    return (n == 1 || n % every == 0) ? n : 0;
}

// Forward declarations: defined after engine struct (which has full type info).
static void saveAlignedSensorData(int64_t rgbTimestampNs);
static void feedOverlayCameraParams(const SXR::FrameData* data);
static void renderHandOverlayToEncoder(int texWidth, int texHeight);
static void renderControllerAxesToEncoder(int texWidth, int texHeight);
// Recording gate helpers (need complete `engine` type; defined after engine struct).
// Declared (non-static) in RecordingGatekeeper.h so other TUs (AudioEncoder) can call them.
bool recordingGateIsOpen(int64_t tsNs);
bool recordingGateActive();  // gate state != IDLE（录制启动中/进行中/收尾）
void recordingGateNotifyReady(RecordingGatekeeper::Source src);

struct CameraAccessExtension{
    const AppCommon::base_engine* engine;
    JavaVM* vm;
    jobject activityObject;
    PFN_xrConvertTimespecTimeToTimeKHR xrConvertTimespecTimeToTimeKHR;
    PFN_xrConvertTimeToTimespecTimeKHR xrConvertTimeToTimespecTimeKHR;

    // Static instance pointer for C-style callback
    static CameraAccessExtension* sInstance;

    // Static wrapper for use as function pointer
    static XrTime staticBoottimeToXrTime(uint64_t boottime_ns) {
        if (sInstance) return sInstance->boottimeToXrTime(boottime_ns);
        return static_cast<XrTime>(boottime_ns);
    }

    // Static wrapper for XrTime -> boottime conversion
    static int64_t staticXrTimeToBoottime(XrTime xrTime) {
        if (sInstance) return sInstance->xrTimeToBoottime(xrTime);
        return static_cast<int64_t>(xrTime);
    }

    // ========== New callback-based camera API (dynamic loading) ==========
    SxrCameraApi api{};  // API function table for dynamic loading
    SxrCameraContext* cameraContext{nullptr};
    bool camerasInitialized{false};

    // RGB camera frame ready flag (for display thread to know when to render)
    std::atomic<bool> rgbFrameReady{false};

    // Camera/Encoder paused state
    std::atomic<bool> isPaused{false};
    bool cameraGroupsOpen{false};  // track whether camera groups are open

    // Callback drain synchronization
    std::atomic<int> inFlightCallbacks{0};
    std::mutex callbackDrainMutex;
    std::condition_variable callbackDrainCV;

    // ========== 独立EGL上下文（每个相机组一个，无锁竞争）==========
    struct CameraGLContext {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLContext context = EGL_NO_CONTEXT;
        EGLSurface surface = EGL_NO_SURFACE;  // Pbuffer for offscreen rendering
        std::atomic<bool> initialized{false};

        bool init(const AppCommon::base_engine* engine) {
            if (initialized) return true;
            EGLDisplay mainDisplay = engine->display;
            EGLContext mainContext = engine->context;
            EGLConfig config = engine->config;
            if (mainDisplay == EGL_NO_DISPLAY || mainContext == EGL_NO_CONTEXT) return false;

            EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            context = eglCreateContext(mainDisplay, config, mainContext, contextAttribs);
            if (context == EGL_NO_CONTEXT) {
                LOGE("Failed to create camera EGL context: 0x%x", eglGetError());
                return false;
            }

            EGLint pbufferAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            surface = eglCreatePbufferSurface(mainDisplay, config, pbufferAttribs);
            if (surface == EGL_NO_SURFACE) {
                LOGE("Failed to create Pbuffer surface: 0x%x", eglGetError());
                eglDestroyContext(mainDisplay, context);
                context = EGL_NO_CONTEXT;
                return false;
            }

            display = mainDisplay;
            initialized = true;
            return true;
        }

        void cleanup() {
            if (!initialized) return;
            if (display != EGL_NO_DISPLAY && context != EGL_NO_CONTEXT) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (surface != EGL_NO_SURFACE) {
                    eglDestroySurface(display, surface);
                    surface = EGL_NO_SURFACE;
                }
                if (context != EGL_NO_CONTEXT) {
                    eglDestroyContext(display, context);
                    context = EGL_NO_CONTEXT;
                }
            }
            display = EGL_NO_DISPLAY;
            initialized = false;
        }

        bool makeCurrent() {
            if (!initialized) return false;
            return eglMakeCurrent(display, surface, surface, context);
        }

        void releaseCurrent() {
            if (display != EGL_NO_DISPLAY) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
        }
    };
    CameraGLContext rgbCtx;
    CameraGLContext trackingCtx;
    CameraGLContext ctrlCtx;

    // 显示纹理（主线程使用，相机线程通过FBO渲染更新）
    GLuint rgbDisplayTextures[2] = {0, 0};  // [0]=left, [1]=right
    GLuint rgbDisplayFBOs[2] = {0, 0};      // FBOs for YUV->RGBA conversion

    // 编码器停止标志（防止停止后立即重新初始化）
    std::atomic<bool> encodersStopped{true};//default do not encode
    std::atomic<bool> stopInProgress{false};//true while async stopEncoder() is running
    // Hand-overlay snapshot at RGB frame time (populated by saveAlignedSensorData
    // in camera callback, used by renderHandOverlayToEncoder direct encode path).
    OverlaySnapshot overlaySnap;
    std::atomic<bool> encodingEnabled{false};//用户按键切换编码状态
    std::atomic<bool> snapshotRequested{false};//快照请求标志（intent或按键触发）

    // Dataset recording: encoder output directory (set when recording starts)
    std::string encoderBaseDir;
    std::atomic<int64_t> mCameraTimeOffsetNs{0};  // BOOTTIME→REALTIME offset for lazy-init encoders

    // Camera params saved flags (reset on each new recording session)
    std::atomic<bool> cameraParamsSavedRgb{false};
    std::atomic<bool> cameraParamsSavedTracking{false};
    std::atomic<bool> cameraParamsSavedCtrl{false};

    // Recording gate "first frame past gate" flags (reset in restartGrayscaleEncoders).
    // atomic：mcap blit worker 化后由 worker 线程置 true、主线程 reset，跨线程可见。
    // 写方共三处、锁域各异：worker（rgbEncodeProcess，持 mRgbEyeEncoderMutex）、mp4 SBS
    // 回调（handleRGBFrame，持 mRgbEyeEncoderMutex）、主线程复位（restartGrayscaleEncoders，
    // 持灰度双锁）——复位与写方无共锁，理论窗口内旧帧写回 true 使新一轮首帧强制 IDR
    // 失效；实际两个复位点（restartGrayscaleEncoders/startEncoders）下 RGB 编码器同批
    // 销毁或 encodersStopped=true 使 worker 入口提前 return，且 requestKeyFrame 幂等无害，
    // 故不锁、靠时序隔离。mp4 SBS 与 mcap worker 两写方因模式互斥不同时活跃（见两处写点）。
    std::atomic<bool> mRgbGateNotified{false};
    bool mTrackingGateNotified{false};
    bool mCtrlGateNotified{false};

    // --- 录制编码 worker（mcap 每眼 blit 移出相机回调线程）---
    // 回调只入队（EGLImage 创建 + gate 谓词求值），worker 持组锁做 makeCurrent/draw/
    // setPresentationTime/swapBuffers。display 在引擎初始化后已知，worker 必须早于首次
    // sxr_camera_open_group 就绪（ensureRecordingWorkers 在 initCameras/openCameraGroups
    // 开组前调用，保证首帧到达时 worker 已在跑——否则首会话 mcap 录制回调入队空指针）。
    // closeCameraGroups 在 sxr_camera_close_group 之前 stopAndJoin（与
    // sensor-align worker 同一条硬规则：在途 item 触及组 GL 对象）。
    std::unique_ptr<SXR::RecordingEncodeWorker> mRgbWorker;
    std::unique_ptr<SXR::RecordingEncodeWorker> mTrackingWorker;
    std::unique_ptr<SXR::RecordingEncodeWorker> mCtrlWorker;
    // worker 自有 GL 资源（各 encoder context 上创建，worker 线程专用——回调线程的
    // rgbPersistentEyeTex/cvPersistentExtTex 每帧 rebind，跨线程同对象 mutate 是
    // GL 规范级竞态，禁用）。相机 context 均以 engine->context 为 share context
    // （CameraGLContext::init），share group 随 engine->context 长存——encoder context
    // 销毁不级联释放共享对象，必须 deleteWorkerGlObjects 显式 glDelete（回调 drain 后）。
    GLuint rgbWorkerEyeTex[2]{0, 0};
    GLuint rgbWorkerVBO{0};
    bool rgbWorkerVboFilled{false};
    // [gi]: 0=tracking 1=ctrl；EyeTex 为 mcap blit 直采纹理；
    // VBO 每帧 subdata blit 半幅 quad（tracking/ctrl 各自独立，消除原单 VBO 跨组
    // mutate 竞态——上游数据灰度 63/63 窗口超时的结构性嫌疑之一）。
    // （源仓库另有 grayWorkerSbsTex[2] 为 SBS 推流纹理；本仓库 mcap 无 SBS 双轨，不移植。）
    GLuint grayWorkerEyeTex[2]{0, 0};
    GLuint grayWorkerVBO[2]{0, 0};
    // VBO 数据存储是否已 glBufferData 分配（glBufferSubData 只能写已分配的存储；
    // 未分配时 subdata 报 GL_INVALID_VALUE、顶点数据丢失 → 灰度每眼通道全黑）。
    bool grayWorkerVboFilled[2]{false, false};
    // 分项耗时累计（worker 单线程访问）：rgb60 满帧优化的数据基础（draw/swap 各占多少）。
    uint64_t rgbWorkerDrawUsAcc{0};
    uint64_t rgbWorkerSwapUsAcc{0};
    uint32_t rgbWorkerTimingCnt{0};
    uint64_t grayWorkerDrawUsAcc[2]{0, 0};
    uint32_t grayWorkerTimingCnt[2]{0, 0};

    // Snapshot pixel cache: camera threads write (in their GL context), main thread reads
    struct SnapshotBuffer {
        std::mutex mutex;
        std::vector<uint8_t> pixels; // RGBA
        uint32_t width = 0, height = 0;
        std::atomic<bool> ready{false};
    };
    SnapshotBuffer snapshotRgbLeft, snapshotRgbRight;
    SnapshotBuffer snapshotCv[4]; // [0]=TL, [1]=TR, [2]=CL, [3]=CR

    // Tracking camera (callback-based, CPU access)
    struct TrackingFrameData {
        SXR::FrameInfo frameInfo;
        std::vector<uint8_t> pixelData;
        std::vector<uint8_t> rgbaData;  // pre-expanded RGBA for display
    };
    std::mutex trackingFrameMutex;
    std::mutex shaderInitMutex;  // protects encoder/grayscale shader init from concurrent threads
    TrackingFrameData trackingFrames[2];  // [0]=left, [1]=right (from TRACKING group)
    TrackingFrameData ctrlFrames[2];      // [0]=left, [1]=right (from CTRL group)
    std::atomic<bool> trackingFrameReady{false};
    std::atomic<bool> ctrlFrameReady{false};

    // CV camera display textures (for projection layer rendering)
    // Order: [0]=CV-TL(tracking-left), [1]=CV-TR(tracking-right),
    //        [2]=CV-BL(ctrl-left), [3]=CV-BR(ctrl-right)
    GLuint cvDisplayTextures[4] = {0, 0, 0, 0};
    GLuint cvDisplayFBOs[4] = {0, 0, 0, 0};  // FBOs for GPU-side hwBuffer->texture copy
    GLuint cvDisplayVBOs[2] = {0, 0};         // per-group VBOs: [0]=tracking, [1]=ctrl (avoids cross-thread glBufferSubData)
    uint32_t cvFrameWidths[4] = {0, 0, 0, 0};
    uint32_t cvFrameHeights[4] = {0, 0, 0, 0};
    uint32_t cvLastUploadedFrameId[4] = {0, 0, 0, 0};  // track which frame was last uploaded

    // FPS tracking for all cameras
    struct FpsTracker {
        uint64_t lastFrameTime{0};
        uint32_t frameCount{0};
        float currentFps{0.0f};
        void update(uint64_t timestamp) {
            frameCount++;
            if (lastFrameTime == 0) { lastFrameTime = timestamp; return; }
            uint64_t elapsed = timestamp - lastFrameTime;
            if (elapsed >= 1000000000ULL) { // 1 second in ns
                currentFps = (float)frameCount * 1000000000ULL / elapsed;
                frameCount = 0;
                lastFrameTime = timestamp;
            }
        }
    };
    FpsTracker cvFpsTrackers[4];  // CV camera FPS trackers
    FpsTracker rgbFpsTrackers[2]; // RGB camera FPS trackers
    float getRgbFps(int idx) const { return rgbFpsTrackers[idx].currentFps; }
    float getCvFps(int idx) const { return cvFpsTrackers[idx].currentFps; }

    // tracking/ctrl 丢帧监控（每组一个实例，仅由该组回调线程触达，无需锁）：
    // - frameId 跳号 = 源端丢帧（传感器→回调链路上丢），逐事件 LOGW 实时上报
    // - gateRejected = 回调到达但录制 gate 未放行（gate 非 IDLE 时才计数；
    //   起止边缘的少量拒绝属正常，录制中段出现即编码端丢帧）
    // - 每 5s 窗口汇总 recv/srcDrop/gateRej/fps，累计丢帧率一并输出
    struct FrameDropMonitor {
        const char* name{""};
        bool     initialized{false};
        uint32_t lastFrameId[2]{0, 0};
        uint64_t lastTsNs{0};
        uint64_t totalRecv{0};
        uint64_t totalSrcDrops{0};
        uint64_t totalGateRejects{0};
        uint64_t windowRecv{0};
        uint64_t windowDrops{0};
        uint64_t windowGateRejects{0};
        std::chrono::steady_clock::time_point windowStart{};

        void onFrame(const SXR::FrameData* data) {
            totalRecv++; windowRecv++;
            if (!initialized) {
                initialized = true;
                lastFrameId[0] = data->frames[0].frameId;
                lastFrameId[1] = data->frames[1].frameId;
                lastTsNs = data->frames[0].timestamp;
                LOGI("FrameDrop: %s first frame id=%u/%u ts=%llu", name,
                     lastFrameId[0], lastFrameId[1],
                     (unsigned long long)lastTsNs);
                return;
            }
            uint32_t drops = 0;
            for (int i = 0; i < 2; i++) {
                uint32_t id = data->frames[i].frameId;
                if (id <= lastFrameId[i]) {
                    // 回退/归零 = 相机会话重建（息屏恢复等），非丢帧；重置基线
                    LOGI("FrameDrop: %s eye%d frameId reset %u -> %u (session recreated?)",
                         name, i, lastFrameId[i], id);
                } else {
                    uint32_t gap = id - lastFrameId[i];
                    if (gap > 1) {
                        drops += gap - 1;
                        LOGW("FrameDrop: %s eye%d frameId jump %u -> %u (+%u dropped)",
                             name, i, lastFrameId[i], id, gap - 1);
                    }
                }
                lastFrameId[i] = id;
            }
            // 帧间隔异常兜底：标称 60fps=16.7ms，间隔 >2 帧但 frameId 未跳号时上报
            uint64_t ts = data->frames[0].timestamp;
            if (ts > lastTsNs) {
                uint64_t gapNs = ts - lastTsNs;
                if (gapNs > 34000000ULL && drops == 0) {
                    LOGW("FrameDrop: %s timestamp gap %.1fms without frameId jump (id=%u)",
                         name, gapNs / 1e6, data->frames[0].frameId);
                }
            }
            lastTsNs = ts;
            totalSrcDrops += drops; windowDrops += drops;

            auto now = std::chrono::steady_clock::now();
            if (windowStart == std::chrono::steady_clock::time_point{}) windowStart = now;
            auto winMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart).count();
            if (winMs >= 5000) {
                float fps = windowRecv * 1000.0f / (float)winMs;
                float dropPct = (totalRecv + totalSrcDrops) > 0
                    ? 100.0f * (float)totalSrcDrops / (float)(totalRecv + totalSrcDrops) : 0.0f;
                LOGI("FrameDrop: %s 5s: recv=%llu srcDrop=%llu gateRej=%llu fps=%.1f | total recv=%llu srcDrop=%llu(%.2f%%) gateRej=%llu",
                     name,
                     (unsigned long long)windowRecv, (unsigned long long)windowDrops,
                     (unsigned long long)windowGateRejects, fps,
                     (unsigned long long)totalRecv, (unsigned long long)totalSrcDrops,
                     dropPct, (unsigned long long)totalGateRejects);
                windowRecv = windowDrops = windowGateRejects = 0;
                windowStart = now;
            }
        }

        void onGateReject() {
            totalGateRejects++; windowGateRejects++;
        }
    };
    FrameDropMonitor cvDropMonitors[2];  // [0]=tracking [1]=ctrl，name 在 initCameras 赋值

    // RGB frame dimensions (populated in callback)
    uint32_t rgbFrameWidths[2] = {0, 0};
    uint32_t rgbFrameHeights[2] = {0, 0};

    // Camera encoders
    SXR::CameraEncoder* grayCameraEncoders[SXR::CAME_MAX];  // Legacy: individual grayscale encoders
    SXR::CameraEncoder* trackingEncoder{nullptr};  // Combined tracking camera encoder (left+right)
    SXR::CameraEncoder* ctrlEncoder{nullptr};      // Combined ctrl camera encoder (left+right)
    SXR::CameraEncoder* rgbEncoder{nullptr};       // RGB camera encoder (side-by-side)

    // 异步编码器预创建：录制开始时后台线程做 AMediaCodec create/configure/start
    // （实测 ~36ms/路，在相机回调内做会 stall libcamera poll 线程引发补帧突发）。
    // 回调线程收养 pending 槽后只做轻量 EGL 部分（surface init ~2.5ms）。
    // 槽位读写一律持 precreateMutex（helper 存入 / 回调收养 / stop&restart 清理三方）。
    std::mutex precreateMutex;
    SXR::CameraEncoder* pendingGrayEncoder[2]{nullptr, nullptr};  // [0]=tracking [1]=ctrl
    SXR::CameraEncoder* pendingRgbEncoder{nullptr};
    // MCAP 每眼编码器预创建槽（mcap 模式专用）：[0/1]=_rgb_l/_r，[2/3]=_tracking_l/_r，
    // [4/5]=_ctrl_l/_r。mcap 6 路每眼原为回调内联创建（首帧 ~66/132ms 尖刺来源），
    // 预创建后回调收养只补 surface init（~2.5ms），消除录制首帧尖刺。
    SXR::CameraEncoder* pendingEyeEncoder[6]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    std::atomic<bool> encoderPrecreateRunning{false};
    // RGB 两眼预创建尝试完毕（成败均置位），由 precreate 线程在灰度预创建之前发布。
    // mcap 路径的 RGB 收养只等本标志，不等全局 encoderPrecreateRunning——灰度预创建
    // 与回调懒创建竞态时单路 start() 可达 ~1.2s，全局等待会把 RGB notifyReady 拖过
    // gatekeeper 5s watchdog（降级 OPEN → 首帧锚点后移，首片散布 181~225ms 根因）。
    std::atomic<bool> rgbEyePrecreateDone{false};

    // Encoder surface for zero-copy rendering (Surface mode, RGB only)
    SXR::EncoderSurface* rgbEncoderSurface{nullptr};

    // FBO and texture for side-by-side stitching
    // Grayscale encoder surfaces for zero-copy rendering (Surface mode)
    SXR::EncoderSurface* trackingEncoderSurface{nullptr};
    SXR::EncoderSurface* ctrlEncoderSurface{nullptr};

    // Y8 to YUV shader for grayscale encoding
    GLuint grayscaleEncoderShaderProgram{0};
    GLuint grayscaleEncoderVBO{0};
    GLuint grayscaleEncoderVAO{0};

    // Cached uniform/attrib locations for grayscale shader (avoid per-frame lookups)
    GLint grayShader_uGrayscaleTexture{0};
    GLint grayShader_aPosition{0};
    GLint grayShader_aTexCoord{0};

    // Persistent EGLImage for CV hwBuffer (per-group to avoid race between tracking/ctrl threads)
    EGLImageKHR cvPersistentEGLImage[2]{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};  // [0]=tracking, [1]=ctrl
    GLuint cvPersistentExtTex[2]{0, 0};

    // Separate Y8 textures for each grayscale encoder (NOT shared, each thread uses its own)
    GLuint trackingY8Texture{0};
    GLuint ctrlY8Texture{0};

    // Simple shader for rendering camera texture to encoder surface
    GLuint encoderShaderProgram{0};
    GLuint encoderVBO{0};
    GLuint encoderVAO{0};

    // Cached uniform/attrib locations for encoder shader (avoid per-frame string lookups)
    GLint encShader_uTexture{0};
    GLint encShader_aPosition{0};
    GLint encShader_aTexCoord{0};

    // Persistent GL_TEXTURE_EXTERNAL_OES for RGB eyes — created once, re-bound
    // via eglImageTargetTexture2DOES each frame (avoids per-frame Gen/Delete).
    GLuint rgbPersistentEyeTex[2]{0, 0};

    // ---- MCAP 数据集（spec §4.1）----
    // 帧路由 Adapter：目标无 WS 推流、listener 当前无人注册，Adapter 作唯一注册者
    struct EncoderStreamAdapter : public SXR::IEncoderOutputListener {
        explicit EncoderStreamAdapter(CameraAccessExtension* ext) : mExt(ext) {}
        CameraAccessExtension* mExt;
        void onEncodedFrame(const char* group, const uint8_t* data, size_t size,
                            int64_t ptsUs, bool isConfig) override {
            if (group == nullptr) return;
            // 拷贝 shared_ptr 延长生命周期再判空使用：injectDatasetFormat 对 mMcapManager 的
            // 首次赋值发生在 per-eye 编码器创建之前（无并发读者），此后不再赋值（契约见
            // injectDatasetFormat）；拷贝语义保证即使赋值时机将来变化，本回调持有的对象
            // 也不会在使用中析构。mExt 构造即绑定（mStreamAdapter{this}），不可能为 null。
            auto mcapMgr = mExt->mMcapManager;
            if (mcapMgr && mcapMgr->isMcap() &&
                xr::mcap::McapChunkManager::isMcapGroup(group)) {
                mcapMgr->onEncodedFrame(group, data, size, ptsUs, isConfig);
                return;
            }
            // 其他（_ 前缀非 mcap 组或未知组）：目标无下游消费者，直接丢弃
        }
    };
    EncoderStreamAdapter mStreamAdapter{this};

    // provider 每次录制在 injectDatasetFormat 重建（构造时读 persist.xr.dataset_type
    // 一次缓存，属性切换"下次录制生效"）；mMcapManager 跨录制存活但持 provider
    // 裸指针，重建后必须同批重注入（防悬垂 UB）
    std::unique_ptr<xr::PersistDatasetFormatProvider> mFormatProvider;
    std::shared_ptr<xr::mcap::McapChunkManager> mMcapManager;

    // MCAP 每眼录制编码器（Surface 模式，空 baseDir streaming-only 不写 FMP4，
    // 帧经 EncoderStreamAdapter → McapChunkManager）：录制专用，停录销毁
    // atomic：precreateEye 后台线程在 precreateMutex 下无锁判空（lazyCreated 复查），
    // 而写方持 mRgbEyeEncoderMutex——指针原子化消除跨线程裸读 data race。
    std::atomic<SXR::CameraEncoder*> rgbEyeEncoder[2]{{nullptr}, {nullptr}};  // _rgb_l/_rgb_r
    SXR::EncoderSurface* rgbEyeEncoderSurface[2]{nullptr, nullptr};
    // 原子镜像：相机回调线程（handleRGBFrame/handleCVFrame mcap 分支）的 gate 谓词
    // 只读镜像做"是否就绪"判空——真实指针的创建/销毁由每眼锁保护，但回调每帧持该锁
    // 会与 worker（rgbEncodeProcess/grayEncodeProcess 整个 blit+swap 持锁 ~17ms）竞争，
    // 把 60/120fps 回调阻塞在 HAL 帧管道上，~115s 压垮相机 HAL（Fatal timestamp
    // validation）。改原子镜像后回调零阻塞；判空仅瞬时提示，worker 在每眼锁内复查真实
    // 指针，安全性不依赖谓词瞬时值。生命周期见 startEyeEncoder/adoptEyeEncoder（持锁
    // 写入真实指针后 store）与 destroy*EyeEncoders（持锁置空后 store nullptr）。
    std::atomic<SXR::EncoderSurface*> rgbEyeEncoderSurfaceReady[2]{{nullptr}, {nullptr}};
    // 下标 0/1=tracking L/R、2/3=ctrl L/R（与 handleCVFrame baseIdx 对齐）
    // atomic：precreateEye 后台线程在 precreateMutex 下无锁判空（lazyCreated 复查/
    // 创建前跳过），而写方持自组锁（mTrackingEncoderMutex/mCtrlEncoderMutex）——
    // 指针原子化消除跨线程裸读 data race（同 rgbEyeEncoder）。
    std::atomic<SXR::CameraEncoder*> grayEyeEncoder[4]{{nullptr}, {nullptr}, {nullptr}, {nullptr}};
    SXR::EncoderSurface* grayEyeEncoderSurface[4]{nullptr, nullptr, nullptr, nullptr};
    // 原子镜像（同 rgbEyeEncoderSurfaceReady，见该处注释）：灰度谓词无锁判空用。
    std::atomic<SXR::EncoderSurface*> grayEyeEncoderSurfaceReady[4]{{nullptr}, {nullptr}, {nullptr}, {nullptr}};
    // RGB 每眼编码器：创建/blit/销毁全临界区互斥（与停录销毁互斥）
    std::mutex mRgbEyeEncoderMutex;
    // 灰度 tracking/ctrl 每眼编码器：双回调线程并发，创建/blit/销毁全临界区互斥。
    // 原单把 mGrayscaleEncoderMutex 拆为 tracking/ctrl 各一把（异步编码 worker 每
    // item 持自组锁，共用一把会让两组 worker 互相串行 ~60% 占锁率，重新引入跨组
    // 耦合）；同批触及两组的路径用 std::scoped_lock 双锁。锁分工：
    //   mTrackingEncoderMutex: trackingEncoder/Surface + grayEyeEncoder[0..1]*
    //   mCtrlEncoderMutex:     ctrlEncoder/Surface     + grayEyeEncoder[2..3]*
    std::mutex mTrackingEncoderMutex;
    std::mutex mCtrlEncoderMutex;

    // 灰度每眼 blit 专用 shader（samplerExternalOES 直通 + 0.299/0.587/0.114 亮度提取）。
    // VBO 由 worker 侧 grayWorkerVBO[gi] 管理（attrib 布局 vec2 aPosition + vec2 aTexCoord，
    // 与 cvDisplayVBOs[gi] 同构），blit 时按眼写 uv 切分（左 u=[0,0.5]、右 u=[0.5,1]）。
    // 不复用 cvDisplayVBOs[gi]：其 uv 归 SBS 渲染所有，覆写会污染该组 VBO 状态。
    GLuint grayEyeBlitShaderProgram{0};
    GLint grayEyeBlit_uTexture{0};
    GLint grayEyeBlit_aPosition{0};
    GLint grayEyeBlit_aTexCoord{0};

    // 首帧过 gate 强制 IDR 标志（RGB 侧复用已有 mRgbGateNotified；灰度每组独立）
    bool mGrayEyeGateNotified[2]{false, false};

    // Initialize encoder shader (YUV to RGB color space conversion)
    void initEncoderShader() {
        const char* vertexShaderSource = R"(
            #version 300 es
            in vec2 aPosition;
            in vec2 aTexCoord;
            out vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        )";

        const char* fragmentShaderSource = R"(
            #version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision highp float;
            in vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            out vec4 fragColor;

            void main() {
                // Flip Y coordinate for correct orientation
                vec2 texCoord = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
                vec4 color = texture(uTexture, texCoord);
                fragColor = color;
            }
        )";

        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
        glCompileShader(vertexShader);

        GLint success;
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
            LOGE("Encoder vertex shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            return;
        }

        // Compile fragment shader
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
        glCompileShader(fragmentShader);

        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
            LOGE("Encoder fragment shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return;
        }

        // Link program
        encoderShaderProgram = glCreateProgram();
        glAttachShader(encoderShaderProgram, vertexShader);
        glAttachShader(encoderShaderProgram, fragmentShader);
        glLinkProgram(encoderShaderProgram);

        glGetProgramiv(encoderShaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(encoderShaderProgram, 512, nullptr, infoLog);
            LOGE("Encoder shader program link failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(encoderShaderProgram);
            encoderShaderProgram = 0;
            return;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        // Create fullscreen quad VBO
        float vertices[] = {
            // position     texcoord
            -1.0f, -1.0f,   0.0f, 0.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
            -1.0f,  1.0f,   0.0f, 1.0f,
             1.0f,  1.0f,   1.0f, 1.0f,
        };

        glGenBuffers(1, &encoderVBO);
        glBindBuffer(GL_ARRAY_BUFFER, encoderVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);

        glGenVertexArrays(1, &encoderVAO);
        glBindVertexArray(encoderVAO);
        glBindBuffer(GL_ARRAY_BUFFER, encoderVBO);
        GLint posLoc = glGetAttribLocation(encoderShaderProgram, "aPosition");
        GLint texLoc = glGetAttribLocation(encoderShaderProgram, "aTexCoord");
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(texLoc);
        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);

        // Cache uniform/attrib locations to avoid per-frame string lookups
        encShader_uTexture = glGetUniformLocation(encoderShaderProgram, "uTexture");
        encShader_aPosition = glGetAttribLocation(encoderShaderProgram, "aPosition");
        encShader_aTexCoord = glGetAttribLocation(encoderShaderProgram, "aTexCoord");

        LOGI("Encoder shader initialized");
    }

    // Initialize grayscale encoder shader (Y8 to YUV conversion)
    void initGrayscaleEncoderShader() {
        const char* vertexShaderSource = R"(
            #version 300 es
            in vec4 aPosition;
            in vec2 aTexCoord;
            out vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = aTexCoord;
            }
        )";

        // Use samplerExternalOES for camera buffer, but extract only Y (luminance) for grayscale
        const char* fragmentShaderSource = R"(
            #version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision highp float;
            in vec2 vTexCoord;
            uniform samplerExternalOES uGrayscaleTexture;
            out vec4 fragColor;

            void main() {
                // Sample from external texture
                vec4 color = texture(uGrayscaleTexture, vTexCoord);
                // For grayscale camera, the image is monochrome
                // Use luminance (Y) from YUV conversion result
                // Y = 0.299*R + 0.587*G + 0.114*B (BT.601)
                float y = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
                // Output grayscale
                fragColor = vec4(vec3(y), 1.0);
            }
        )";

        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
        glCompileShader(vertexShader);

        // Check vertex shader compilation
        GLint success;
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
            LOGE("Grayscale encoder vertex shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            return;
        }

        // Compile fragment shader
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
        glCompileShader(fragmentShader);

        // Check fragment shader compilation
        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
            LOGE("Grayscale encoder fragment shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return;
        }

        // Link program
        grayscaleEncoderShaderProgram = glCreateProgram();
        glAttachShader(grayscaleEncoderShaderProgram, vertexShader);
        glAttachShader(grayscaleEncoderShaderProgram, fragmentShader);
        glLinkProgram(grayscaleEncoderShaderProgram);

        // Check link status
        glGetProgramiv(grayscaleEncoderShaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(grayscaleEncoderShaderProgram, 512, nullptr, infoLog);
            LOGE("Grayscale encoder shader program link failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(grayscaleEncoderShaderProgram);
            grayscaleEncoderShaderProgram = 0;
            return;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        // Create fullscreen quad VBO
        float vertices[] = {
            // position     texcoord
            -1.0f, -1.0f,   0.0f, 1.0f,  // Flip Y for correct orientation
             1.0f, -1.0f,   1.0f, 1.0f,
            -1.0f,  1.0f,   0.0f, 0.0f,
             1.0f,  1.0f,   1.0f, 0.0f,
        };

        glGenBuffers(1, &grayscaleEncoderVBO);
        glBindBuffer(GL_ARRAY_BUFFER, grayscaleEncoderVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        glGenVertexArrays(1, &grayscaleEncoderVAO);
        glBindVertexArray(grayscaleEncoderVAO);

        GLint posLoc = glGetAttribLocation(grayscaleEncoderShaderProgram, "aPosition");
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        GLint texLoc = glGetAttribLocation(grayscaleEncoderShaderProgram, "aTexCoord");
        glEnableVertexAttribArray(texLoc);
        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Cache uniform/attrib locations to avoid per-frame lookups
        grayShader_uGrayscaleTexture = glGetUniformLocation(grayscaleEncoderShaderProgram, "uGrayscaleTexture");
        grayShader_aPosition = glGetAttribLocation(grayscaleEncoderShaderProgram, "aPosition");
        grayShader_aTexCoord = glGetAttribLocation(grayscaleEncoderShaderProgram, "aTexCoord");

        LOGI("Grayscale encoder shader initialized");
    }

    // Initialize MCAP grayscale per-eye blit shader (OES 采样 + 亮度提取转中性灰)。
    // 相机 OES 纹理（cvPersistentExtTex，SBS 左右半幅）→ 每眼 EncoderSurface 绘制。
    // 直接采样 OES（EGLImage 直读，跨 EGL context 内容新鲜）；不再经 cvDisplayTextures
    //（GL_TEXTURE_2D 跨 context 无同步读到陈旧缓存 → ctrl/tracking 首帧后全冻结，见
    // handleCVFrame per-eye blit 注释）。无 Y flip：v=0 行即 SBS 输出的底行，直通方向一致。
    // VBO 由 worker 侧 grayWorkerVBO[gi] 管理（attrib 布局 vec2 aPosition + vec2 aTexCoord，
    // 与 cvDisplayVBOs[gi] 同构），blit 时按眼写 uv 切分（左 u=[0,0.5]、右 u=[0.5,1]）。
    // 不复用 cvDisplayVBOs[gi]：其 uv 归 SBS 渲染所有，覆写会污染该组 VBO 状态。
    void initGrayEyeBlitShader() {
        const char* vertexShaderSource = R"(
            #version 300 es
            in vec2 aPosition;
            in vec2 aTexCoord;
            out vec2 vTexCoord;
            void main() {
                gl_Position = vec4(aPosition, 0.0, 1.0);
                vTexCoord = aTexCoord;
            }
        )";

        const char* fragmentShaderSource = R"(
            #version 300 es
            #extension GL_OES_EGL_image_external_essl3 : require
            precision highp float;
            in vec2 vTexCoord;
            uniform samplerExternalOES uTexture;
            out vec4 fragColor;
            void main() {
                // 灰度相机 OES 采样亮度在 G 通道（R=B=0）：直通会把 (0,y,0) 写进编码器
                // surface，编码后 U≈60/V≈42 画面全绿（实测 150118 录制 U=59.5/V=41.6，
                // 反推写入 RGB≈(0,235,0)）。与 grayscaleEncoderShaderProgram 同法
                // 提取亮度转中性灰（编码后 U=V=128，旧 cvDisplayTextures 路径实测正确）。
                vec4 color = texture(uTexture, vTexCoord);
                float y = 0.299 * color.r + 0.587 * color.g + 0.114 * color.b;
                fragColor = vec4(vec3(y), 1.0);
            }
        )";

        // Compile vertex shader
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
        glCompileShader(vertexShader);

        GLint success;
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
            LOGE("Gray eye blit vertex shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            return;
        }

        // Compile fragment shader
        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
        glCompileShader(fragmentShader);

        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
            LOGE("Gray eye blit fragment shader compilation failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return;
        }

        // Link program
        grayEyeBlitShaderProgram = glCreateProgram();
        glAttachShader(grayEyeBlitShaderProgram, vertexShader);
        glAttachShader(grayEyeBlitShaderProgram, fragmentShader);
        glLinkProgram(grayEyeBlitShaderProgram);

        glGetProgramiv(grayEyeBlitShaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(grayEyeBlitShaderProgram, 512, nullptr, infoLog);
            LOGE("Gray eye blit shader program link failed: %s", infoLog);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            glDeleteProgram(grayEyeBlitShaderProgram);
            grayEyeBlitShaderProgram = 0;
            return;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        // Cache uniform/attrib locations to avoid per-frame lookups
        grayEyeBlit_uTexture = glGetUniformLocation(grayEyeBlitShaderProgram, "uTexture");
        grayEyeBlit_aPosition = glGetAttribLocation(grayEyeBlitShaderProgram, "aPosition");
        grayEyeBlit_aTexCoord = glGetAttribLocation(grayEyeBlitShaderProgram, "aTexCoord");

        LOGI("Grayscale per-eye blit shader initialized");
    }

    // Render texture to encoder surface (called from GL thread)
    void renderTextureToEncoderSurface(SXR::EncoderSurface* encoderSurface, GLuint textureId, int width, int height) {
        if (!encoderSurface || !encoderSurface->isInitialized()) {
            return;
        }

        // Save current EGL context
        EGLDisplay prevDisplay = eglGetCurrentDisplay();
        EGLSurface prevDrawSurface = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface prevReadSurface = eglGetCurrentSurface(EGL_READ);
        EGLContext prevContext = eglGetCurrentContext();

        // Make encoder surface current
        if (!encoderSurface->makeCurrent()) {
            LOGE("Failed to make encoder surface current");
            return;
        }

        // Clear and render
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Use shader (shaders are shared between contexts)
        glUseProgram(encoderShaderProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, textureId);
        glUniform1i(glGetUniformLocation(encoderShaderProgram, "uTexture"), 0);

        // Draw fullscreen quad without VAO (VAOs are not shared between contexts)
        // Use direct vertex attribute setup
        float vertices[] = {
            // position     texcoord
            -1.0f, -1.0f,   0.0f, 0.0f,
             1.0f, -1.0f,   1.0f, 0.0f,
            -1.0f,  1.0f,   0.0f, 1.0f,
             1.0f,  1.0f,   1.0f, 1.0f,
        };

        GLint posLoc = glGetAttribLocation(encoderShaderProgram, "aPosition");
        GLint texLoc = glGetAttribLocation(encoderShaderProgram, "aTexCoord");

        glBindBuffer(GL_ARRAY_BUFFER, encoderVBO);
        glEnableVertexAttribArray(posLoc);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(texLoc);
        glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(posLoc);
        glDisableVertexAttribArray(texLoc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Swap buffers to submit frame to encoder
        encoderSurface->swapBuffers();

        // Restore previous EGL context
        if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(prevDisplay, prevDrawSurface, prevReadSurface, prevContext);
        }
    }

    void initTimeConversion() {
        sInstance = this;
        XrResult res;
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrConvertTimespecTimeToTimeKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&xrConvertTimespecTimeToTimeKHR));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrConvertTimespecTimeToTimeKHR function failed!");
        }
        res = xrGetInstanceProcAddr(engine->state.xrInstance,"xrConvertTimeToTimespecTimeKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&xrConvertTimeToTimespecTimeKHR));
        if(res != XR_SUCCESS)
        {
            LOGE("get xrConvertTimeToTimespecTimeKHR function failed!");
        }

        g_boottimeToXrTimeFn = &CameraAccessExtension::staticBoottimeToXrTime;
        g_xrTimeToBoottimeFn = &CameraAccessExtension::staticXrTimeToBoottime;
    }

    // Cleanup all camera GL contexts and related resources
    void cleanupAllGLContexts() {
        // Save current context to restore later
        EGLDisplay prevDisplay = eglGetCurrentDisplay();
        EGLSurface prevDraw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface prevRead = eglGetCurrentSurface(EGL_READ);
        EGLContext prevContext = eglGetCurrentContext();

        // Cleanup RGB context resources
        if (rgbCtx.initialized) {
            rgbCtx.makeCurrent();
            for (int i = 0; i < 2; i++) {
                if (rgbDisplayTextures[i] != 0) { glDeleteTextures(1, &rgbDisplayTextures[i]); rgbDisplayTextures[i] = 0; }
                if (rgbDisplayFBOs[i] != 0) { glDeleteFramebuffers(1, &rgbDisplayFBOs[i]); rgbDisplayFBOs[i] = 0; }
                if (rgbPersistentEyeTex[i] != 0) { glDeleteTextures(1, &rgbPersistentEyeTex[i]); rgbPersistentEyeTex[i] = 0; }
            }
            if (rgbEncoderSurface) { rgbEncoderSurface->release(); delete rgbEncoderSurface; rgbEncoderSurface = nullptr; }
            if (rgbEncoder) { rgbEncoder->stop(); delete rgbEncoder; rgbEncoder = nullptr; }
            if (encoderShaderProgram != 0) { glDeleteProgram(encoderShaderProgram); encoderShaderProgram = 0; }
            if (encoderVBO != 0) { glDeleteBuffers(1, &encoderVBO); encoderVBO = 0; }
            if (encoderVAO != 0) { glDeleteVertexArrays(1, &encoderVAO); encoderVAO = 0; }
            if (trackingY8Texture) { glDeleteTextures(1, &trackingY8Texture); trackingY8Texture = 0; }
            if (ctrlY8Texture) { glDeleteTextures(1, &ctrlY8Texture); ctrlY8Texture = 0; }
            rgbCtx.releaseCurrent();
            rgbCtx.cleanup();
        }

        // Cleanup Tracking/CTRL context resources
        if (trackingCtx.initialized || ctrlCtx.initialized) {
            // Use tracking context for CV cleanup (resources are shared)
            trackingCtx.makeCurrent();
            if (cvPersistentEGLImage[0] != EGL_NO_IMAGE_KHR) {
                glext::eglDestroyImageKHR(trackingCtx.display, cvPersistentEGLImage[0]);
                cvPersistentEGLImage[0] = EGL_NO_IMAGE_KHR;
            }
            if (cvPersistentEGLImage[1] != EGL_NO_IMAGE_KHR) {
                glext::eglDestroyImageKHR(trackingCtx.display, cvPersistentEGLImage[1]);
                cvPersistentEGLImage[1] = EGL_NO_IMAGE_KHR;
            }
            if (cvPersistentExtTex[0] != 0) {
                glDeleteTextures(1, &cvPersistentExtTex[0]);
                cvPersistentExtTex[0] = 0;
            }
            if (cvPersistentExtTex[1] != 0) {
                glDeleteTextures(1, &cvPersistentExtTex[1]);
                cvPersistentExtTex[1] = 0;
            }
            for (int i = 0; i < 4; i++) {
                if (cvDisplayTextures[i] != 0) { glDeleteTextures(1, &cvDisplayTextures[i]); cvDisplayTextures[i] = 0; }
                if (cvDisplayFBOs[i] != 0) { glDeleteFramebuffers(1, &cvDisplayFBOs[i]); cvDisplayFBOs[i] = 0; }
            }
            if (cvDisplayVBOs[0] != 0) { glDeleteBuffers(1, &cvDisplayVBOs[0]); cvDisplayVBOs[0] = 0; }
            if (cvDisplayVBOs[1] != 0) { glDeleteBuffers(1, &cvDisplayVBOs[1]); cvDisplayVBOs[1] = 0; }
            if (grayscaleEncoderShaderProgram != 0) { glDeleteProgram(grayscaleEncoderShaderProgram); grayscaleEncoderShaderProgram = 0; }
            if (grayscaleEncoderVBO != 0) { glDeleteBuffers(1, &grayscaleEncoderVBO); grayscaleEncoderVBO = 0; }
            if (grayscaleEncoderVAO != 0) { glDeleteVertexArrays(1, &grayscaleEncoderVAO); grayscaleEncoderVAO = 0; }
            if (grayEyeBlitShaderProgram != 0) { glDeleteProgram(grayEyeBlitShaderProgram); grayEyeBlitShaderProgram = 0; }
            trackingCtx.releaseCurrent();
        }
        trackingCtx.cleanup();
        ctrlCtx.cleanup();

        // Cleanup encoders
        if (trackingEncoder) { trackingEncoder->stop(); delete trackingEncoder; trackingEncoder = nullptr; }
        if (ctrlEncoder) { ctrlEncoder->stop(); delete ctrlEncoder; ctrlEncoder = nullptr; }
        if (trackingEncoderSurface) { trackingEncoderSurface->release(); delete trackingEncoderSurface; trackingEncoderSurface = nullptr; }
        if (ctrlEncoderSurface) { ctrlEncoderSurface->release(); delete ctrlEncoderSurface; ctrlEncoderSurface = nullptr; }

        // Restore previous context
        if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
        }
        LOGI("All camera GL contexts cleaned up");
    }

    // 构建 camera_params_<group>.json 原始 JSON 文本（MP4 写盘格式，逐字节一致）。
    // MP4 模式写盘用；mcap 模式缓存后经 attachment 嵌入。
    static std::string buildCameraParamsJson(const SXR::FrameData* data, const char* groupName) {
        const char* eyeNames[2] = {"left", "right"};
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "{\n  \"version\": " << xr::kDatasetFormatVersion
            << ",\n  \"group\": \"" << groupName << "\",\n  \"cameras\": [\n";

        for (int i = 0; i < 2; i++) {
            const auto& f = data->frames[i];
            if (i > 0) oss << ",\n";
            oss << "    {\n"
                << "      \"eye\": \"" << eyeNames[i] << "\",\n"
                << "      \"width\": " << f.width << ",\n"
                << "      \"height\": " << f.height << ",\n"
                << "      \"intrinsics\": {\n"
                << "        \"focalX\": " << f.focalX << ",\n"
                << "        \"focalY\": " << f.focalY << ",\n"
                << "        \"centerX\": " << f.centerX << ",\n"
                << "        \"centerY\": " << f.centerY << ",\n"
                << "        \"radialDistortion\": [";
            for (int d = 0; d < 8; d++) {
                if (d > 0) oss << ", ";
                oss << f.radialDistortion[d];
            }
            oss << "]\n      },\n";

            // Extrinsics (raw SVR body frame, no coordinate transform).
            float ep[3], er[4];
            memcpy(ep, f.position, sizeof(ep));
            memcpy(er, f.rotation, sizeof(er));

            oss << "      \"extrinsics\": {\n"
                << "        \"position\": [" << ep[0] << ", " << ep[1] << ", " << ep[2] << "],\n"
                << "        \"rotation\": [" << er[0] << ", " << er[1] << ", " << er[2] << ", " << er[3] << "]\n"
                << "      }\n"
                << "    }";
        }

        oss << "\n  ]\n}\n";
        return oss.str();
    }

    // Save camera intrinsics/extrinsics to JSON (called once per group per recording session)
    void saveCameraParams(const SXR::FrameData* data, const char* groupName,
                          std::atomic<bool>& savedFlag) {
        if (savedFlag.load() || encoderBaseDir.empty()) return;

        const char* eyeNames[2] = {"left", "right"};

        // mcap 分支：不写 camera_params_*.json，标定缓存入 McapChunkManager
        //（/camera/<group>/<eye>/info 每眼一条 + 全部相机合并一条 /tf_static），
        // 由 mcap 侧在分片 finish() 前统一写（startNewChunk swap / end()）。
        if (mMcapManager && mMcapManager->isMcap()) {
            // JSON 内 timestamp 用首帧曝光 UTC（与 onEncodedFrame 锚定的会话起点一致）：
            // sessionStartNs() 在首帧前返回 begin 时刻，比数据起点早 ~2.4s（RecordingGatekeeper
            // 门控延迟），会与锚定后的 /info log_time 产生 2.4s 错位。
            const int64_t t0Ns = (int64_t)(data->frames[0].timestamp + data->frames[0].exposure / 2)
                               + mCameraTimeOffsetNs.load();
            // /tf_static 不做坐标转换：直接写原始 SVR 外参（Camera→Body，OpenXR/SVR 约定），
            // 与附件 camera_params_<group>.json 的 extrinsics 完全一致，parent=body →
            // child=相机 SVR 系。SVR 相机系（X上/Y右/Z前）≠ optical 系（X右/Y下/Z前），
            // 二者固定相差基变换 M=Hᵀ（H=[[0,1,0],[-1,0,0],[0,0,1]]），由下游按需自行转换
            //（旧版曾在此做 H·R·Hᵀ/t·H 相似变换，等效把 parent 换成了非 Body 系，与 /tf 链差 90°，已移除）。
            for (int i = 0; i < 2; i++) {
                const auto& f = data->frames[i];
                const std::string grp(groupName);
                const std::string fid = "camera_" + grp + "_" + eyeNames[i] + "_optical_frame";
                const float D[4] = { f.radialDistortion[0], f.radialDistortion[1],
                                     f.radialDistortion[2], f.radialDistortion[3] };
                mMcapManager->cacheCameraInfo(
                    "/camera/" + grp + "/" + eyeNames[i] + "/info",
                    xr::mcap::cameraCalibrationJson(t0Ns, fid, f.width, f.height,
                                                    f.focalX, f.focalY, f.centerX, f.centerY, D));
                // /tf_static child 用 _svr_frame 命名，与 info/视频的 optical frame 明确区分
                const std::string tfFid = "camera_" + grp + "_" + eyeNames[i] + "_svr_frame";
                mMcapManager->cacheTfTransform(tfFid,
                    {(double)f.position[0], (double)f.position[1], (double)f.position[2]},
                    {(double)f.rotation[0], (double)f.rotation[1],
                     (double)f.rotation[2], (double)f.rotation[3]});
            }
            // 原始 JSON 缓存：分片定稿时作 camera_params_<group>.json attachment 嵌入
            mMcapManager->cacheCameraParamsJson(groupName, buildCameraParamsJson(data, groupName));
            savedFlag = true;
            LOGI("Camera params cached to mcap channels: group=%s", groupName);
            return;
        }

        const std::string json = buildCameraParamsJson(data, groupName);

        std::string filePath = encoderBaseDir + "/camera_params_" + groupName + ".json";
        std::ofstream ofs(filePath);
        if (ofs.is_open()) {
            ofs << json;
            ofs.close();
            savedFlag = true;
            LOGI("Camera params saved: %s", filePath.c_str());
        } else {
            LOGE("Failed to save camera params: %s", filePath.c_str());
        }
    }

    // Write IMU calibration sidecar (strict JSON, no comments; documented in Readme.md)
    void saveImuCalibration(const std::string& path, const SXR::SxrImuCalibration& c) {
        auto arr3 = [&](const float* v) {
            std::ostringstream o; o << std::fixed << std::setprecision(9);
            o << "[" << v[0] << ", " << v[1] << ", " << v[2] << "]";
            return o.str();
        };
        std::ostringstream o;
        o << std::fixed << std::setprecision(9);
        // IMU bias is written raw in sensor-native frame (no coordinate
        // transform) — straight from the sensor calibration API.
        o << "{\n"
          << "  \"version\": " << xr::kDatasetFormatVersion << ",\n"
          << "  \"device_uid\": \"" << c.deviceUid << "\",\n"
          << "  \"imu\": {\n"
          << "    \"imu_id\": " << c.imuId << ",\n"
          << "    \"is_primary\": " << (c.isPrimary ? "true" : "false") << ",\n"
          << "    \"bias\": {\n"
          << "      \"accelerometer_mps2\": " << arr3(c.accelBias) << ",\n"
          << "      \"gyroscope_rads\": " << arr3(c.gyroBias) << "\n"
          << "    },\n"
          << "    \"scale_factor\": {\n"
          << "      \"accelerometer\": " << arr3(c.accelScale) << ",\n"
          << "      \"gyroscope\": " << arr3(c.gyroScale) << "\n"
          << "    },\n"
          << "    \"nonorthogonality\": {\n"
          << "      \"accelerometer\": " << arr3(c.accelNonorth) << ",\n"
          << "      \"gyroscope\": " << arr3(c.gyroNonorth) << "\n"
          << "    },\n"
          << "    \"time_alignment_s\": {\n"
          << "      \"imu_to_pose\": " << c.stateinitDelta << ",\n"
          << "      \"cameras\": {\n";
    for (int i = 0; i < c.cameraTimeAlignCount; ++i) {
        o << "        \"" << c.cameraTimeAlign[i].name << "\": "
          << c.cameraTimeAlign[i].deltaSec
          << (i + 1 < c.cameraTimeAlignCount ? ",\n" : "\n");
    }
    o << "      },\n"
          << "      \"accel\": " << c.accelDelta << "\n"
          << "    }\n"
          << "  },\n"
          << "  \"noise\": {\n"
          << "    \"accel_noise_std_mps2\": " << arr3(c.accelNoiseStd) << ",\n"
          << "    \"gyro_noise_std_rads\": " << arr3(c.gyroNoiseStd) << ",\n"
          << "    \"accel_bias_std_mps2\": " << arr3(c.accelBiasStd) << ",\n"
          << "    \"gyro_bias_std_rads\": " << arr3(c.gyroBiasStd) << "\n"
          << "  }\n"
          << "}\n";
        std::ofstream ofs(path);
        if (ofs.is_open()) {
            ofs << o.str();
            ofs.close();
            LOGI("IMU calibration saved: %s", path.c_str());
        } else {
            LOGE("Failed to save IMU calibration: %s", path.c_str());
        }
    }

    void restartGrayscaleEncoders() {
        // 与 stopEncoder 互斥：防止并发 stop+delete 同一 encoder 导致
        // std::thread 析构时仍 joinable（std::terminate）。本函数同批触及
        // tracking/ctrl 两组（SBS + mcap 每眼），双锁一起拿。
        std::scoped_lock lock(mTrackingEncoderMutex, mCtrlEncoderMutex);
        encodersStopped = true;
        // 复位与 worker 写方（rgbEncodeProcess，持 mRgbEyeEncoderMutex）无共锁：此处置
        // false 后理论上在途旧帧仍可在 worker 侧写回 true，但本函数同批销毁 RGB SBS
        // 编码器、encodersStopped=true 使 worker 入口提前 return，写回不会发生（见
        // mRgbGateNotified 声明处注释）。
        mRgbGateNotified = false;
        mTrackingGateNotified = false;
        mCtrlGateNotified = false;
        discardPendingEncoders();
        if (trackingEncoder) { trackingEncoder->stop(); delete trackingEncoder; trackingEncoder = nullptr; }
        if (trackingEncoderSurface) { trackingEncoderSurface->release(); delete trackingEncoderSurface; trackingEncoderSurface = nullptr; }
        if (ctrlEncoder) { ctrlEncoder->stop(); delete ctrlEncoder; ctrlEncoder = nullptr; }
        if (ctrlEncoderSurface) { ctrlEncoderSurface->release(); delete ctrlEncoderSurface; ctrlEncoderSurface = nullptr; }
        // mcap 残留每眼编码器随重启一并清理（mRgbGateNotified 已在上方重置，不重复）
        { std::lock_guard<std::mutex> rlock(mRgbEyeEncoderMutex); destroyRgbEyeEncoders(); }
        destroyGrayEyeEncoders();
        mGrayEyeGateNotified[0] = false;
        mGrayEyeGateNotified[1] = false;
    }

    // 清空未被收养的预创建编码器（stop/restart 时；与 helper 存入、回调收养互斥）
    void discardPendingEncoders() {
        std::lock_guard<std::mutex> lock(precreateMutex);
        for (int i = 0; i < 2; i++) {
            if (pendingGrayEncoder[i]) {
                pendingGrayEncoder[i]->stop();
                delete pendingGrayEncoder[i];
                pendingGrayEncoder[i] = nullptr;
            }
        }
        if (pendingRgbEncoder) {
            pendingRgbEncoder->stop();
            delete pendingRgbEncoder;
            pendingRgbEncoder = nullptr;
        }
        // mcap 每眼预创建槽同批清理（防泄漏/防旧目录收养）
        for (int i = 0; i < 6; i++) {
            if (pendingEyeEncoder[i]) {
                pendingEyeEncoder[i]->stop();
                delete pendingEyeEncoder[i];
                pendingEyeEncoder[i] = nullptr;
            }
        }
    }

    // 录制开始时后台预创建编码器，把 ~36ms/路 的 AMediaCodec create/configure/start
    // 移出相机回调线程。只做纯 C++/binder/文件部分；EGL surface + 纹理仍由回调
    // 线程收养时创建。
    // - mp4 模式：预创建 3 路 SBS（tracking/ctrl 灰度 + RGB）。
    // - mcap 模式：预创建 6 路每眼（_rgb_l/_r、_tracking_l/_r、_ctrl_l/_r）——原为
    //   回调内联创建，录制首帧 78~132ms 尖刺来源；收养后回调只补 surface init。
    void precreateEncodersAsync() {
        if (encoderPrecreateRunning.exchange(true)) return;  // 防重入
        // 已知微小窗口：复位前读者可能见到 running=true + 上一轮 done=true 的
        // 残留组合而提前放行，后果由 precreateEye 内 lazyCreated 兜底复查吸收。
        rgbEyePrecreateDone.store(false, std::memory_order_release);
        std::thread([this]() {
            // 等帧尺寸就绪（相机通常已在出流，立即满足；2s 超时放弃，走回调内联兜底）
            for (int i = 0; i < 400; i++) {
                if (cvFrameWidths[0] && cvFrameWidths[2] && rgbFrameWidths[0]) break;
                usleep(5000);
            }
            bool mcapMode = (mMcapManager && mMcapManager->isMcap());
            if (mcapMode && !encodersStopped.load()) {
                // helper：create→configure→start 后存槽（存槽时复查 encodersStopped，
                // 创建期间录制被停止则直接销毁，防旧目录收养/泄漏）
                // （本仓库 CameraEncoder 无 setTimeOffset指针版/setChunkState 接口：
                // 每眼路径 mcap log_time 由 McapChunkManager 注入 offset，无需两者。）
                auto precreateEye = [&](SXR::CameraEncoder* enc, const char* gn, int slot) {
                    if (enc->start()) {
                        std::lock_guard<std::mutex> lock(precreateMutex);
                        // 存槽前复查 lazy 侧（initEncodersAndSurfaces/initGrayscaleEncoder
                        // 回调线程）是否已先创建该眼编码器：并发时回调内联创建成功后，
                        // 本预创建结果存入槽位将永不被收养（lazy 幂等守卫认已存在编码器
                        // 直接 return），孤儿编码器白占 msm_vidc 实例直到 stop 才清理。
                        // 槽位语义：0/1=rgbEye，2/3=trackingEye，4/5=ctrlEye。
                        // 槽位数组为 atomic（见声明处）：lazy 写方持各自锁、与本处
                        // precreateMutex 不互斥，原子读消除 data race；可能读到旧值，
                        // 漏判的最坏情况即上述孤儿编码器，由下方分支消化。
                        const bool lazyCreated = (slot < 2)
                            ? (rgbEyeEncoder[slot] != nullptr)
                            : (grayEyeEncoder[slot - 2] != nullptr);
                        if (encodersStopped.load() || lazyCreated) {
                            enc->stop();
                            delete enc;
                        } else {
                            pendingEyeEncoder[slot] = enc;
                            LOGI("Precreated %s eye encoder off-thread", gn);
                        }
                    } else {
                        LOGE("Precreate %s eye encoder failed (callback will retry inline)", gn);
                        delete enc;
                    }
                };
                uint32_t rw = rgbFrameWidths[0], rh = rgbFrameHeights[0];
                for (int i = 0; i < 2 && rw && rh; i++) {
                    const char* gn = (i == 0) ? "_rgb_l" : "_rgb_r";
                    auto* enc = new SXR::CameraEncoder(rw, rh, readRgbFps(), 4000000,
                                                       /*outputName*/"rgb.mp4", /*baseDir*/"");
                    enc->setGroupName(gn);
                    precreateEye(enc, gn, i);
                }
                // RGB 两眼预创建尝试完毕（成败均置位，失败路由由回调内联兜底）：
                // 在灰度预创建之前发布，让 mcap 路径的 RGB 收养不再等灰度。
                rgbEyePrecreateDone.store(true, std::memory_order_release);
                for (int gi = 0; gi < 2; gi++) {
                    uint32_t w = cvFrameWidths[gi * 2], h = cvFrameHeights[gi * 2];
                    if (!w || !h) continue;
                    const char* groupName = (gi == 0) ? "tracking" : "ctrl";
                    for (int i = 0; i < 2; i++) {
                        std::string gn = std::string("_") + groupName + (i == 0 ? "_l" : "_r");
                        // 创建前复查 lazy 侧：灰度回调懒创建（restartGrayscaleEncoders
                        // 重建路径）与本线程竞态，已存在则跳过——避免重复
                        // create/start（高负载下单次 ~1.2s）白做并拖长
                        // encoderPrecreateRunning 窗口。
                        // 注意：lazy 写方持 mTrackingEncoderMutex/mCtrlEncoderMutex，
                        // 与本处 precreateMutex 不互斥，槽位 atomic 化后本检查为无锁
                        // 原子读（消除 data race），但仍可能读到旧值——仅 best-effort
                        // 快路径（宁可漏跳不可错跳）；正确性由 start() 后 precreateEye
                        // 内的 lazyCreated 兜底复查保证（漏跳的最坏情况是孤儿编码器
                        // 白占一路 msm_vidc 实例直到 stop，见 precreateEye 注释）。
                        {
                            std::lock_guard<std::mutex> lock(precreateMutex);
                            if (grayEyeEncoder[gi * 2 + i] != nullptr) continue;
                        }
                        auto* enc = new SXR::CameraEncoder(gn.c_str(), w, h, 60,
                                                           SXR::EncoderMode::SURFACE, /*baseDir*/"");
                        precreateEye(enc, gn.c_str(), 2 + gi * 2 + i);
                    }
                }
            }
            if (!mcapMode && !encodersStopped.load()) {
                for (int gi = 0; gi < 2; gi++) {
                    uint32_t w = cvFrameWidths[gi * 2], h = cvFrameHeights[gi * 2];
                    if (!w || !h) continue;
                    const char* groupName = (gi == 0) ? "tracking" : "ctrl";
                    auto* encoder = new SXR::CameraEncoder(groupName, w * 2, h, 60,
                                                           SXR::EncoderMode::SURFACE, encoderBaseDir);
                    encoder->setTimeOffset(mCameraTimeOffsetNs);
                    if (encoder->start()) {
                        std::lock_guard<std::mutex> lock(precreateMutex);
                        if (encodersStopped.load()) {
                            // 创建期间录制被停止：不入槽（stop 的槽清理可能已跑过），直接销毁
                            encoder->stop();
                            delete encoder;
                        } else {
                            pendingGrayEncoder[gi] = encoder;
                            LOGI("Precreated %s encoder off-thread (%ux%u)", groupName, w * 2, h);
                        }
                    } else {
                        LOGE("Precreate %s encoder failed (callback will retry inline)", groupName);
                        delete encoder;
                    }
                }
                uint32_t w = rgbFrameWidths[0], h = rgbFrameHeights[0];
                if (w && h) {
                    auto* enc = new SXR::CameraEncoder(w * 2, h, readRgbFps(), 8000000,
                                                       "rgb.mp4", encoderBaseDir);
                    enc->setGroupName("_rgb");
                    enc->setTimeOffset(mCameraTimeOffsetNs);
                    if (enc->start()) {
                        std::lock_guard<std::mutex> lock(precreateMutex);
                        if (encodersStopped.load()) {
                            enc->stop();
                            delete enc;
                        } else {
                            pendingRgbEncoder = enc;
                            LOGI("Precreated RGB encoder off-thread (%ux%u)", w * 2, h);
                        }
                    } else {
                        LOGE("Precreate RGB encoder failed (callback will retry inline)");
                        delete enc;
                    }
                }
            }
            encoderPrecreateRunning.store(false, std::memory_order_release);
        }).detach();
    }

    // 每眼编码器启动（裁剪版）：空 baseDir → streaming-only 不写 FMP4/csv，
    // 帧经 listener → McapChunkManager；setTimeOffset 省略（mcap log_time 由
    // McapChunkManager 注入 offset）；目标 CameraEncoder 无 setRecordingEnabled/
    // setChunkState 接口，不移植。出参 atomic：precreateEye 后台线程持 precreateMutex
    // 无锁判空（lazyCreated 复查），与持每眼锁的写方并发——原子化消除 data race；
    // outSurfReady 为回调谓词无锁判空镜像（见 rgbEyeEncoderSurfaceReady 注释）。
    bool startEyeEncoder(SXR::CameraEncoder* enc, const char* tag,
                         EGLDisplay dpy, EGLContext eglCtx,
                         std::atomic<SXR::CameraEncoder*>* outEnc, SXR::EncoderSurface** outSurf,
                         std::atomic<SXR::EncoderSurface*>* outSurfReady) {
        if (!enc->start()) {
            LOGE("per-eye encoder %s start failed", tag);
            delete enc;
            return false;
        }
        auto* surf = new SXR::EncoderSurface();
        if (!surf->init(enc->getInputSurface(), dpy, eglCtx)) {
            LOGE("per-eye encoder surface %s init failed", tag);
            delete surf;
            enc->stop();
            delete enc;
            return false;
        }
        outEnc->store(enc, std::memory_order_release);
        *outSurf = surf;
        // 真实指针（锁保护）写好后发布原子镜像（回调谓词无锁判空用）
        outSurfReady->store(surf, std::memory_order_release);
        return true;
    }

    // MCAP 每眼编码器收养：pendingEyeEncoder 槽中的编码器已在后台完成 start（codec
    // 部分），此处只补 surface init（~2.5ms）。任一步失败整体清理并返回 false。
    // 调用方须已持对应每眼锁（mRgbEyeEncoderMutex/mTrackingEncoderMutex/mCtrlEncoderMutex）。
    // （本仓库 CameraEncoder 无 setRecordingEnabled 接口，空 baseDir 即 streaming-only，
    // 与 startEyeEncoder 同语义。）
    bool adoptEyeEncoder(SXR::CameraEncoder* enc, const char* tag,
                         EGLDisplay dpy, EGLContext eglCtx,
                         std::atomic<SXR::CameraEncoder*>* outEnc, SXR::EncoderSurface** outSurf,
                         std::atomic<SXR::EncoderSurface*>* outSurfReady) {
        auto* surf = new SXR::EncoderSurface();
        if (!surf->init(enc->getInputSurface(), dpy, eglCtx)) {
            LOGE("per-eye encoder surface %s init failed", tag);
            delete surf;
            enc->stop();
            delete enc;
            return false;
        }
        outEnc->store(enc, std::memory_order_release);
        *outSurf = surf;
        outSurfReady->store(surf, std::memory_order_release);
        return true;
    }

    // 调用方须持 mRgbEyeEncoderMutex（stopRecordingAsync / initEncodersAndSurfaces 失败清理）
    void destroyRgbEyeEncoders() {
        for (int i = 0; i < 2; i++) {
            auto* e = rgbEyeEncoder[i].load(std::memory_order_relaxed);
            if (e) { e->stop(); delete e; rgbEyeEncoder[i] = nullptr; }
            if (rgbEyeEncoderSurface[i]) {
                rgbEyeEncoderSurface[i]->release();
                delete rgbEyeEncoderSurface[i];
                rgbEyeEncoderSurface[i] = nullptr;
            }
            rgbEyeEncoderSurfaceReady[i].store(nullptr, std::memory_order_release);
        }
    }

    // 调用方须持对应组锁（mTrackingEncoderMutex/mCtrlEncoderMutex；双组同批用 scoped_lock）
    void destroyGrayEyeEncoders() {
        for (int i = 0; i < 4; i++) {
            auto* e = grayEyeEncoder[i].load(std::memory_order_relaxed);
            if (e) { e->stop(); delete e; grayEyeEncoder[i] = nullptr; }
            if (grayEyeEncoderSurface[i]) {
                grayEyeEncoderSurface[i]->release();
                delete grayEyeEncoderSurface[i];
                grayEyeEncoderSurface[i] = nullptr;
            }
            grayEyeEncoderSurfaceReady[i].store(nullptr, std::memory_order_release);
        }
    }

    // mcap 模式判定（mMcapManager 赋值先于回调出流、清空晚于停流，回调线程只读安全）。
    bool isMcapMode() const {
        return mMcapManager && mMcapManager->isMcap();
    }

    // 灰度组资源选择助手（baseIdx 域不变量：0=tracking、2=ctrl，见 grayEyeEncoder
    // 下标注释）——收拢散落的按组三元选择，新增组资源只改此处。
    std::mutex& grayGroupMutex(int baseIdx) {
        return (baseIdx == 0) ? mTrackingEncoderMutex : mCtrlEncoderMutex;
    }
    std::unique_ptr<SXR::RecordingEncodeWorker>& grayGroupWorker(int baseIdx) {
        return (baseIdx == 0) ? mTrackingWorker : mCtrlWorker;
    }

    // 三处 mcap 入队（RGB 双眼/灰度每眼）的公共字段组装；差异字段
    // （img 数、do 标志）由调用点补齐。exposureStartBootNs 语义见 EncodeFrameItem。
    static SXR::EncodeFrameItem makeMcapFrameItem(const SXR::FrameData* data,
                                                  int64_t midExposureNs,
                                                  int64_t exposureStartBootNs,
                                                  int eyeW, int eyeH) {
        SXR::EncodeFrameItem item;
        item.ownImages = true;
        item.midExposureNs = midExposureNs;
        item.exposureStartBootNs = exposureStartBootNs;
        item.exposure = data->frames[0].exposure;
        item.gain = data->frames[0].gain;
        item.frameId = data->frames[0].frameId;
        item.eyeWidth = eyeW;
        item.eyeHeight = eyeH;
        return item;
    }

    // RGB 收养路径的等待谓词：非 mcap（SBS）路径等全部预创建完成；mcap 每眼路径
    // 只等 RGB 自己的（rgbEyePrecreateDone，存在理由见该字段注释）。
    // 调用方约束：等待期帧直接跳过（本仓库无 encoderInitBackoff，无配额消耗问题，
    // 但仍须避免预创建进行中内联创建与 precreate 线程竞态白做）。
    bool waitingOnRgbEncoderPrecreate() const {
        if (!encoderPrecreateRunning.load(std::memory_order_acquire)) return false;
        return !isMcapMode() || !rgbEyePrecreateDone.load(std::memory_order_acquire);
    }

    // 灰度侧的等待谓词：仅非 mcap 路径等预创建（mcap 每眼不依赖预创建不受挡）；
    // 非 mcap 时与 RGB 谓词等价，直接复用。
    bool waitingOnGrayEncoderPrecreate() const {
        return !isMcapMode() && waitingOnRgbEncoderPrecreate();
    }

    // Initialize encoders and surfaces (called in RGB callback with rgbCtx current)
    void initEncodersAndSurfaces(int width, int height) {
        // 与 destroyRgbEyeEncoders 互斥：守卫读取/懒创建/失败清理均触及 rgbEyeEncoder*
        std::lock_guard<std::mutex> lock(mRgbEyeEncoderMutex);
        // mcap 模式 rgbEncoder 恒 null（走每眼编码器），守卫必须同时认 rgbEyeEncoder[0]，
        // 否则懒初始化（main.cpp:1735 调用点只查 !rgbEncoder）每帧重入重复创建——
        // 目标无源项目的 encoderInitBackoff 节流，后果是每帧重建 MediaCodec
        if (rgbEncoder || rgbEyeEncoder[0]) {
            return;  // Already initialized
        }

        // Initialize encoder shader if not done
        if (encoderShaderProgram == 0) {
            initEncoderShader();
        }

        // MCAP：每眼独立编码器（宽=每眼宽，各 4Mbps，fps=readRgbFps()；组名 _ 前缀）
        if (mMcapManager && mMcapManager->isMcap()) {
            for (int i = 0; i < 2; i++) {
                const char* gn = (i == 0) ? "_rgb_l" : "_rgb_r";
                // 优先收养后台预创建（codec start 已完成，消录制首帧 66ms 尖刺）；
                // 无预创建（失败/超时/未及）则原地创建兜底。
                SXR::CameraEncoder* enc = nullptr;
                {
                    std::lock_guard<std::mutex> plock(precreateMutex);
                    enc = pendingEyeEncoder[i];
                    pendingEyeEncoder[i] = nullptr;
                }
                if (enc) {
                    LOGI("Adopting precreated %s eye encoder (codec start done off-thread)", gn);
                    adoptEyeEncoder(enc, gn, rgbCtx.display, rgbCtx.context,
                                    &rgbEyeEncoder[i], &rgbEyeEncoderSurface[i],
                                    &rgbEyeEncoderSurfaceReady[i]);
                } else {
                    auto* enc2 = new SXR::CameraEncoder(width, height, readRgbFps(), 4000000,
                                                        /*outputName*/"rgb.mp4", /*baseDir*/"");
                    enc2->setGroupName(gn);
                    startEyeEncoder(enc2, gn, rgbCtx.display, rgbCtx.context,
                                    &rgbEyeEncoder[i], &rgbEyeEncoderSurface[i],
                                    &rgbEyeEncoderSurfaceReady[i]);
                }
            }
            if (rgbEyeEncoder[0] && rgbEyeEncoder[1]) {
                LOGI("RGB per-eye encoders initialized: %dx%d", width, height);
                // S3：mcap 模式不建 SBS（原 ready 通知绑在 SBS start 成功路径），
                // 必须补发，否则录制门永不开、全路空录
                recordingGateNotifyReady(RecordingGatekeeper::Source::RGB);
            } else {
                // 单眼失败：整体置空，下帧懒初始化重试（否则失败眼永不重试、
                // RGB gate 永不 ready 静默空录）
                LOGE("per-eye RGB encoders init incomplete, cleaned up for retry");
                destroyRgbEyeEncoders();
            }
            return;  // mcap 模式不创建 SBS 编码器
        }

        int sbsWidth = width * 2;

        // 优先收养录制开始时后台预创建的编码器（codec create/start ~33ms 已在后台完成）；
        // 预创建进行中则本帧跳过等下帧；无预创建（失败/超时）原地创建兜底
        {
            std::lock_guard<std::mutex> plock(precreateMutex);
            if (pendingRgbEncoder) {
                rgbEncoder = pendingRgbEncoder;
                pendingRgbEncoder = nullptr;
                LOGI("Adopting precreated RGB encoder (codec start done off-thread)");
            }
        }
        if (!rgbEncoder) {
            // 等后台预创建（防御性复查；调用方 handleRGBFrame 已用同一谓词预过滤，
            // 正常路径不会触发）。gate 未开帧本就不编码，等待期间本帧跳过等下帧，
            // 避免预创建进行中内联创建白做 + 首帧尖刺回归。
            if (waitingOnRgbEncoderPrecreate()) {
                return;
            }
            LOGI("Initializing RGB SBS encoder with Surface mode: %dx%d", sbsWidth, height);

            // Create single SBS encoder (2W x H, 8Mbps, rgb.mp4)
            rgbEncoder = new SXR::CameraEncoder(sbsWidth, height, readRgbFps(), 8000000, "rgb.mp4", encoderBaseDir);
            rgbEncoder->setGroupName("_rgb");  // filtered from streaming
            rgbEncoder->setTimeOffset(mCameraTimeOffsetNs);
            if (!rgbEncoder->start()) {
                LOGE("RGB encoder start failed");
                delete rgbEncoder;
                rgbEncoder = nullptr;
                return;
            }
        }

        // Create encoder surface
        rgbEncoderSurface = new SXR::EncoderSurface();
        ANativeWindow* window = rgbEncoder->getInputSurface();
        if (!window || !rgbEncoderSurface->init(window, rgbCtx.display, rgbCtx.context)) {
            LOGE("RGB encoder surface init failed");
            delete rgbEncoderSurface;
            rgbEncoderSurface = nullptr;
            rgbEncoder->stop();
            delete rgbEncoder;
            rgbEncoder = nullptr;
            return;
        }
        LOGI("RGB SBS encoder initialized: %dx%d", sbsWidth, height);
        // RGB save pipeline (encoder + surface) is now running. This is the
        // recording-session "ready" signal: the RGB encoder is lazily created
        // on the first frame AFTER recording starts, so a successful init here
        // means the RGB source is up. (The frame-level swapBuffers is still
        // gated by isOpen; readiness is about the pipeline being alive.)
        recordingGateNotifyReady(RecordingGatekeeper::Source::RGB);
    }

    // Static callback for RGB camera frames
    static void onRGBFrame(void* userData, const SXR::FrameData* data) {
        auto* ext = static_cast<CameraAccessExtension*>(userData);
        if (!data || !ext) return;

        ext->inFlightCallbacks.fetch_add(1);
        if (ext->isPaused.load()) {
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        auto t0 = std::chrono::steady_clock::now();

        // Init RGB-specific GL context
        if (!ext->rgbCtx.init(ext->engine)) {
            LOGE("Failed to init RGB GL context");
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        if (!ext->rgbCtx.makeCurrent()) {
            LOGE("Failed to make RGB context current: 0x%x", eglGetError());
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        ext->handleRGBFrame(data);

        ext->rgbCtx.releaseCurrent();

        ext->inFlightCallbacks.fetch_sub(1);
        ext->callbackDrainCV.notify_all();

        auto t1 = std::chrono::steady_clock::now();
        auto durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        static std::atomic<uint32_t> sRgbFrameLogCounter{0};
        uint32_t logCount = sRgbFrameLogCounter.fetch_add(1);
        if (logCount % 300 == 0) {
            LOGI("onRGBFrame timing: total=%ld us", durUs);
        }
    }

    // Static callback for TRACKING camera frames
    static void onTrackingFrame(void* userData, const SXR::FrameData* data) {
        auto* ext = static_cast<CameraAccessExtension*>(userData);
        if (!data || !ext) return;

        ext->inFlightCallbacks.fetch_add(1);
        if (ext->isPaused.load()) {
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        auto t0 = std::chrono::steady_clock::now();

        // Init tracking-specific GL context
        if (!ext->trackingCtx.init(ext->engine)) {
            LOGE("Failed to init tracking GL context");
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        if (!ext->trackingCtx.makeCurrent()) {
            LOGE("Failed to make tracking context current: 0x%x", eglGetError());
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        ext->handleTrackingFrame(data);

        ext->trackingCtx.releaseCurrent();

        ext->inFlightCallbacks.fetch_sub(1);
        ext->callbackDrainCV.notify_all();

        auto t1 = std::chrono::steady_clock::now();
        auto durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        static std::atomic<uint32_t> sTrackFrameLogCounter{0};
        uint32_t logCount = sTrackFrameLogCounter.fetch_add(1);
        if (logCount % 300 == 0) {
            LOGI("onTrackingFrame timing: total=%ld us", durUs);
        }
    }

    // Static callback for CTRL camera frames
    static void onCtrlFrame(void* userData, const SXR::FrameData* data) {
        auto* ext = static_cast<CameraAccessExtension*>(userData);
        if (!data || !ext) return;

        ext->inFlightCallbacks.fetch_add(1);
        if (ext->isPaused.load()) {
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        auto t0 = std::chrono::steady_clock::now();

        // Init ctrl-specific GL context
        if (!ext->ctrlCtx.init(ext->engine)) {
            LOGE("Failed to init ctrl GL context");
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        if (!ext->ctrlCtx.makeCurrent()) {
            LOGE("Failed to make ctrl context current: 0x%x", eglGetError());
            ext->inFlightCallbacks.fetch_sub(1);
            ext->callbackDrainCV.notify_all();
            return;
        }

        ext->handleCtrlFrame(data);

        ext->ctrlCtx.releaseCurrent();

        ext->inFlightCallbacks.fetch_sub(1);
        ext->callbackDrainCV.notify_all();

        auto t1 = std::chrono::steady_clock::now();
        auto durUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        static std::atomic<uint32_t> sCtrlFrameLogCounter{0};
        uint32_t logCount = sCtrlFrameLogCounter.fetch_add(1);
        if (logCount % 300 == 0) {
            LOGI("onCtrlFrame timing: total=%ld us", durUs);
        }
    }

    // Handle RGB camera frame (render directly in callback, release hwBuffer immediately)
    // Caller ensures rgbCtx is current, no mutex needed
    void handleRGBFrame(const SXR::FrameData* data) {
        // Save camera params on first frame of recording session
        saveCameraParams(data, "rgb", cameraParamsSavedRgb);

        // Feed camera params to hand overlay renderer
        if (g_engine) {
            feedOverlayCameraParams(data);
        }

        // Get frame dimensions and lazy init encoders
        int frameWidth = 0, frameHeight = 0;
        if (data->hwBuffer[0]) {
            AHardwareBuffer_Desc desc;
            AHardwareBuffer_describe(data->hwBuffer[0], &desc);
            frameWidth = desc.width;
            frameHeight = desc.height;

            // Initialize display shader (needed for YUV->RGBA conversion regardless of encoding)
            if (encoderShaderProgram == 0) {
                initEncoderShader();
            }

            // Lazy init recording encoder (only when dataset recording is active)
            // 预创建进行中不进入懒初始化：等待帧跳过等下帧，避免与 precreate 线程
            // 竞态内联创建白做 + 首帧尖刺回归（源仓库此处在 encoderInitBackoff 盖章
            // 前预过滤；本仓库无 backoff，仅做防竞态早退）。
            if (!rgbEncoder && !encodersStopped.load() && !encoderBaseDir.empty()
                && !waitingOnRgbEncoderPrecreate()) {
                initEncodersAndSurfaces(frameWidth, frameHeight);
            }
        }

        // Capture snapshot flag once for this frame (both eyes must behave consistently)
        bool doSnapshot = snapshotRequested.load();

        // Save previous FBO for restoration
        GLint prevFBO;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

        // Per-frame EGLImages for each eye (buffer handles change each frame).
        // GL textures are persistent (rgbPersistentEyeTex) — created once, re-bound.
        EGLImageKHR eyeEglImage[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
        int texWidth = 0, texHeight = 0;

        // Create temporary GL textures from AHardwareBuffer for each eye
        for (int i = 0; i < 2; i++) {
            if (!data->hwBuffer[i]) {
                LOGI("RGB cam %d: hwBuffer is null, skipping", i);
                continue;
            }

            // Populate RGB frame dimensions for info panel
            rgbFrameWidths[i] = data->frames[i].width;
            rgbFrameHeights[i] = data->frames[i].height;

            // Persistent GL_TEXTURE_EXTERNAL_OES: create once, re-bind via
            // eglImageTargetTexture2DOES each frame (avoids per-frame Gen/Delete).
            if (rgbPersistentEyeTex[i] == 0) {
                glGenTextures(1, &rgbPersistentEyeTex[i]);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, rgbPersistentEyeTex[i]);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else {
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, rgbPersistentEyeTex[i]);
            }

            EGLClientBuffer clientBuffer = glext::eglGetNativeClientBufferANDROID(data->hwBuffer[i]);
            if (!clientBuffer) {
                LOGE("Failed to get EGL client buffer for camera %d", i);
                continue;
            }

            EGLint eglImageAttributes[] = {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_LINEAR,
                EGL_NONE
            };
            eyeEglImage[i] = glext::eglCreateImageKHR(rgbCtx.display, EGL_NO_CONTEXT,
                                                     EGL_NATIVE_BUFFER_ANDROID,
                                                     clientBuffer, eglImageAttributes);
            if (eyeEglImage[i] == EGL_NO_IMAGE_KHR) {
                LOGE("Failed to create EGLImage for camera %d: 0x%x", i, eglGetError());
                continue;
            }

            glext::glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)eyeEglImage[i]);

            AHardwareBuffer_Desc desc;
            AHardwareBuffer_describe(data->hwBuffer[i], &desc);
            texWidth = desc.width;
            texHeight = desc.height;
        }

        if (texWidth == 0 || texHeight == 0) {
            // No valid frames
            return;
        }

        // Setup shader state (shared across all draw calls, cached locations)
        glUseProgram(encoderShaderProgram);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(encShader_uTexture, 0);
        glBindBuffer(GL_ARRAY_BUFFER, encoderVBO);
        glEnableVertexAttribArray(encShader_aPosition);
        glVertexAttribPointer(encShader_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(encShader_aTexCoord);
        glVertexAttribPointer(encShader_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        // Determine effective mid-exposure timestamp once (used by both encoding paths).
        int64_t midExposureNs = (int64_t)(data->frames[0].timestamp + data->frames[0].exposure / 2);

        // Post timestamp to sensor-align worker (async OpenXR queries + projection).
        s_alignTs.store(midExposureNs, std::memory_order_release);
        s_alignCv.notify_one();

        // --- Encoding path: render SBS directly to encoder surface ---
        // Single render pass (like tracking/ctrl). Saves ~24.4M pixels/frame
        // of GPU bandwidth vs the old 2-pass pipeline (~1.5G px/s at 60fps).
        bool encodedDirect = false;
        // worker 化：回调只求值 gate 谓词（atomic 读）+ 组装 item + 入队，blit 由
        // rgbEncodeProcess 在 worker 线程持 mRgbEyeEncoderMutex 执行（销毁侧同锁串行化，
        // 防悬垂指针 UAF 的语义不变，仅持锁线程从回调线程变为 worker 线程）。
        // EGLImage 所有权随入队转移（ownImages=true）：队列满丢旧/处理完/退出 drain 均
        // 由 worker 侧销毁，故置 NO_IMAGE 防回调末尾双重销毁；未入队（gate 关/编码器
        // 空）时所有权不转移，回调末尾照旧销毁。
        if (isMcapMode()) {
            // 谓词读原子镜像（无锁）：回调每帧持 mRgbEyeEncoderMutex 会与 worker
            //（rgbEncodeProcess 整个 blit+swap 持锁 ~17ms）竞争，把回调阻塞在 HAL 帧
            // 管道上压垮相机 HAL（见 rgbEyeEncoderSurfaceReady 注释）。判空仅瞬时提示，
            // worker 在锁内复查真实指针。
            const bool eyeSurfacesReady =
                rgbEyeEncoderSurfaceReady[0].load(std::memory_order_acquire) != nullptr
                && rgbEyeEncoderSurfaceReady[1].load(std::memory_order_acquire) != nullptr;
            const bool mcapDo = eyeSurfacesReady
                && !encodersStopped.load() && recordingGateWithinWindow(midExposureNs);
            if (mcapDo) {
                SXR::EncodeFrameItem item = makeMcapFrameItem(
                    data, midExposureNs, (int64_t)data->frames[0].timestamp, texWidth, texHeight);
                item.img[0] = eyeEglImage[0];
                item.img[1] = eyeEglImage[1];
                item.doMcapEyes = true;
                // 与灰度两处判空对齐：worker 在 initCameras/openCameraGroups 开组前已
                // 创建+启动，正常路径非空；判空防极端时序（如 init 失败半路径）下空指针。
                if (mRgbWorker) {
                    mRgbWorker->enqueue(std::move(item));
                    eyeEglImage[0] = EGL_NO_IMAGE_KHR;
                    eyeEglImage[1] = EGL_NO_IMAGE_KHR;
                    encodedDirect = true;   // 复用后续 display-preview 跳过语义
                } else {
                    // worker 未就绪（极端时序）：所有权未转移，eyeEglImage 保持原值，
                    // 由回调末尾的兜底销毁遍历回收（置 NO_IMAGE 会每帧泄漏 2 个
                    // EGLImage）；限频告警防 _rgb_l/_r mcap 通道静默空录无迹可查
                    // （对齐灰度侧口径）。
                    static std::atomic<int> s_rgbWorkerAbsentLogCount{0};
                    if (int n = logThrottled(s_rgbWorkerAbsentLogCount, 600)) {
                        LOGW("RGB worker absent, per-eye mcap frames dropped (x%d)", n);
                    }
                }
            }
            // mcap 模式不再走 SBS 编码路径；继续下落 display 路径（未入队时 encodedDirect=false）
        } else if (rgbEncoderSurface != nullptr && !encodersStopped.load()
            && rgbEncoderSurface->makeCurrent()) {
            // NOTE: makeCurrent() switches to the encoder surface's EGLContext,
            // which shares objects (shaders, textures, VBO) with rgbCtx but
            // NOT state — must re-bind all GL state here.

            // --- Gate: discard frames until the synchronized start opens ---
            // A1: also drop frames past the end-of-recording cutoff (stopBootNs)
            // so the dataset end aligns with RGB's last frame.
            if (!recordingGateIsOpen(midExposureNs) || !recordingGateBeforeStop(midExposureNs)) {
                rgbCtx.makeCurrent();  // restore before bailing
                // fall through to preview path below (encodedDirect stays false)
            } else {
            // First frame past the gate: force IDR so the mp4 starts clean.
            // (notifyReady(RGB) already fired at encoder init; this flag now
            // only gates the one-time IDR request.)
            if (rgbEncoder && !mRgbGateNotified) {
                SXR::CameraEncoder::requestKeyFrame(rgbEncoder->getCodec(), "rgb");
                mRgbGateNotified = true;
            }
            // A2: re-anchor the (provisional) gate to this first RGB frame's
            // mid-exposure so the dataset start == RGB first frame. Idempotent.
            recordingGateRefineToRgbFirstFrame(midExposureNs);

            int sbsW = texWidth * 2;
            int sbsH = texHeight;

            glUseProgram(encoderShaderProgram);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(encShader_uTexture, 0);
            glBindBuffer(GL_ARRAY_BUFFER, encoderVBO);
            glEnableVertexAttribArray(encShader_aPosition);
            glVertexAttribPointer(encShader_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(encShader_aTexCoord);
            glVertexAttribPointer(encShader_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

            // Both eyes cover the full surface — skip clear.
            for (int i = 0; i < 2; i++) {
                if (rgbPersistentEyeTex[i] == 0) continue;
                glViewport(i * texWidth, 0, texWidth, texHeight);
                glBindTexture(GL_TEXTURE_EXTERNAL_OES, rgbPersistentEyeTex[i]);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }

            // Hand overlay projection (checked inside helper)
            renderHandOverlayToEncoder(texWidth, texHeight);
            renderControllerAxesToEncoder(texWidth, texHeight);

            rgbEncoderSurface->setPresentationTime((uint64_t)midExposureNs);
            if (rgbEncoder) {
                SXR::FrameMeta fm;
                fm.exposureStartBootNs = (int64_t)data->frames[0].timestamp;
                fm.exposure            = data->frames[0].exposure;
                fm.gain                = data->frames[0].gain;
                fm.frameId             = data->frames[0].frameId;
                fm.midExposureBootNs   = midExposureNs;
                rgbEncoder->submitFrameMeta(fm);
            }
            rgbEncoderSurface->swapBuffers();

            // Restore camera GL context
            rgbCtx.makeCurrent();
            encodedDirect = true;
            }  // end gate-open else
        }

        // --- Display preview: render each eye to its own display texture ---
        // Skip during direct encode — the headset preview update adds ~16.4M
        // pixels/frame of GPU work (2 × 2328×1748 clear+draw), enough to steal
        // bandwidth and drop frames below 60fps.
        // Exception: when a snapshot is requested mid-recording, render this one
        // frame so glReadPixels can cache pixels (snapshot capture lives here).
        // Low frequency — only frame(s) with a pending request pay the cost, so
        // the 60fps optimization is preserved outside of captures.
        if (!encodedDirect || doSnapshot) {
        for (int i = 0; i < 2; i++) {

            if (rgbDisplayTextures[i] == 0) {
                glGenTextures(1, &rgbDisplayTextures[i]);
                glBindTexture(GL_TEXTURE_2D, rgbDisplayTextures[i]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, texWidth, texHeight);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glGenFramebuffers(1, &rgbDisplayFBOs[i]);
                glBindFramebuffer(GL_FRAMEBUFFER, rgbDisplayFBOs[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, rgbDisplayTextures[i], 0);
                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    LOGE("RGB display FBO %d not complete: 0x%x", i, status);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }

            glBindFramebuffer(GL_FRAMEBUFFER, rgbDisplayFBOs[i]);
            glViewport(0, 0, texWidth, texHeight);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, rgbPersistentEyeTex[i]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // Cache pixels for snapshot
            if (doSnapshot) {
                auto& buf = (i == 0) ? snapshotRgbLeft : snapshotRgbRight;
                std::lock_guard<std::mutex> lock(buf.mutex);
                buf.width = texWidth;
                buf.height = texHeight;
                buf.pixels.resize(texWidth * texHeight * 4);
                std::vector<uint8_t> raw(texWidth * texHeight * 4);
                glReadPixels(0, 0, texWidth, texHeight, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
                int rowBytes = texWidth * 4;
                for (uint32_t y = 0; y < texHeight; y++) {
                    memcpy(buf.pixels.data() + y * rowBytes,
                           raw.data() + (texHeight - 1 - y) * rowBytes,
                           rowBytes);
                }
                buf.ready = true;
            }
        }
        } // !encodedDirect (display preview skip)

        // Restore previous FBO and cleanup
        glDisableVertexAttribArray(encShader_aPosition);
        glDisableVertexAttribArray(encShader_aTexCoord);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);

        // GL textures are persistent — don't delete.
        // Only destroy per-frame EGLImages.
        for (int i = 0; i < 2; i++) {
            if (eyeEglImage[i] != EGL_NO_IMAGE_KHR) glext::eglDestroyImageKHR(rgbCtx.display, eyeEglImage[i]);
        }

        // Mark frame ready
        rgbFrameReady = true;
        rgbFpsTrackers[0].update(data->frames[0].timestamp);
        rgbFpsTrackers[1].update(data->frames[1].timestamp);
    }

    // AHardwareBuffer → EGLImage（EGL_IMAGE_PRESERVED + sRGB colorspace），失败返回
    // EGL_NO_IMAGE_KHR。mcap 每眼 blit 入队两处共用（行为与原内联块一致）。
    static EGLImageKHR createFrameEglImage(EGLDisplay display, AHardwareBuffer* hwBuffer) {
        EGLClientBuffer clientBuffer = glext::eglGetNativeClientBufferANDROID(hwBuffer);
        if (!clientBuffer) return EGL_NO_IMAGE_KHR;
        EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                          EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_LINEAR, EGL_NONE};
        return glext::eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                        EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attrs);
    }

    // worker 专用 OES 纹理懒创建（首帧 gen + 4 个 texparam，否则仅 bind）。
    // rgb/gray 两处纹理共用同一生命周期语义（worker 线程专用，close 后名重置）。
    static void bindWorkerOesTexture(GLuint& tex) {
        if (tex == 0) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        }
    }

    // RGB mcap 每眼 blit —— rgb-enc worker 线程执行（原 handleRGBFrame mcap 分支整体
    // 迁入，逐行等价；仅纹理换为 worker 自有 rgbWorkerEyeTex 并增加 draw/swap 分项计时）。
    // 持 mRgbEyeEncoderMutex：与编码器生命周期（destroyRgbEyeEncoders/requestKeyFrame）
    // 互斥，防 null 检查与使用之间 UAF——与 worker 化前相同，仅持锁线程不同。
    void rgbEncodeProcess(SXR::EncodeFrameItem& item) {
        std::lock_guard<std::mutex> lock(mRgbEyeEncoderMutex);
        if (encodersStopped.load() ||
            !rgbEyeEncoderSurface[0] || !rgbEyeEncoderSurface[1]) {
            return;  // item.img 由 worker 按 ownImages 销毁
        }
        for (int i = 0; i < 2; i++) {
            const auto tEye = std::chrono::steady_clock::now();
            if (item.img[i] == EGL_NO_IMAGE_KHR || !rgbEyeEncoderSurface[i]) continue;
            if (!rgbEyeEncoderSurface[i]->makeCurrent()) continue;
            // NOTE: makeCurrent() 切到该 EncoderSurface 自有 EGLContext（与 rgbCtx 同
            // share group 但不共享状态），须重绑全部 GL 状态。
            glUseProgram(encoderShaderProgram);
            glActiveTexture(GL_TEXTURE0);
            glUniform1i(encShader_uTexture, 0);
            if (rgbWorkerVBO == 0) {
                glGenBuffers(1, &rgbWorkerVBO);
                rgbWorkerVboFilled = false;
            }
            glBindBuffer(GL_ARRAY_BUFFER, rgbWorkerVBO);
            {
                // 与回调侧 encoderVBO 同布局的全幅 quad（share group 内每 context 状态
                // 独立，VBO 数据一次写入后各 context 可见）。filled 标志用成员（close
                // 时随 GL 名一起重置），不用 static（context 重建后 stale）。
                // v 布局必须与 encoderVBO 一致（NDC bottom→v=0, top→v=1）：本 shader
                // 片元含 1-v Y 翻转（为 mp4 回调路径设计），quad v 再反向会负负得反，
                // mcap 每眼保存画面上下颠倒（XRCameraControl@35b0ad5 2026-09-02
                // 151857 会话实测，mp4 同 shader 同源 EGLImage 正立、灰度 blit 无翻转
                // shader 正立佐证）。
                static const GLfloat quad[] = {
                    -1.0f, -1.0f, 0.0f, 0.0f,
                     1.0f, -1.0f, 1.0f, 0.0f,
                    -1.0f,  1.0f, 0.0f, 1.0f,
                     1.0f,  1.0f, 1.0f, 1.0f,
                };
                if (!rgbWorkerVboFilled) {
                    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
                    rgbWorkerVboFilled = true;
                }
            }
            glEnableVertexAttribArray(encShader_aPosition);
            glVertexAttribPointer(encShader_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(encShader_aTexCoord);
            glVertexAttribPointer(encShader_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            bindWorkerOesTexture(rgbWorkerEyeTex[i]);
            // 绑定本帧 buffer 到 worker 自有纹理（跨 context 采样同一 gralloc buffer，
            // 与灰度 mcap blit 直采 OES 的现役模式同构）
            glext::glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, item.img[i]);
            // 每眼全幅绘制（viewport 0,0,eyeW,eyeH）
            glViewport(0, 0, item.eyeWidth, item.eyeHeight);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            const auto t1 = std::chrono::steady_clock::now();
            // 曝光中点 BOOTTIME 写入 surface presentation time：编码器输出
            // info.presentationTimeUs 即此值，McapChunkManager 据此直出 log_time
            // （ptsUs*1000+offset，与帧 1:1 配对）。
            rgbEyeEncoderSurface[i]->setPresentationTime((uint64_t)item.midExposureNs);
            rgbEyeEncoderSurface[i]->swapBuffers();
            const auto t2 = std::chrono::steady_clock::now();
            rgbWorkerDrawUsAcc += std::chrono::duration_cast<std::chrono::microseconds>(t1 - tEye).count();
            rgbWorkerSwapUsAcc += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
            if (++rgbWorkerTimingCnt % 300 == 0) {
                LOGI("rgbEncodeProcess timing: drawAvg=%llu us swapAvg=%llu us dropped=%u",
                     (unsigned long long)(rgbWorkerDrawUsAcc / rgbWorkerTimingCnt),
                     (unsigned long long)(rgbWorkerSwapUsAcc / rgbWorkerTimingCnt),
                     mRgbWorker ? mRgbWorker->droppedCount() : 0);
            }
        }
        // First frame past the gate: force IDR on BOTH eyes so each stream starts
        // clean（漏右眼则 /camera/rgb/right 开头 ≤1 GOP 的 P 帧被 mAwaitFirstIdr
        // 丢弃，双目录制起点错位）。worker 单线程写，atomic 保证主线程 reset 可见。
        if (!mRgbGateNotified.load(std::memory_order_acquire)) {
            if (auto* e = rgbEyeEncoder[0].load(std::memory_order_relaxed)) {
                SXR::CameraEncoder::requestKeyFrame(e->getCodec(), "_rgb_l");
            }
            if (auto* e = rgbEyeEncoder[1].load(std::memory_order_relaxed)) {
                SXR::CameraEncoder::requestKeyFrame(e->getCodec(), "_rgb_r");
            }
            mRgbGateNotified.store(true, std::memory_order_release);
        }
        // Re-anchor the (provisional) gate to this first RGB frame's
        // mid-exposure so the dataset start == RGB first frame. Idempotent.
        recordingGateRefineToRgbFirstFrame(item.midExposureNs);
        // item 结束解绑 current（无后续 display/preview 渲染需恢复；亦避免
        // EncoderSurface::release 销毁 context 时被 "deferred until uncurrent" 拖住）
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // Initialize grayscale encoder with Surface mode
    void initGrayscaleEncoder(SXR::CameraGroup group, int width, int height, CameraGLContext& ctx) {
        const char* groupName = (group == SXR::CameraGroup::TRACKING) ? "tracking" : "ctrl";
        int combinedWidth = width * 2;  // Side-by-side layout

        // mcap 模式跳过 SBS 编码器创建（目标无 WS 推流，SBS 灰度由每眼编码器替代；
        // 沿用源路径避免双轨 7 路 HEVC 过载）
        const bool mcapStreamOnly = (mMcapManager && mMcapManager->isMcap());
        if (!mcapStreamOnly) {
        // mcap 模式下本函数因 SBS 指针恒 null 每帧重入（幂等守卫兜底），SBS 初始化
        // 日志挪进此分支避免每帧刷屏 + 误报 SBS 宽度；mcap 初始化日志见下方
        // "Grayscale per-eye encoders initialized"（仅成功时一条）。
        LOGI("Initializing grayscale encoder for %s: %dx%d @ 60fps (Surface mode)",
             groupName, combinedWidth, height);

        // 优先收养录制开始时后台线程预创建的编码器（codec create/configure/start
        // ~36ms 已在后台完成）；预创建进行中则本帧跳过等下帧（gate 未开，帧本就不编码，
        // 且 libcamera 侧补帧保证不丢）；无预创建（失败/超时）则原地创建兜底
        const int giAdopt = (group == SXR::CameraGroup::TRACKING) ? 0 : 1;
        SXR::CameraEncoder* encoder = nullptr;
        {
            std::lock_guard<std::mutex> lock(precreateMutex);
            encoder = pendingGrayEncoder[giAdopt];
            pendingGrayEncoder[giAdopt] = nullptr;
        }
        if (encoder) {
            LOGI("Adopting precreated %s encoder (codec start done off-thread)", groupName);
        } else {
            // 等后台预创建（防御性复查；handleCVFrame 已预过滤，mcap 每眼不受挡）
            if (waitingOnGrayEncoderPrecreate()) {
                return;
            }
            encoder = new SXR::CameraEncoder(groupName, combinedWidth, height,
                                             60, SXR::EncoderMode::SURFACE, encoderBaseDir);
            encoder->setTimeOffset(mCameraTimeOffsetNs);
            if (!encoder->start()) {
                LOGE("Failed to start grayscale encoder for %s", groupName);
                delete encoder;
                return;
            }
        }

        // Create EncoderSurface
        auto* surface = new SXR::EncoderSurface();
        if (!surface->init(encoder->getInputSurface(), ctx.display, ctx.context)) {
            LOGE("Failed to initialize EncoderSurface for %s", groupName);
            surface->release();
            delete surface;
            encoder->stop();
            delete encoder;
            return;
        }

        // Make the encoder surface's context current to create texture
        if (!surface->makeCurrent()) {
            LOGE("Failed to make encoder surface current for %s", groupName);
            surface->release();
            delete surface;
            encoder->stop();
            delete encoder;
            return;
        }

        // Create texture for this encoder (each encoder has its own texture)
        // NOTE: Grayscale camera buffers are actually YUV format, not R8!
        // Use GL_TEXTURE_EXTERNAL_OES for YUV camera buffers - this is the standard way on Android
        // The GPU will automatically handle YUV->RGB conversion when sampling
        GLuint y8Texture;
        glGenTextures(1, &y8Texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, y8Texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        LOGI("Created Y8 texture %u for %s encoder", y8Texture, groupName);

        // Store references
        if (group == SXR::CameraGroup::TRACKING) {
            trackingEncoder = encoder;
            trackingEncoderSurface = surface;
            trackingY8Texture = y8Texture;
        } else {
            ctrlEncoder = encoder;
            ctrlEncoderSurface = surface;
            ctrlY8Texture = y8Texture;
        }

        LOGI("Grayscale encoder initialized for %s with Y8 texture %u", groupName, y8Texture);
        }

        if (mcapStreamOnly) {
            // 幂等守卫（S2）：调用点只查 !*targetEncoder（main.cpp:2166），mcap 模式
            // 恒 null 每帧重入；锁内防并发双份创建。单组只持自组锁（原单把
            // mGrayscaleEncoderMutex 拆分后 tracking/ctrl worker 各持自组锁并行）。
            const int baseIdx = (group == SXR::CameraGroup::TRACKING) ? 0 : 2;
            std::lock_guard<std::mutex> lock(grayGroupMutex(baseIdx));
            if (grayEyeEncoder[baseIdx] || grayEyeEncoder[baseIdx + 1]) return;
            for (int i = 0; i < 2; i++) {
                std::string gn = std::string("_") + groupName + (i == 0 ? "_l" : "_r");
                // 优先收养后台预创建（同 RGB 每眼；无则内联兜底）。槽下标 2+baseIdx+i：
                // baseIdx=0(tracking)→槽2/3，baseIdx=2(ctrl)→槽4/5。
                SXR::CameraEncoder* enc = nullptr;
                {
                    std::lock_guard<std::mutex> plock(precreateMutex);
                    enc = pendingEyeEncoder[2 + baseIdx + i];
                    pendingEyeEncoder[2 + baseIdx + i] = nullptr;
                }
                if (enc) {
                    LOGI("Adopting precreated %s eye encoder (codec start done off-thread)", gn.c_str());
                    adoptEyeEncoder(enc, gn.c_str(), ctx.display, ctx.context,
                                    &grayEyeEncoder[baseIdx + i], &grayEyeEncoderSurface[baseIdx + i],
                                    &grayEyeEncoderSurfaceReady[baseIdx + i]);
                } else {
                    auto* enc2 = new SXR::CameraEncoder(gn, width, height, 60,
                                                        SXR::EncoderMode::SURFACE, /*baseDir*/"");
                    startEyeEncoder(enc2, gn.c_str(), ctx.display, ctx.context,
                                    &grayEyeEncoder[baseIdx + i], &grayEyeEncoderSurface[baseIdx + i],
                                    &grayEyeEncoderSurfaceReady[baseIdx + i]);
                }
            }
            if (grayEyeEncoder[baseIdx] && grayEyeEncoder[baseIdx + 1]) {
                LOGI("Grayscale per-eye encoders initialized for %s: %dx%d", groupName, width, height);
            }
            // 裁决（对齐源项目）：单眼失败不做销毁重试，照落公共尾发 gate ready——
            // 失败路径提前 return 会让下帧幂等守卫恒命中已成功眼、失败眼永不重试、
            // 录制门永停 ARMING；失败眼 mcap 通道为空属可接受降级，由 spec §7
            // 左右眼帧数校验发现
        }
        // 公共尾（S3）：两种模式都在此发 gate ready
        // This group's save pipeline (encoder + surface + texture) is now
        // running — the recording-session "ready" signal. (Frame-level
        // renderGrayscaleToEncoder is still gated by isOpen.)
        recordingGateNotifyReady(group == SXR::CameraGroup::TRACKING
                ? RecordingGatekeeper::Source::TRACKING
                : RecordingGatekeeper::Source::CTRL);
    }

    // Render grayscale Y8 buffer to encoder surface using hardware acceleration
    // Render grayscale buffer to encoder surface
    void renderGrayscaleToEncoder(SXR::EncoderSurface* surface, GLuint y8Texture,
                                     AHardwareBuffer* hwBuffer,
                                     int width, int height, EGLDisplay display,
                                     int64_t timestampNs) {
        if (!surface || !hwBuffer || !grayscaleEncoderShaderProgram || !y8Texture) {
            return;
        }

        // Save current EGL context
        EGLDisplay prevDisplay = eglGetCurrentDisplay();
        EGLSurface prevDraw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface prevRead = eglGetCurrentSurface(EGL_READ);
        EGLContext prevContext = eglGetCurrentContext();

        // Make encoder surface current
        if (!surface->makeCurrent()) {
            LOGE("renderGrayscaleToEncoder: failed to make current");
            return;
        }

        // Bind AHardwareBuffer to texture via EGLImage
        EGLClientBuffer clientBuffer = glext::eglGetNativeClientBufferANDROID(hwBuffer);
        if (!clientBuffer) {
            if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
                eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
            }
            return;
        }

        EGLint attrs[] = {
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
            EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_LINEAR,
            EGL_NONE
        };
        EGLImageKHR eglImage = glext::eglCreateImageKHR(
            display, EGL_NO_CONTEXT,
            EGL_NATIVE_BUFFER_ANDROID,
            clientBuffer, attrs);

        if (eglImage == EGL_NO_IMAGE_KHR) {
            if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
                eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
            }
            return;
        }

        glBindTexture(GL_TEXTURE_EXTERNAL_OES, y8Texture);
        glext::glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, eglImage);

        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(grayscaleEncoderShaderProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, y8Texture);
        glUniform1i(grayShader_uGrayscaleTexture, 0);

        glBindBuffer(GL_ARRAY_BUFFER, grayscaleEncoderVBO);
        glEnableVertexAttribArray(grayShader_aPosition);
        glVertexAttribPointer(grayShader_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(grayShader_aTexCoord);
        glVertexAttribPointer(grayShader_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(grayShader_aPosition);
        glDisableVertexAttribArray(grayShader_aTexCoord);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glext::eglDestroyImageKHR(display, eglImage);
        surface->setPresentationTime(timestampNs);
        surface->swapBuffers();

        // Restore previous EGL context
        if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
        }
    }

    // Handle TRACKING camera frame — called from onTrackingFrame with trackingCtx current
    void handleTrackingFrame(const SXR::FrameData* data) {
        cvDropMonitors[0].onFrame(data);
        saveCameraParams(data, "tracking", cameraParamsSavedTracking);
        handleCVFrame(data, SXR::CameraGroup::TRACKING, trackingCtx, 0,
                      &trackingEncoder, trackingEncoderSurface, trackingY8Texture);
        trackingFrameReady = true;
        cvFpsTrackers[0].update(data->frames[0].timestamp);
        cvFpsTrackers[1].update(data->frames[1].timestamp);
    }

    // Handle CTRL camera frame — called from onCtrlFrame with ctrlCtx current
    void handleCtrlFrame(const SXR::FrameData* data) {
        cvDropMonitors[1].onFrame(data);
        saveCameraParams(data, "ctrl", cameraParamsSavedCtrl);
        handleCVFrame(data, SXR::CameraGroup::CTRL, ctrlCtx, 2,
                      &ctrlEncoder, ctrlEncoderSurface, ctrlY8Texture);
        ctrlFrameReady = true;
        cvFpsTrackers[2].update(data->frames[0].timestamp);
        cvFpsTrackers[3].update(data->frames[1].timestamp);
    }

    // Common CV frame handler for TRACKING and CTRL
    void handleCVFrame(const SXR::FrameData* data, SXR::CameraGroup group,
                       CameraGLContext& ctx, int baseIdx,
                       SXR::CameraEncoder** targetEncoder,
                       SXR::EncoderSurface* targetSurface,
                       GLuint targetY8Texture) {
        // Group-local index: 0=tracking, 1=ctrl — used to isolate per-group EGL resources
        const int gi = baseIdx / 2;

        // Initialize encoder shader if needed (mutex-protected: called from tracking + ctrl threads)
        if (encoderShaderProgram == 0) {
            std::lock_guard<std::mutex> lock(shaderInitMutex);
            if (encoderShaderProgram == 0) {
                initEncoderShader();
            }
        }
        if (encoderShaderProgram == 0) return;  // shader init failed

        uint32_t width = data->frames[0].width;
        uint32_t height = data->frames[0].height;

        // Lazy initialize encoder on first frame（等待期不消耗预创建窗口，同 RGB 侧）
        if (!*targetEncoder && !encodersStopped.load() &&
            !waitingOnGrayEncoderPrecreate()) {
            initGrayscaleEncoder(group, width, height, ctx);
        }

        if (!data->hwBuffer[0]) return;

        // Lazy create VBO for CV rendering (per-group to avoid cross-thread glBufferSubData)
        if (cvDisplayVBOs[gi] == 0) {
            glGenBuffers(1, &cvDisplayVBOs[gi]);
            glBindBuffer(GL_ARRAY_BUFFER, cvDisplayVBOs[gi]);
            float defaultVerts[] = {
                -1.0f, -1.0f,   0.0f, 1.0f,
                 1.0f, -1.0f,   1.0f, 1.0f,
                -1.0f,  1.0f,   0.0f, 0.0f,
                 1.0f,  1.0f,   1.0f, 0.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(defaultVerts), defaultVerts, GL_DYNAMIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        // Initialize grayscale display shader if needed (mutex-protected: called from tracking + ctrl threads)
        if (grayscaleEncoderShaderProgram == 0) {
            std::lock_guard<std::mutex> lock(shaderInitMutex);
            if (grayscaleEncoderShaderProgram == 0) {
                initGrayscaleEncoderShader();
            }
        }
        if (grayscaleEncoderShaderProgram == 0) return;  // shader init failed

        // Reuse persistent external OES texture — create once per group, rebind per frame
        if (cvPersistentExtTex[gi] == 0) {
            glGenTextures(1, &cvPersistentExtTex[gi]);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, cvPersistentExtTex[gi]);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        // Destroy previous EGLImage and create new one for this frame's hwBuffer
        if (cvPersistentEGLImage[gi] != EGL_NO_IMAGE_KHR) {
            glext::eglDestroyImageKHR(ctx.display, cvPersistentEGLImage[gi]);
            cvPersistentEGLImage[gi] = EGL_NO_IMAGE_KHR;
        }

        EGLClientBuffer clientBuffer = glext::eglGetNativeClientBufferANDROID(data->hwBuffer[0]);
        EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_GL_COLORSPACE, EGL_GL_COLORSPACE_LINEAR, EGL_NONE};
        cvPersistentEGLImage[gi] = glext::eglCreateImageKHR(ctx.display, EGL_NO_CONTEXT,
                                                         EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attrs);
        if (cvPersistentEGLImage[gi] == EGL_NO_IMAGE_KHR) {
            LOGE("CV frame: failed to create EGLImage: 0x%x", eglGetError());
            return;
        }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, cvPersistentExtTex[gi]);
        glext::glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)cvPersistentEGLImage[gi]);

        // Render left half and right half to separate display textures
        glUseProgram(grayscaleEncoderShaderProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, cvPersistentExtTex[gi]);
        glUniform1i(grayShader_uGrayscaleTexture, 0);

        for (int i = 0; i < 2; i++) {
            int idx = baseIdx + i;

            // Lazy create display texture + FBO
            if (cvDisplayTextures[idx] == 0 ||
                cvFrameWidths[idx] != width || cvFrameHeights[idx] != height) {
                if (cvDisplayTextures[idx] != 0) {
                    glDeleteTextures(1, &cvDisplayTextures[idx]);
                }
                if (cvDisplayFBOs[idx] != 0) {
                    glDeleteFramebuffers(1, &cvDisplayFBOs[idx]);
                }
                glGenTextures(1, &cvDisplayTextures[idx]);
                glBindTexture(GL_TEXTURE_2D, cvDisplayTextures[idx]);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glGenFramebuffers(1, &cvDisplayFBOs[idx]);
                glBindFramebuffer(GL_FRAMEBUFFER, cvDisplayFBOs[idx]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, cvDisplayTextures[idx], 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                cvFrameWidths[idx] = width;
                cvFrameHeights[idx] = height;
            }

            // Render left half (i=0: U=0..0.5) or right half (i=1: U=0.5..1.0)
            GLint prevFBO;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
            glBindFramebuffer(GL_FRAMEBUFFER, cvDisplayFBOs[idx]);
            glViewport(0, 0, width, height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            float uMin = (i == 0) ? 0.0f : 0.5f;
            float uMax = (i == 0) ? 0.5f : 1.0f;
            float verts[] = {
                -1.0f, -1.0f,  uMin, 1.0f,
                 1.0f, -1.0f,  uMax, 1.0f,
                -1.0f,  1.0f,  uMin, 0.0f,
                 1.0f,  1.0f,  uMax, 0.0f,
            };
            glBindBuffer(GL_ARRAY_BUFFER, cvDisplayVBOs[gi]);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

            glEnableVertexAttribArray(grayShader_aPosition);
            glVertexAttribPointer(grayShader_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(grayShader_aTexCoord);
            glVertexAttribPointer(grayShader_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisableVertexAttribArray(grayShader_aPosition);
            glDisableVertexAttribArray(grayShader_aTexCoord);

            glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
            cvLastUploadedFrameId[idx] = data->frames[i].frameId;
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Cache pixels for snapshot if requested (read in this GL context to avoid cross-thread GL access)
        if (snapshotRequested.load()) {
            for (int i = 0; i < 2; i++) {
                int idx = baseIdx + i;
                auto& buf = snapshotCv[idx];
                glBindFramebuffer(GL_FRAMEBUFFER, cvDisplayFBOs[idx]);
                std::lock_guard<std::mutex> lock(buf.mutex);
                buf.width = width;
                buf.height = height;
                buf.pixels.resize(width * height * 4);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buf.pixels.data());
                buf.ready = true;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // MCAP 灰度每眼 blit：专用 grayEyeBlitShaderProgram 采样独立 EGLImage，
        // 按眼切 uv；不走 cvDisplayTextures（跨 context 无同步读陈旧帧的实测坑）；
        // 不 submitFrameMeta。
        // MCAP 每眼 blit worker 化：回调只做 gate 谓词求值（onGateReject 保持回调时刻
        // 语义）+ shader 懒初始化 + item 入队；blit（makeCurrent/draw/setPts/swap）由
        // grayEncodeProcess 在自组 worker 线程持组锁执行。每眼独立编码器帧经 listener →
        // McapChunkManager 写 mcap，不 submitFrameMeta（ptsUs 直出，同 RGB 分支）。
        // 灰度帧为单 SBS buffer（hwBuffer[0]），blit 半幅切用同一 buffer；
        // item 携带独立 EGLImage（ownImages=true，worker 处理完销毁）——不复用回调
        // persistent EGLImage，规避"回调下一帧 destroy 而 worker 队列仍引用"的跨帧陷阱。
        if (isMcapMode()) {
            int64_t midExpNs = (int64_t)(data->frames[0].timestamp + data->frames[0].exposure / 2);
            const bool grayGateOk = recordingGateWithinWindow(midExpNs);
            if (!grayGateOk) cvDropMonitors[gi].onGateReject();
            // 谓词读原子镜像（无锁，同 RGB 分支）：避免回调每帧持自组锁与 worker
            //（grayEncodeProcess 整个 blit+swap 持锁）竞争阻塞相机回调压垮 HAL。
            const bool eyeSurfacesReady =
                grayEyeEncoderSurfaceReady[baseIdx].load(std::memory_order_acquire) != nullptr
                && grayEyeEncoderSurfaceReady[baseIdx + 1].load(std::memory_order_acquire) != nullptr;
            const bool eyeOk = eyeSurfacesReady && !stopInProgress.load();
            auto& worker = grayGroupWorker(baseIdx);
            if (eyeOk && grayEyeBlitShaderProgram == 0) {
                // sampler2D 直通 blit shader 懒初始化（mutex 保护：tracking/ctrl 双线程）
                // —— shader program 为 share group 共享对象，回调建好 worker 直接用。
                // 不置于 gateOk 之内：ARMING 期即预热，shader 编译不落录制首帧。
                std::lock_guard<std::mutex> lock(shaderInitMutex);
                if (grayEyeBlitShaderProgram == 0) {
                    initGrayEyeBlitShader();
                }
            }
            if (eyeOk && grayGateOk) {
                if (grayEyeBlitShaderProgram != 0 && worker) {
                    // item 携带独立 EGLImage（ownImages=true）：worker 持有引用保证
                    // 重绑时句柄必然存活；销毁由 worker 延迟 8 帧执行（等 GPU 命令流
                    // 完成后 buffer 才回 HAL）。不复用 persistent：回调每帧 destroy
                    // 旧 persistent，worker 队列在途帧对已销毁句柄重绑 = UB（实测
                    // 灰度 worker 处理 ~150 帧后 swapBuffers 死等 GPU fence）。
                    EGLImageKHR img = createFrameEglImage(ctx.display, data->hwBuffer[0]);
                    if (img != EGL_NO_IMAGE_KHR) {
                        // 每眼宽高（非 SBS combinedWidth）
                        SXR::EncodeFrameItem item = makeMcapFrameItem(
                            data, midExpNs, (int64_t)data->frames[0].timestamp, width, height);
                        item.img[0] = img;
                        item.doMcapEyes = true;
                        worker->enqueue(std::move(item));
                    } else {
                        // EGLImage 创建失败：限频告警（同下方 blit 不可用分支口径），
                        // 防 _tracking_*/_ctrl_* mcap 通道静默丢帧无迹可查。
                        static std::atomic<int> s_blitImgFailLogCount{0};
                        if (int n = logThrottled(s_blitImgFailLogCount, 600)) {
                            LOGW("gray eye blit EGLImage create failed, per-eye mcap frames dropped (x%d)", n);
                        }
                    }
                } else if (grayEyeBlitShaderProgram == 0 || !worker) {
                    // blit shader 初始化失败 / worker 未就绪：限频告警让
                    // _tracking_*/_ctrl_* mcap 通道空录可事后发现（tracking/ctrl 双线程并发）。
                    static std::atomic<int> s_blitFailLogCount{0};
                    if (int n = logThrottled(s_blitFailLogCount, 600)) {
                        LOGW("gray eye blit unavailable (shader=%d worker=%d), per-eye mcap frames dropped (x%d)",
                             grayEyeBlitShaderProgram != 0 ? 1 : 0, worker ? 1 : 0, n);
                    }
                }
            }
        }

        // Render to encoder surface (for recording)
        // NOTE: This does context switch to encoder surface and back — main overhead
        // mcap 模式跳过 SBS 编码渲染（每眼编码器已替代；targetSurface 恒 null 双保险）
        if (!isMcapMode() && targetSurface && *targetEncoder && targetY8Texture && !stopInProgress.load()) {
            int64_t midExpNs = (int64_t)(data->frames[0].timestamp + data->frames[0].exposure / 2);
            if (!recordingGateIsOpen(midExpNs) || !recordingGateBeforeStop(midExpNs)) {
                // gate 非 IDLE（录制启动中/进行中/收尾）的拒绝才计数：
                // 起止边缘属正常，录制中段（OPEN）出现即编码端丢帧
                if (recordingGateActive()) cvDropMonitors[gi].onGateReject();
                return;  // gate not open yet (or past end cutoff): drop before touching encoder
            }
            bool& notified = (group == SXR::CameraGroup::TRACKING) ? mTrackingGateNotified
                                                                   : mCtrlGateNotified;
            // First frame past the gate: force IDR (notifyReady already fired at
            // encoder init; this flag now only gates the one-time IDR request).
            if (!notified) {
                SXR::CameraEncoder::requestKeyFrame((*targetEncoder)->getCodec(),
                                                    (*targetEncoder)->getGroupName());
                notified = true;
            }
            int64_t frameTimestampNs = data->frames[0].timestamp;
            SXR::FrameMeta fm;
            fm.exposureStartBootNs = (int64_t)data->frames[0].timestamp;
            fm.exposure            = data->frames[0].exposure;
            fm.gain                = data->frames[0].gain;
            fm.frameId             = data->frames[0].frameId;
            fm.midExposureBootNs   = (int64_t)(data->frames[0].timestamp + data->frames[0].exposure / 2);
            (*targetEncoder)->submitFrameMeta(fm);
            renderGrayscaleToEncoder(targetSurface, targetY8Texture,
                                     data->hwBuffer[0],
                                     width * 2, height, ctx.display,
                                     frameTimestampNs);
        }
    }

    // 灰度（tracking/ctrl）mcap 每眼 blit —— 自组 worker 线程执行
    // （原 handleCVFrame mcap blit 逻辑迁入）。
    // 持自组锁（mTrackingEncoderMutex/mCtrlEncoderMutex）：与编码器生命周期（destroy/
    // requestKeyFrame）互斥，语义与 worker 化前相同，仅持锁线程不同；两组 worker 各持
    // 各锁独立并行（原单 VBO 跨组 mutate 竞态随自有 VBO 消除）。
    void grayEncodeProcess(SXR::EncodeFrameItem& item, const int baseIdx) {
        const int gi = baseIdx / 2;  // 组序：0=tracking、1=ctrl（计时数组下标）
        std::lock_guard<std::mutex> lock(grayGroupMutex(baseIdx));
        if (stopInProgress.load()) return;

        if (item.doMcapEyes) {
            if (!grayEyeEncoderSurface[baseIdx] || !grayEyeEncoderSurface[baseIdx + 1]) return;
            for (int i = 0; i < 2; i++) {
                const auto tEye = std::chrono::steady_clock::now();
                auto* surf = grayEyeEncoderSurface[baseIdx + i];
                if (surf == nullptr) continue;
                if (!surf->makeCurrent()) continue;
                // NOTE: makeCurrent() 切到每眼 EncoderSurface 的 EGLContext——与 ctx
                // 共享对象（shader/texture/VBO）但不共享状态，须重绑全部 GL 状态。
                // 绑定本帧 EGLImage 到 worker 自有纹理（跨 context 内容新鲜，与原
                // 回调直采 OES 模式同构），按眼切分 SBS 左右半幅。
                bindWorkerOesTexture(grayWorkerEyeTex[gi]);
                glext::glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, item.img[0]);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, item.eyeWidth, item.eyeHeight);
                glUseProgram(grayEyeBlitShaderProgram);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(grayEyeBlit_uTexture, 0);
                const float uMin = (i == 0) ? 0.0f : 0.5f;
                const float uMax = (i == 0) ? 0.5f : 1.0f;
                float verts[] = {
                    -1.0f, -1.0f,  uMin, 1.0f,
                     1.0f, -1.0f,  uMax, 1.0f,
                    -1.0f,  1.0f,  uMin, 0.0f,
                     1.0f,  1.0f,  uMax, 0.0f,
                };
                if (grayWorkerVBO[gi] == 0) {
                    glGenBuffers(1, &grayWorkerVBO[gi]);
                    grayWorkerVboFilled[gi] = false;
                }
                glBindBuffer(GL_ARRAY_BUFFER, grayWorkerVBO[gi]);
                // 首次必须 glBufferData 分配数据存储，否则 glBufferSubData 报
                // GL_INVALID_VALUE、顶点数据从未上传（灰度每眼 mcap 通道全黑/垃圾）。
                // 对齐 rgbWorkerVBO（rgbWorkerVboFilled）的一次性分配写法。
                if (!grayWorkerVboFilled[gi]) {
                    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), nullptr, GL_DYNAMIC_DRAW);
                    grayWorkerVboFilled[gi] = true;
                }
                glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
                glEnableVertexAttribArray(grayEyeBlit_aPosition);
                glVertexAttribPointer(grayEyeBlit_aPosition, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                glEnableVertexAttribArray(grayEyeBlit_aTexCoord);
                glVertexAttribPointer(grayEyeBlit_aTexCoord, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                glDisableVertexAttribArray(grayEyeBlit_aPosition);
                glDisableVertexAttribArray(grayEyeBlit_aTexCoord);
                // 曝光中点 BOOTTIME 写入 surface presentation time：编码器输出
                // info.presentationTimeUs 即此值，McapChunkManager 据此直出 log_time。
                surf->setPresentationTime((uint64_t)item.midExposureNs);
                surf->swapBuffers();
                const auto tDraw = std::chrono::steady_clock::now();
                grayWorkerDrawUsAcc[gi] += std::chrono::duration_cast<std::chrono::microseconds>(tDraw - tEye).count();
                if (++grayWorkerTimingCnt[gi] % 300 == 0) {
                    auto* w = grayGroupWorker(baseIdx).get();
                    LOGI("grayEncodeProcess[%s] timing: drawAvg=%llu us dropped=%u",
                         baseIdx == 0 ? "tracking" : "ctrl",
                         (unsigned long long)(grayWorkerDrawUsAcc[gi] / grayWorkerTimingCnt[gi]),
                         w ? w->droppedCount() : 0);
                }
            }
            // 首帧过 gate：强制 IDR，mcap 通道从干净关键帧开始（独立于 mp4 SBS 分支的
            // mTrackingGateNotified/mCtrlGateNotified——两组编码器各自需一次 IDR）。
            bool& eyeNotified = mGrayEyeGateNotified[gi];
            if (auto* e = grayEyeEncoder[baseIdx].load(std::memory_order_relaxed); !eyeNotified && e) {
                SXR::CameraEncoder::requestKeyFrame(e->getCodec(), e->getGroupName());
                if (auto* e2 = grayEyeEncoder[baseIdx + 1].load(std::memory_order_relaxed)) {
                    SXR::CameraEncoder::requestKeyFrame(e2->getCodec(), e2->getGroupName());
                }
                eyeNotified = true;
            }
        }
        // item 结束解绑 current（无后续 display/preview 渲染需恢复；亦避免
        // EncoderSurface::release 销毁 context 时被 "deferred until uncurrent" 拖住）
        eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    // Initialize cameras using dynamic loading API
    bool initCameras(JavaVM* jvm, jobject activity) {
        vm = jvm;
        activityObject = activity;

        cvDropMonitors[0].name = "tracking";
        cvDropMonitors[1].name = "ctrl";

        // Initialize the API table by loading the library dynamically
        if (sxr_camera_api_init(&api, NULL) != 0) {
            LOGE("Failed to load sxr_camera library (libsxr_camera_client.so)");
            return false;
        }

        // Verify API is valid
        if (!sxr_camera_api_is_valid(&api)) {
            LOGE("SxrCamera API is not valid after initialization");
            sxr_camera_api_deinit(&api);
            return false;
        }

        cameraContext = sxr_camera_create(&api, vm, activityObject);
        if (!cameraContext) {
            LOGE("Failed to create SxrCameraContext");
            sxr_camera_api_deinit(&api);
            return false;
        }

        // Set extrinsics convention to OpenCV before opening camera groups.
        sxr_camera_set_extrinsics_convention(&api, 1);
        LOGI("Set extrinsics convention to OpenCV (type=1)");

        // Open RGB camera group（录制编码 worker 须先就绪，保证首帧到达时已在跑）
        ensureRecordingWorkers();
        int ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::RGB,
                                        onRGBFrame, this);
        if (ret != 0) {
            LOGE("Failed to open RGB camera group: %d", ret);
        } else {
            LOGI("RGB camera group opened successfully");
        }

        // Open TRACKING camera group
        ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::TRACKING,
                                    onTrackingFrame, this);
        if (ret != 0) {
            LOGE("Failed to open TRACKING camera group: %d", ret);
        } else {
            LOGI("TRACKING camera group opened successfully");
        }

        // Open CTRL camera group
        ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::CTRL,
                                    onCtrlFrame, this);
        if (ret != 0) {
            LOGE("Failed to open CTRL camera group: %d", ret);
        } else {
            LOGI("CTRL camera group opened successfully");
        }

        // Initialize legacy grayscale encoders (not used, kept for compatibility)
        for (int i = 0; i < SXR::CAME_MAX; i++) {
            grayCameraEncoders[i] = nullptr;
        }

        // RGB/Tracking/CTRL encoders will be initialized lazily when first frame arrives
        // This allows us to get actual frame dimensions from AHardwareBuffer

        camerasInitialized = true;
        cameraGroupsOpen = true;
        LOGI("Cameras initialized successfully with dynamic loading API");
        return true;
    }

    void Update() {
        // Consider app active only when both:
        // 1. Android lifecycle says Resumed (standard Android backgrounding)
        // 2. OpenXR session is FOCUSED (XR runtime focus management)
        // On some XR devices, switching apps does NOT trigger Android lifecycle
        // events, so we must also check the OpenXR session state.
        bool appActive = engine->state.Resumed &&
                         engine->sessionState == XR_SESSION_STATE_FOCUSED;
        if (appActive) {
            resume();
        } else {
            pause();
        }
    }

    // Pause camera processing and encoding (called when app goes to background)
    void pause() {
        bool wasPaused = isPaused.exchange(true);
        if (!wasPaused) {
            LOGI("CameraAccessExtension: pausing...");

            // Close camera groups FIRST to stop new callbacks from being triggered
            closeCameraGroups();

            // Wait for in-flight callbacks to drain (with timeout)
            {
                std::unique_lock<std::mutex> lk(callbackDrainMutex);
                callbackDrainCV.wait_for(lk, std::chrono::milliseconds(500),
                    [this] { return inFlightCallbacks.load() == 0; });
                if (inFlightCallbacks.load() > 0) {
                    LOGW("pause: %d callbacks still in flight after timeout",
                         inFlightCallbacks.load());
                }
            }

            // 回调已 drain：删除 worker 自有 GL 对象（pause/resume 会话级，
            // share group 长存不级联释放，见 deleteWorkerGlObjects 注释）
            deleteWorkerGlObjects();

            // Mark encoders unavailable but KEEP the instances alive. Stopping
            // them here would force resume() to rebuild them, which truncates
            // the in-progress mp4s and restarts video PTS at 0 mid-recording —
            // breaking the synchronized dataset timeline. Idle MediaCodec
            // instances are cheap: the output loop just polls on its 50ms
            // dequeue timeout until frames resume.
            encodersStopped = true;
            LOGI("CameraAccessExtension: paused, cameras closed");
        }
    }

    // Resume camera processing and encoding (called when app comes to foreground)
    void resume() {
        bool wasPaused = isPaused.exchange(false);
        if (wasPaused) {
            LOGI("CameraAccessExtension: resuming...");
            // Fully destroy + recreate the SxrCameraContext before reopening.
            // pause() already closed the groups (stopping the camera streams, so
            // no camera resources are held while the screen is off), but the QVR
            // client session (qvrHelper + cameraClient) inside the context does
            // NOT reset on a simple close_group+open_group: after a detach+
            // re-attach, grayscale (tracking/ctrl) groups report "started" yet
            // never deliver a frame, while RGB reopens fine. Recreating the whole
            // context (as on a fresh app launch, where grayscale works) is the
            // only way to reset that session. sxr_camera_destroy frees the
            // context but leaves the dlopen'd api loaded, so it can be reused.
            if (cameraContext) {
                sxr_camera_destroy(&api, cameraContext);
                cameraContext = nullptr;
            }
            cameraContext = sxr_camera_create(&api, vm, activityObject);
            if (!cameraContext) {
                LOGE("resume: sxr_camera_create failed; cameras unavailable until restart");
            }
            // Re-open camera groups on the fresh context
            openCameraGroups();
            // Restart encoders
            startEncoders();
            LOGI("CameraAccessExtension: resumed, context recreated");
        }
    }

    // worker 自有 GL 对象显式删除（对齐 cleanupAllGLContexts 对各 GL 对象的显式
    // glDelete 惯例）：相机 context 均以 engine->context 为 share context，share group
    // 长存——只清名不删则每个 pause/resume 会话泄漏一批纹理/VBO（下会话重新 glGen）。
    // 必须在相机回调 drain 之后调用（在途回调可能正持这些 context current）。
    void deleteWorkerGlObjects() {
        EGLDisplay prevDisplay = eglGetCurrentDisplay();
        EGLSurface prevDraw = eglGetCurrentSurface(EGL_DRAW);
        EGLSurface prevRead = eglGetCurrentSurface(EGL_READ);
        EGLContext prevContext = eglGetCurrentContext();
        if (rgbCtx.initialized && rgbCtx.makeCurrent()) {
            for (int i = 0; i < 2; i++) {
                if (rgbWorkerEyeTex[i] != 0) { glDeleteTextures(1, &rgbWorkerEyeTex[i]); rgbWorkerEyeTex[i] = 0; }
            }
            if (rgbWorkerVBO != 0) { glDeleteBuffers(1, &rgbWorkerVBO); rgbWorkerVBO = 0; }
            rgbWorkerVboFilled = false;
        }
        // tracking/ctrl 各为独立 context（互不在对方线程 current，drain 后均空闲）
        CameraGLContext* grayCtx[2] = { &trackingCtx, &ctrlCtx };
        for (int gi = 0; gi < 2; gi++) {
            if (grayCtx[gi]->initialized && grayCtx[gi]->makeCurrent()) {
                if (grayWorkerEyeTex[gi] != 0) { glDeleteTextures(1, &grayWorkerEyeTex[gi]); grayWorkerEyeTex[gi] = 0; }
                if (grayWorkerVBO[gi] != 0) { glDeleteBuffers(1, &grayWorkerVBO[gi]); grayWorkerVBO[gi] = 0; }
                grayWorkerVboFilled[gi] = false;
            }
        }
        if (prevDisplay != EGL_NO_DISPLAY && prevContext != EGL_NO_CONTEXT) {
            eglMakeCurrent(prevDisplay, prevDraw, prevRead, prevContext);
        } else {
            // 调用线程原无 current：解绑，避免 camera context 滞留本线程（后续
            // 回调线程 makeCurrent 同 context 会 EGL_BAD_ACCESS）。
            eglMakeCurrent(engine->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
    }

    // 构造+启动三个录制编码 worker。必须在首次 sxr_camera_open_group 之前调用
    // （initCameras 与 openCameraGroups 共用），保证首帧到达时 worker 已运行；
    // start() 幂等，重复调用安全。display 与主 context 同源（share group）。
    void ensureRecordingWorkers() {
        // RGB 录制编码 worker（mcap 每眼 blit）。cap=8：60fps 下吸收余量 66→133ms，
        // 消化编码管线小抖动（源仓库 2026-09-02 会话 13:01/13:02 两次 <130ms 突发共
        // 3 帧可被吸收；秒级突发仍丢——源仓库靠预览 30fps 门控减负，本仓库无预览
        // 不适用）。代价是 worker 满队时同时 pin 住的相机 EGLImage 上限翻倍：
        // RGB 每 item 双眼 2 张（img[0/1]）即 8→16 张、灰度每组单张 4→8 张，
        // 验证时须对账 HAL 源丢（cam4/5 ts 空洞）无恶化。
        if (!mRgbWorker) {
            mRgbWorker = std::make_unique<SXR::RecordingEncodeWorker>(
                "rgb-enc", engine->display, 8);
        }
        mRgbWorker->start([this](SXR::EncodeFrameItem& it) { rgbEncodeProcess(it); });
        // 灰度两组 worker 独立并行（各持自组锁，互不串行）；cap 同 rgb=8（灰度
        // 1280x480 buffer 小，pin 上限翻倍无内存压力）
        if (!mTrackingWorker) {
            mTrackingWorker = std::make_unique<SXR::RecordingEncodeWorker>(
                "tracking-enc", engine->display, 8);
        }
        mTrackingWorker->start([this](SXR::EncodeFrameItem& it) {
            grayEncodeProcess(it, /*baseIdx*/0);
        });
        if (!mCtrlWorker) {
            mCtrlWorker = std::make_unique<SXR::RecordingEncodeWorker>(
                "ctrl-enc", engine->display, 8);
        }
        mCtrlWorker->start([this](SXR::EncodeFrameItem& it) {
            grayEncodeProcess(it, /*baseIdx*/2);
        });
    }

    // Close all camera groups (release camera hardware)
    void closeCameraGroups() {
        if (!camerasInitialized || !cameraContext || !cameraGroupsOpen) return;
        LOGI("Closing camera groups...");
        // 录制编码 worker 同规则：在途 item 触及组 GL 对象/编码器 surface，必须先
        // join 再 sxr_camera_close_group（QVR session 拆除 UAF 同源约束）。
        if (mRgbWorker) mRgbWorker->stopAndJoin();
        if (mTrackingWorker) mTrackingWorker->stopAndJoin();
        if (mCtrlWorker) mCtrlWorker->stopAndJoin();
        // 每会话复位计数（对齐 sensor-align worker 的 s_alignDropped 每会话清零），
        // 日志/dropped 口径随 open/close 会话独立，不跨会话累计。
        if (mRgbWorker) mRgbWorker->resetCounters();
        if (mTrackingWorker) mTrackingWorker->resetCounters();
        if (mCtrlWorker) mCtrlWorker->resetCounters();
        // worker GL 对象删除在 deleteWorkerGlObjects（回调 drain 后由 pause/
        // cleanupCameras 调用）：此处回调可能仍在途并持有 rgbCtx/trackingCtx
        // current，并发 makeCurrent 会 EGL_BAD_ACCESS。
        sxr_camera_close_group(&api, cameraContext, SXR::CameraGroup::RGB);
        sxr_camera_close_group(&api, cameraContext, SXR::CameraGroup::TRACKING);
        sxr_camera_close_group(&api, cameraContext, SXR::CameraGroup::CTRL);
        cameraGroupsOpen = false;
        LOGI("Camera groups closed");
        // Stop sensor-align worker (no more timestamps posted after camera close)
        s_alignRun.store(false, std::memory_order_release);
        s_alignCv.notify_all();
        if (s_alignThread.joinable()) s_alignThread.join();
    }

    // Re-open all camera groups (resume camera streaming)
    void openCameraGroups() {
        if (!camerasInitialized || !cameraContext || cameraGroupsOpen) return;
        LOGI("Re-opening camera groups...");
        // 录制编码 worker 必须先于 open_group 就绪：worker 已 stopAndJoin（close 时），
        // 若开组后才启动，恢复瞬间到达的首帧会命中"未运行丢帧"分支（mcap 每眼丢帧）。
        ensureRecordingWorkers();
        int ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::RGB,
                                        onRGBFrame, this);
        if (ret != 0) {
            LOGE("Failed to re-open RGB camera group: %d", ret);
        }

        ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::TRACKING,
                                    onTrackingFrame, this);
        if (ret != 0) {
            LOGE("Failed to re-open TRACKING camera group: %d", ret);
        }

        ret = sxr_camera_open_group(&api, cameraContext, SXR::CameraGroup::CTRL,
                                    onCtrlFrame, this);
        if (ret != 0) {
            LOGE("Failed to re-open CTRL camera group: %d", ret);
        }

        cameraGroupsOpen = true;
        LOGI("Camera groups re-opened");
        // Start sensor-align worker (async OpenXR queries + projection off camera callback)
        if (!s_alignRun.load(std::memory_order_acquire)) {
            s_alignRun.store(true, std::memory_order_release);
            s_alignTs.store(-1, std::memory_order_release);
            s_alignThread = std::thread(sensorAlignWorker);
        }
    }

    // Re-enable encoders after pause (called from resume). The encoder
    // instances survive the pause (see pause()), so this only flips the
    // availability flag and resets the ready/IDR flags — the in-progress mp4s
    // and their PTS continue uninterrupted.
    void startEncoders() {
        if (!encodersStopped.load()) return;
        encodersStopped = false;
        // Just reset the ready flags
        rgbFrameReady = false;
        trackingFrameReady = false;
        ctrlFrameReady = false;
        // First frame past resume should re-request an IDR and re-fire
        // notifyReady if the gate somehow re-armed. Symmetric with
        // restartGrayscaleEncoders().
        mRgbGateNotified = false;
        mTrackingGateNotified = false;
        mCtrlGateNotified = false;
        LOGI("Encoders ready to start");
    }

    void stopEncoder() {
        // Mark stop in progress to block new recording starts
        stopInProgress = true;
        // Set flag first to prevent camera thread from entering SBS block
        encodersStopped = true;
        LOGI("stopEncoder: setting encodersStopped=true");
        discardPendingEncoders();  // 清理未被收养的预创建编码器（防泄漏/防旧目录收养）

        // Stop RGB encoder first (signal EOS + join output thread), then release surface.
        // RGB SBS 销毁入 mRgbEyeEncoderMutex：与 mcap 每眼/worker 侧（rgbEyeEncoder*/
        // rgbEyeEncoderSurface* 读写）同锁串行化，消除其 null 检查与 delete 之间 UAF。
        {
            std::lock_guard<std::mutex> lock(mRgbEyeEncoderMutex);
            if (rgbEncoder) {
                rgbEncoder->stop();
                delete rgbEncoder;
                rgbEncoder = nullptr;
            }
            if (rgbEncoderSurface) {
                rgbEncoderSurface->release();
                delete rgbEncoderSurface;
                rgbEncoderSurface = nullptr;
            }
        }

        // Stop legacy grayscale encoders
        for (int i = 0; i < SXR::CAME_MAX; i++) {
            if (grayCameraEncoders[i]) {
                grayCameraEncoders[i]->stop();
                delete grayCameraEncoders[i];
                grayCameraEncoders[i] = nullptr;
            }
        }

        // tracking/ctrl encoder+surface+Y8 纹理销毁与灰度两组 worker（各持自组锁）
        // 串行化：scoped_lock 防锁序死锁（worker 只持单组锁）。
        {
            std::scoped_lock lock(mTrackingEncoderMutex, mCtrlEncoderMutex);
            // Stop tracking encoder
            if (trackingEncoder) {
                trackingEncoder->stop();
                delete trackingEncoder;
                trackingEncoder = nullptr;
            }

            // Stop ctrl encoder
            if (ctrlEncoder) {
                ctrlEncoder->stop();
                delete ctrlEncoder;
                ctrlEncoder = nullptr;
            }

            // Release grayscale encoder surfaces
            if (trackingEncoderSurface) {
                trackingEncoderSurface->release();
                delete trackingEncoderSurface;
                trackingEncoderSurface = nullptr;
            }
            if (ctrlEncoderSurface) {
                ctrlEncoderSurface->release();
                delete ctrlEncoderSurface;
                ctrlEncoderSurface = nullptr;
            }

            // Delete per-encoder Y8 textures
            if (trackingY8Texture) {
                glDeleteTextures(1, &trackingY8Texture);
                trackingY8Texture = 0;
            }
            if (ctrlY8Texture) {
                glDeleteTextures(1, &ctrlY8Texture);
                ctrlY8Texture = 0;
            }
        }

        stopInProgress = false;
        LOGI("stopEncoder: completed, stopInProgress=false");
    }

    void cleanupCameras() {
        if (!camerasInitialized) return;

        // Stop camera callbacks immediately
        isPaused = true;

        // Close camera groups (safe to call even if already closed)
        closeCameraGroups();

        // Wait for in-flight callbacks to drain (with timeout)
        {
            std::unique_lock<std::mutex> lk(callbackDrainMutex);
            callbackDrainCV.wait_for(lk, std::chrono::milliseconds(500),
                [this] { return inFlightCallbacks.load() == 0; });
            if (inFlightCallbacks.load() > 0) {
                LOGW("cleanupCameras: %d callbacks still in flight after timeout, proceeding anyway",
                     inFlightCallbacks.load());
            }
        }

        // 回调已 drain：删除 worker 自有 GL 对象（cleanupAllGLContexts 不覆盖这批）
        deleteWorkerGlObjects();

        // Destroy camera context
        if (cameraContext) {
            sxr_camera_destroy(&api, cameraContext);
            cameraContext = nullptr;
        }

        // Unload the library and cleanup API
        sxr_camera_api_deinit(&api);

        // Cleanup encoders
        stopEncoder();

        // Cleanup shared EGL context (including displayTextures and encoder surfaces)
        cleanupAllGLContexts();

        camerasInitialized = false;
        LOGI("Cameras cleaned up");
    }

    // Convert boottime nanoseconds to XrTime
    // boottime → monotonic (- offset) → XrTime (via xrConvertTimespecTimeToTimeKHR)
    XrTime boottimeToXrTime(uint64_t boottime_ns) {
        static int64_t boottime_to_mono_offset = 0;
        static bool offset_calculated = false;

        if (!offset_calculated) {
            struct timespec boot_ts, mono_ts;
            clock_gettime(CLOCK_BOOTTIME, &boot_ts);
            clock_gettime(CLOCK_MONOTONIC, &mono_ts);
            int64_t boottime_ns_now = (int64_t)boot_ts.tv_sec * 1000000000LL + boot_ts.tv_nsec;
            int64_t mono_ns_now = (int64_t)mono_ts.tv_sec * 1000000000LL + mono_ts.tv_nsec;
            boottime_to_mono_offset = boottime_ns_now - mono_ns_now;
            offset_calculated = true;
            LOGI("boottimeToXrTime: boottime-mono offset = %ld ns", (long)boottime_to_mono_offset);
        }

        // Step 1: boottime → monotonic
        int64_t mono_ns = (int64_t)boottime_ns - boottime_to_mono_offset;

        // Step 2: monotonic → XrTime
        if (!xrConvertTimespecTimeToTimeKHR) {
            return static_cast<XrTime>(mono_ns);
        }

        struct timespec ts;
        ts.tv_sec = mono_ns / 1000000000LL;
        ts.tv_nsec = mono_ns % 1000000000LL;

        XrTime xrTime;
        XrResult res = xrConvertTimespecTimeToTimeKHR(engine->state.xrInstance, &ts, &xrTime);
        if (XR_FAILED(res)) {
            LOGE("xrConvertTimespecTimeToTimeKHR failed: %d", res);
            return static_cast<XrTime>(mono_ns);
        }
        return xrTime;
    }

    // Convert XrTime to boottime nanoseconds (inverse of boottimeToXrTime)
    // XrTime → monotonic (via xrConvertTimeToTimespecTimeKHR) → boottime (+ offset)
    int64_t xrTimeToBoottime(XrTime xrTime) {
        static int64_t boottime_to_mono_offset = 0;
        static bool offset_calculated = false;

        if (!offset_calculated) {
            struct timespec boot_ts, mono_ts;
            clock_gettime(CLOCK_BOOTTIME, &boot_ts);
            clock_gettime(CLOCK_MONOTONIC, &mono_ts);
            int64_t boottime_ns_now = (int64_t)boot_ts.tv_sec * 1000000000LL + boot_ts.tv_nsec;
            int64_t mono_ns_now = (int64_t)mono_ts.tv_sec * 1000000000LL + mono_ts.tv_nsec;
            boottime_to_mono_offset = boottime_ns_now - mono_ns_now;
            offset_calculated = true;
        }

        // Step 1: XrTime → monotonic
        int64_t mono_ns;
        if (xrConvertTimeToTimespecTimeKHR) {
            struct timespec ts;
            XrResult res = xrConvertTimeToTimespecTimeKHR(engine->state.xrInstance, xrTime, &ts);
            if (XR_SUCCEEDED(res)) {
                mono_ns = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            } else {
                LOGE("xrConvertTimeToTimespecTimeKHR failed: %d", res);
                mono_ns = static_cast<int64_t>(xrTime);
            }
        } else {
            mono_ns = static_cast<int64_t>(xrTime);
        }

        // Step 2: monotonic → boottime
        return mono_ns + boottime_to_mono_offset;
    }
};
/**
 * Shared state for our app.
 */
struct engine : public AppCommon::base_engine {
    // render target width
    uint32_t width;

    // render target height
    uint32_t height;

    // <sample count, stereo swapchain>
    std::unordered_map<uint32_t, StereoSwapchain> swapchainMap;

    // cube geometry
    QtiGL::Geometry cube;

    // cube shader
    QtiGL::Shader *cubeShader;

    // cube texture
    GLuint cubeTexture;
    // cube texture
    GLuint testTexture;
    // cube position
    std::vector<glm::mat4> cubeMatrices;

    // cube colors
    std::vector<glm::vec3> cubeColors;

    // max sample count
    GLint maxSampleCount;

    // current sample count
    GLint currentSampleCount;
    const uint32_t quadImageWidth = 500;
    const uint32_t quadImageHeight = 422;

    AppCommon::Swapchain quadSwapchain;
    AppCommon::Swapchain quadSwapchain2;
    XrExtent2Df quadLayerSize = {.width = 2.f, .height = 2.f};
    XrPosef  quadLayerPose = {.orientation = {.x = 0.f,.y=0.f,.z=0.f,.w = 1.f},
            .position = {.x = 0.f,.y=-2.1f,.z=-5.f}};

    GLuint depthBuffer; // common depth buffer... should be alright as we assume

    bool useProjectHand = false;
    bool projectController = false;

    // When a data source (controller/hand) is inactive, only save the FIRST empty row.
    // These flags track whether that single inactive row has been saved.
    bool controllerPoseInactiveRowSaved = false;
    bool handTrackingInactiveRowSaved = false;

    CameraAccessExtension mCameraAccessExtension{this};
    HandTrackerLogic mHandTrackerLogic{this};
    HandOverlayRenderer handOverlay;
    DatasetRecorder mDatasetRecorder;
    // Synchronized-start gate coordinator for all recording sources.
    RecordingGatekeeper mGate;
    ControllerPoseSaver mControllerPoseSaver;
    uint64_t controllerFrameCounter = 0;
    std::unique_ptr<Input> inputPtr;
    bool dpadCenterPressed = false;
    bool prevRecordingToggle = false;

    // Wall-clock time when recording started; checked by the 12h auto-stop
    // guard (AAudio int32 frame counter overflows at ~13.5h @ 44100Hz). Set
    // right before start() in both recording entry points (intent + controller)
    // so it's always valid once isRecording() is true.
    std::chrono::steady_clock::time_point recordingStartTime{};

    // Latch: once the auto-stop guard has fired for this recording session,
    // suppress further triggers until the next recording starts. Without this
    // the level-triggered check fires every frame (isRecording() stays true
    // until the async stop completes), spawning concurrent stop threads that
    // race on MediaCodec → SIGSEGV.
    std::atomic<bool> autoStopRequested{false};

    // Single-flight guard for the async recording teardown. stopRecordingAsync
    // may be triggered concurrently (manual stop vs. gatekeeper-timeout abort vs.
    // auto-stop); without this, two teardown threads could both run stopEncoder()
    // and double-delete the encoder pointers. Set on start (so a stray stop during
    // recording isn't dropped), exchanged-and-checked in stopRecordingAsync.
    std::atomic<bool> recordingStopRequested{false};

    // Propagate BOOTTIME→REALTIME offset to all components.
    // CameraEncoders are lazy-initialized and read the offset from
    // CameraAccessExtension::mCameraTimeOffsetNs during init.
    void propagateTimeOffset() {
        int64_t timeOffset = mDatasetRecorder.getTimeOffset();
        mCameraAccessExtension.mCameraTimeOffsetNs = timeOffset;
        mControllerPoseSaver.SetTimeOffset(timeOffset);
        mHandTrackerLogic.rawDateSave->SetTimeOffset(timeOffset);
    }

    AlignedSensorSnapshot alignedSnapshot;
    PoseHandSampleRing poseHandRing;
    ControllerPoseRing controllerPoseRing;
    engine()
            : width(0), height(0), cubeShader(nullptr), cubeTexture(0),
              maxSampleCount(4), currentSampleCount(1)
    {
    }
};

// Recording gate helpers: thin wrappers so CameraAccessExtension member
// functions (defined before the engine struct) can reach g_engine->mGate.
// Non-static: declared in RecordingGatekeeper.h for other TUs (AudioEncoder).
bool recordingGateIsOpen(int64_t tsNs) {
    return g_engine && g_engine->mGate.isOpen(tsNs);
}
bool recordingGateActive() {
    return g_engine && g_engine->mGate.state() != RecordingGatekeeper::State::IDLE;
}
void recordingGateNotifyReady(RecordingGatekeeper::Source src) {
    if (g_engine) g_engine->mGate.notifyReady(src);
}
void recordingGateRefineToRgbFirstFrame(int64_t rgbMidExposureNs) {
    if (g_engine) g_engine->mGate.refineGateToRgbFirstFrame(rgbMidExposureNs);
}

// A1/A4: end-of-recording alignment timestamp (CLOCK_BOOTTIME ns). Held in a
// TU-local holder (not a RecordingGatekeeper member) so this header needs no
// forward declaration of `engine`. Default INT64_MAX disables filtering.
static std::atomic<int64_t> s_recordingStopNs{INT64_MAX};
void recordingGateSetStopNs(int64_t stopBootNs) {
    s_recordingStopNs.store(stopBootNs, std::memory_order_release);
}
bool recordingGateBeforeStop(int64_t tsNs) {
    return tsNs <= s_recordingStopNs.load(std::memory_order_acquire);
}
bool recordingGateWithinWindow(int64_t tsNs) {
    return recordingGateIsOpen(tsNs) && recordingGateBeforeStop(tsNs);
}
void recordingGateResetStop() {
    s_recordingStopNs.store(INT64_MAX, std::memory_order_release);
}

// Save aligned head pose and hand tracking data with the RGB frame timestamp.
// Called from CameraAccessExtension::handleRGBFrame (camera callback thread).
//
// Samples the ring buffer at the camera frame's mid-exposure time
// (start_of_exposure + exposure/2, CLOCK_BOOTTIME) to recover time-aligned
// head pose (slerp) and hand joints (lerp). Both overlaySnap (for encoder
// overlay) and CSV output use the same time-aligned data so that recorded
// timestamps match the actual sensor data.
static void saveAlignedSensorData(int64_t rgbTimestampNs) {
    if (!g_engine || !g_engine->mDatasetRecorder.isRecording()) {
        return;
    }

    // Snapshot the alignedSnapshot once for rgbFrameCount and fallback.
    AlignedSensorSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(g_engine->alignedSnapshot.mutex);
        snap.copyFrom(g_engine->alignedSnapshot);
    }

    // --- DIRECT OPENXR QUERY AT CAMERA FRAME TIME ---
    // Query xrLocateSpace and xrLocateHandJointsEXT directly with the camera
    // frame's mid-exposure XrTime. This avoids prediction/extrapolation error
    // from sampling render-thread "now" poses and interpolating to the past.
    // OpenXR runtime will return the historical pose at the exact frame time.
    PoseHandSampleRing::Sample rs;
    PoseHandSampleRing::SampleInfo info;
    bool ringOk = false;
    bool directOk = false;
    if (g_boottimeToXrTimeFn) {
        XrTime camXrTime = g_boottimeToXrTimeFn((uint64_t)rgbTimestampNs);

        // Head pose (always queried — both controller and hand tracking modes)
        XrSpace refSpace = g_engine->useRootSpace ? g_engine->state.xrRootSpace
                                                  : g_engine->state.xrLocalSpace;
        XrSpaceLocation devLoc{XR_TYPE_SPACE_LOCATION};
        XrResult r1 = xrLocateSpace(g_engine->state.xrViewSpace, refSpace,
                                     camXrTime, &devLoc);
        bool headOk = XR_SUCCEEDED(r1) &&
            (devLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (devLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);

        {
            // Combined head pose + hand tracking query — both subsystems always active.
            // Head pose is decoupled from hand tracking — headOk alone is
            // sufficient for head pose; hand data is populated independently.

            // --- Head pose (always populated when valid) ---
            if (headOk) {
                rs.poseValid = true;
                rs.headPos[0] = devLoc.pose.position.x;
                rs.headPos[1] = devLoc.pose.position.y;
                rs.headPos[2] = devLoc.pose.position.z;
                rs.headQuat[0] = devLoc.pose.orientation.x;
                rs.headQuat[1] = devLoc.pose.orientation.y;
                rs.headQuat[2] = devLoc.pose.orientation.z;
                rs.headQuat[3] = devLoc.pose.orientation.w;
                directOk = true;
            }

            // --- Hand joints (independent of head pose, best-effort) ---
            XrHandJointLocationEXT leftJL[XR_HAND_JOINT_COUNT_EXT] = {};
            XrHandJointLocationEXT rightJL[XR_HAND_JOINT_COUNT_EXT] = {};
            XrHandJointLocationsEXT leftLocs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr,
                                              false, XR_HAND_JOINT_COUNT_EXT, leftJL};
            XrHandJointLocationsEXT rightLocs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr,
                                               false, XR_HAND_JOINT_COUNT_EXT, rightJL};
            XrHandJointsLocateInfoEXT locInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            locInfo.time = camXrTime;
            locInfo.baseSpace = refSpace;
            auto& ht = g_engine->mHandTrackerLogic;
            bool leftHandOk = false, rightHandOk = false;
            if (ht.pfnLocateHandJointsEXT && ht.LeftHandTrackerHandle) {
                XrResult r2 = ht.pfnLocateHandJointsEXT(ht.LeftHandTrackerHandle,
                                                        &locInfo, &leftLocs);
                leftHandOk = XR_SUCCEEDED(r2);
            }
            if (ht.pfnLocateHandJointsEXT && ht.RightHandTrackerHandle) {
                XrResult r3 = ht.pfnLocateHandJointsEXT(ht.RightHandTrackerHandle,
                                                        &locInfo, &rightLocs);
                rightHandOk = XR_SUCCEEDED(r3);
            }

            if (leftHandOk && rightHandOk) {
                rs.leftActive = leftLocs.isActive;
                rs.rightActive = rightLocs.isActive;
                for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                    rs.leftJoints[j][0] = leftJL[j].pose.position.x;
                    rs.leftJoints[j][1] = leftJL[j].pose.position.y;
                    rs.leftJoints[j][2] = leftJL[j].pose.position.z;
                    rs.leftQuats[j][0] = leftJL[j].pose.orientation.x;
                    rs.leftQuats[j][1] = leftJL[j].pose.orientation.y;
                    rs.leftQuats[j][2] = leftJL[j].pose.orientation.z;
                    rs.leftQuats[j][3] = leftJL[j].pose.orientation.w;
                    rs.leftRadii[j] = leftJL[j].radius;
                    rs.rightJoints[j][0] = rightJL[j].pose.position.x;
                    rs.rightJoints[j][1] = rightJL[j].pose.position.y;
                    rs.rightJoints[j][2] = rightJL[j].pose.position.z;
                    rs.rightQuats[j][0] = rightJL[j].pose.orientation.x;
                    rs.rightQuats[j][1] = rightJL[j].pose.orientation.y;
                    rs.rightQuats[j][2] = rightJL[j].pose.orientation.z;
                    rs.rightQuats[j][3] = rightJL[j].pose.orientation.w;
                    rs.rightRadii[j] = rightJL[j].radius;
                }
            }
            if (headOk || (leftHandOk && rightHandOk)) {
                static int s_directLogCnt = 0;
                if ((s_directLogCnt++ % 30) == 0) {
                    LOGI("direct-query OK: rgbTs=%lld camXrTime=%lld head=%d L_act=%d R_act=%d",
                         (long long)rgbTimestampNs, (long long)camXrTime,
                         (int)headOk, (int)leftLocs.isActive, (int)rightLocs.isActive);
                }
            }
        } // end combined head + hand tracking query
    }

    // Fallback: sample the ring buffer at the camera frame's mid-exposure
    // timestamp (CLOCK_BOOTTIME) if direct query failed.
    // Ring buffer now has head pose samples in both controller and hand
    // tracking modes, so it serves as fallback in all modes.
    if (!directOk) {
        ringOk = g_engine->poseHandRing.sample(rgbTimestampNs, rs, &info);
    }

    // Diagnostic: throttle to ~1 Hz at 30 fps (only log when ring was actually sampled)
    static int s_logCnt = 0;
    if (!directOk && (s_logCnt++ % 30) == 0) {
        LOGI("ring sample: rgbTs=%lld count=%d window=[%lld..%lld] dtOld=%.1fms dtNew=%.1fms alpha=%.3f clamp=%c%c ok=%d",
             (long long)rgbTimestampNs, info.curCount,
             (long long)info.oldestNs, (long long)info.newestNs,
             (rgbTimestampNs - info.oldestNs) / 1e6,
             (rgbTimestampNs - info.newestNs) / 1e6,
             info.alpha,
             info.clampedLow ? 'L' : '-',
             info.clampedHigh ? 'H' : '-',
             (int)ringOk);
    }

    bool useRing = (directOk || ringOk) && rs.poseValid;

    // --- Diagnostic: compare direct query vs ring buffer interpolation ---
    // Logs position/orientation delta when both paths succeed, to verify
    // whether xrLocateSpace with historical XrTime returns true historical
    // poses or just the latest. Throttled to ~0.1 Hz (every 300th frame).
    if (directOk) {
        PoseHandSampleRing::Sample ringCmp;
        PoseHandSampleRing::SampleInfo cmpInfo;
        bool cmpOk = g_engine->poseHandRing.sample(rgbTimestampNs, ringCmp, &cmpInfo);
        if (cmpOk) {
            static int s_cmpCnt = 0;
            if ((s_cmpCnt++ % 300) == 0) {
                float dx = rs.headPos[0] - ringCmp.headPos[0];
                float dy = rs.headPos[1] - ringCmp.headPos[1];
                float dz = rs.headPos[2] - ringCmp.headPos[2];
                float posDeltaMm = sqrtf(dx*dx + dy*dy + dz*dz) * 1000.0f;
                // Orientation delta: angular distance between quaternions
                float qd = rs.headQuat[0]*ringCmp.headQuat[0]
                         + rs.headQuat[1]*ringCmp.headQuat[1]
                         + rs.headQuat[2]*ringCmp.headQuat[2]
                         + rs.headQuat[3]*ringCmp.headQuat[3];
                if (qd < -1.0f) qd = -1.0f;
                if (qd >  1.0f) qd =  1.0f;
                float orientDeltaDeg = 2.0f * acosf(fabsf(qd)) * 57.29578f;
                LOGI("direct-vs-ring: rgbTs=%lld posDelta=%.2fmm orientDelta=%.3fdeg alpha=%.3f clamp=%c%c",
                     (long long)rgbTimestampNs, (double)posDeltaMm, (double)orientDeltaDeg,
                     cmpInfo.alpha,
                     cmpInfo.clampedLow ? 'L' : '-',
                     cmpInfo.clampedHigh ? 'H' : '-');
            }
        }
    }

    // --- Overlay snapshot for encoder hand projection ---
    {
        std::lock_guard<std::mutex> lock(g_engine->mCameraAccessExtension.overlaySnap.mutex);
        auto& os = g_engine->mCameraAccessExtension.overlaySnap;
        if (useRing) {
            os.headValid = true;
            memcpy(os.headPos, rs.headPos, sizeof(os.headPos));
            memcpy(os.headQuat, rs.headQuat, sizeof(os.headQuat));
            os.leftActive = rs.leftActive;
            os.rightActive = rs.rightActive;
            memcpy(os.leftJoints, rs.leftJoints, sizeof(os.leftJoints));
            memcpy(os.rightJoints, rs.rightJoints, sizeof(os.rightJoints));
        } else {
            // Ring not warmed up yet — fall back to the most recent snapshot.
            os.copyFrom(snap);
        }
    }

    // --- Save head pose (time-aligned via ring buffer or direct query) ---
    {
        XrPosef pose{};
        if (useRing) {
            pose.position.x = rs.headPos[0];
            pose.position.y = rs.headPos[1];
            pose.position.z = rs.headPos[2];
            pose.orientation.x = rs.headQuat[0];
            pose.orientation.y = rs.headQuat[1];
            pose.orientation.z = rs.headQuat[2];
            pose.orientation.w = rs.headQuat[3];
        } else if (snap.headPose.valid) {
            // Fall back to most recent render-thread snapshot (controller mode or ring not warmed up).
            pose.position.x = snap.headPose.pos[0];
            pose.position.y = snap.headPose.pos[1];
            pose.position.z = snap.headPose.pos[2];
            pose.orientation.x = snap.headPose.quat[0];
            pose.orientation.y = snap.headPose.quat[1];
            pose.orientation.z = snap.headPose.quat[2];
            pose.orientation.w = snap.headPose.quat[3];
        }
        g_engine->mDatasetRecorder.saveHeadPose(rgbTimestampNs, pose);
    }

    // --- Save controller pose (time-aligned via controller ring buffer) ---
    // Always save; controller ring returns inactive samples when no controller connected.
    {
        ControllerPoseRing::Sample ctrlSample;
        if (g_engine->controllerPoseRing.sample(rgbTimestampNs, ctrlSample)) {
            // Apply the view-frame correction rotated into world space by the
            // time-aligned head orientation (same timestamp source as the
            // head_pose row above). Both the CSV record and the overlaySnap
            // axis projection below consume these corrected positions.
            {
                const float* headQ = nullptr;
                float fallbackQ[4];
                if (useRing) {
                    headQ = rs.headQuat;
                } else if (snap.headPose.valid) {
                    fallbackQ[0] = snap.headPose.quat[0];
                    fallbackQ[1] = snap.headPose.quat[1];
                    fallbackQ[2] = snap.headPose.quat[2];
                    fallbackQ[3] = snap.headPose.quat[3];
                    headQ = fallbackQ;
                }
                if (headQ) {
                    if (ctrlSample.leftActive)
                        applyCtrlViewCorrection(ctrlSample.leftPos, headQ);
                    if (ctrlSample.rightActive)
                        applyCtrlViewCorrection(ctrlSample.rightPos, headQ);
                }
                // Head pose unavailable: skip correction (same as no fix)
                // rather than rotating by a stale/identity orientation.
            }
            ControllerPoseRecord rec{};
            rec.frameNumber = ctrlSample.frameNumber;
            rec.timestamp = rgbTimestampNs;  // use camera mid-exposure time, not render time
            rec.leftActive = ctrlSample.leftActive;
            if (rec.leftActive) {
                rec.leftPos[0] = ctrlSample.leftPos[0];
                rec.leftPos[1] = ctrlSample.leftPos[1];
                rec.leftPos[2] = ctrlSample.leftPos[2];
                rec.leftQuat[0] = ctrlSample.leftQuat[0];
                rec.leftQuat[1] = ctrlSample.leftQuat[1];
                rec.leftQuat[2] = ctrlSample.leftQuat[2];
                rec.leftQuat[3] = ctrlSample.leftQuat[3];
            }
            rec.rightActive = ctrlSample.rightActive;
            if (rec.rightActive) {
                rec.rightPos[0] = ctrlSample.rightPos[0];
                rec.rightPos[1] = ctrlSample.rightPos[1];
                rec.rightPos[2] = ctrlSample.rightPos[2];
                rec.rightQuat[0] = ctrlSample.rightQuat[0];
                rec.rightQuat[1] = ctrlSample.rightQuat[1];
                rec.rightQuat[2] = ctrlSample.rightQuat[2];
                rec.rightQuat[3] = ctrlSample.rightQuat[3];
            }
            // When active: save all frames. When inactive: save only the first empty row.
            // 录制门窗口外不喂 saver（writer 线程同样丢弃）：防 arming 期间 inactive 闩锁
            // 提前置位、门开后永不再存（/controller_poses 0 消息根因，移植自源 36e10c7）
            if (recordingGateWithinWindow(rgbTimestampNs)) {
                if (rec.leftActive || rec.rightActive) {
                    g_engine->mControllerPoseSaver.SaveFrame(rec);
                    g_engine->controllerPoseInactiveRowSaved = false;
                } else if (!g_engine->controllerPoseInactiveRowSaved) {
                    g_engine->mControllerPoseSaver.SaveFrame(rec);
                    g_engine->controllerPoseInactiveRowSaved = true;
                }
            }

            // Stash controller poses into overlaySnap for coordinate-axis
            // projection onto encoded RGB frames.
            if (g_engine->projectController) {
                auto& os = g_engine->mCameraAccessExtension.overlaySnap;
                std::lock_guard<std::mutex> lock(os.mutex);
                os.ctrlLeftActive = ctrlSample.leftActive;
                if (ctrlSample.leftActive) {
                    memcpy(os.ctrlLeftPos, ctrlSample.leftPos, sizeof(os.ctrlLeftPos));
                    memcpy(os.ctrlLeftQuat, ctrlSample.leftQuat, sizeof(os.ctrlLeftQuat));
                }
                os.ctrlRightActive = ctrlSample.rightActive;
                if (ctrlSample.rightActive) {
                    memcpy(os.ctrlRightPos, ctrlSample.rightPos, sizeof(os.ctrlRightPos));
                    memcpy(os.ctrlRightQuat, ctrlSample.rightQuat, sizeof(os.ctrlRightQuat));
                }
            }
        }
    }

    // --- Save hand tracking (time-aligned via ring buffer) ---
    // Always save; only active hands produce joint data.
    {
        FrameData fd{};
        fd.frameNumber = snap.rgbFrameCount;
        fd.timestamp = rgbTimestampNs;

        if (useRing) {
        fd.hasLeftHand = rs.leftActive;
        fd.hasRightHand = rs.rightActive;
        if (rs.leftActive) {
            fd.leftHand.isActive = true;
            fd.leftHand.jointCount = XR_HAND_JOINT_COUNT_EXT;
            for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                fd.leftHand.joints[j].radius = rs.leftRadii[j];
                fd.leftHand.joints[j].position[0] = rs.leftJoints[j][0];
                fd.leftHand.joints[j].position[1] = rs.leftJoints[j][1];
                fd.leftHand.joints[j].position[2] = rs.leftJoints[j][2];
                fd.leftHand.joints[j].orientation[0] = rs.leftQuats[j][0];
                fd.leftHand.joints[j].orientation[1] = rs.leftQuats[j][1];
                fd.leftHand.joints[j].orientation[2] = rs.leftQuats[j][2];
                fd.leftHand.joints[j].orientation[3] = rs.leftQuats[j][3];
            }
        }
        if (rs.rightActive) {
            fd.rightHand.isActive = true;
            fd.rightHand.jointCount = XR_HAND_JOINT_COUNT_EXT;
            for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                fd.rightHand.joints[j].radius = rs.rightRadii[j];
                fd.rightHand.joints[j].position[0] = rs.rightJoints[j][0];
                fd.rightHand.joints[j].position[1] = rs.rightJoints[j][1];
                fd.rightHand.joints[j].position[2] = rs.rightJoints[j][2];
                fd.rightHand.joints[j].orientation[0] = rs.rightQuats[j][0];
                fd.rightHand.joints[j].orientation[1] = rs.rightQuats[j][1];
                fd.rightHand.joints[j].orientation[2] = rs.rightQuats[j][2];
                fd.rightHand.joints[j].orientation[3] = rs.rightQuats[j][3];
            }
        }
    } else {
        // Ring not warmed up — fall back to most recent snapshot.
        fd.hasLeftHand = snap.leftHand.active;
        fd.hasRightHand = snap.rightHand.active;
        if (snap.leftHand.active) {
            fd.leftHand.isActive = true;
            fd.leftHand.jointCount = XR_HAND_JOINT_COUNT_EXT;
            for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                fd.leftHand.joints[j].radius = snap.leftHand.radii[j];
                fd.leftHand.joints[j].position[0] = snap.leftHand.joints[j][0];
                fd.leftHand.joints[j].position[1] = snap.leftHand.joints[j][1];
                fd.leftHand.joints[j].position[2] = snap.leftHand.joints[j][2];
                fd.leftHand.joints[j].orientation[0] = snap.leftHand.quats[j][0];
                fd.leftHand.joints[j].orientation[1] = snap.leftHand.quats[j][1];
                fd.leftHand.joints[j].orientation[2] = snap.leftHand.quats[j][2];
                fd.leftHand.joints[j].orientation[3] = snap.leftHand.quats[j][3];
            }
        }
        if (snap.rightHand.active) {
            fd.rightHand.isActive = true;
            fd.rightHand.jointCount = XR_HAND_JOINT_COUNT_EXT;
            for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
                fd.rightHand.joints[j].radius = snap.rightHand.radii[j];
                fd.rightHand.joints[j].position[0] = snap.rightHand.joints[j][0];
                fd.rightHand.joints[j].position[1] = snap.rightHand.joints[j][1];
                fd.rightHand.joints[j].position[2] = snap.rightHand.joints[j][2];
                fd.rightHand.joints[j].orientation[0] = snap.rightHand.quats[j][0];
                fd.rightHand.joints[j].orientation[1] = snap.rightHand.quats[j][1];
                fd.rightHand.joints[j].orientation[2] = snap.rightHand.quats[j][2];
                fd.rightHand.joints[j].orientation[3] = snap.rightHand.quats[j][3];
            }
        }
    }  // end else (ring fallback)
        // When active: save all frames. When inactive: save only the first empty row.
        // 录制门窗口外不喂 saver：防 arming 期间 inactive 闩锁提前置位、门开后
        // 永不再存（/hand_tracking 0 消息根因，同 controller 修复）
        if (recordingGateWithinWindow(rgbTimestampNs)) {
            if (fd.hasLeftHand || fd.hasRightHand) {
                g_engine->mHandTrackerLogic.rawDateSave->SaveFrame(fd);
                g_engine->handTrackingInactiveRowSaved = false;
            } else if (!g_engine->handTrackingInactiveRowSaved) {
                g_engine->mHandTrackerLogic.rawDateSave->SaveFrame(fd);
                g_engine->handTrackingInactiveRowSaved = true;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_engine->alignedSnapshot.mutex);
            g_engine->alignedSnapshot.rgbFrameCount++;
        }
    }  // end hand tracking save block
}

// --- Sensor-align worker thread ---
// Runs OpenXR queries + KB projection off the camera callback thread so
// the HAL buffer pipeline is never blocked (>2ms → provider crash risk).
static void sensorAlignWorker() {
    while (s_alignRun.load(std::memory_order_acquire)) {
        int64_t ts = -1;
        {
            std::unique_lock<std::mutex> lk(s_alignMutex);
            s_alignCv.wait(lk, [] {
                return s_alignTs.load(std::memory_order_acquire) >= 0 ||
                       !s_alignRun.load(std::memory_order_acquire);
            });
            if (!s_alignRun.load(std::memory_order_acquire)) break;
            // atomic read-and-reset: avoids race where camera callback
            // writes a new timestamp between load and store(-1).
            ts = s_alignTs.exchange(-1, std::memory_order_acq_rel);
        }
        if (ts <= 0) continue;

        // (1) OpenXR queries + dataset recording
        saveAlignedSensorData(ts);

        // (2) KB fisheye projection (was in renderHandOverlayToEncoder)
        if (!g_engine || !g_engine->useProjectHand) continue;
        auto& os = g_engine->mCameraAccessExtension.overlaySnap;
        std::lock_guard<std::mutex> lock(os.mutex);
        if (!os.headValid) continue;
        g_engine->handOverlay.computeProjection(
            os.leftActive, os.leftJoints,
            os.rightActive, os.rightJoints,
            os.headPos, os.headQuat);
    }
}

// Feed camera params to hand overlay renderer (needs complete engine type)
static void feedOverlayCameraParams(const SXR::FrameData* data) {
    if (!g_engine) return;
    for (int i = 0; i < 2; i++) {
        const auto& f = data->frames[i];
        static bool logged = false;
        if (!logged) {
            LOGI("feedOverlayCameraParams[%d]: %ux%u focal=[%.1f,%.1f] center=[%.1f,%.1f] "
                 "dist=[%.4f,%.4f,%.4f,%.4f] pos=[%.4f,%.4f,%.4f] quat=[%.4f,%.4f,%.4f,%.4f]",
                 i, f.width, f.height, f.focalX, f.focalY, f.centerX, f.centerY,
                 f.radialDistortion[0], f.radialDistortion[1], f.radialDistortion[2], f.radialDistortion[3],
                 f.position[0], f.position[1], f.position[2],
                 f.rotation[0], f.rotation[1], f.rotation[2], f.rotation[3]);
            if (i == 1) logged = true;
        }
        // HandOverlayRenderer expects extQuat pre-rotated by conj(Rz(90°)):
        // it pairs with the renderer's fixed 90° CW image rotation
        // (rotate_uv_90cw) that matches the RGB sensor's mounted orientation.
        // extPos stays raw. This compensates the camera *image* orientation
        // for the renderer only — independent of the dataset body frame
        // (the dataset still outputs raw OpenXR Body, dataset paths untouched).
        const float s = 0.7071067811865475f;  // cos(45°) = sqrt(2)/2
        float pos[3] = {f.position[0], f.position[1], f.position[2]};
        float rot[4] = {
            (f.rotation[0] - f.rotation[1]) * s,
            (f.rotation[0] + f.rotation[1]) * s,
            (f.rotation[2] - f.rotation[3]) * s,
            (f.rotation[3] + f.rotation[2]) * s,
        };
        g_engine->handOverlay.updateCameraParams(i, f.focalX, f.focalY,
                                                   f.centerX, f.centerY,
                                                   f.radialDistortion,
                                                   pos, rot,
                                                   f.width, f.height);
    }
    // Per-eye UV pixel offsets for fine-tuning (default 0).
    // Projection now matches the Python reference; offsets are
    // only needed for residual calibration.
    static bool offsetsSet = false;
    if (!offsetsSet) {
        g_engine->handOverlay.setUVOffset(0, 0.0f, 0.0f);
        g_engine->handOverlay.setUVOffset(1, 0.0f, 0.0f);
        offsetsSet = true;
        LOGI("HandOverlay UV offsets set: L=(0,0) R=(0,0)");
    }
}

// Render hand skeleton overlay to encoder surface (called from RGB camera
// callback in the direct-encode path).  Avoids the render-thread copy pass
// by compositing overlay + SBS in a single GPU submit.
// Requires g_engine->useProjectHand and overlaySnap.headValid.
static void renderHandOverlayToEncoder(int texWidth, int texHeight) {
    if (!g_engine || !g_engine->useProjectHand) return;
    auto& os = g_engine->mCameraAccessExtension.overlaySnap;
    std::lock_guard<std::mutex> lock(os.mutex);
    if (!os.headValid) return;
    // computeProjection is pre-computed by sensorAlignWorker thread.
    g_engine->handOverlay.render(0, 0, texWidth, texHeight);          // left eye
    g_engine->handOverlay.render(1, texWidth, texWidth, texHeight);   // right eye
}

// Render controller coordinate axes to encoder surface. Requires
// persist.xr.project_controller=1 in controller mode.
static void renderControllerAxesToEncoder(int texWidth, int texHeight) {
    if (!g_engine || !g_engine->projectController) return;
    auto& os = g_engine->mCameraAccessExtension.overlaySnap;
    std::lock_guard<std::mutex> lock(os.mutex);
    if (!os.headValid) return;
    // Compute projection for both eyes at once
    g_engine->handOverlay.computeControllerAxes(
        os.ctrlLeftActive,  os.ctrlLeftPos,  os.ctrlLeftQuat,
        os.ctrlRightActive, os.ctrlRightPos, os.ctrlRightQuat,
        os.headPos, os.headQuat);
    // Render: each eye's viewport matches the SBS layout.
    g_engine->handOverlay.renderControllerAxes(
        0, 0, 0, texWidth, texHeight, texWidth, texHeight);           // left eye
    g_engine->handOverlay.renderControllerAxes(
        1, texWidth, 0, texWidth, texHeight, texWidth, texHeight);    // right eye
}

// JNI native methods for intent control (needs complete engine type)
// g_engine is declared near top of file (before CameraAccessExtension)

CameraAccessExtension* CameraAccessExtension::sInstance = nullptr;


extern "C" JNIEXPORT void JNICALL
Java_com_ssnwt_helloxr_VrNativeActivity_nativeRequestSnapshot(JNIEnv *env, jobject thiz) {
    if (g_engine) {
        g_engine->mCameraAccessExtension.snapshotRequested = true;
        LOGI("Snapshot requested via intent");
    }
}

// 每次录制重建 format provider 并注入四个传感器组件 + mcap manager。
// 必须在 DatasetRecorder.start() 之前调用：start()/StartSession() 按 provider 决定
// 是否打开 CSV——注入晚于 start 会用 stale provider 决策，传感器 CSV 静默为空。
// 次序（防悬垂）：先建新 provider → 各组件 setFormatProvider 切换 → 最后退役旧实例。
// 旧 provider 不 delete：McapSinkCtx::isMcap() 无锁读 fmt（load 到旧指针后、解引用前
// 若旧实例被删则 UAF）；退役列表进程期驻留，每次录制仅新增数十字节，代价可忽略。
static void injectDatasetFormat(struct engine* e) {
    auto& ext = e->mCameraAccessExtension;
    auto newFormatProvider = std::make_unique<xr::PersistDatasetFormatProvider>();
    if (newFormatProvider->isMcap() && !ext.mMcapManager) {
        // 契约：仅此处首次赋值（早于 per-eye 编码器创建，无并发读者），此后不再赋值——
        // EncoderStreamAdapter::onEncodedFrame 的无锁 shared_ptr 拷贝依赖该约定
        ext.mMcapManager = std::make_shared<xr::mcap::McapChunkManager>();
    }
    e->mDatasetRecorder.setFormatProvider(newFormatProvider.get());
    e->mDatasetRecorder.setMcapManager(ext.mMcapManager.get());
    e->mControllerPoseSaver.setFormatProvider(newFormatProvider.get());
    e->mControllerPoseSaver.setMcapManager(ext.mMcapManager.get());
    e->mHandTrackerLogic.rawDateSave->setFormatProvider(newFormatProvider.get());
    e->mHandTrackerLogic.rawDateSave->setMcapManager(ext.mMcapManager.get());
    if (ext.mMcapManager) {
        ext.mMcapManager->setFormatProvider(newFormatProvider.get());
        ext.mMcapManager->setTimeOffsetPtr(&ext.mCameraTimeOffsetNs);
    }
    SXR::CameraEncoder::setOutputListener(&ext.mStreamAdapter);  // 幂等重注册
    // 退役而非删除旧 provider（见函数注释：无锁读者 UAF 窗口）
    static std::mutex sRetiredProvidersMtx;
    static std::vector<std::unique_ptr<xr::IDatasetFormatProvider>> sRetiredProviders;
    {
        std::lock_guard<std::mutex> lock(sRetiredProvidersMtx);
        if (ext.mFormatProvider) sRetiredProviders.push_back(std::move(ext.mFormatProvider));
    }
    ext.mFormatProvider = std::move(newFormatProvider);
}

// MCAP：begin 单文件（<datasetDir>/<basename>.mcap），并防御性销毁 SBS rgbEncoder。
// 防御块说明：stopEncoder 每次停录已销毁 rgbEncoder（无跨录制存活语义），当前为
// 死代码，保留以兼容未来生命周期变化；注意若未来真触发，此处与 RGB 回调线程的
// rgbEncoderSurface->makeCurrent() 路径存在无锁竞争（UAF），届时须先加锁/停回调。
static void beginMcapSession(CameraAccessExtension& ext, const char* tag) {
    if (!ext.mMcapManager || !ext.mMcapManager->isMcap()) return;
    ext.mMcapManager->begin(ext.encoderBaseDir);
    // 持 mRgbEyeEncoderMutex 与回调/worker 侧同锁串行化（不依赖时序论证，
    // 与 startEncoders/stopEncoder 一致）。
    {
        std::lock_guard<std::mutex> lock(ext.mRgbEyeEncoderMutex);
        if (ext.rgbEncoder) {
            if (ext.rgbEncoderSurface) {
                ext.rgbEncoderSurface->release();
                delete ext.rgbEncoderSurface;
                ext.rgbEncoderSurface = nullptr;
            }
            ext.rgbEncoder->stop();
            delete ext.rgbEncoder;
            ext.rgbEncoder = nullptr;
            LOGI("%s: SBS rgbEncoder destroyed for mcap mode", tag);
        }
    }
}

// Unified async recording-stop path. Shared by intent, controller button,
// and the 12h auto-stop guard. Tears down encoder → recorder → hand/controller
// session off the render thread, then TTS + log. Ordering (encoder before
// recorder) keeps head_pose/hand_tracking CSV rows 1:1 with metainfo.
static void stopRecordingAsync(struct engine* e, const char* reason, const char* ttsMsg) {
    LOGI("Stopping dataset recording (%s, async)...", reason);
    // Single-flight: if a teardown is already running (e.g. manual stop racing a
    // gatekeeper-timeout abort), do nothing — the first one owns the teardown and
    // the raw encoder pointers. Prevents double-delete in stopEncoder().
    if (e->recordingStopRequested.exchange(true, std::memory_order_acq_rel)) {
        LOGI("stopRecordingAsync (%s): teardown already in progress, skipping", reason);
        return;
    }
    // A1: capture the end-of-recording cutoff BEFORE tearing down encoders, so
    // every source drops samples later than this and the dataset end aligns
    // across streams. The camera callback side drops late frames; Audio/IMU
    // drop late packets/samples during drain.
    {
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        recordingGateSetStopNs((int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    }
    e->mCameraAccessExtension.encodingEnabled = false;
    std::string reasonStr(reason);
    std::thread([e, ttsMsg, reasonStr]() {
        auto& ext = e->mCameraAccessExtension;
        ext.stopEncoder();
        // MCAP：先持锁销毁 per-eye 编码器（stop() join 输出线程，杜绝在途帧回调
        // 写入已收尾的 writer）
        { std::lock_guard<std::mutex> lock(ext.mRgbEyeEncoderMutex); ext.destroyRgbEyeEncoders(); }
        // 双锁覆盖两组 worker 的自组锁（scoped_lock 防锁序死锁）。
        { std::scoped_lock lock(ext.mTrackingEncoderMutex, ext.mCtrlEncoderMutex); ext.destroyGrayEyeEncoders(); }
        ext.encoderBaseDir.clear();

        // S1 次序是正确性约束：音频/传感器组件先 stop（audio.m4a finalize 完成 +
        // 尾部样本 drain 经 current() 写入 mcap），mMcapManager->end() 最后
        // （写静态标定 + finalizeOne 嵌附件 + finish）。end() 提前会嵌入未 finalize
        // 的 audio.m4a（附件截断+侧文件被删 → 音频丢失）并丢弃传感器尾部样本。
        e->mDatasetRecorder.stop();
        e->mControllerPoseSaver.StopSession();
        e->mHandTrackerLogic.rawDateSave->StopSession();
        if (ext.mMcapManager) { ext.mMcapManager->end(); }

        // Reset the gate BEFORE clearing the single-flight flag, and rely on the
        // start paths waiting on `recordingStopRequested` (not just stopInProgress)
        // so a follow-on recording can never arm() into a stale non-IDLE gate.
        e->mGate.reset();
        recordingGateResetStop();  // A1/A4: clear the end cutoff for next recording
        e->recordingStopRequested.store(false, std::memory_order_release);
        ttsSpeak(ttsMsg);
        LOGI("%s: async encoder + recorder stop completed", reasonStr.c_str());
    }).detach();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ssnwt_helloxr_VrNativeActivity_nativeStartRecording(JNIEnv *env, jobject thiz) {
    if (g_engine && !g_engine->mDatasetRecorder.isRecording()) {
        // Storage guard: refuse to start below 1 GiB free.
        int64_t avail = getAvailableBytes(storagePath);
        if (avail >= 0 && avail < MIN_FREE_BYTES) {
            LOGW("nativeStartRecording: insufficient storage (%lld bytes free)", (long long)avail);
            ttsSpeak("存储空间已满，无法录制");
            return;
        }
        // Wait for any async stop (encoder + recorder) to complete before starting.
        // recordingStopRequested stays true until the teardown thread has reset the
        // gate to IDLE, so this also guarantees we never arm() into a stale gate.
        int waitCount = 0;
        while (g_engine->mCameraAccessExtension.stopInProgress.load() ||
               g_engine->mDatasetRecorder.isRecording() ||
               g_engine->recordingStopRequested.load()) {
            usleep(10000); // 10ms
            if (++waitCount % 100 == 0) {
                LOGW("nativeStartRecording: waiting for stopEncoder to finish (%d ms)", waitCount * 10);
            }
            if (waitCount > 300) { // 3s timeout
                LOGE("nativeStartRecording: timed out waiting for stopEncoder");
                return;
            }
        }
        LOGI("Start recording via intent");
        g_engine->recordingStartTime = std::chrono::steady_clock::now();
        g_engine->autoStopRequested = false;
        recordingGateResetStop();  // A4: fresh recording starts with no end cutoff
        g_engine->mGate.arm();
        injectDatasetFormat(g_engine);   // provider 重建+注入（须在 start() 前，见函数注释）
        g_engine->mDatasetRecorder.start();
        g_engine->mCameraAccessExtension.encoderBaseDir = g_engine->mDatasetRecorder.getDatasetDir();
        beginMcapSession(g_engine->mCameraAccessExtension, "nativeStartRecording");
        g_engine->mCameraAccessExtension.restartGrayscaleEncoders();
        // IMU calibration sidecar (device-global; no camera context needed)
        {
            auto& ext = g_engine->mCameraAccessExtension;
            if (sxr_camera_api_is_valid(&ext.api) && ext.api.get_imu_calibration) {
                SXR::SxrImuCalibration calib{};
                if (sxr_camera_get_imu_calibration(&ext.api, &calib) == 0 && calib.valid) {
                    ext.saveImuCalibration(ext.encoderBaseDir + "/imu_calibration.json", calib);
                } else {
                    LOGW("IMU calibration unavailable; imu_calibration.json not written");
                }
            }
        }
        g_engine->mCameraAccessExtension.encodingEnabled = true;
        g_engine->mCameraAccessExtension.encodersStopped = false;
        g_engine->mCameraAccessExtension.precreateEncodersAsync();  // 后台预创建编码器，免回调 stall
        g_engine->mCameraAccessExtension.cameraParamsSavedRgb = false;
        g_engine->mCameraAccessExtension.cameraParamsSavedTracking = false;
        g_engine->mCameraAccessExtension.cameraParamsSavedCtrl = false;
        {
            std::lock_guard<std::mutex> lock(g_engine->alignedSnapshot.mutex);
            g_engine->alignedSnapshot.headPose.valid = false;
            g_engine->alignedSnapshot.leftHand.active = false;
            g_engine->alignedSnapshot.rightHand.active = false;
            g_engine->alignedSnapshot.rgbFrameCount = 0;
        }
        g_engine->controllerPoseRing.clear();
        g_engine->controllerPoseInactiveRowSaved = false;
        g_engine->handTrackingInactiveRowSaved = false;
        g_engine->poseHandRing.clear();
        // Propagate BOOTTIME→REALTIME offset to non-lazy-init components
        g_engine->propagateTimeOffset();
        // Always start both CSV sessions — only active data sources will write rows.
        g_engine->mControllerPoseSaver.StartSession(
            g_engine->mDatasetRecorder.getControllerPoseCsvPath());
        g_engine->mHandTrackerLogic.rawDateSave->StartNewSession(
            g_engine->mDatasetRecorder.getHandTrackingCsvPath());
        ttsSpeak("开始录制");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_ssnwt_helloxr_VrNativeActivity_nativeStopRecording(JNIEnv *env, jobject thiz) {
    if (g_engine && g_engine->mDatasetRecorder.isRecording()) {
        // Stop encoder first, then stop recorder in the same async thread.
        // This ordering guarantees that saveAlignedSensorData (called from the
        // render thread during encoder submission) completes before the recorder
        // is torn down, so head_pose / hand_tracking CSV rows stay 1:1 with mett.
        stopRecordingAsync(g_engine, "intent", "录制已保存");
    }
}


// CameraInfoPanel implementation (needs complete engine type)
void CameraInfoPanel::update(struct engine* engine) {
    auto& cam = engine->mCameraAccessExtension;

    // Only rebuild texture every 30 frames (FPS changes ~1/sec at 30fps)
    frameCounter++;
    if (frameCounter % 30 != 0 && texture != 0) return;

    const char* names[6] = {"RGB-L", "RGB-R", "CV-TL", "CV-TR", "CV-BL", "CV-BR"};
    uint32_t resW[6] = {cam.rgbFrameWidths[0], cam.rgbFrameWidths[1], cam.cvFrameWidths[0], cam.cvFrameWidths[1],
                        cam.cvFrameWidths[2], cam.cvFrameWidths[3]};
    uint32_t resH[6] = {cam.rgbFrameHeights[0], cam.rgbFrameHeights[1], cam.cvFrameHeights[0], cam.cvFrameHeights[1],
                        cam.cvFrameHeights[2], cam.cvFrameHeights[3]};
    float fps[6] = {cam.getRgbFps(0), cam.getRgbFps(1),
                    cam.getCvFps(0), cam.getCvFps(1), cam.getCvFps(2), cam.getCvFps(3)};

    // Build 6-line text
    std::string allText;
    for (int i = 0; i < 6; i++) {
        char buf[128];
        if (resW[i] > 0) {
            snprintf(buf, sizeof(buf), "%s %ux%u %.0ffps", names[i], resW[i], resH[i], fps[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s %.0ffps", names[i], fps[i]);
        }
        if (i > 0) allText += '\n';
        allText += buf;
    }

    if (allText == cachedText && texture != 0) return;
    cachedText = allText;

    // Generate multi-line text texture
    const int scale = 2;
    int charW = 8 * scale;
    int charH = 8 * scale;
    int lineSpacing = 2 * scale;

    // Find max line width
    int maxLineLen = 0;
    int lineCount = 0;
    size_t pos = 0;
    while (pos < allText.length()) {
        size_t eol = allText.find('\n', pos);
        int len = (eol == std::string::npos) ? (int)(allText.length() - pos) : (int)(eol - pos);
        if (len > maxLineLen) maxLineLen = len;
        lineCount++;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    if (lineCount == 0) return;

    uint32_t imgW = (uint32_t)(maxLineLen * charW + 8 * scale);  // padding each side
    uint32_t imgH = (uint32_t)(lineCount * (charH + lineSpacing) - lineSpacing + 4 * scale);

    std::vector<uint8_t> pixels(imgW * imgH * 4, 0);
    // Fill with semi-transparent black background
    for (uint32_t p = 0; p < imgW * imgH; p++) {
        pixels[p * 4 + 3] = 180;  // alpha
    }

    // Render each line using the bitmap font
    pos = 0;
    int lineIdx = 0;
    while (pos < allText.length()) {
        size_t eol = allText.find('\n', pos);
        std::string line = (eol == std::string::npos) ? allText.substr(pos) : allText.substr(pos, eol - pos);

        int baseX = 4 * scale;  // left padding
        int baseY = 2 * scale + lineIdx * (charH + lineSpacing);

        for (size_t ci = 0; ci < line.length(); ci++) {
            int ch = (unsigned char)line[ci];
            if (ch < 32 || ch > 127) ch = 32;
            const uint8_t* glyph = FONT8X8[ch - 32];
            int cx = baseX + (int)ci * charW;
            for (int gy = 0; gy < 8; gy++) {
                uint8_t row = glyph[gy];
                for (int gx = 0; gx < 8; gx++) {
                    if (row & (0x80 >> gx)) {
                        // Fill scale x scale block for each glyph pixel
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                int px = cx + gx * scale + sx;
                                int py = baseY + gy * scale + sy;
                                if (px < (int)imgW && py < (int)imgH) {
                                    int idx = (py * imgW + px) * 4;
                                    pixels[idx + 0] = 0;      // R
                                    pixels[idx + 1] = 255;    // G
                                    pixels[idx + 2] = 0;      // B
                                    pixels[idx + 3] = 255;    // A
                                }
                            }
                        }
                    }
                }
            }
        }

        lineIdx++;
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }

    // Upload texture
    if (texture) glDeleteTextures(1, &texture);
    glGenTextures(1, &texture);
    if (texture == 0) return;  // GL context lost
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, imgW, imgH);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imgW, imgH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texWidth = imgW;
    texHeight = imgH;

    // Create geometry: wide flat quad below cameras
    // Flip V coords (0↔1) to counteract vertex shader's (1-y) flip
    float worldW = 2.5f;
    float worldH = worldW * (float)imgH / (float)imgW;
    if (geometry) delete geometry;

    VertexLayoutPos3Uv2 verts[4];
    uint32_t indices[6] = {0, 2, 1, 1, 2, 3};
    verts[0] = {{-worldW/2,  worldH/2, 0}, {0, 1}};
    verts[1] = {{ worldW/2,  worldH/2, 0}, {1, 1}};
    verts[2] = {{-worldW/2, -worldH/2, 0}, {0, 0}};
    verts[3] = {{ worldW/2, -worldH/2, 0}, {1, 0}};
    QtiGL::ProgramAttribute attribs[2] = {
        {QtiGL::kPosition,  3, GL_FLOAT, false, sizeof(VertexLayoutPos3Uv2), 0},
        {QtiGL::kTexcoord0, 2, GL_FLOAT, false, sizeof(VertexLayoutPos3Uv2), 12}
    };
    geometry = new QtiGL::Geometry();
    geometry->Initialize(attribs, 2, indices, 6, verts, 4 * sizeof(VertexLayoutPos3Uv2), 4);
    if (geometry->GetVaoId() == 0) {  // GL context lost during init
        delete geometry;
        geometry = nullptr;
        return;
    }
}

/**
 * Initializes OpenXR
 */
static int engine_init_openxr(struct engine *engine)
{
    AppCommon::app_query_layers_and_extensions(engine);

    XrInstanceCreateInfoAndroidKHR instanceCreateInfoAndroidKHR;
    instanceCreateInfoAndroidKHR.type =
            XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    instanceCreateInfoAndroidKHR.next = nullptr;
    instanceCreateInfoAndroidKHR.applicationVM =
            (void *)engine->app->activity->vm;
    instanceCreateInfoAndroidKHR.applicationActivity =
            (void *)engine->app->activity->clazz;

    // TODO: should not hard-code these ideally
    const char *const enabledExtensions[] = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
            XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
            XR_EXT_HAND_TRACKING_EXTENSION_NAME,
            XR_MSFT_HAND_TRACKING_MESH_EXTENSION_NAME,
            XR_QCOM_HAND_TRACKING_GESTURE_EXTENSION_NAME,
            "XR_EXT_hand_interaction",
            "XR_EXT_palm_pose",
            "XR_MSFT_hand_interaction"};
    std::vector<const char*> enabledApiLayerNames;
    enabledApiLayerNames.push_back("XR_APILAYER_QCOM_retina_tracking");
    enabledApiLayerNames.push_back("XR_APILAYER_QCOM_handtracking");
    XrInstanceCreateInfo instanceCreateInfo = {
            .type = XR_TYPE_INSTANCE_CREATE_INFO,
            .next = &instanceCreateInfoAndroidKHR,
            .createFlags = 0,
            .applicationInfo =
                    {
                            .applicationName = "OpenXR MSAA Sample",
                            .engineName = "",
                            .applicationVersion = 1,
                            .engineVersion = 0,
                            .apiVersion = XR_CURRENT_API_VERSION,
                    },
            .enabledApiLayerCount = static_cast<uint32_t>(enabledApiLayerNames.size()),
            .enabledApiLayerNames = enabledApiLayerNames.data(),
            .enabledExtensionCount =
                    sizeof(enabledExtensions) / sizeof(*enabledExtensions),
            .enabledExtensionNames = enabledExtensions,
    };
    AppCommon::app_create_instance(&instanceCreateInfo, engine);

    AppCommon::app_get_system_prop(engine);

    AppCommon::app_enum_view_configuration(engine);
    engine->width = engine->state.viewConfigs[0].recommendedImageRectWidth;
    engine->height =
            engine->state.viewConfigs[0].recommendedImageRectHeight;

    // Create XR session
    assert(!engine->state.xrSession);
    XrGraphicsBindingOpenGLESAndroidKHR gfxBinding = {
            .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
            .next = nullptr,
            .display = engine->display,
            .config = engine->config,
            .context = engine->context};
    XrSessionCreateInfo createInfo = {.type = XR_TYPE_SESSION_CREATE_INFO,
                                      .next = &gfxBinding,
                                      .systemId = engine->state.xrSysId};
    AppCommon::app_create_session(&createInfo, engine);

    // initialize space
    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    referenceSpaceCreateInfo.poseInReferenceSpace = idPose;
    referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    AppCommon::app_create_space(&referenceSpaceCreateInfo, engine);

    XrReferenceSpaceCreateInfo viewSpaceInfo{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.poseInReferenceSpace = idPose;
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    AppCommon::app_create_space(&viewSpaceInfo, engine);

    // Create RootSpace for hand tracking
    PFN_xrCreateRootSpaceQCOM createRootSpaceQCOM{};
    XrResult result = xrGetInstanceProcAddr(engine->state.xrInstance,"xrCreateRootSpaceQCOM",reinterpret_cast<PFN_xrVoidFunction*>(&createRootSpaceQCOM));
    if (result == XR_SUCCESS) {
        XrRootSpaceCreateInfoQCOM rootSpaceCreateInfo{};
        rootSpaceCreateInfo.poseInSpace.orientation.w = 1.0f;
        rootSpaceCreateInfo.poseInSpace.position.x = 0.0f;
        rootSpaceCreateInfo.poseInSpace.position.y = 0.0f;
        rootSpaceCreateInfo.poseInSpace.position.z = 0.0f;
        rootSpaceCreateInfo.poseInSpace.orientation.x = 0.0f;
        rootSpaceCreateInfo.poseInSpace.orientation.y = 0.0f;
        rootSpaceCreateInfo.poseInSpace.orientation.z = 0.0f;
        result = createRootSpaceQCOM(engine->state.xrSession, &rootSpaceCreateInfo, &engine->state.xrRootSpace);
        if (result == XR_SUCCESS) {
            engine->useRootSpace = true;
            LOGI("createRootSpaceQCOM succeed, useRootSpace=true");
        }
    }

    engine->inputPtr = std::make_unique<Input>();
    engine->inputPtr->Init(engine->state.xrInstance, engine->state.xrSession,
                           engine->useRootSpace ? engine->state.xrRootSpace : engine->state.xrLocalSpace);

    return engine_init_xr_swapchains(engine);

}

/**
 * Shutdown OpenXR
 */
static void engine_shutdown_openxr(struct engine *engine)
{
    XrResult result = AppCommon::app_destroy_space(engine->state.xrLocalSpace);
    if (XR_SUCCESS != result) {
        LOGW("Destroy XR local space failed: %d", result);
        assert(0);
    }

    result = AppCommon::app_destroy_space(engine->state.xrViewSpace);
    if (XR_SUCCESS != result) {
        LOGW("Destroy XR view space failed: %d", result);
        assert(0);
    }

    result = AppCommon::app_destroy_session(engine->state.xrSession);
    if (XR_SUCCESS != result) {
        LOGW("Destroy XR session failed: %d", result);
        assert(0);
    }

    result = AppCommon::app_destroy_instance(engine);
    if (XR_SUCCESS != result) {
        LOGW("Destroy XR instance failed: %d", result);
        assert(0);
    }
}

/**
 * Create XR swapchains
 */
static int engine_init_xr_swapchains(struct engine *engine) {
    AppCommon::app_enum_sc_format(engine);

    // Looking for multisample extension
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEIMGPROC
            glFramebufferTexture2DMultisampleEXT = nullptr;
    glFramebufferTexture2DMultisampleEXT =
            (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEIMGPROC) eglGetProcAddress(
                    "glFramebufferTexture2DMultisampleEXT");
    if (!glFramebufferTexture2DMultisampleEXT) {
        LOGW("Couldn't get function pointer to "
             "glFramebufferTexture2DMultisampleEXT()!");
        assert(0);
        return 1;
    }

    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEIMGPROC
            glRenderbufferStorageMultisampleEXT =
            (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEIMGPROC)
                    eglGetProcAddress(
                            "glRenderbufferStorageMultisampleEXT");
    if (!glRenderbufferStorageMultisampleEXT) {
        LOGE("Couldn't get function pointer to "
             "glRenderbufferStorageMultisampleEXT()!");
        return false;
    }

    // Create swapchain with different sample count and cached in map
    uint32_t samples = engine->currentSampleCount;
    {
        StereoSwapchain stereoSwapchain;
        stereoSwapchain.eyeSwapchain.resize(engine->state.viewCount);
        std::vector<uint32_t> swapchainLengths;
        swapchainLengths.resize(engine->state.viewCount);
        uint32_t maxSwapchainLength = 0;

        for (uint32_t eye = 0; eye < engine->state.viewCount; ++eye) {
            auto &swapchain = stereoSwapchain.eyeSwapchain[eye];
            XrSwapchainCreateInfo swapchainCreateInfo = {
                    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                    .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                  XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
                    .createFlags = 0,
                    .format = GL_SRGB8_ALPHA8,
                    .sampleCount = samples,
                    .width = engine->width,
                    .height = engine->height,
                    .faceCount = 1,
                    .arraySize = 1,
                    .mipCount = 1,
                    .next = nullptr,
            };

            XrResult result = xrCreateSwapchain(engine->state.xrSession,
                                                &swapchainCreateInfo,
                                                &swapchain.xrSwapchain);
            if (XR_FAILED(result)) {
                LOGW("xrCreateSwapchain failed");
                assert(0);
                return 1;
            }

            result = xrEnumerateSwapchainImages(
                    swapchain.xrSwapchain, 0, &swapchainLengths[eye], nullptr);
            if (XR_FAILED(result)) {
                LOGW("xrEnumerateSwapchainImages failed");
                assert(0);
                return 1;
            }

            if (swapchainLengths[eye] > maxSwapchainLength)
                maxSwapchainLength = swapchainLengths[eye];

            swapchain.xrImages.resize(swapchainLengths[eye],
                                      {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            swapchain.fbos.resize(swapchainLengths[eye]);
            swapchain.dbos.resize(swapchainLengths[eye]);

            result = xrEnumerateSwapchainImages(
                    swapchain.xrSwapchain, swapchainLengths[eye],
                    &swapchainLengths[eye],
                    (XrSwapchainImageBaseHeader *) &swapchain.xrImages[0]);

            if (XR_SUCCESS != result) {
                LOGW("xrEnumerateSwapchainImages failed");
                assert(0);
                return 1;
            }

            for (uint32_t index = 0; index < swapchainLengths[eye]; ++index) {
                // Create depth buffer
                GL(glGenRenderbuffers(1, &swapchain.dbos[index]));
                GL(glBindRenderbuffer(GL_RENDERBUFFER, swapchain.dbos[index]));
                GL(glRenderbufferStorageMultisampleEXT(
                        GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT16,
                        engine->width, engine->height));
                GL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

                // Create MSAA fbo
                GL(glGenFramebuffers(1, &swapchain.fbos[index]));
                GL(glBindFramebuffer(GL_FRAMEBUFFER, swapchain.fbos[index]));
                GL(glFramebufferRenderbuffer(
                        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                        swapchain.dbos[index]));
                LOGI("glFramebufferTexture2DMultisampleEXT index:%d sample: %d",
                     index, samples);
                GL(glFramebufferTexture2DMultisampleEXT(
                        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                        swapchain.xrImages[index].image, 0, samples));

                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                // check frame buffer status
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    LOGE("framebuffer is incomplete, status: %d! Error code %d",
                         status, glGetError());
                    assert(0);
                    return 1;
                }
            }
        }

        LOGI("Insert to table, sample :%d", samples);
        engine->swapchainMap[samples] = std::move(stereoSwapchain);
    }

// Projection
    {
        XrSwapchainCreateInfo swapchainCreateInfo = {
                .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                              XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
                .createFlags = 0,
                .format = GL_SRGB8_ALPHA8,//GL_SRGB8_ALPHA8,//GL_RGBA8,
                .sampleCount = 1,
                .width = engine->quadImageWidth,
                .height = engine->quadImageHeight,
                .faceCount = 1,
                .arraySize = 1,
                .mipCount = 1,
                .next = nullptr,
        };

        AppCommon::app_create_swapchain(
                &swapchainCreateInfo, &engine->state.xrSession, &engine->quadSwapchain);

        AppCommon::app_create_swapchain(
                &swapchainCreateInfo, &engine->state.xrSession, &engine->quadSwapchain2);
    }
    GL();
    // Init depth buffer
    glGenTextures(1, &engine->depthBuffer);
    glBindTexture(GL_TEXTURE_2D, engine->depthBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16,
                 engine->quadImageWidth,
                 engine->quadImageHeight, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (engine->depthBuffer == 0) {
        LOGE(LOG_TAG, "Failed to init depth buffer");
        assert(0);
    }
    GL();
    LOGI("render_gles,Init depth buffer");
//    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_PROTECTED_EXT, GL_TRUE);
    return 0;
}

/**
 * Destroy XR swapchains
 */
static int engine_destroy_xr_swapchains(struct engine *engine)
{
    for (auto it = engine->swapchainMap.begin();
         it != engine->swapchainMap.end(); ++it) {
        for (auto &swapchain : it->second.eyeSwapchain) {
            GL(glDeleteFramebuffers(swapchain.fbos.size(),
                                    swapchain.fbos.data()));
            GL(glDeleteRenderbuffers(swapchain.dbos.size(),
                                     swapchain.dbos.data()));
            if (XR_FAILED(xrDestroySwapchain(swapchain.xrSwapchain))) {
                LOGW("xrDestroySwapchain failed");
                assert(0);
                return 1;
            }
        }
    }
    engine->swapchainMap.clear();

    return 0;
}

/**
 * Initialize an EGL context for the current display.
 */
static int engine_init_display(struct engine *engine)
{
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLint numConfig;

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(display != EGL_NO_DISPLAY);

    EGLBoolean res = eglInitialize(display, nullptr, nullptr);
    assert(res == EGL_TRUE);

    EGLint const configAttrs[] = {
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            // This is tricky, you can't use glGetIntegerv until egl context was
            // created. Maybe create a temp context and get max sample count
            // first.
            EGL_SAMPLES, EGL_SAMPLE_COUNT, EGL_NONE};

    res = eglChooseConfig(display, configAttrs, &config, 1, &numConfig);
    LOGI("numConfig: %d", numConfig);
    assert(res == EGL_TRUE);

    EGLint const contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                     EGL_PROTECTED_CONTENT_EXT, false,
                                     EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
    assert(context != EGL_NO_CONTEXT);

    res = eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
    assert(res == EGL_TRUE);

    engine->config = config;
    engine->context = context;
    engine->display = display;

    return 0;
}
uint32_t GetDepthTexture(uint32_t colorTexture,int TextureWidth,int TextureHeight)
{
    // If a depth-stencil view has already been created for this back-buffer, use it.
    auto depthBufferIt = m_colorToDepthMap.find(colorTexture);
    if (depthBufferIt != m_colorToDepthMap.end())
    {
        return depthBufferIt->second;
    }
    GLuint glType = GL_TEXTURE_2D;
    GL(glBindTexture(glType, colorTexture));

    uint32_t depthTexture;
    GL(glGenTextures(1, &depthTexture));
    GL(glBindTexture(glType, depthTexture));
    GL(glTexParameteri(glType, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL(glTexParameteri(glType, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL(glTexParameteri(glType, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL(glTexParameteri(glType, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    if (glType == GL_TEXTURE_2D_ARRAY)
    {
        GL(glTexStorage3D(glType, 1, GL_DEPTH_COMPONENT24, TextureWidth, TextureHeight, 2));
    }
    else
    {
        GL(glTexImage2D(glType, 0, GL_DEPTH_COMPONENT24, TextureWidth, TextureHeight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr));
    }

    m_colorToDepthMap.insert(std::make_pair(colorTexture, depthTexture));
    return depthTexture;
}

// Upload CV camera textures to GPU (only when new frames arrive)
static void ensureCvTextures(struct engine *engine) {
    // Textures are now uploaded directly by GPU in handleCVFrame callback.
    // No CPU-side upload needed. Just check if textures are ready.
    auto& cam = engine->mCameraAccessExtension;
    (void)cam;
}

// Composite all camera frames (4 grayscale) into a single texture
static void compositeAllCameras(struct engine *engine, uint32_t targetTexture) {
    auto& cam = engine->mCameraAccessExtension;

    // Check if tracking cameras have frames available
    bool hasTracking = cam.trackingFrameReady;
    bool hasCtrl = cam.ctrlFrameReady;

    if (!hasTracking && !hasCtrl) {
        // No camera frames available yet, clear with black
        glBindTexture(GL_TEXTURE_2D, targetTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, engine->quadImageWidth, engine->quadImageHeight,
                       GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }

    // Allocate buffer for composite texture
    uint32_t* compositeData = (uint32_t*)malloc(engine->quadImageWidth * engine->quadImageHeight * sizeof(uint32_t));
    if (!compositeData) {
        LOGE("Failed to allocate composite buffer");
        return;
    }
    memset(compositeData, 0, engine->quadImageWidth * engine->quadImageHeight * sizeof(uint32_t));

    // Create a 2x2 grid layout for cameras:
    // +------------------+------------------+
    // |   GRAY_LEFT      |  GRAY_RIGHT      |
    // |      (0)         |       (1)        |
    // +------------------+------------------+
    // |   GRAY_LEFT_UP   |  GRAY_RIGHT_UP   |
    // |      (2)         |       (3)        |
    // +------------------+------------------+

    int gridCols = 2;
    int gridRows = 2;
    int cellWidth = engine->quadImageWidth / gridCols;
    int cellHeight = engine->quadImageHeight / gridRows;

    // Helper lambda to copy tracking frame to grid position
    auto copyTrackingToGrid = [&](const CameraAccessExtension::TrackingFrameData& frame,
                                   int gridCol, int gridRow) {
        if (frame.pixelData.empty()) return;

        uint32_t camWidth = frame.frameInfo.width;
        uint32_t camHeight = frame.frameInfo.height;
        if (camWidth == 0 || camHeight == 0) return;

        int startX = gridCol * cellWidth;
        int startY = gridRow * cellHeight;

        for (int y = 0; y < cellHeight && y < (int)camHeight; y++) {
            for (int x = 0; x < cellWidth && x < (int)camWidth; x++) {
                int srcX = x * camWidth / cellWidth;
                int srcY = y * camHeight / cellHeight;
                uint8_t pix = frame.pixelData[srcY * camWidth + srcX];
                uint32_t rgba = (0xFF000000 | (pix<<16) | (pix<<8) | pix);
                int destX = startX + x;
                int destY = startY + (cellHeight - 1 - y); // Flip Y
                compositeData[destY * engine->quadImageWidth + destX] = rgba;
            }
        }
    };

    // Copy tracking frames (GRAY_LEFT=0, GRAY_RIGHT=1)
    if (hasTracking) {
        std::lock_guard<std::mutex> lock(cam.trackingFrameMutex);
        copyTrackingToGrid(cam.trackingFrames[0], 0, 0);  // GRAY_LEFT - top left
        copyTrackingToGrid(cam.trackingFrames[1], 1, 0);  // GRAY_RIGHT - top right
    }

    // Copy ctrl frames (GRAY_LEFT_UP=2, GRAY_RIGHT_UP=3)
    if (hasCtrl) {
        std::lock_guard<std::mutex> lock(cam.trackingFrameMutex);
        copyTrackingToGrid(cam.ctrlFrames[0], 0, 1);  // GRAY_LEFT_UP - bottom left
        copyTrackingToGrid(cam.ctrlFrames[1], 1, 1);  // GRAY_RIGHT_UP - bottom right
    }

    // Upload composite texture
    glBindTexture(GL_TEXTURE_2D, targetTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, engine->quadImageWidth, engine->quadImageHeight,
                   GL_RGBA, GL_UNSIGNED_BYTE, compositeData);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOGE("GL Error uploading composite texture: 0x%x", err);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    free(compositeData);
}

static void draw(struct engine *engine,uint32_t imgIndex){
    GL(glBindFramebuffer(GL_FRAMEBUFFER,
                      engine->quadSwapchain.glFramebuffers[imgIndex]));
    uint32_t colorTexture = engine->quadSwapchain.xrImages[imgIndex].image;
    GL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                              colorTexture, 0));
    auto depthBufferIt = GetDepthTexture(colorTexture,engine->quadImageWidth,engine->quadImageHeight);
    GL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                              depthBufferIt, 0));

    GLint scissor[4];
    scissor[0] = 0;
    scissor[1] = 0;
    scissor[2] = engine->quadImageWidth;
    scissor[3] = engine->quadImageHeight;
    GL(glEnable(GL_SCISSOR_TEST));
    GL(glViewport(0, 0, engine->quadImageWidth, engine->quadImageHeight));
    GL(glScissor(scissor[0], scissor[1], scissor[2], scissor[3]));
    GL(glClearColor(0.f, 0.f, 0.f, 0.f));
    GL(glClear(GL_COLOR_BUFFER_BIT| GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
    GL(glDisable(GL_SCISSOR_TEST));
    GL(glFrontFace(GL_CW));
    GL(glEnable(GL_CULL_FACE));
    GL(glCullFace(GL_FRONT));
    GL(glEnable(GL_DEPTH_TEST));

    mNotificationShader->Bind();
    LOGI("render_gles,srcTex:%d",engine->testTexture);
    mNotificationShader->SetUniformSampler("srcTex", engine->testTexture, GL_TEXTURE_2D, 0);
    mNotificationMesh.Submit();
    mNotificationShader->Unbind();
    GL();
    GL(glBindFramebuffer(GL_FRAMEBUFFER,0));
}
static void  engine_draw_layer(struct engine *engine){
    XrSwapchainImageAcquireInfo swapchainImageAcquireInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
    uint32_t bufferIndex;
    XrResult result = xrAcquireSwapchainImage(engine->quadSwapchain.xrSwapchain,
                                              &swapchainImageAcquireInfo, &bufferIndex);

    if (XR_SUCCESS != result) {
        LOGW("xrAcquireSwapchainImage failed, %d", result);
    }

    XrSwapchainImageWaitInfo swapchainImageWaitInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .next = nullptr,
            .timeout = 1000};
    result = xrWaitSwapchainImage(engine->quadSwapchain.xrSwapchain,
                                  &swapchainImageWaitInfo);

    if (XR_SUCCESS != result) {
        LOGW("xrWaitSwapchainImage failed %d", result);
    }

    // Composite all camera frames to the quad swapchain
    uint32_t colorTexture = engine->quadSwapchain.xrImages[bufferIndex].image;
    compositeAllCameras(engine, colorTexture);

    XrSwapchainImageReleaseInfo swapchainImageReleaseInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
    result = xrReleaseSwapchainImage(engine->quadSwapchain.xrSwapchain,
                                     &swapchainImageReleaseInfo);

    if (XR_SUCCESS != result) {
        LOGW("xrReleaseSwapchainImage failed %d", result);
    }
}
static void  engine_draw_layer2(struct engine *engine){
    XrSwapchainImageAcquireInfo swapchainImageAcquireInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, .next = nullptr};
    uint32_t bufferIndex;
    XrResult result = xrAcquireSwapchainImage(engine->quadSwapchain2.xrSwapchain,
                                              &swapchainImageAcquireInfo, &bufferIndex);

    if (XR_SUCCESS != result) {
        LOGW("xrAcquireSwapchainImage failed, %d", result);
    }

    XrSwapchainImageWaitInfo swapchainImageWaitInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .next = nullptr,
            .timeout = 1000};
    result = xrWaitSwapchainImage(engine->quadSwapchain2.xrSwapchain,
                                  &swapchainImageWaitInfo);

    if (XR_SUCCESS != result) {
        LOGW("xrWaitSwapchainImage failed %d", result);
    }

//    draw(engine,bufferIndex);
    uint32_t colorTexture = engine->quadSwapchain2.xrImages[bufferIndex].image;
//    GL_APICALL void GL_APIENTRY glCopyTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    glCopyImageSubData(engine->testTexture,GL_TEXTURE_2D,0,0,0,0,colorTexture,GL_TEXTURE_2D,0,0,0,0,engine->quadImageWidth,
                       engine->quadImageHeight,1);

    XrSwapchainImageReleaseInfo swapchainImageReleaseInfo = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, .next = nullptr};
    result = xrReleaseSwapchainImage(engine->quadSwapchain2.xrSwapchain,
                                     &swapchainImageReleaseInfo);

    if (XR_SUCCESS != result) {
        LOGW("xrReleaseSwapchainImage failed %d", result);
    }
}
/**
 * Render scene.
 */
static void engine_draw_frame(struct engine *engine,
                              const uint32_t viewIndex,
                              const uint32_t imgIndex,
                              const XrView &xrView)
{
    assert(engine->display);
    auto &stereoSwapchain = engine->swapchainMap[engine->currentSampleCount];
    auto &swapchain = stereoSwapchain.eyeSwapchain[viewIndex];
    GL(glBindFramebuffer(GL_FRAMEBUFFER, swapchain.fbos[imgIndex]));

    GL(glEnable(GL_SCISSOR_TEST));
    GL(glEnable(GL_DEPTH_TEST));
    GL(glEnable(GL_CULL_FACE));
    GL(glDepthFunc(GL_LESS));
    GL(glDepthMask(GL_TRUE));

    GL(glViewport(0, 0, engine->width, engine->height));
    GL(glScissor(0, 0, engine->width, engine->height));
    GL(glClearColor(0.1f, 0.1f, 0.1f, 0.0f));
    GL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    glm::mat4 eyeProjMat, eyeViewMat;

    XrMatrix4x4f result;
    XrMatrix4x4f_CreateProjectionFov(&result, GRAPHICS_OPENGL_ES, xrView.fov,
                                     0.05f, 100.f);
    AppCommon::array2matrix(result, eyeProjMat);
    glm::mat4 rot = glm::mat4_cast(
            glm::fquat(xrView.pose.orientation.w, xrView.pose.orientation.x,
                       xrView.pose.orientation.y, xrView.pose.orientation.z));
    glm::mat4 trans =
            glm::translate(glm::mat4(1.0f), glm::vec3(xrView.pose.position.x,
                                                      xrView.pose.position.y,
                                                      xrView.pose.position.z));
    eyeViewMat = trans * rot;
    eyeViewMat = glm::inverse(eyeViewMat);

    engine->cubeShader->Bind();
    engine->cubeShader->SetUniformMat4("projectionMatrix", eyeProjMat);
    engine->cubeShader->SetUniformMat4("viewMatrix", eyeViewMat);
//    if(!engine->mCameraAccessExtension.frames.empty()){
//        engine->cubeShader->SetUniformSampler("srcTex", engine->mCameraAccessExtension.frames[0].shareTexture->getBindTexture(),
//                                              GL_TEXTURE_2D, 0);
//    }
//    engine->cubeShader->SetUniformSampler("srcTex", engine->testTexture,
//                                          GL_TEXTURE_2D, 0);
    glm::vec3 eyePos =
            glm::vec3(-eyeViewMat[3][0], -eyeViewMat[3][1], -eyeViewMat[3][2]);
    engine->cubeShader->SetUniformVec3("eyePos", eyePos);

    // Render cube scene
//    for (size_t i = 0; i < engine->cubeMatrices.size(); ++i) {
//        engine->cubeShader->SetUniformMat4("modelMatrix",
//                                           engine->cubeMatrices[i]);
//        engine->cubeShader->SetUniformVec3("modelColor", engine->cubeColors[i]);
//        engine->cube.Submit();
//    }

    glm::vec3 color = glm::vec3(1.0f,1.0f,1.0f);
    engine->cubeShader->SetUniformVec3("modelColor", color);

    // Render head-locked info panel (follows gaze, stays in front)
    engine->cubeShader->SetUniform1i("useTexture", 1);
    gInfoPanel.update(engine);
    gInfoPanel.render(engine->cubeShader);

    engine->cubeShader->Unbind();
    GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

//    engine_draw_layer(engine);
//    engine_draw_layer2(engine);
}

static std::string read_text_file(const std::string &file)
{
    std::stringstream ss;
    std::ifstream ifs(file);
    if (ifs) {
        ss << ifs.rdbuf();
    } else {
        LOGW("Read %s failed", file.c_str());
    }

    return ss.str();
}

static void engine_create_cube(QtiGL::Geometry &geometry, float width)
{
    // Create attributes of the cube
    unsigned int numElementsPerVert = 8;
    int stride = (int)(numElementsPerVert * sizeof(float));

    std::vector<QtiGL::ProgramAttribute> attribs = {
            {QtiGL::kPosition, 3, GL_FLOAT, false, stride, 0},
            {QtiGL::kNormal, 3, GL_FLOAT, false, stride, 3 * sizeof(float)},
            {QtiGL::kTexcoord0, 2, GL_FLOAT, false, stride, 6 * sizeof(float)}};

    float halfWidth = width / 2.0f;

    // Create vertex data of the cube with position and normal data
    float cubeVerts[] = {
            // Front
            halfWidth, halfWidth, halfWidth, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
            -halfWidth, halfWidth, halfWidth, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
            -halfWidth, -halfWidth, halfWidth, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            halfWidth, -halfWidth, halfWidth, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
            // Right
            halfWidth, halfWidth, halfWidth, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            halfWidth, -halfWidth, halfWidth, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            halfWidth, -halfWidth, -halfWidth, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            halfWidth, halfWidth, -halfWidth, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
            // Top
            halfWidth, halfWidth, halfWidth, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f,
            halfWidth, halfWidth, -halfWidth, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f,
            -halfWidth, halfWidth, -halfWidth, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
            -halfWidth, halfWidth, halfWidth, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
            // Left
            -halfWidth, halfWidth, halfWidth, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
            -halfWidth, halfWidth, -halfWidth, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
            -halfWidth, -halfWidth, -halfWidth, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            -halfWidth, -halfWidth, halfWidth, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            // Bottom
            -halfWidth, -halfWidth, -halfWidth, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f,
            halfWidth, -halfWidth, -halfWidth, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f,
            halfWidth, -halfWidth, halfWidth, 0.0f, -1.0f, 0.0f, 1.0f, 1.0f,
            -halfWidth, -halfWidth, halfWidth, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f,
            // Back
            halfWidth, -halfWidth, -halfWidth, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f,
            -halfWidth, -halfWidth, -halfWidth, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f,
            -halfWidth, halfWidth, -halfWidth, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f,
            halfWidth, halfWidth, -halfWidth, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f

    };
    int numCubeVerts = 24;

    // Create index data of the cube
    unsigned int cubeIndices[] = {// Front
                                  0, 1, 2, 2, 3, 0,
                                  // Right
                                  4, 5, 6, 6, 7, 4,
                                  // Top
                                  8, 9, 10, 10, 11, 8,
                                  // Left
                                  12, 13, 14, 14, 15, 12,
                                  // Bottom
                                  16, 17, 18, 18, 19, 16,
                                  // Back
                                  20, 21, 22, 22, 23, 20

    };
    int numCubeIndices = 36;

    geometry.Initialize(attribs.data(), attribs.size(), cubeIndices,
                        numCubeIndices, cubeVerts,
                        numCubeVerts * numElementsPerVert * sizeof(float),
                        numCubeVerts);
}
void glInit() {
    VertexLayoutPos3Uv2 verts[4];

    uint32_t quadIndices[6] = {0, 2, 1, 1, 2, 3};
    int32_t indexCount = sizeof(quadIndices) / sizeof(uint32_t);

    verts[0].position[0] = -0.5f;
    verts[0].position[1] = 0.5f;
    verts[0].position[2] = 0.f;
    verts[0].texCoord[0] = 0.f;
    verts[0].texCoord[1] = 0.f;

    verts[1].position[0] = 0.5f;
    verts[1].position[1] = 0.5f;
    verts[1].position[2] = 0.f;
    verts[1].texCoord[0] = 1.f;
    verts[1].texCoord[1] = 0.f;

    verts[2].position[0] = -0.5f;
    verts[2].position[1] = -0.5f;
    verts[2].position[2] = 0.f;
    verts[2].texCoord[0] = 0.f;
    verts[2].texCoord[1] = 1.f;

    verts[3].position[0] = 0.5f;
    verts[3].position[1] = -0.5f;
    verts[3].position[2] = 0.f;
    verts[3].texCoord[0] = 1.f;
    verts[3].texCoord[1] = 1.f;

    QtiGL::ProgramAttribute MeshAttribsPos3Uv2[2] =
            {
                    //  Index               Size    Type        Normalized      Stride                      Offset
                    {QtiGL::kPosition,  3, GL_FLOAT, false, sizeof(VertexLayoutPos3Uv2), 0},
                    {QtiGL::kTexcoord0, 2, GL_FLOAT, false, sizeof(VertexLayoutPos3Uv2), 12}
            };

    mNotificationMesh.Initialize(
            &MeshAttribsPos3Uv2[0], 2,
            &quadIndices[0], indexCount,
            &verts[0], 4 * sizeof(VertexLayoutPos3Uv2), 4);


    const char *vNotiShaderGlsl = R"_(#version 320 es
    precision highp float;
    precision mediump int;
    layout (location = 0) in vec3 position;
    layout (location = 3) in vec2 texcoord0;
    out vec3 PSVertexColor;
    out vec2 v_Coords;
    void main() {
       gl_Position = vec4(position.x,position.y,0, 1.0);
       v_Coords = texcoord0.xy;
    }
    )_";


    const char *fNotiShaderGlsl = R"_(#version 320 es
    precision highp float;
    precision mediump int;
    uniform sampler2D srcTex;
    in vec2 v_Coords;
    out vec4 FragColor;
    void main() {
       	FragColor = texture(srcTex,v_Coords);
//        FragColor = vec4(v_Coords.x,v_Coords.y,0, 1);
    }
    )_";

    mNotificationShader = new QtiGL::Shader;
    mNotificationShader->Initialize(1, &vNotiShaderGlsl,
                                    1, &fNotiShaderGlsl,
                                    "VS", "FS");

    char texFilePath[512];
    const char *pModelTexFile = "white.ktx";
    sprintf(texFilePath, "%s/%s", storagePath,
            pModelTexFile);

    GLenum texTarget;
    QtiGL::KtxTexture texHelper;
    int texBuffSize;
    char *pTexBuffer =
            (char *) AppCommon::get_file_buffer(texFilePath, &texBuffSize);
    if (pTexBuffer == nullptr) {
        return;
    }

    QtiGL::TKTXHeader *pOutHeader = nullptr;
    QtiGL::TKTXErrorCode resultCode = texHelper.LoadKtxFromBuffer(
            pTexBuffer, texBuffSize, &quadTexture, &texTarget,
            pOutHeader, false);

    if (resultCode != QtiGL::KTX_SUCCESS || 0 == quadTexture) {
        free(pTexBuffer);
        return;
    }

    quadTextureWidth = texHelper.GetWidht();
    quadTextureHeight = texHelper.GetHeight();

    free(pTexBuffer);

    LOGW("render_gles::Init over");
}
// type = 0 不重复纹理 1重复纹理
GLuint GetTexutre(int width, int height,int nrComponents, void *pTexUint,int type = 0)
{
    XR_DEBUG("pTexUint %p", pTexUint);
    LOGE("nrComponents:%d",nrComponents);
    if (pTexUint == NULL)
            XR_DEBUG("texture pTexUint is NULL");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    unsigned int texture = -1;
    GL(glGenTextures(1, &texture));
    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, texture));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA));
//    if(type==0)
//    {
//        //不重复
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
//    }
//    else
//    {
//        //重复纹理
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
//        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
//    }
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));


    GLenum format = GL_RGBA;
    if (nrComponents == 1)
    {
        format = GL_RED;
    }
    else if (nrComponents == 3)
    {
        format = GL_RGB;
    }
    else if (nrComponents == 4)
    {
        format = GL_RGBA;
    }
//    GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pTexUint));
    GL(glTexStorage2D(GL_TEXTURE_2D,1,GL_SRGB8_ALPHA8,width,height));
    GL(glTexSubImage2D(GL_TEXTURE_2D,0,0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pTexUint));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
    return texture;
}
/**
 * Init resources for rendering scene
 */
static int engine_init_scene_resources(struct engine *engine)
{
    std::string externalDir =
            std::string(engine->app->activity->externalDataPath);

    // load shader
    {
        std::string vsFilePath = externalDir + "/model_v.glsl";
        std::string fsFilePath = externalDir + "/model_f.glsl";
        // load shader sources
        std::string vsSource = read_text_file(vsFilePath);
        if (vsSource.length() <= 0) {
            return 1;
        }

        std::string fsSource = read_text_file(fsFilePath);
        if (fsSource.length() <= 0) {
            return 1;
        }

        engine->cubeShader = new QtiGL::Shader();
        std::vector<const char *> vs = {vsSource.c_str()};
        std::vector<const char *> fs = {fsSource.c_str()};
        if (!engine->cubeShader->Initialize(vs.size(), vs.data(), fs.size(),
                                            fs.data(), vsFilePath.c_str(),
                                            fsFilePath.c_str())) {
            return 1;
        }
    }
    //load png
    {
        LOGI("load png");
        std::string textureFilePath = externalDir + "/happy.jpg";
        std::ifstream ifs(textureFilePath);
        if (!ifs) {
            return false;
        }

        std::streampos texBuffSize = ifs.tellg();
        ifs.seekg(0, std::ios::end);
        texBuffSize = ifs.tellg() - texBuffSize;
        ifs.seekg(0);

        std::vector<char> data(texBuffSize);
        ifs.read(data.data(), data.size());
        ifs.close();
        auto imgid = 0;
        int width, height, nrChannels;
        unsigned char *contnet = stbi_load_from_memory(reinterpret_cast<unsigned char *>(data.data()), data.size(), &width, &height, &nrChannels, 4);
        if (contnet) {
            LOGI("getAssets::Init2++ stbi_load ok width：%d,height:%d", width, height);
            auto imgid = 0;
            imgid = GetTexutre(width, height,nrChannels,contnet,0);

            if (imgid > 0) {
                LOGI("getAssets Load %d", imgid);
                engine->testTexture = imgid;
            }
        } else {
            LOGI("getAssets::Failed to load texture");
        }
    }
    // load texture
    {
        LOGI("load texture");
        std::string textureFilePath = externalDir + "/src.ktx";
        std::ifstream ifs(textureFilePath);
        if (!ifs) {
            return false;
        }

        std::streampos texBuffSize = ifs.tellg();
        ifs.seekg(0, std::ios::end);
        texBuffSize = ifs.tellg() - texBuffSize;
        ifs.seekg(0);

        std::vector<char> data(texBuffSize);
        ifs.read(data.data(), data.size());
        ifs.close();

        GLenum texTarget;
        QtiGL::KtxTexture texHelper;
        QtiGL::TKTXHeader pOutHeader;
        QtiGL::TKTXErrorCode result = texHelper.LoadKtxFromBuffer(
                data.data(), data.size(), &engine->cubeTexture, &texTarget,
                &pOutHeader, false);
        LOGI("texture width: %d, height: %d", pOutHeader.pixelWidth,
             pOutHeader.pixelHeight);

        if (result != QtiGL::KTX_SUCCESS || 0 == engine->cubeTexture) {
            return 1;
        }
    }

    // cube geometry
    engine_create_cube(engine->cube, 0.3f);

    // Init 2D hand overlay renderer
    engine->handOverlay.init();

    // Create cube sea around origin
    float xPos = -(CUBE_COUNT / 2);
    float yPos = -(CUBE_COUNT / 2);
    float zPos = -(CUBE_COUNT / 2);

    // rotate 45 degrees along y axis so we can see the edge
    glm::mat4 rotationMat =
            glm::rotate(glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    for (int z = 0; z < CUBE_COUNT; ++z) {
        for (int y = 0; y < CUBE_COUNT; ++y) {
            for (int x = 0; x < CUBE_COUNT; ++x) {
                engine->cubeMatrices.push_back(
                        glm::translate(
                                glm::mat4(1.0f),
                                glm::vec3(xPos + x, yPos + y, zPos + z)) *
                        rotationMat);
                engine->cubeColors.push_back(CUBE_COLORS[y]);
            }
        }
    }
    glInit();
    return 0;
}

/**
 * Destroys resources for rendering scene
 */
static void engine_destroy_scene_resources(struct engine *engine)
{
    engine->cube.Destroy();

    engine->cubeShader->Destroy();
    delete engine->cubeShader;
    engine->cubeShader = nullptr;

    glDeleteTextures(1, &engine->cubeTexture);
    engine->cubeTexture = 0;
}

// Generate timestamp string for filenames
static std::string getTimestampString() {
    auto now = std::chrono::system_clock::now();
    auto time_t_val = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_val, &tm_buf);
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return ss.str();
}

// Read RGBA pixels from a GL texture (flipped vertically for PNG)
static bool readTexturePixels(GLuint textureId, int width, int height,
                               std::vector<uint8_t>& outPixels) {
    if (textureId == 0 || width == 0 || height == 0) return false;

    GLint prevFbo;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureId, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("readTexturePixels: FBO incomplete for texture %u", textureId);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        glDeleteFramebuffers(1, &fbo);
        return false;
    }

    std::vector<uint8_t> raw(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

    // Flip vertically (OpenGL origin is bottom-left, PNG is top-left)
    int rowBytes = width * 4;
    outPixels.resize(raw.size());
    for (int y = 0; y < height; y++) {
        memcpy(outPixels.data() + y * rowBytes,
               raw.data() + (height - 1 - y) * rowBytes,
               rowBytes);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glDeleteFramebuffers(1, &fbo);
    return true;
}

// Save a stereo pair (left + right) as a single side-by-side PNG
static bool saveStereoPairAsPng(GLuint leftTex, int leftW, int leftH,
                                 GLuint rightTex, int rightW, int rightH,
                                 const std::string& filePath) {
    std::vector<uint8_t> leftPixels, rightPixels;
    bool hasLeft = readTexturePixels(leftTex, leftW, leftH, leftPixels);
    bool hasRight = readTexturePixels(rightTex, rightW, rightH, rightPixels);

    if (!hasLeft && !hasRight) return false;

    if (hasLeft && hasRight && leftW == rightW && leftH == rightH) {
        // Both eyes available, same size: stitch horizontally [left | right]
        int combinedW = leftW * 2;
        std::vector<uint8_t> combined(combinedW * leftH * 4);
        int srcRowBytes = leftW * 4;
        int dstRowBytes = combinedW * 4;
        for (int y = 0; y < leftH; y++) {
            memcpy(combined.data() + y * dstRowBytes,
                   leftPixels.data() + y * srcRowBytes, srcRowBytes);
            memcpy(combined.data() + y * dstRowBytes + srcRowBytes,
                   rightPixels.data() + y * srcRowBytes, srcRowBytes);
        }
        return ImageSaver::Instance().saveImage(filePath, combined, combinedW, leftH);
    } else if (hasLeft) {
        // Only left eye
        return ImageSaver::Instance().saveImage(filePath, leftPixels, leftW, leftH);
    } else {
        // Only right eye
        return ImageSaver::Instance().saveImage(filePath, rightPixels, rightW, rightH);
    }
}

// Read RGBA pixels from a GL texture and save as PNG
static bool saveTextureAsPng(GLuint textureId, int width, int height,
                              const std::string& filePath) {
    std::vector<uint8_t> pixels;
    if (!readTexturePixels(textureId, width, height, pixels)) return false;
    return ImageSaver::Instance().saveImage(filePath, pixels, width, height);
}

// Save a stereo pair from cached snapshot buffers (thread-safe, no GL access needed)
static bool saveSnapshotStereoPair(CameraAccessExtension::SnapshotBuffer& leftBuf,
                                    CameraAccessExtension::SnapshotBuffer& rightBuf,
                                    const std::string& filePath) {
    bool hasLeft = false, hasRight = false;
    std::vector<uint8_t> leftPixels, rightPixels;
    int leftW = 0, leftH = 0, rightW = 0, rightH = 0;

    {
        std::lock_guard<std::mutex> lock(leftBuf.mutex);
        if (leftBuf.ready && !leftBuf.pixels.empty()) {
            leftPixels = leftBuf.pixels;
            leftW = leftBuf.width;
            leftH = leftBuf.height;
            hasLeft = true;
            leftBuf.ready = false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(rightBuf.mutex);
        if (rightBuf.ready && !rightBuf.pixels.empty()) {
            rightPixels = rightBuf.pixels;
            rightW = rightBuf.width;
            rightH = rightBuf.height;
            hasRight = true;
            rightBuf.ready = false;
        }
    }

    if (!hasLeft || !hasRight) return false;

    if (leftW == rightW && leftH == rightH) {
        int combinedW = leftW * 2;
        std::vector<uint8_t> combined(combinedW * leftH * 4);
        int srcRowBytes = leftW * 4;
        int dstRowBytes = combinedW * 4;
        for (int y = 0; y < leftH; y++) {
            memcpy(combined.data() + y * dstRowBytes,
                   leftPixels.data() + y * srcRowBytes, srcRowBytes);
            memcpy(combined.data() + y * dstRowBytes + srcRowBytes,
                   rightPixels.data() + y * srcRowBytes, srcRowBytes);
        }
        return ImageSaver::Instance().saveImage(filePath, combined, combinedW, leftH);
    }
    return false;
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        int32_t keyCode = AKeyEvent_getKeyCode(event);
        int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN) {
            struct engine* e = static_cast<struct engine*>(app->userData);
            if (keyCode == AKEYCODE_VOLUME_UP) {
                e->mCameraAccessExtension.snapshotRequested = true;
                return 1;
            }
            if (keyCode == AKEYCODE_DPAD_CENTER || keyCode == AKEYCODE_ENTER) {
                e->dpadCenterPressed = true;
                return 1;
            }
        }
    }
    return 0;
}

void android_main(struct android_app *state)
{
    struct engine engine;

    LOGI("SkyEgoSenseSDK %s (dataset format v%d)", SDK_VERSION, xr::kDatasetFormatVersion);

    state->userData = &engine;
    state->onAppCmd = AppCommon::app_handle_cmd;
    state->onInputEvent = handle_input;
    g_engine = &engine;
    engine.app = state;

    if (engine_init_display(&engine) != 0) {
        LOGW("Failed to create EGL resources");
        return;
    }

    glGetIntegerv(GL_MAX_SAMPLES, &engine.maxSampleCount);
    LOGI("Max sample count: %d", engine.maxSampleCount);
	if(engine.maxSampleCount < 4)
	{
		engine.currentSampleCount = engine.maxSampleCount;
        LOGW("maxSampleCount < 4. Render quality may be impacted ... ");
	}


    if (engine_init_scene_resources(&engine) != 0) {
        LOGW("Failed to load scene resources!  Exiting");
        return;
    }

    AppCommon::app_wait_window((AppCommon::base_engine *)&engine);
    engine_init_openxr(&engine);
    engine.mCameraAccessExtension.initTimeConversion();
    engine.mDatasetRecorder.init(storagePath);

    // Initialize cameras using new callback-based API
    JavaVM* vm = engine.app->activity->vm;
    jobject activity = engine.app->activity->clazz;

    // Cache for TTS JNI bridge
    g_javaVm = vm;
    JNIEnv* ttsEnv = nullptr;
    vm->GetEnv(reinterpret_cast<void**>(&ttsEnv), JNI_VERSION_1_6);
    if (ttsEnv && activity) {
        g_activity = ttsEnv->NewGlobalRef(activity);
    }

    if (engine.mCameraAccessExtension.initCameras(vm, activity)) {
        LOGI("All cameras initialized successfully");
    } else {
        LOGE("Failed to initialize cameras");
    }

    // Start image saver worker thread
    ImageSaver::Instance().start();

    // Initialize hand tracking and controller pose saver — both always active.
    engine.useProjectHand = readProjectHandProperty();
    engine.projectController = readProjectControllerProperty();
    LOGI("Input mode: unified (hand+controller), project_hand: %s, project_controller: %s",
         engine.useProjectHand ? "on" : "off",
         engine.projectController ? "on" : "off");
    engine.mHandTrackerLogic.Init();
    engine.mControllerPoseSaver.Init(storagePath);
    LOGI("HandTrackerLogic + ControllerPoseSaver both initialized");

    while (1) {
        for (;;) {
            int events;
            struct android_poll_source* source;
            // If the timeout is zero, returns immediately without blocking.
            // If the timeout is negative, waits indefinitely until an event appears.
            const int timeoutMilliseconds =
                    (!engine.state.Resumed && !engine.ready) ? -1 : 0;
            if (ALooper_pollAll(timeoutMilliseconds, nullptr, &events, (void**)&source) < 0) {
                break;
            }

            // Check if the system requested us to exit
            if (state->destroyRequested != 0) {
                LOGI("App destroy requested, exiting main loop");
                goto cleanup;
            }

            // Process this event.
            if (source != nullptr) {
                source->process(state, source);
            }
        }
        AppCommon::app_poll_events(&engine);

        // Always handle camera pause/resume, even when XR session is not ready.
        // This ensures cameras are stopped when the app goes to background,
        // regardless of OpenXR session state transitions (e.g. STOPPING).
        engine.mCameraAccessExtension.Update();

        if (!engine.ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        XrFrameState frameState = {.type = XR_TYPE_FRAME_STATE,
                                   .next = nullptr};
        XrFrameWaitInfo frameWaitInfo = {.type = XR_TYPE_FRAME_WAIT_INFO,
                                         .next = nullptr};
        XrResult result = xrWaitFrame(engine.state.xrSession, &frameWaitInfo,
                                      &frameState);
        if (XR_FAILED(result)) {
            LOGW("xrWaitFrame failed");
            continue;
        }


        XrFrameBeginInfo frameBeginInfo = {.type = XR_TYPE_FRAME_BEGIN_INFO,
                                           .next = nullptr};
        result = xrBeginFrame(engine.state.xrSession, &frameBeginInfo);

        if (XR_FAILED(result)) {
            LOGW("xrBeginFrame failed");
            continue;
        }

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCapacityInput = (uint32_t)engine.state.m_views.size();
        uint32_t viewCountOutput;

        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        viewLocateInfo.displayTime = frameState.predictedDisplayTime;
        viewLocateInfo.space = engine.useRootSpace ? engine.state.xrRootSpace : engine.state.xrLocalSpace;
        result = xrLocateViews(engine.state.xrSession, &viewLocateInfo,
                               &viewState, viewCapacityInput, &viewCountOutput,
                               engine.state.m_views.data());
        if (XR_FAILED(result)) {
            LOGW("xrLocateViews failed");
        }
        // Use current CLOCK_BOOTTIME as the single clock source for all sensor
        // queries and dataset timestamps. predictedDisplayTime is a future
        // prediction meant to reduce rendering latency; for dataset recording
        // we want timestamps that faithfully represent when data was captured.
        // XrTime (for OpenXR APIs) is derived from the same boottime sample.
        int64_t sensorBoottimeNs;
        XrTime sensorXrTime = frameState.predictedDisplayTime;  // fallback
        {
            struct timespec nowBoot;
            clock_gettime(CLOCK_BOOTTIME, &nowBoot);
            sensorBoottimeNs = (int64_t)nowBoot.tv_sec * 1000000000LL + nowBoot.tv_nsec;
            if (g_boottimeToXrTimeFn) {
                sensorXrTime = g_boottimeToXrTimeFn((uint64_t)sensorBoottimeNs);
            }
        }

        // 使用predictedDisplayTime而不是当前时间
        // 只有预测时间不会有突然回退的数据帧，当前时间和过去时间都会存在
        //engine.inputPtr->UpdateInput(sensorXrTime);
        engine.inputPtr->UpdateInput(frameState.predictedDisplayTime);

        // Always update hand tracking and push controller pose — both subsystems
        // run concurrently. Each is safe when its underlying tracker is unavailable.
        engine.mHandTrackerLogic.Update(sensorXrTime);

        // Push controller pose to ring buffer for time-aligned CSV write in camera callback.
        {
            ControllerPoseRing::Sample ctrlSample{};
            // 统一使用 boottime 作为时间基准：把 predictedDisplayTime(XrTime) 转成 boottime
            if (g_xrTimeToBoottimeFn) {
                ctrlSample.bootTimeNs = g_xrTimeToBoottimeFn(frameState.predictedDisplayTime);
            }
            ctrlSample.frameNumber = engine.controllerFrameCounter;
            ctrlSample.leftActive = engine.inputPtr->IsControllerActive(0);
            if (ctrlSample.leftActive) {
                auto& p = engine.inputPtr->mControllerPose[0];
                ctrlSample.leftPos[0] = p.position.x;
                ctrlSample.leftPos[1] = p.position.y;
                ctrlSample.leftPos[2] = p.position.z;
                ctrlSample.leftQuat[0] = p.orientation.x;
                ctrlSample.leftQuat[1] = p.orientation.y;
                ctrlSample.leftQuat[2] = p.orientation.z;
                ctrlSample.leftQuat[3] = p.orientation.w;
            }
            ctrlSample.rightActive = engine.inputPtr->IsControllerActive(1);
            if (ctrlSample.rightActive) {
                auto& p = engine.inputPtr->mControllerPose[1];
                ctrlSample.rightPos[0] = p.position.x;
                ctrlSample.rightPos[1] = p.position.y;
                ctrlSample.rightPos[2] = p.position.z;
                ctrlSample.rightQuat[0] = p.orientation.x;
                ctrlSample.rightQuat[1] = p.orientation.y;
                ctrlSample.rightQuat[2] = p.orientation.z;
                ctrlSample.rightQuat[3] = p.orientation.w;
            }
            // Ring stores raw poses; the view-frame correction is applied at
            // the consume site with the time-aligned head orientation.
            engine.controllerPoseRing.push(ctrlSample);
            engine.controllerFrameCounter++;
        }

        // Locate device/IMU pose (view space in root/local space).
        // Camera extrinsics are relative to the device frame, not individual eyes.
        XrSpaceLocation deviceLocation{XR_TYPE_SPACE_LOCATION};
        XrSpace refSpace = engine.useRootSpace ? engine.state.xrRootSpace
                                                : engine.state.xrLocalSpace;
        XrResult locateResult = xrLocateSpace(engine.state.xrViewSpace, refSpace,
                                               sensorXrTime,
                                               &deviceLocation);
        bool devicePoseValid = XR_SUCCEEDED(locateResult) &&
            (deviceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (deviceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);

        // Update shared sensor snapshot with device pose and hand tracking (always)
        {
            std::lock_guard<std::mutex> lock(engine.alignedSnapshot.mutex);
            // Device/IMU pose (not eye pose — camera extrinsics are in device frame)
            if (devicePoseValid) {
                auto& dp = deviceLocation.pose;
                engine.alignedSnapshot.headPose.pos[0] = dp.position.x;
                engine.alignedSnapshot.headPose.pos[1] = dp.position.y;
                engine.alignedSnapshot.headPose.pos[2] = dp.position.z;
                engine.alignedSnapshot.headPose.quat[0] = dp.orientation.x;
                engine.alignedSnapshot.headPose.quat[1] = dp.orientation.y;
                engine.alignedSnapshot.headPose.quat[2] = dp.orientation.z;
                engine.alignedSnapshot.headPose.quat[3] = dp.orientation.w;
                engine.alignedSnapshot.headPose.valid = true;
            }
            // Hand tracking
            auto& snap = engine.alignedSnapshot;
            snap.leftHand.active = engine.mHandTrackerLogic.LeftHandIsActive;
            snap.rightHand.active = engine.mHandTrackerLogic.RightHandIsActive;
            for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
                if (snap.leftHand.active) {
                    auto& j = engine.mHandTrackerLogic.LeftHandJointLocations[i];
                    snap.leftHand.joints[i][0] = j.pose.position.x;
                    snap.leftHand.joints[i][1] = j.pose.position.y;
                    snap.leftHand.joints[i][2] = j.pose.position.z;
                    snap.leftHand.quats[i][0] = j.pose.orientation.x;
                    snap.leftHand.quats[i][1] = j.pose.orientation.y;
                    snap.leftHand.quats[i][2] = j.pose.orientation.z;
                    snap.leftHand.quats[i][3] = j.pose.orientation.w;
                    snap.leftHand.radii[i] = j.radius;
                }
                if (snap.rightHand.active) {
                    auto& j = engine.mHandTrackerLogic.RightHandJointLocations[i];
                    snap.rightHand.joints[i][0] = j.pose.position.x;
                    snap.rightHand.joints[i][1] = j.pose.position.y;
                    snap.rightHand.joints[i][2] = j.pose.position.z;
                    snap.rightHand.quats[i][0] = j.pose.orientation.x;
                    snap.rightHand.quats[i][1] = j.pose.orientation.y;
                    snap.rightHand.quats[i][2] = j.pose.orientation.z;
                    snap.rightHand.quats[i][3] = j.pose.orientation.w;
                    snap.rightHand.radii[i] = j.radius;
                }
            }
        }

        // Push timestamped sample to ring buffer so the camera callback can
        // recover time-aligned head pose (+ hand joints in hand tracking mode)
        // at start_of_exposure. Head pose always pushed (both modes).
        if (devicePoseValid) {
            PoseHandSampleRing::Sample rs;
            rs.bootTimeNs = sensorBoottimeNs;
            rs.poseValid = true;
            // Device/IMU pose (camera extrinsics are in device frame)
            auto& dp = deviceLocation.pose;
            rs.headPos[0] = dp.position.x;
            rs.headPos[1] = dp.position.y;
            rs.headPos[2] = dp.position.z;
            rs.headQuat[0] = dp.orientation.x;
            rs.headQuat[1] = dp.orientation.y;
            rs.headQuat[2] = dp.orientation.z;
            rs.headQuat[3] = dp.orientation.w;
            // Always populate hand data — LeftHandIsActive/RightHandIsActive
            // will be false when hand tracking is unavailable.
            {
                rs.leftActive = engine.mHandTrackerLogic.LeftHandIsActive;
                rs.rightActive = engine.mHandTrackerLogic.RightHandIsActive;
                if (rs.leftActive) {
                    for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; ++j) {
                        const auto& loc = engine.mHandTrackerLogic.LeftHandJointLocations[j];
                        rs.leftJoints[j][0] = loc.pose.position.x;
                        rs.leftJoints[j][1] = loc.pose.position.y;
                        rs.leftJoints[j][2] = loc.pose.position.z;
                        rs.leftRadii[j] = loc.radius;
                        rs.leftQuats[j][0] = loc.pose.orientation.x;
                        rs.leftQuats[j][1] = loc.pose.orientation.y;
                        rs.leftQuats[j][2] = loc.pose.orientation.z;
                        rs.leftQuats[j][3] = loc.pose.orientation.w;
                    }
                }
                if (rs.rightActive) {
                    for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; ++j) {
                        const auto& loc = engine.mHandTrackerLogic.RightHandJointLocations[j];
                        rs.rightJoints[j][0] = loc.pose.position.x;
                        rs.rightJoints[j][1] = loc.pose.position.y;
                        rs.rightJoints[j][2] = loc.pose.position.z;
                        rs.rightRadii[j] = loc.radius;
                        rs.rightQuats[j][0] = loc.pose.orientation.x;
                        rs.rightQuats[j][1] = loc.pose.orientation.y;
                        rs.rightQuats[j][2] = loc.pose.orientation.z;
                        rs.rightQuats[j][3] = loc.pose.orientation.w;
                    }
                }
            }
            engine.poseHandRing.push(rs);
        }

        // Toggle recording on rising edge of B button or DPAD_CENTER
        bool curToggle = engine.inputPtr->mRightBPressed || engine.dpadCenterPressed;
        engine.dpadCenterPressed = false;
        if (curToggle && !engine.prevRecordingToggle) {
            if (engine.mDatasetRecorder.isRecording()) {
                stopRecordingAsync(&engine, "right B", "录制已保存");
            } else {
                // Storage guard: refuse to start below 1 GiB free.
                int64_t avail = getAvailableBytes(storagePath);
                if (avail >= 0 && avail < MIN_FREE_BYTES) {
                    LOGW("Right B start: insufficient storage (%lld bytes free)", (long long)avail);
                    ttsSpeak("存储空间已满，无法录制");
                } else {
                    // Wait for any async stop (encoder + recorder) to complete before starting.
                    // recordingStopRequested stays true until the teardown thread has reset
                    // the gate to IDLE, so this also guarantees we never arm() into a stale gate.
                    int waitCount = 0;
                    while (engine.mCameraAccessExtension.stopInProgress.load() ||
                           engine.mDatasetRecorder.isRecording() ||
                           engine.recordingStopRequested.load()) {
                        usleep(10000); // 10ms
                        if (++waitCount % 100 == 0) {
                            LOGW("Right B start: waiting for stopEncoder (%d ms)", waitCount * 10);
                        }
                        if (waitCount > 300) break; // 3s timeout
                    }
                    LOGI("Starting dataset recording (right B)...");
                    engine.recordingStartTime = std::chrono::steady_clock::now();
                    engine.autoStopRequested = false;
                    recordingGateResetStop();  // A4: fresh recording starts with no end cutoff
                    engine.mGate.arm();
                    injectDatasetFormat(&engine);   // provider 重建+注入（须在 start() 前，见函数注释）
                    engine.mDatasetRecorder.start();
                    engine.mCameraAccessExtension.encoderBaseDir = engine.mDatasetRecorder.getDatasetDir();
                    beginMcapSession(engine.mCameraAccessExtension, "Right B start");
                    engine.mCameraAccessExtension.restartGrayscaleEncoders();
                    // IMU calibration sidecar (device-global; no camera context needed)
                    {
                        auto& ext = engine.mCameraAccessExtension;
                        if (sxr_camera_api_is_valid(&ext.api) && ext.api.get_imu_calibration) {
                            SXR::SxrImuCalibration calib{};
                            if (sxr_camera_get_imu_calibration(&ext.api, &calib) == 0 && calib.valid) {
                                ext.saveImuCalibration(ext.encoderBaseDir + "/imu_calibration.json", calib);
                            } else {
                                LOGW("IMU calibration unavailable; imu_calibration.json not written");
                            }
                        }
                    }
                    engine.mCameraAccessExtension.encodingEnabled = true;
                    engine.mCameraAccessExtension.encodersStopped = false;
                    engine.mCameraAccessExtension.precreateEncodersAsync();  // 后台预创建编码器，免回调 stall
                    engine.mCameraAccessExtension.cameraParamsSavedRgb = false;
                    engine.mCameraAccessExtension.cameraParamsSavedTracking = false;
                    engine.mCameraAccessExtension.cameraParamsSavedCtrl = false;
                    {
                        std::lock_guard<std::mutex> lock(engine.alignedSnapshot.mutex);
                        engine.alignedSnapshot.headPose.valid = false;
                        engine.alignedSnapshot.leftHand.active = false;
                        engine.alignedSnapshot.rightHand.active = false;
                        engine.alignedSnapshot.rgbFrameCount = 0;
                    }
                    engine.poseHandRing.clear();
                    engine.controllerPoseInactiveRowSaved = false;
                    engine.handTrackingInactiveRowSaved = false;
                    engine.controllerPoseRing.clear();
                    // Propagate BOOTTIME→REALTIME offset to non-lazy-init components
                    engine.propagateTimeOffset();
                    // Always start both CSV sessions.
                    engine.mControllerPoseSaver.StartSession(
                        engine.mDatasetRecorder.getControllerPoseCsvPath());
                    engine.mHandTrackerLogic.rawDateSave->StartNewSession(
                        engine.mDatasetRecorder.getHandTrackingCsvPath());
                    ttsSpeak("开始录制");
                }
            }
        }
        engine.prevRecordingToggle = curToggle;

        // Auto-stop guard: end recording before AAudio's signed int32 frame
        // counter overflows at ~13.5h @ 44100Hz (libaaudio internal, unfixable —
        // aborts in getBestTimestamp). Reuses the same async stop path as manual
        // stop so encoder/recorder/session tear down consistently.
        if (engine.mDatasetRecorder.isRecording() &&
            !engine.autoStopRequested.load() &&
            std::chrono::steady_clock::now() - engine.recordingStartTime >=
                std::chrono::hours(12)) {
            engine.autoStopRequested = true;
            stopRecordingAsync(&engine, "12h auto-stop (AAudio int32 overflow guard)",
                               "录制已保存");
        }

        // Auto-stop when free space drops below 1 GiB mid-recording (checked ~1/sec).
        // Reuses stopRecordingAsync so encoder/recorder/session tear down consistently;
        // autoStopRequested prevents repeat triggers while the async stop drains.
        if (engine.mDatasetRecorder.isRecording() &&
            !engine.autoStopRequested.load()) {
            static int storageCheckFrame = 0;
            if (++storageCheckFrame >= 30) {  // ~once per second at 30fps
                storageCheckFrame = 0;
                int64_t avail = getAvailableBytes(storagePath);
                if (avail >= 0 && avail < MIN_FREE_BYTES) {
                    LOGW("Storage low mid-recording (%lld bytes free); auto-stopping",
                         (long long)avail);
                    engine.autoStopRequested = true;
                    stopRecordingAsync(&engine, "low storage auto-stop",
                                       "存储空间已满，无法继续保存");
                }
            }
        }

        // Handle snapshot: non-blocking — check each frame, save when data is ready
        {
            bool req = engine.mCameraAccessExtension.snapshotRequested.load();
            bool lOk = engine.mCameraAccessExtension.snapshotRgbLeft.ready.load();
            bool rOk = engine.mCameraAccessExtension.snapshotRgbRight.ready.load();
            if (req && lOk && rOk) {

            // Both eyes ready — camera callback has cached pixels
            engine.mCameraAccessExtension.snapshotRequested.exchange(false);
            LOGI("Snapshot: saving");

            std::string ts = getTimestampString();
            std::string basePath = std::string(storagePath) + "/images";
            mkdir(basePath.c_str(), 0777);

            bool anySaved = false;

            // RGB cameras: left + right stitched horizontally
            {
                std::string path = basePath + "/rgb_" + ts + ".png";
                if (saveSnapshotStereoPair(engine.mCameraAccessExtension.snapshotRgbLeft,
                                           engine.mCameraAccessExtension.snapshotRgbRight, path)) {
                    anySaved = true;
                }
            }

            // Tracking cameras: left(0) + right(1) stitched horizontally
            {
                std::string path = basePath + "/tracking_" + ts + ".png";
                if (saveSnapshotStereoPair(engine.mCameraAccessExtension.snapshotCv[0],
                                           engine.mCameraAccessExtension.snapshotCv[1], path)) {
                    anySaved = true;
                }
            }

            // Ctrl cameras: left(2) + right(3) stitched horizontally
            {
                std::string path = basePath + "/ctrl_" + ts + ".png";
                if (saveSnapshotStereoPair(engine.mCameraAccessExtension.snapshotCv[2],
                                           engine.mCameraAccessExtension.snapshotCv[3], path)) {
                    anySaved = true;
                }
            }

            ttsSpeak(anySaved ? "图片已保存" : "图片保存失败");
            } // end if (req && lOk && rOk)
        } // end snapshot scope

        XrCompositionLayerProjectionView
                projectionViews[engine.state.viewCount];
        auto &stereoSwapchain = engine.swapchainMap[engine.currentSampleCount];
        for (uint32_t i = 0; i < engine.state.viewCount; ++i) {
            auto &swapchain = stereoSwapchain.eyeSwapchain[i];
            XrSwapchainImageAcquireInfo swapchainImageAcquireInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
                    .next = nullptr};
            uint32_t bufferIndex;
            result = xrAcquireSwapchainImage(swapchain.xrSwapchain,
                                             &swapchainImageAcquireInfo,
                                             &bufferIndex);

            if (XR_FAILED(result)) {
                LOGW("xrAcquireSwapchainImage failed");
            }

            XrSwapchainImageWaitInfo swapchainImageWaitInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                    .next = nullptr,
                    .timeout = 1000};
            result = xrWaitSwapchainImage(swapchain.xrSwapchain,
                                          &swapchainImageWaitInfo);

            if (XR_FAILED(result)) {
                LOGW("xrWaitSwapchainImage failed");
            }

            // NOTE: since xrLocateViews is not implemented (neither is
            // tracking) yet... we hard-code some things in the following
            projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[i].next = nullptr;
            projectionViews[i].pose = engine.state.m_views[i].pose;
            projectionViews[i].fov = engine.state.m_views[i].fov;
            projectionViews[i].subImage.swapchain = swapchain.xrSwapchain;
            projectionViews[i].subImage.imageArrayIndex = 0;
            projectionViews[i].subImage.imageRect.offset.x = 0;
            projectionViews[i].subImage.imageRect.offset.y = 0;
            projectionViews[i].subImage.imageRect.extent.width = engine.width;
            projectionViews[i].subImage.imageRect.extent.height = engine.height;

            // Draw scene
            engine_draw_frame(&engine, i, bufferIndex, engine.state.m_views[i]);

            XrSwapchainImageReleaseInfo swapchainImageReleaseInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
                    .next = nullptr};
            result = xrReleaseSwapchainImage(swapchain.xrSwapchain,
                                             &swapchainImageReleaseInfo);

            if (XR_FAILED(result)) {
                LOGW("xrReleaseSwapchainImage failed");
            }
        }

        glFlush();

        XrCompositionLayerProjection projectionLayer = {
                .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
                .next = nullptr,
                .layerFlags =
                        XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
                                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
                .space = engine.useRootSpace ? engine.state.xrRootSpace : engine.state.xrLocalSpace,
                .viewCount = engine.state.viewCount,
                .views = projectionViews,
        };

        const XrCompositionLayerBaseHeader *const layers[] = {
                (const XrCompositionLayerBaseHeader *const) & projectionLayer,
        };

        XrFrameEndInfo frameEndInfo = {
                .type = XR_TYPE_FRAME_END_INFO,
                .displayTime = frameState.predictedDisplayTime,
                .layerCount = sizeof(layers) / sizeof(layers[0]),
                .layers = layers,
                .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                .next = nullptr};

        result = xrEndFrame(engine.state.xrSession, &frameEndInfo);

        if (XR_FAILED(result)) {
            LOGW("xrEndFrame failed");
        }
    }

cleanup:
    LOGI("Shutting down...");
    // Cleanup cameras first (stops streaming, releases camera hardware)
    engine.mCameraAccessExtension.cleanupCameras();

    // Stop image saver worker thread
    ImageSaver::Instance().shutdown();

    // Release JNI global reference
    if (g_javaVm && g_activity) {
        JNIEnv* env = nullptr;
        g_javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (env) {
            env->DeleteGlobalRef(g_activity);
        }
        g_activity = nullptr;
        g_javaVm = nullptr;
    }

    // Destroy OpenXR resources
    engine_destroy_xr_swapchains(&engine);
    engine_destroy_scene_resources(&engine);
    gInfoPanel.cleanup();
    engine_shutdown_openxr(&engine);

    LOGI("Shutdown complete");
}
