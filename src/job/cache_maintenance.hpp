#pragma once

#include <string>
#include <vector>

#include "job/job.hpp"

namespace tagimg {

// 重扫的数据库事务成功后调用：先提交每张图片新下载的源缓存，再按当前
// 编码配置删除 mini/thumb/small/regular 的旧变体。缓存错误只记录日志。
void finalize_rescan_cache(JobContext &context,
                           const std::vector<std::string> &image_ids,
                           std::vector<ScanResult> &scans);

} // namespace tagimg
