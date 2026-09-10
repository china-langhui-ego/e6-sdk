#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <mcap/mcap.hpp>

namespace xr::mcap {

// 官方 mcap::McapWriter 的线程安全薄封装（单锁）：
//   chunk（官方默认 1MB）+ summary/index，compression 显式 None（官方默认 Zstd）
//   publishTime 恒 = logTime（单时间轴）
//   每通道 sequence 从 0 自增（官方 Message.sequence 由调用方填，本层锁内维护）
//   每 500 条消息 closeLastChunk() 一次（writeChunk 末尾 output.flush() → fflush，
//   崩溃丢数据窗口 ≤500 条）
// 写路径：std::ofstream + exceptions(failbit|badbit) + 官方 StreamWriter——写失败抛
// std::ios_base::failure（v2.1.3 FileWriter 仅 assert(written==size)，release 静默吞短写）。
// 所有写路径 try/catch 置 mFailed。
// 注意：xr::mcap 内裸写 mcap:: 会被外层 xr::mcap 遮蔽（C++ 命名空间内层优先），
// 官方类型一律显式 ::mcap:: 前缀。
class McapWriter {
public:
    McapWriter() = default;
    ~McapWriter();                            // 未 finish 则兜底 finish()

    bool open(const std::string& path);       // 已打开或已 finish 返回 false（finish 后禁止重开）
    void finish();                            // 幂等：close() 写 DataEnd+summary+footer；mFailed 时 terminate()
    bool isOpen() const;                      // open 成功且未 finish
    bool failed() const;                      // 任一 write/flush/finish 写失败即置位

    // 同名复用（官方 addSchema/addChannel 不去重，本层 map 去重）
    ::mcap::Schema registerSchema(const std::string& name, const std::string& encoding,
                                  const std::string& data);
    ::mcap::SchemaId schemaIdOf(const std::string& name) const;  // 未注册返回 0
    ::mcap::Channel registerChannel(const std::string& topic, const std::string& messageEncoding,
                                    ::mcap::SchemaId schemaId);
    bool write(const ::mcap::Channel& ch, int64_t logTimeNs, const uint8_t* data, size_t len);

    // topic 便捷层（传感器散点写用）：topic 未注册时自动建 channel（schemaId=0；
    // writeJson 自动通道 encoding 固定 "json"、writeCdr 固定空串）
    bool writeJson(const std::string& topic, int64_t logTimeNs, const std::string& json);
    bool writeCdr(const std::string& topic, int64_t logTimeNs, const uint8_t* data, size_t len);

    // 写附件（音频/标定等侧文件嵌入）：锁内走官方 write(Attachment&)（自动算 CRC），
    // 失败置 mFailed 返回 false。logTimeNs 同时作 logTime/createTime（定稿时刻 UTC）。
    // 附件是不入 chunk 的独立 record，官方 write 前会先 closeLastChunk 落盘在途 chunk。
    bool writeAttachment(const std::string& name, const std::string& mediaType,
                         int64_t logTimeNs, const uint8_t* data, size_t len);

    // 写 metadata record（录制会话标识等自定义 key-value，规范定义的不入 chunk 的
    // 独立 record，Foxglove 可直接展示；失败置 mFailed 返回 false）
    bool writeMetadata(const std::string& name, const std::map<std::string, std::string>& kv);

private:
    // 以下调用方须持锁
    ::mcap::Channel& channelLocked(const std::string& topic, const char* autoEncoding);
    bool writeLocked(::mcap::ChannelId id, int64_t logTimeNs, const uint8_t* data, size_t len);

    mutable std::mutex mtx;
    std::ofstream mStream;                    // ofstream+exceptions 获写失败抛出语义（官方 StreamWriter 走 stream_.write）
    ::mcap::McapWriter mWriter;
    bool mOpened = false;
    bool mFinished = false;
    bool mFailed = false;
    std::map<std::string, ::mcap::Schema> mSchemas;     // schema name -> schema（含 id）
    std::map<std::string, ::mcap::Channel> mChannels;   // topic -> channel（含 id）
    std::map<::mcap::ChannelId, uint32_t> mNextSeq;     // channel id -> 下一条 sequence（从 0 起）
    uint32_t mMessageCount = 0;
    static constexpr uint32_t kFlushInterval = 500;   // 每 500 条消息结束一次 chunk 落盘
};

} // namespace xr::mcap
