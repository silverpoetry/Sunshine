#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platf::windows {
  enum class user_virtual_clipboard_result {
    not_required,
    success,
    failure,
  };

  bool get_user_file_clipboard_paths(std::vector<std::filesystem::path> &paths);
  user_virtual_clipboard_result set_user_virtual_file_clipboard(
    const std::vector<std::uint8_t> &manifest,
    const std::string &transfer_id,
    std::uint64_t origin_id,
    std::uint64_t item_id
  );
}  // namespace platf::windows
