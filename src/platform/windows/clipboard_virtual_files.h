#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platf::windows {
  bool set_virtual_file_clipboard(const std::vector<std::uint8_t> &manifest, const std::string &transfer_id, std::uint64_t origin_id, std::uint64_t item_id);
}
