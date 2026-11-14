#pragma once

#include <string_view>

namespace tagimg {

// 是否是标准的 8-4-4-4-12 十六进制 UUID 文本，允许大写。
bool is_uuid(std::string_view value);

} // namespace tagimg
