#pragma once
#include <atomic>
namespace xr {
namespace mcap { class McapChunkManager; }
enum class DatasetFormat { MP4, MCAP };
// 数据集 JSON / mcap metadata 的格式版本：字段结构或坐标系约定变化时 +1（映射表见 Readme）。
// 2 = raw OpenXR Body + SVR 相机系(X上/Y右/Z前) + extrinsics convention 1 + sensor 原始帧 IMU bias。
// 1/缺失 = 历史格式（无版本字段；历史文件横跨 convention 0 与 raw SVR 两代，按录制日期判断）。
inline constexpr int kDatasetFormatVersion = 2;
class IDatasetFormatProvider {
public:
    virtual ~IDatasetFormatProvider() = default;
    virtual DatasetFormat format() const = 0;
    bool isMcap() const { return format() == DatasetFormat::MCAP; }
};
// PersistDatasetFormatProvider：构造时读 persist.xr.dataset_type 一次并缓存，
// "mcap" → MCAP，否则 MP4。声明可移植；实现仅 Android（见 PersistDatasetFormatProvider.cpp，
// 依赖 <sys/system_properties.h>，宿主测试不编译该文件）。
class PersistDatasetFormatProvider : public IDatasetFormatProvider {
public:
    PersistDatasetFormatProvider();
    DatasetFormat format() const override { return mFormat; }
private:
    DatasetFormat mFormat;
};
// 传感器组件 MCAP 注入组合体：mcap manager 裸指针 + 格式 provider（均 atomic，写入线程读、
// 录制启动线程经 setMcapManager/setFormatProvider 写，跨线程用 atomic 消除 data race）。
// 四个传感器类（ControllerPoseSaver/RawDateSave/ImuPoseCollector/DatasetRecorder）按值组合，
// 消除各自重复的 mFmt/mMcapMgr/setFormatProvider/setMcapManager/isMcapMode 样板。
struct McapSinkCtx {
    std::atomic<mcap::McapChunkManager*> mgr{nullptr};
    std::atomic<const IDatasetFormatProvider*> fmt{nullptr};
    void setMcapManager(mcap::McapChunkManager* m) { mgr.store(m); }
    void setFormatProvider(const IDatasetFormatProvider* p) { fmt = p; }
    bool isMcap() const { auto* f = fmt.load(); return f && f->isMcap(); }
};
} // namespace xr
