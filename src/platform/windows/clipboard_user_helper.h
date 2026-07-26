#pragma once

#include <filesystem>
#include <vector>

namespace platf::windows {
  bool get_user_file_clipboard_paths(std::vector<std::filesystem::path> &paths);
}  // namespace platf::windows
