#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xr::mcap {
// 所有 jsonschema / ros2msg 定义（逐字对齐 11_ingest_mcap.py §4：
// GYRO/ACCEL/HAND_TRACKING/CONTROLLER/RGB_META/COMPRESSED_VIDEO/CAMERA_CALIBRATION_SCHEMA
// 与 POSESTAMPED/PATH/TF_STATIC_MSGDEF）。
// JSON schema 文本对齐 Python json.dumps 默认输出（ensure_ascii=True：非 ASCII 转 \uXXXX；
// 分隔符 ", " / ": "），与参考脚本生成的 mcap schema data 字节一致。
extern const char* kSchemaCompressedVideo;   // foxglove.CompressedVideo (jsonschema)
extern const char* kSchemaCameraCalibration; // foxglove.CameraCalibration (jsonschema)
extern const char* kSchemaGyro;              // 自定义 JSON (jsonschema)
extern const char* kSchemaAccel;
extern const char* kSchemaHandTracking;
extern const char* kSchemaControllerPoses;
extern const char* kSchemaMetainfo;
extern const char* kMsgdefPoseStamped;       // ros2msg
extern const char* kMsgdefPath;
extern const char* kMsgdefTfMessage;
extern const char* kRos2MsgEncoding;         // "ros2msg"（Schema encoding：ROS2 .msg 文本定义格式）
extern const char* kCdrMessageEncoding;      // "cdr"（Channel message_encoding：ROS2 消息体 XCDR1 序列化）
extern const char* kJsonEncoding;            // "jsonschema"

std::string jsonEscape(const std::string& s);
std::string gyroJson(int64_t tsNs, float x, float y, float z);     // {"timestamp":{sec,nanosec},"angular_velocity":{x,y,z}}
std::string accelJson(int64_t tsNs, float x, float y, float z);    // linear_acceleration
std::string handTrackingJson(int64_t tsNs, const std::string& rowJson);   // {"timestamp":ns,"row":{...}}
std::string controllerPosesJson(int64_t tsNs, const std::string& rowJson);
std::string metainfoJson(uint64_t frameIndex, int64_t tsNs);       // {"frame_index":n,"ts_ns":ns}
std::string compressedVideoJson(int64_t tsNs, const std::string& frameId,
                                const uint8_t* annexb, size_t len); // base64(data)
std::string cameraCalibrationJson(int64_t tsNs, const std::string& frameId,
        int width, int height, double fx,double fy,double cx,double cy,
        const float D[4]);                                          // kannala_brandt
} // namespace xr::mcap
