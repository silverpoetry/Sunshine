#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace clipboard_file_store {
  constexpr std::size_t max_chunk_bytes = 4ULL * 1024ULL * 1024ULL;
  constexpr int source_ttl_seconds = 24 * 60 * 60;
  constexpr int request_timeout_seconds = 60;
  constexpr int poll_timeout_seconds = 25;

  using digest_t = std::array<std::uint8_t, 32>;

  struct reference_result_t {
    bool ok {};
    std::string id;
    std::size_t manifest_size {};
    digest_t manifest_sha256 {};
    std::string error;
  };

  struct manifest_result_t {
    bool found {};
    std::vector<std::uint8_t> bytes;
    digest_t sha256 {};
    std::uint64_t origin_id {};
  };

  struct chunk_result_t {
    bool ok {};
    std::vector<std::uint8_t> bytes;
    digest_t sha256 {};
    std::string error;
  };

  struct operation_result_t {
    bool ok {};
    std::string error;
  };

  struct source_result_t {
    bool ok {};
    std::vector<std::uint8_t> manifest;
    std::string error;
  };

  struct request_result_t {
    bool found {};
    std::string request_id;
    std::uint32_t file_index {};
    std::uint64_t offset {};
    std::size_t length {};
    std::string error;
  };

  reference_result_t register_sources(const std::vector<std::filesystem::path> &paths, std::uint64_t origin_id, std::string idempotency_key);

  reference_result_t register_remote_source(std::vector<std::uint8_t> manifest, std::uint64_t origin_id, std::string idempotency_key);

  source_result_t resolve_remote_source(const std::string &id, std::uint64_t origin_id, std::size_t manifest_size, const digest_t &manifest_sha256);

  request_result_t poll_remote_request(const std::string &id, std::uint64_t origin_id, int timeout_seconds = poll_timeout_seconds);

  operation_result_t fulfill_remote_request(const std::string &id, std::uint64_t origin_id, const std::string &request_id, const std::vector<std::uint8_t> &bytes, const digest_t &expected_sha256);

  operation_result_t fail_remote_request(const std::string &id, std::uint64_t origin_id, const std::string &request_id, std::string error);

  operation_result_t release_remote_source(const std::string &id, std::uint64_t origin_id);

  chunk_result_t request_remote_chunk(const std::string &id, std::uint64_t origin_id, std::uint32_t file_index, std::uint64_t offset, std::size_t length);

  manifest_result_t get_manifest(const std::string &id);

  chunk_result_t read_chunk(const std::string &id, std::uint32_t file_index, std::uint64_t offset, std::size_t length);

  void sweep_expired();
  void release_origin(std::uint64_t origin_id);
  void release_all_remote_sources();

#ifdef SUNSHINE_TESTS
  void clear_for_tests();
#endif
}  // namespace clipboard_file_store
