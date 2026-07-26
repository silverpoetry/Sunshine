#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace clipboard_file_store {
  constexpr std::size_t max_chunk_bytes = 4ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t max_staged_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t minimum_free_bytes = 512ULL * 1024ULL * 1024ULL;
  constexpr int pending_ttl_seconds = 10 * 60;
  constexpr int completed_ttl_seconds = 24 * 60 * 60;

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

  struct resolve_result_t {
    bool ok {};
    std::vector<std::filesystem::path> paths;
    std::string error;
  };

  reference_result_t register_sources(const std::vector<std::filesystem::path> &paths, std::uint64_t origin_id, std::string idempotency_key);

  reference_result_t begin_upload(std::vector<std::uint8_t> manifest, std::uint64_t origin_id, std::string idempotency_key);

  operation_result_t write_chunk(const std::string &id, std::uint64_t origin_id, std::uint32_t file_index, std::uint64_t offset, const std::vector<std::uint8_t> &bytes, const digest_t &expected_sha256);

  operation_result_t complete_upload(const std::string &id, std::uint64_t origin_id);

  manifest_result_t get_manifest(const std::string &id);

  chunk_result_t read_chunk(const std::string &id, std::uint32_t file_index, std::uint64_t offset, std::size_t length);

  resolve_result_t resolve_upload(const std::string &id, std::uint64_t origin_id, std::size_t manifest_size, const digest_t &manifest_sha256);

  void sweep_expired();

#ifdef SUNSHINE_TESTS
  void clear_for_tests();
#endif
}  // namespace clipboard_file_store
