// 官方 mcap::McapWriter 薄封装实现。线程安全（单锁）；可移植（官方库仅依赖 C++17 标准库）。
#include "McapWriter.h"

namespace xr::mcap {

McapWriter::~McapWriter() { finish(); }

bool McapWriter::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx);
    if (mOpened) return false;
    if (mFinished) return false;   // finish 后禁止重开（mNextSeq 等状态不复位）
    ::mcap::McapWriterOptions options("");           // profile 空（Header 不带 profile）
    options.compression = ::mcap::Compression::None; // 官方默认 Zstd——必须显式关闭（无 zstd 依赖）
    // chunkSize 保持官方默认 1MB；summary/index/statistics 全开（各 no* 默认 false，
    // noRepeatedSchemas/noRepeatedChannels 同样保持默认）。
    // v2.1.3 FileWriter 仅 assert(written==size)（release 静默吞短写），故弃用 open(path)
    // 改用 ofstream+exceptions+StreamWriter：写失败抛 std::ios_base::failure，下方 try/catch 生效。
    try {
        mStream.open(path, std::ios::binary | std::ios::trunc);
        if (!mStream.is_open()) return false;
        mStream.exceptions(std::ios::failbit | std::ios::badbit);
        mWriter.open(mStream, options);              // void 重载：写头失败走 throw
    } catch (const std::exception&) {
        try { mWriter.terminate(); } catch (...) {}  // 先切断 writer→stream 关联（否则析构 close() 写已关闭流 → terminate）
        try { mStream.close(); } catch (...) {}
        return false;
    }
    mOpened = true;
    mFinished = false;
    mFailed = false;
    mMessageCount = 0;
    return true;
}

void McapWriter::finish() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished) return;             // 幂等（旋转 swap 与析构可能重复触发）
    try {
        if (!mFailed) {
            mWriter.close();      // closeLastChunk + DataEnd + summary + footer + flush
        } else {
            mWriter.terminate();  // 已失败：不写尾部（文件结构已不完整，直接截断）
        }
        mStream.close();          // 关 ofstream（exceptions 下关流失败同样抛 → 置 mFailed）
    } catch (const std::exception&) {   // 写失败抛 std::ios_base::failure（StreamWriter 路径）
        mFailed = true;
        // terminate() 仅释放内部状态、切断 writer→stream 关联（不写任何记录，见官方
        // writer.inl）——即使 mWriter.close() 已成功（仅 mStream.close() 抛异常）再调
        // 也安全；且必须切断，防 writer 析构对异常流二次操作。
        try { mWriter.terminate(); } catch (...) {}
        try { mStream.close(); } catch (...) {}
    }
    mFinished = true;
    mOpened = false;
}

bool McapWriter::isOpen() const {
    std::lock_guard<std::mutex> lk(mtx);
    return mOpened && !mFinished;
}

bool McapWriter::failed() const {
    std::lock_guard<std::mutex> lk(mtx);
    return mFailed;
}

::mcap::Schema McapWriter::registerSchema(const std::string& name, const std::string& encoding,
                                          const std::string& data) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished) return {};        // 未打开/已 finish：id=0 空对象
    auto it = mSchemas.find(name);
    if (it != mSchemas.end()) return it->second;
    ::mcap::Schema schema(name, encoding, data);
    mWriter.addSchema(schema);          // id 由官方回填（从 1 递增）
    mSchemas.emplace(name, schema);
    return schema;
}

::mcap::SchemaId McapWriter::schemaIdOf(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mtx);
    auto it = mSchemas.find(name);
    return it != mSchemas.end() ? it->second.id : ::mcap::SchemaId(0);
}

::mcap::Channel McapWriter::registerChannel(const std::string& topic,
                                            const std::string& messageEncoding,
                                            ::mcap::SchemaId schemaId) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished) return {};        // 未打开/已 finish：id=0 空对象
    auto it = mChannels.find(topic);
    if (it != mChannels.end()) return it->second;
    ::mcap::Channel ch(topic, messageEncoding, schemaId);
    mWriter.addChannel(ch);             // id 由官方回填
    mChannels.emplace(topic, ch);
    return ch;
}

::mcap::Channel& McapWriter::channelLocked(const std::string& topic, const char* autoEncoding) {
    auto it = mChannels.find(topic);
    if (it != mChannels.end()) return it->second;
    ::mcap::Channel ch(topic, autoEncoding, ::mcap::SchemaId(0));  // 自动通道 schemaId=0
    mWriter.addChannel(ch);
    return mChannels.emplace(topic, ch).first->second;
}

bool McapWriter::writeLocked(::mcap::ChannelId id, int64_t logTimeNs,
                             const uint8_t* data, size_t len) {
    ::mcap::Message msg;
    msg.channelId = id;
    msg.sequence = mNextSeq[id]++;      // 每通道从 0 自增
    msg.logTime = ::mcap::Timestamp(logTimeNs);
    msg.publishTime = msg.logTime;      // publishTime == logTime
    msg.data = reinterpret_cast<const std::byte*>(data);
    msg.dataSize = len;
    try {
        const ::mcap::Status st = mWriter.write(msg);
        if (!st.ok()) { mFailed = true; return false; }
        if (++mMessageCount % kFlushInterval == 0) {
            mWriter.closeLastChunk();   // 结束当前 chunk 落盘（writeChunk 末尾 output.flush() → fflush）
        }
    } catch (const std::exception&) {   // 写失败抛 std::ios_base::failure（磁盘满）
        mFailed = true;
        return false;
    }
    return true;
}

bool McapWriter::write(const ::mcap::Channel& ch, int64_t logTimeNs,
                       const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished || mFailed) return false;
    if (data == nullptr && len > 0) return false;
    return writeLocked(ch.id, logTimeNs, data, len);
}

bool McapWriter::writeJson(const std::string& topic, int64_t logTimeNs, const std::string& json) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished || mFailed) return false;
    ::mcap::Channel& ch = channelLocked(topic, "json");  // 自动通道 encoding 固定 "json"
    return writeLocked(ch.id, logTimeNs,
                       reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

bool McapWriter::writeCdr(const std::string& topic, int64_t logTimeNs,
                          const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished || mFailed) return false;
    if (data == nullptr && len > 0) return false;
    ::mcap::Channel& ch = channelLocked(topic, "");      // CDR 自动通道 encoding 空串
    return writeLocked(ch.id, logTimeNs, data, len);
}

bool McapWriter::writeMetadata(const std::string& name, const std::map<std::string, std::string>& kv) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished || mFailed) return false;
    ::mcap::Metadata md;
    md.name = name;
    md.metadata.insert(kv.begin(), kv.end());   // KeyValueMap=unordered_map，与 std::map 显式转换
    try {
        const ::mcap::Status st = mWriter.write(md);
        if (!st.ok()) { mFailed = true; return false; }
    } catch (const std::exception&) {
        mFailed = true;
        return false;
    }
    return true;
}

bool McapWriter::writeAttachment(const std::string& name, const std::string& mediaType,
                                 int64_t logTimeNs, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lk(mtx);
    if (!mOpened || mFinished || mFailed) return false;
    if (data == nullptr && len > 0) return false;
    ::mcap::Attachment att;
    att.logTime = ::mcap::Timestamp(logTimeNs);
    att.createTime = att.logTime;           // createTime == logTime（统一定稿时刻语义）
    att.name = name;
    att.mediaType = mediaType;
    att.data = reinterpret_cast<const std::byte*>(data);
    att.dataSize = len;
    try {
        const ::mcap::Status st = mWriter.write(att);   // 官方自动算 CRC（noAttachmentCRC 默认 false）
        if (!st.ok()) { mFailed = true; return false; }
    } catch (const std::exception&) {
        mFailed = true;
        return false;
    }
    return true;
}

} // namespace xr::mcap
