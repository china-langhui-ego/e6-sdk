#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace xr::mcap {

// ROS2 CDR 小端序列化（仅固定布局消息；string = u32(长度含结尾 NUL) + 字节 + NUL + pad 到 4 —— Fast-CDR/XCDR1 语义，勿与 mcap 容器内 string 混淆）。
// 对齐语义为 XCDR1 标准（fastcdr）：string 尾部 pad 到 4 字节、f64 前 8 字节对齐，非紧凑布局。
class CdrWriter {
public:
    // XCDR1 LE encapsulation header（00 01 00 00）：所有 ROS2 CDR 消息必须以 encapsulation
    // 开头（Fast-CDR 语义）。Foxglove 的 rosmsg2-serialization 解码器期望它，缺则把后续
    // 字符串数据误读为长度 → "Invalid typed array length"。
    static constexpr uint8_t kEncapsulation[4] = {0x00, 0x01, 0x00, 0x00};
    CdrWriter() { buf.insert(buf.end(), kEncapsulation, kEncapsulation + 4); }
    void u32(uint32_t v); void i32(int32_t v);
    void u64(uint64_t v); void f64(double v);
    void str(const std::string& s);              // align4 → u32(len+1 含 NUL) + bytes + NUL + 尾部 pad 到 4（XCDR1）
    void align4();
    void align8();                               // f64 前 8 字节对齐（XCDR1 标准）
    const std::vector<uint8_t>& data() const { return buf; }

    // 便捷：geometry_msgs/Pose（7×f64，f64 前 align8）
    void pose(double px,double py,double pz,double qx,double qy,double qz,double qw);
    // std_msgs/Header（stamp + frame_id）
    void header(int64_t tsNs, const std::string& frameId);

    // 顶层构建（返回完整消息字节）：
    static std::vector<uint8_t> poseStamped(int64_t tsNs, const std::string& frameId,
                                            double px,double py,double pz,
                                            double qx,double qy,double qz,double qw);
    // tf2_msgs/msg/TFMessage：transforms = {(parent, child, t, q), ...}
    // ⚠ tuple 元素用 std::array<double,N>——C++ 不允许数组类型作 tuple 元素（double[3] 编译不过）
    static std::vector<uint8_t> tfMessage(
            const std::vector<std::tuple<std::string,std::string,std::array<double,3>,std::array<double,4>>>& transforms,
            int64_t tsNs);
    // nav_msgs/Path：header + PoseStamped[]（各 PoseStamped 共用同一 tsNs/frameId）
    static std::vector<uint8_t> pathMessage(int64_t tsNs, const std::string& frameId,
            const std::vector<std::array<double,7>>& poses);  // 每项 {x,y,z,qx,qy,qz,qw}
private:
    std::vector<uint8_t> buf;
};

} // namespace xr::mcap
