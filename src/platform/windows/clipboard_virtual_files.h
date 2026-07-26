#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platf::windows {
  bool get_file_clipboard_paths(std::vector<std::filesystem::path> &paths);
  bool set_virtual_file_clipboard(const std::vector<std::uint8_t> &manifest, const std::string &transfer_id, std::uint64_t origin_id, std::uint64_t item_id);
}
