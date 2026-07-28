#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace platf::windows {
#ifdef SUNSHINE_CLIPBOARD_HELPER
  bool request_virtual_file_manifest_from_service(
    std::vector<std::uint8_t> &manifest
  );

  bool request_virtual_file_chunk_from_service(
    std::uint32_t file_index,
    std::uint64_t offset,
    std::size_t length,
    std::vector<std::uint8_t> &bytes
  );
#endif

  bool get_file_clipboard_paths(std::vector<std::filesystem::path> &paths);
  bool set_virtual_file_clipboard(const std::string &transfer_id, std::uint64_t origin_id, std::uint64_t item_id);
}
