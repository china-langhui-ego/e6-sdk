#pragma once
#include <cstdint>
#include <vector>

class AnnexBConverter {
public:
    // 将 AVCC 帧转换为 Annex-B；已是 Annex-B 则直接拷贝
    static bool convert(const uint8_t* data, size_t size, std::vector<uint8_t>& out);

    // AU（访问单元）NAL 扫描结果。判型遍历 AU 内全部 NAL（而非只看首个）——编码器
    // "SPS/PPS+IDR 同 AU"（prepend）模式下只判首 NAL 会把含 IDR 的整帧误判为参数集
    // 吞掉，视频通道零帧且无法恢复。输入支持 Annex-B（3/4 字节起始码）与 AVCC
    //（4 字节长度前缀），与 convert() 的输入域一致。
    struct AuInfo {
        bool hasVcl = false;           // 含 VCL/slice NAL（HEVC 0-31）
        bool hasParamSet = false;      // 含 VPS/SPS/PPS（HEVC 32-34）
        bool hasKey = false;           // 含 BLA/IDR/CRA（HEVC 16-21）
        size_t paramSetPrefixLen = 0;  // AU 头部连续参数集 NAL 字节数（含起始码/长度前缀）
    };
    static AuInfo scanAccessUnit(const uint8_t* data, size_t size);
};
