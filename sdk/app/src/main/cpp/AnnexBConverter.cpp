#include "AnnexBConverter.h"

bool AnnexBConverter::convert(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        out.assign(data, data + size);
        return true;
    }
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        out.assign(data, data + size);
        return true;
    }
    out.clear();
    out.reserve(size + 64);
    size_t offset = 0;
    while (offset + 4 <= size) {
        uint32_t naluLen = (data[offset] << 24) | (data[offset+1] << 16) |
                           (data[offset+2] << 8) | data[offset+3];
        offset += 4;
        if (offset + naluLen > size) break;
        out.insert(out.end(), {0,0,0,1});
        out.insert(out.end(), data + offset, data + offset + naluLen);
        offset += naluLen;
    }
    return !out.empty();
}

AnnexBConverter::AuInfo AnnexBConverter::scanAccessUnit(const uint8_t* data, size_t size) {
    AuInfo info;
    if (data == nullptr || size == 0) return info;
    size_t prefixEnd = 0;    // 头部连续参数集段的结束偏移
    bool prefixOpen = true;  // 仍处头部参数集段（遇首个非参数集 NAL 关闭）
    auto onNal = [&](int nalType, size_t nalEnd) {
        if (nalType <= 31) info.hasVcl = true;
        if (nalType >= 16 && nalType <= 21) info.hasKey = true;
        if (nalType >= 32 && nalType <= 34) {
            info.hasParamSet = true;
        } else {
            prefixOpen = false;
        }
        if (prefixOpen) prefixEnd = nalEnd;
    };

    const bool annexB = size >= 3 && data[0] == 0 && data[1] == 0 &&
                        (data[2] == 1 || (size >= 4 && data[2] == 0 && data[3] == 1));
    if (annexB) {
        // Annex-B：逐个起始码取其后 NAL 首字节；emulation prevention 保证载荷内
        // 不出现 00 00 {00,}01 序列，扫描下一个起始码即 NAL 边界
        size_t i = 0;
        while (i + 4 <= size) {
            size_t sc = 0;
            if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) sc = 3;
            else if (i + 5 <= size && data[i] == 0 && data[i+1] == 0 &&
                     data[i+2] == 0 && data[i+3] == 1) sc = 4;
            else { ++i; continue; }
            const size_t nalStart = i + sc;
            size_t j = nalStart + 1;
            while (j + 3 <= size) {
                if (data[j] == 0 && data[j+1] == 0 &&
                    (data[j+2] == 1 || (j + 4 <= size && data[j+2] == 0 && data[j+3] == 1))) break;
                ++j;
            }
            onNal((data[nalStart] >> 1) & 0x3F, j);
            i = j;
        }
    } else {
        // AVCC：4 字节大端长度前缀
        size_t offset = 0;
        while (offset + 5 <= size) {
            const uint32_t naluLen = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
                                     (uint32_t(data[offset+2]) << 8) | data[offset+3];
            if (naluLen == 0 || offset + 4 + naluLen > size) break;
            onNal((data[offset+4] >> 1) & 0x3F, offset + 4 + naluLen);
            offset += 4 + naluLen;
        }
    }
    info.paramSetPrefixLen = prefixEnd;
    return info;
}
