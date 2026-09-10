// PersistDatasetFormatProvider 实现（仅 Android：依赖 <sys/system_properties.h>，
// 宿主测试不编译本文件）。构造时读 persist.xr.dataset_type 一次并缓存：
// "mcap" → DatasetFormat::MCAP，否则 MP4。
#include "DatasetFormat.h"
#include <sys/system_properties.h>
#include <cstring>

namespace xr {

PersistDatasetFormatProvider::PersistDatasetFormatProvider() : mFormat(DatasetFormat::MP4) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.xr.dataset_type", buf) > 0 &&
        strcmp(buf, "mcap") == 0) {
        mFormat = DatasetFormat::MCAP;
    }
}

} // namespace xr
