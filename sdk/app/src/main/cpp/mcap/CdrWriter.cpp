#include "CdrWriter.h"

#include <cstring>

namespace xr::mcap {

void CdrWriter::align4() { while (buf.size() % 4 != 0) buf.push_back(0); }
void CdrWriter::align8() {
    // XCDR1 对齐相对数据流起点（encapsulation 之后，即 buf 偏移 4 处）：float64 需
    // (offset-4) % 8 == 0，等价于 buf.size() % 8 == 4。mcap_ros2 的 CdrReader._align 同此
    // 语义（`alignment = (offset - 4) % size`）。若按 buf.size() % 8 对齐会少 4 字节，
    // Foxglove 解码后续 float64 错位 4 字节 → "Invalid typed array length"。
    while ((buf.size() - 4) % 8 != 0) buf.push_back(0);
}

// 全部整型/浮点定长小端
void CdrWriter::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
void CdrWriter::i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
void CdrWriter::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(uint8_t((v >> (8 * i)) & 0xFF));
}
void CdrWriter::f64(double v) {
    align8();  // XCDR1：f64 前 8 字节对齐
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    u64(bits);
}

void CdrWriter::str(const std::string& s) {
    align4();
    u32(static_cast<uint32_t>(s.size()) + 1);      // XCDR1：长度含结尾 NUL
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);                              // 结尾 NUL
    align4();                                      // XCDR1：尾部 pad 到 4 字节对齐
}

void CdrWriter::pose(double px, double py, double pz,
                     double qx, double qy, double qz, double qw) {
    f64(px); f64(py); f64(pz);
    f64(qx); f64(qy); f64(qz); f64(qw);
}

void CdrWriter::header(int64_t tsNs, const std::string& frameId) {
    // builtin_interfaces/Time = int32 sec + uint32 nanosec（各 4 字节定长 LE）
    i32(static_cast<int32_t>(tsNs / 1000000000LL));
    u32(static_cast<uint32_t>(tsNs % 1000000000LL));
    str(frameId);
}

std::vector<uint8_t> CdrWriter::poseStamped(int64_t tsNs, const std::string& frameId,
                                            double px, double py, double pz,
                                            double qx, double qy, double qz, double qw) {
    CdrWriter w;
    w.header(tsNs, frameId);
    w.pose(px, py, pz, qx, qy, qz, qw);
    return w.data();
}

std::vector<uint8_t> CdrWriter::tfMessage(
        const std::vector<std::tuple<std::string,std::string,std::array<double,3>,std::array<double,4>>>& transforms,
        int64_t tsNs) {
    // tf2_msgs/TFMessage = sequence<TransformStamped>；
    // TransformStamped = Header + string child_frame_id + Transform(3×f64 translation + 4×f64 rotation)
    CdrWriter w;
    w.align4();
    w.u32(static_cast<uint32_t>(transforms.size()));  // sequence 长度前缀
    for (const auto& t : transforms) {
        w.header(tsNs, std::get<0>(t));               // header.frame_id = parent
        w.str(std::get<1>(t));                        // child_frame_id
        const auto& tr = std::get<2>(t);
        const auto& q  = std::get<3>(t);
        w.f64(tr[0]); w.f64(tr[1]); w.f64(tr[2]);
        w.f64(q[0]);  w.f64(q[1]);  w.f64(q[2]);  w.f64(q[3]);
    }
    return w.data();
}

std::vector<uint8_t> CdrWriter::pathMessage(int64_t tsNs, const std::string& frameId,
        const std::vector<std::array<double,7>>& poses) {
    // nav_msgs/Path = Header + sequence<PoseStamped>（各 PoseStamped 共用同一 tsNs/frameId）
    CdrWriter w;
    w.header(tsNs, frameId);
    w.align4();
    w.u32(static_cast<uint32_t>(poses.size()));       // sequence 长度前缀
    for (const auto& ps : poses) {
        w.header(tsNs, frameId);
        w.pose(ps[0], ps[1], ps[2], ps[3], ps[4], ps[5], ps[6]);
    }
    return w.data();
}

} // namespace xr::mcap
