#include "clipboard_blob_store.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace clipboard_blob_store {
  namespace {
    using clock_t = std::chrono::steady_clock;

    struct entry_t {
      std::vector<std::uint8_t> bytes;
      std::string mime;
      digest_t sha256;
      std::uint64_t origin_id;
      std::string idempotency_key;
      clock_t::time_point expires_at;
    };

    std::mutex store_mutex;
    std::unordered_map<std::string, entry_t> entries;
    std::unordered_map<std::string, std::string> idempotency_entries;
    std::deque<std::string> insertion_order;
    std::size_t resident_bytes {};

    std::string idempotency_map_key(std::uint64_t origin_id, const std::string &key) {
      return std::to_string(origin_id) + ':' + key;
    }

    digest_t calculate_sha256(const std::vector<std::uint8_t> &bytes) {
      digest_t digest {};
      unsigned int digest_length = 0;
      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (!context) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
      }

      const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                      EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
                      EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1;
      EVP_MD_CTX_free(context);
      if (!ok || digest_length != digest.size()) {
        throw std::runtime_error("SHA-256 calculation failed");
      }
      return digest;
    }

    std::string make_id() {
      std::array<std::uint8_t, 16> raw {};
      if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
      }
      raw[6] = static_cast<std::uint8_t>((raw[6] & 0x0F) | 0x40);
      raw[8] = static_cast<std::uint8_t>((raw[8] & 0x3F) | 0x80);

      static constexpr char hex[] = "0123456789abcdef";
      static constexpr int slots[] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};
      std::string id(36, '-');
      for (std::size_t i = 0; i < raw.size(); ++i) {
        id[slots[i]] = hex[raw[i] >> 4];
        id[slots[i] + 1] = hex[raw[i] & 0x0F];
      }
      return id;
    }

    void erase_entry_locked(const std::string &id) {
      auto position = entries.find(id);
      if (position == entries.end()) {
        return;
      }
      resident_bytes -= position->second.bytes.size();
      if (!position->second.idempotency_key.empty()) {
        idempotency_entries.erase(idempotency_map_key(position->second.origin_id,
                                                        position->second.idempotency_key));
      }
      entries.erase(position);
    }

    void sweep_locked(clock_t::time_point now) {
      while (!insertion_order.empty()) {
        auto position = entries.find(insertion_order.front());
        if (position == entries.end()) {
          insertion_order.pop_front();
          continue;
        }
        if (position->second.expires_at > now) {
          break;
        }
        erase_entry_locked(insertion_order.front());
        insertion_order.pop_front();
      }
    }

    void evict_for_locked(std::size_t incoming) {
      while (!insertion_order.empty() && resident_bytes + incoming > max_store_bytes) {
        auto id = std::move(insertion_order.front());
        insertion_order.pop_front();
        erase_entry_locked(id);
      }
    }
  }  // namespace

  put_result_t put(std::vector<std::uint8_t> bytes,
                   std::string mime,
                   std::uint64_t origin_id,
                   std::string idempotency_key) {
    if (bytes.empty()) {
      return {.error = "empty"};
    }
    if (bytes.size() > max_blob_bytes) {
      return {.error = "too_large"};
    }
    if (origin_id == 0) {
      return {.error = "inactive_origin"};
    }
    if (idempotency_key.empty() || idempotency_key.size() > 128) {
      return {.error = "bad_idempotency_key"};
    }

    try {
      auto digest = calculate_sha256(bytes);
      auto now = clock_t::now();
      std::lock_guard lock(store_mutex);
      sweep_locked(now);

      const auto map_key = idempotency_map_key(origin_id, idempotency_key);
      auto previous = idempotency_entries.find(map_key);
      if (previous != idempotency_entries.end()) {
        auto existing = entries.find(previous->second);
        if (existing != entries.end()) {
          if (existing->second.sha256 == digest &&
              existing->second.mime == mime &&
              existing->second.bytes.size() == bytes.size()) {
            return {
              .ok = true,
              .id = existing->first,
              .size = existing->second.bytes.size(),
              .sha256 = existing->second.sha256,
            };
          }
          return {.error = "idempotency_conflict"};
        }
        idempotency_entries.erase(previous);
      }

      evict_for_locked(bytes.size());
      std::string id = make_id();
      while (entries.contains(id)) {
        id = make_id();
      }

      const auto size = bytes.size();
      entry_t entry {
        .bytes = std::move(bytes),
        .mime = std::move(mime),
        .sha256 = digest,
        .origin_id = origin_id,
        .idempotency_key = std::move(idempotency_key),
        .expires_at = now + std::chrono::seconds(blob_ttl_seconds),
      };
      resident_bytes += size;
      insertion_order.push_back(id);
      idempotency_entries.emplace(map_key, id);
      entries.emplace(id, std::move(entry));
      return {
        .ok = true,
        .id = std::move(id),
        .size = size,
        .sha256 = digest,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  get_result_t get(const std::string &id) {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
    auto position = entries.find(id);
    if (position == entries.end()) {
      return {};
    }
    return {
      .found = true,
      .bytes = position->second.bytes,
      .mime = position->second.mime,
      .sha256 = position->second.sha256,
      .origin_id = position->second.origin_id,
    };
  }

  void sweep_expired() {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
  }

#ifdef SUNSHINE_TESTS
  void clear_for_tests() {
    std::lock_guard lock(store_mutex);
    entries.clear();
    idempotency_entries.clear();
    insertion_order.clear();
    resident_bytes = 0;
  }
#endif
}  // namespace clipboard_blob_store
