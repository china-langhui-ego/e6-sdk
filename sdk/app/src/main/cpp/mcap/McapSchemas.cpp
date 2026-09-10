// MCAP 通道 schema 定义 + 消息 JSON 构建（可移植 C++17，仅标准库，无 Android 依赖）。
// schema 文本逐字对齐参考脚本 11_ingest_mcap.py（json.dumps 默认输出：ensure_ascii=True，
// 非 ASCII 字符为 \uXXXX 转义；分隔符 ", " / ": "），保证与参考脚本生成的 mcap 字节一致。
// 消息 JSON 用紧凑分隔符（无空格，等效 json.dumps(separators=(',',':'))）。
// 性能：录制期约 1000 msg/s，字符串拼接一律 reserve 预分配，避免逐帧反复堆分配。
#include "McapSchemas.h"
#include <iomanip>
#include <sstream>

namespace xr::mcap {

const char* kRos2MsgEncoding = "ros2msg";
const char* kCdrMessageEncoding = "cdr";
const char* kJsonEncoding = "jsonschema";

// ---------------------------------------------------------------------------
// ROS2 .msg 定义（mcap_ros2 动态序列化用，==== 分隔多个消息块，逐字复制自参考脚本）
// ---------------------------------------------------------------------------
const char* kMsgdefPoseStamped = R"MSG(std_msgs/Header header
geometry_msgs/Pose pose

================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id

================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec

================================================================================
MSG: geometry_msgs/Pose
geometry_msgs/Point position
geometry_msgs/Quaternion orientation

================================================================================
MSG: geometry_msgs/Point
float64 x
float64 y
float64 z

================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
)MSG";

const char* kMsgdefPath = R"MSG(std_msgs/Header header
geometry_msgs/PoseStamped[] poses

================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id

================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec

================================================================================
MSG: geometry_msgs/PoseStamped
std_msgs/Header header
geometry_msgs/Pose pose

================================================================================
MSG: geometry_msgs/Pose
geometry_msgs/Point position
geometry_msgs/Quaternion orientation

================================================================================
MSG: geometry_msgs/Point
float64 x
float64 y
float64 z

================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
)MSG";

const char* kMsgdefTfMessage = R"MSG(geometry_msgs/TransformStamped[] transforms

================================================================================
MSG: geometry_msgs/TransformStamped
# This expresses a transform from coordinate frame header.frame_id
# to the coordinate frame child_frame_id.
std_msgs/Header header
string child_frame_id
geometry_msgs/Transform transform

================================================================================
MSG: std_msgs/Header
builtin_interfaces/Time stamp
string frame_id

================================================================================
MSG: builtin_interfaces/Time
int32 sec
uint32 nanosec

================================================================================
MSG: geometry_msgs/Transform
geometry_msgs/Vector3 translation
geometry_msgs/Quaternion rotation

================================================================================
MSG: geometry_msgs/Vector3
float64 x
float64 y
float64 z

================================================================================
MSG: geometry_msgs/Quaternion
float64 x
float64 y
float64 z
float64 w
)MSG";

// ---------------------------------------------------------------------------
// 自定义 JSON schema（以下 7 个常量为 11_ingest_mcap.py 中 json.dumps 默认输出的逐字节
// 副本：ensure_ascii=True，中文描述为 \uXXXX 转义；分隔符 ", " / ": "。
// 中文原文：纳秒时间戳 / 原始行 / 陀螺仪角速度 / 加速度计 m/s² / 标定时间（= 会话开始时间）/
// 相机坐标系，如 … / 图像宽度/高度 px / 畸变模型（kannala_brandt：KB4 鱼眼）/ 畸变系数 /
// 内参矩阵 3x3 行优先：… / 投影矩阵 3x4 行优先：…）
// ---------------------------------------------------------------------------
const char* kSchemaHandTracking = R"JSON({"type": "object", "properties": {"timestamp": {"type": "integer", "description": "\u7eb3\u79d2\u65f6\u95f4\u6233"}, "row": {"type": "object", "description": "hand_tracking.csv \u539f\u59cb\u884c\uff0826 \u5173\u8282\u00d7\u5de6\u53f3\u624b + active\uff09", "additionalProperties": true}}, "required": ["timestamp", "row"]})JSON";

const char* kSchemaControllerPoses = R"JSON({"type": "object", "properties": {"timestamp": {"type": "integer", "description": "\u7eb3\u79d2\u65f6\u95f4\u6233"}, "row": {"type": "object", "description": "controller_poses.csv \u539f\u59cb\u884c\uff08frame_number,timestamp_ns,left_*,right_*\uff09", "additionalProperties": true}}, "required": ["timestamp", "row"]})JSON";

const char* kSchemaGyro = R"JSON({"type": "object", "properties": {"timestamp": {"type": "object", "properties": {"sec": {"type": "integer"}, "nanosec": {"type": "integer"}}}, "angular_velocity": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}}, "description": "\u9640\u87ba\u4eea\u89d2\u901f\u5ea6 rad/s"}}, "required": ["timestamp", "angular_velocity"]})JSON";

const char* kSchemaAccel = R"JSON({"type": "object", "properties": {"timestamp": {"type": "object", "properties": {"sec": {"type": "integer"}, "nanosec": {"type": "integer"}}}, "linear_acceleration": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}}, "description": "\u52a0\u901f\u5ea6\u8ba1 m/s\u00b2"}}, "required": ["timestamp", "linear_acceleration"]})JSON";

const char* kSchemaMetainfo = R"JSON({"type": "object", "properties": {"frame_index": {"type": "integer"}, "ts_ns": {"type": "integer", "description": "mid_exposure_utc_ns"}}, "required": ["frame_index", "ts_ns"]})JSON";

const char* kSchemaCompressedVideo = R"JSON({"title": "foxglove.CompressedVideo", "type": "object", "properties": {"timestamp": {"type": "object", "properties": {"sec": {"type": "integer", "minimum": 0}, "nsec": {"type": "integer", "minimum": 0, "maximum": 999999999}}, "description": "Timestamp of video frame"}, "frame_id": {"type": "string", "description": "Frame of reference for the video."}, "data": {"type": "string", "contentEncoding": "base64", "description": "Annex B \u683c\u5f0f\u7684 HEVC \u6570\u636e\uff0c\u5173\u952e\u5e27\u542b VPS/SPS/PPS"}, "format": {"type": "string", "description": "h265"}}, "required": ["timestamp", "frame_id", "data", "format"]})JSON";

const char* kSchemaCameraCalibration = R"JSON({"title": "foxglove.CameraCalibration", "type": "object", "properties": {"timestamp": {"type": "object", "properties": {"sec": {"type": "integer", "minimum": 0}, "nanosec": {"type": "integer", "minimum": 0, "maximum": 999999999}}, "description": "\u6807\u5b9a\u65f6\u95f4\uff08= \u4f1a\u8bdd\u5f00\u59cb\u65f6\u95f4\uff09"}, "frame_id": {"type": "string", "description": "\u76f8\u673a\u5750\u6807\u7cfb\uff0c\u5982 camera_rgb_left_optical_frame"}, "width": {"type": "integer", "description": "\u56fe\u50cf\u5bbd\u5ea6 px"}, "height": {"type": "integer", "description": "\u56fe\u50cf\u9ad8\u5ea6 px"}, "distortion_model": {"type": "string", "description": "\u7578\u53d8\u6a21\u578b\uff08kannala_brandt\uff1aKB4 \u9c7c\u773c\uff09"}, "D": {"type": "array", "items": {"type": "number"}, "description": "\u7578\u53d8\u7cfb\u6570"}, "K": {"type": "array", "items": {"type": "number"}, "description": "\u5185\u53c2\u77e9\u9635 3x3 \u884c\u4f18\u5148\uff1afx,0,cx, 0,fy,cy, 0,0,1"}, "P": {"type": "array", "items": {"type": "number"}, "description": "\u6295\u5f71\u77e9\u9635 3x4 \u884c\u4f18\u5148\uff1afx,0,cx,0, 0,fy,cy,0, 0,0,1,0"}}, "required": ["timestamp", "frame_id", "width", "height", "distortion_model", "D", "K", "P"]})JSON";

// ---------------------------------------------------------------------------
// 数值格式化：fixed + setprecision（float 7 位 ≈ FLT_DIG；double 10 位保留内参精度）
// ---------------------------------------------------------------------------
static std::string ftoa(float v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(7) << static_cast<double>(v);
    return oss.str();
}
static std::string dtoa(double v) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(10) << v;
    return oss.str();
}

// tsNs → {"sec":S,"nanosec":N}（整数分支，无浮点误差）
static void appendTimestampSecNanosec(std::string& out, int64_t tsNs, const char* nsecKey) {
    out += "{\"sec\":";
    out += std::to_string(tsNs / 1000000000LL);
    out += ",\"";
    out += nsecKey;
    out += "\":";
    out += std::to_string(tsNs % 1000000000LL);
    out += "}";
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 手写 base64（标准字母表 + '=' padding；无第三方库）
static std::string base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(kTable[(v >> 6) & 63]);
        out.push_back(kTable[v & 63]);
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kTable[(v >> 18) & 63]);
        if (i + 1 < len) {
            v |= uint32_t(data[i + 1]) << 8;
            out.push_back(kTable[(v >> 12) & 63]);
            out.push_back(kTable[(v >> 6) & 63]);
            out.push_back('=');
        } else {
            out.push_back(kTable[(v >> 12) & 63]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

// gyro/accel 共用实现：仅向量字段名不同（angular_velocity / linear_acceleration）
static std::string imuJson(int64_t tsNs, const char* vecKey, float x, float y, float z) {
    std::string s;
    s.reserve(128);
    s += "{\"timestamp\":";
    appendTimestampSecNanosec(s, tsNs, "nanosec");
    s += ",\"";
    s += vecKey;
    s += "\":{\"x\":";
    s += ftoa(x);
    s += ",\"y\":";
    s += ftoa(y);
    s += ",\"z\":";
    s += ftoa(z);
    s += "}}";
    return s;
}

std::string gyroJson(int64_t tsNs, float x, float y, float z) {
    return imuJson(tsNs, "angular_velocity", x, y, z);
}

std::string accelJson(int64_t tsNs, float x, float y, float z) {
    return imuJson(tsNs, "linear_acceleration", x, y, z);
}

// hand_tracking/controller_poses 共用实现：{"timestamp":ns,"row":{...}}
// rowJson 已是完整 JSON 对象文本，原样嵌入
static std::string rowJson(int64_t tsNs, const std::string& row) {
    std::string s;
    s.reserve(32 + row.size());
    s += "{\"timestamp\":";
    s += std::to_string(tsNs);
    s += ",\"row\":";
    s += row;
    s += "}";
    return s;
}

std::string handTrackingJson(int64_t tsNs, const std::string& row) {
    return rowJson(tsNs, row);
}

std::string controllerPosesJson(int64_t tsNs, const std::string& row) {
    return rowJson(tsNs, row);
}

std::string metainfoJson(uint64_t frameIndex, int64_t tsNs) {
    std::string s;
    s.reserve(64);
    s += "{\"frame_index\":";
    s += std::to_string(frameIndex);
    s += ",\"ts_ns\":";
    s += std::to_string(tsNs);
    s += "}";
    return s;
}

std::string compressedVideoJson(int64_t tsNs, const std::string& frameId,
                                const uint8_t* annexb, size_t len) {
    std::string s;
    s.reserve(96 + frameId.size() + ((len + 2) / 3) * 4);
    s += "{\"timestamp\":";
    appendTimestampSecNanosec(s, tsNs, "nsec");  // 参考脚本视频通道用 "nsec"（非 "nanosec"）
    s += ",\"frame_id\":\"";
    s += jsonEscape(frameId);
    s += "\",\"format\":\"h265\",\"data\":\"";
    s += base64Encode(annexb, len);
    s += "\"}";
    return s;
}

std::string cameraCalibrationJson(int64_t tsNs, const std::string& frameId,
        int width, int height, double fx, double fy, double cx, double cy,
        const float D[4]) {
    const std::string sfx = dtoa(fx), sfy = dtoa(fy), scx = dtoa(cx), scy = dtoa(cy);
    std::string s;
    s.reserve(512 + frameId.size());
    s += "{\"timestamp\":";
    appendTimestampSecNanosec(s, tsNs, "nanosec");
    s += ",\"frame_id\":\"";
    s += jsonEscape(frameId);
    s += "\",\"width\":";
    s += std::to_string(width);
    s += ",\"height\":";
    s += std::to_string(height);
    s += ",\"distortion_model\":\"kannala_brandt\",\"D\":[";
    for (int i = 0; i < 4; ++i) {
        if (i) s += ",";
        s += ftoa(D[i]);  // KB4 鱼眼 k1..k4
    }
    // K = [fx,0,cx, 0,fy,cy, 0,0,1]；P = [fx,0,cx,0, 0,fy,cy,0, 0,0,1,0]（3x3/3x4 行优先）
    s += "],\"K\":[";
    s += sfx; s += ",0,"; s += scx; s += ",0,"; s += sfy; s += ","; s += scy; s += ",0,0,1";
    s += "],\"P\":[";
    s += sfx; s += ",0,"; s += scx; s += ",0,0,"; s += sfy; s += ","; s += scy; s += ",0,0,0,1,0";
    s += "]}";
    return s;
}

} // namespace xr::mcap
