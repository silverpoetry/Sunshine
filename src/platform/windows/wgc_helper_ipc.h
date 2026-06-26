/**
 * @file src/platform/windows/wgc_helper_ipc.h
 * @brief Wire protocol shared by Sunshine and the Windows.Graphics.Capture helper.
 */
#pragma once

#include <cstdint>

#include <d3d11.h>

namespace platf::dxgi::wgc_helper {
  constexpr std::uint32_t protocol_magic = 0x57474348;  // WGCH
  constexpr std::uint32_t protocol_version = 1;
  constexpr std::uint32_t max_name_chars = 128;
  constexpr std::uint32_t max_display_name_chars = 32;

  enum class message_type: std::uint32_t {
    init = 1,
    frame = 2,
    cursor = 3,
    shutdown = 4,
    error = 5,
    frame_request = 6,
    frame_release = 7,
    no_frame = 8,
  };

  enum class error_code: std::uint32_t {
    generic = 1,
    winrt_init = 2,
    dispatcher = 3,
    d3d_device = 4,
    dxgi_device = 5,
    capture_unsupported = 6,
    monitor_not_found = 7,
    capture_item = 8,
    frame_pool = 9,
    start_capture = 10,
    frame_surface = 11,
    shared_texture = 12,
    pipe_write = 13,
  };

  struct message_header {
    std::uint32_t magic;
    std::uint32_t version;
    message_type type;
    std::uint32_t size;
  };

  struct init_message {
    message_header header;
    wchar_t display_name[max_display_name_chars];
    DXGI_FORMAT format;
    bool cursor_visible;
  };

  struct cursor_message {
    message_header header;
    bool cursor_visible;
  };

  struct frame_request_message {
    message_header header;
    std::uint32_t timeout_ms;
    std::uint64_t request_id;
  };

  struct frame_message {
    message_header header;
    wchar_t texture_name[max_name_chars];
    std::uint64_t shared_handle;
    std::uint32_t width;
    std::uint32_t height;
    DXGI_FORMAT format;
    std::uint64_t qpc_timestamp;
    std::uint64_t sequence;
    std::uint64_t helper_send_qpc;
    std::uint64_t request_id;
  };

  struct no_frame_message {
    message_header header;
    std::uint64_t request_id;
  };

  struct frame_release_message {
    message_header header;
    std::uint64_t shared_handle;
    std::uint64_t sequence;
    std::uint64_t helper_send_qpc;
    std::uint64_t main_received_qpc;
    std::uint64_t main_acquired_qpc;
    std::uint64_t main_released_qpc;
  };

  struct error_message {
    message_header header;
    std::uint32_t code;
    std::uint32_t detail;
  };

  inline message_header make_header(message_type type, std::uint32_t size) {
    return {protocol_magic, protocol_version, type, size};
  }

  inline bool valid_header(const message_header &header, message_type type, std::uint32_t size) {
    return header.magic == protocol_magic &&
           header.version == protocol_version &&
           header.type == type &&
           header.size == size;
  }
}  // namespace platf::dxgi::wgc_helper
