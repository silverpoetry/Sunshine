/**
 * @file src/platform/windows/clipboard_helper_ipc.h
 * @brief Wire formats used by the interactive-user clipboard helper.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace platf::windows::clipboard_helper {
  constexpr std::uint32_t protocol_magic = 0x434C5048;  // CLPH
  constexpr std::uint32_t protocol_version = 3;
  constexpr std::uint32_t max_path_chars = 32767;
  constexpr std::uint32_t max_transfer_id_bytes = 256;
  constexpr std::size_t max_response_bytes = 16ULL * 1024ULL * 1024ULL;

  enum class status: std::uint32_t {
    success = 0,
    no_files = 1,
    clipboard_error = 2,
    invalid_data = 3,
    transfer_error = 4,
  };

#pragma pack(push, 1)

  struct response_header {
    std::uint32_t magic;
    std::uint32_t version;
    status result;
    std::uint32_t detail;
    std::uint32_t path_count;
  };

  enum class message_type: std::uint32_t {
    publish_request = 1,
    publish_ready = 2,
    file_request = 3,
    file_response = 4,
  };

  enum class file_request_kind: std::uint32_t {
    manifest = 1,
    chunk = 2,
  };

  struct publish_request_header {
    std::uint32_t magic;
    std::uint32_t version;
    message_type type;
    std::uint32_t transfer_id_size;
    std::uint64_t origin_id;
    std::uint64_t item_id;
  };

  struct publish_ready_message {
    std::uint32_t magic;
    std::uint32_t version;
    message_type type;
    status result;
    std::uint32_t detail;
  };

  struct file_request_message {
    std::uint32_t magic;
    std::uint32_t version;
    message_type type;
    file_request_kind kind;
    std::uint32_t file_index;
    std::uint64_t offset;
    std::uint32_t length;
  };

  struct file_response_header {
    std::uint32_t magic;
    std::uint32_t version;
    message_type type;
    status result;
    std::uint32_t detail;
    std::uint32_t length;
  };

#pragma pack(pop)

  static_assert(sizeof(response_header) == 20);
  static_assert(sizeof(publish_request_header) == 32);
  static_assert(sizeof(publish_ready_message) == 20);
  static_assert(sizeof(file_request_message) == 32);
  static_assert(sizeof(file_response_header) == 24);
}  // namespace platf::windows::clipboard_helper
