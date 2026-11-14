#include "common/uuid.hpp"

#include <cctype>

namespace tagimg {

bool is_uuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') {
        return false;
      }
    } else if (std::isxdigit(static_cast<unsigned char>(value[i])) == 0) {
      return false;
    }
  }
  return true;
}

} // namespace tagimg
