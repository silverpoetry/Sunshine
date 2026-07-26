/**
 * @file src/platform/windows/clipboard_helper_ipc.h
 * @brief Wire format for reading the interactive user's file clipboard.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace platf::windows::clipboard_helper {
  constexpr std::uint32_t protocol_magic = 0x434C5048;  // CLPH
  constexpr std::uint32_t protocol_version = 1;
  constexpr std::uint32_t max_path_chars = 32767;
  constexpr std::size_t max_response_bytes = 16ULL * 1024ULL * 1024ULL;

  enum class status: std::uint32_t {
    success = 0,
    no_files = 1,
    clipboard_error = 2,
    invalid_data = 3,
  };

  struct response_header {
    std::uint32_t magic;
    std::uint32_t version;
    status result;
    std::uint32_t detail;
    std::uint32_t path_count;
  };

  static_assert(sizeof(response_header) == 20);
}  // namespace platf::windows::clipboard_helper
