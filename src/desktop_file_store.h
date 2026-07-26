#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace desktop_file_store {
  constexpr std::size_t max_chunk_bytes = 4ULL * 1024ULL * 1024ULL;
  constexpr int pending_ttl_seconds = 10 * 60;
  constexpr int completed_ttl_seconds = 24 * 60 * 60;

  using digest_t = std::array<std::uint8_t, 32>;

  struct begin_result_t {
    bool ok {};
    std::string id;
    std::size_t manifest_size {};
    digest_t manifest_sha256 {};
    std::string error;
  };

  struct operation_result_t {
    bool ok {};
    std::string error;
  };

  begin_result_t begin(std::vector<std::uint8_t> manifest, std::string token, std::string idempotency_key);

  operation_result_t write_chunk(const std::string &id, const std::string &token, std::uint32_t file_index, std::uint64_t offset, const std::vector<std::uint8_t> &bytes, const digest_t &expected_sha256);

  operation_result_t complete(const std::string &id, const std::string &token);

  void sweep_expired();

#ifdef SUNSHINE_TESTS
  void clear_for_tests();
  void set_desktop_for_tests(std::filesystem::path desktop);
#endif
}  // namespace desktop_file_store
