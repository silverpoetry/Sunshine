#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clipboard_blob_store {
  constexpr std::size_t max_blob_bytes = 32ULL * 1024ULL * 1024ULL;
  constexpr std::size_t max_store_bytes = 128ULL * 1024ULL * 1024ULL;
  constexpr int blob_ttl_seconds = 60;

  using digest_t = std::array<std::uint8_t, 32>;

  struct put_result_t {
    bool ok {};
    std::string id;
    std::size_t size {};
    digest_t sha256 {};
    std::string error;
  };

  struct get_result_t {
    bool found {};
    std::vector<std::uint8_t> bytes;
    std::string mime;
    digest_t sha256 {};
    std::uint64_t origin_id {};
  };

  put_result_t put(std::vector<std::uint8_t> bytes,
                   std::string mime,
                   std::uint64_t origin_id,
                   std::string idempotency_key);

  get_result_t get(const std::string &id);
  void sweep_expired();

#ifdef SUNSHINE_TESTS
  void clear_for_tests();
#endif
}  // namespace clipboard_blob_store
