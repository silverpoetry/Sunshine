#include "clipboard_file_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

namespace clipboard_file_store {
  namespace {
    namespace fs = std::filesystem;
    using clock_t = std::chrono::steady_clock;

    struct file_t {
      std::string relative_path;
      std::uint8_t type {};
      std::uint64_t size {};
      std::uint64_t modified_time_ms {};
      fs::path path;
      std::uint64_t received_bytes {};
    };

    struct entry_t {
      std::vector<std::uint8_t> manifest;
      digest_t manifest_sha256 {};
      std::uint64_t origin_id {};
      std::string idempotency_key;
      std::vector<file_t> files;
      std::vector<fs::path> top_level_paths;
      fs::path staging_root;
      std::uint64_t total_file_bytes {};
      bool upload {};
      bool complete {};
      clock_t::time_point expires_at;
    };

    std::mutex store_mutex;
    std::unordered_map<std::string, entry_t> entries;
    std::unordered_map<std::string, std::string> idempotency_entries;

    std::string idempotency_map_key(std::uint64_t origin_id, const std::string &key) {
      return std::to_string(origin_id) + ':' + key;
    }

    digest_t calculate_sha256(const void *data, std::size_t size) {
      digest_t digest {};
      unsigned int digest_length = 0;
      EVP_MD_CTX *context = EVP_MD_CTX_new();
      if (!context) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
      }
      const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                      EVP_DigestUpdate(context, data, size) == 1 &&
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

    std::string path_to_utf8(const fs::path &path) {
      const auto value = path.generic_u8string();
      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    fs::path path_from_utf8(const std::string &path) {
      return fs::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
    }

    std::uint64_t modified_time_ms(const fs::path &path) {
      std::error_code error;
      const auto file_time = fs::last_write_time(path, error);
      if (error) {
        return 0;
      }
      const auto system_time = std::chrono::time_point_cast<std::chrono::milliseconds>(
        file_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
      );
      const auto value = system_time.time_since_epoch().count();
      return value > 0 ? static_cast<std::uint64_t>(value) : 0;
    }

    bool is_safe_source(const fs::path &path, std::error_code &error) {
      const auto status = fs::symlink_status(path, error);
      return !error &&
             !fs::is_symlink(status) &&
             (fs::is_regular_file(status) || fs::is_directory(status));
    }

    bool append_source(std::vector<file_t> &files, const fs::path &source, const std::string &relative_path, std::uint64_t &total_bytes, std::uint32_t &file_count, std::string &error_message) {
      std::error_code error;
      if (!is_safe_source(source, error)) {
        error_message = error ? error.message() : "unsupported_file_type";
        return false;
      }

      file_t file {
        .relative_path = relative_path,
        .type = static_cast<std::uint8_t>(
          fs::is_directory(source) ?
            LI_CLIPBOARD_FILE_TYPE_DIRECTORY :
            LI_CLIPBOARD_FILE_TYPE_REGULAR
        ),
        .modified_time_ms = modified_time_ms(source),
        .path = source,
      };
      if (file.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
        file.size = fs::file_size(source, error);
        if (error ||
            file.size > LI_CLIPBOARD_MAX_FILE_BYTES ||
            total_bytes > LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES - file.size) {
          error_message = error ? error.message() : "file_transfer_too_large";
          return false;
        }
        total_bytes += file.size;
        file_count++;
      }

      LI_CLIPBOARD_FILE_MANIFEST_ENTRY validation_entry {
        .type = file.type,
        .pathLength = static_cast<std::uint32_t>(relative_path.size()),
        .size = file.size,
        .modifiedTimeMs = file.modified_time_ms,
        .path = reinterpret_cast<const std::uint8_t *>(relative_path.data()),
      };
      std::array<std::uint8_t, LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE + LI_CLIPBOARD_MAX_FILE_PATH_BYTES> validation_buffer {};
      std::size_t validation_length = 0;
      if (!LiEncodeClipboardFileManifestEntry(validation_buffer.data(), validation_buffer.size(), &validation_entry, &validation_length)) {
        error_message = "unsafe_file_name";
        return false;
      }
      files.push_back(std::move(file));
      return true;
    }

    bool enumerate_sources(const std::vector<fs::path> &paths, std::vector<file_t> &files, std::vector<fs::path> &top_level_paths, std::string &error_message) {
      if (paths.empty()) {
        error_message = "empty_file_list";
        return false;
      }

      std::unordered_set<std::string> top_names;
      std::uint64_t total_bytes = 0;
      std::uint32_t file_count = 0;
      for (const auto &input_path : paths) {
        std::error_code error;
        auto path = fs::absolute(input_path, error);
        if (error || !is_safe_source(path, error)) {
          error_message = error ? error.message() : "unsupported_file_type";
          return false;
        }

        const auto root_name = path_to_utf8(path.filename());
        std::string folded_name = root_name;
        std::transform(folded_name.begin(), folded_name.end(), folded_name.begin(), [](unsigned char value) {
          return static_cast<char>(std::tolower(value));
        });
        if (root_name.empty() || !top_names.insert(folded_name).second) {
          error_message = "duplicate_top_level_name";
          return false;
        }

        top_level_paths.push_back(path);
        if (!append_source(files, path, root_name, total_bytes, file_count, error_message)) {
          return false;
        }
        if (!fs::is_directory(path)) {
          continue;
        }

        fs::recursive_directory_iterator iterator(path, fs::directory_options::none, error);
        const fs::recursive_directory_iterator end;
        while (!error && iterator != end) {
          if (files.size() >= LI_CLIPBOARD_MAX_FILE_ENTRIES) {
            error_message = "too_many_files";
            return false;
          }
          const auto child = iterator->path();
          if (!is_safe_source(child, error)) {
            error_message = error ? error.message() : "symbolic_links_not_supported";
            return false;
          }
          const auto relative = fs::relative(child, path, error);
          if (error) {
            error_message = error.message();
            return false;
          }
          const auto protocol_path = root_name + '/' + path_to_utf8(relative);
          if (!append_source(files, child, protocol_path, total_bytes, file_count, error_message)) {
            return false;
          }
          iterator.increment(error);
        }
        if (error) {
          error_message = error.message();
          return false;
        }
      }
      return files.size() <= LI_CLIPBOARD_MAX_FILE_ENTRIES;
    }

    bool encode_manifest(const std::vector<file_t> &files, std::vector<std::uint8_t> &manifest, std::string &error_message) {
      std::uint64_t total_bytes = 0;
      std::uint32_t file_count = 0;
      std::size_t manifest_size = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
      for (const auto &file : files) {
        manifest_size += LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE + file.relative_path.size();
        if (file.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
          total_bytes += file.size;
          file_count++;
        }
      }
      if (manifest_size > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES) {
        error_message = "manifest_too_large";
        return false;
      }

      manifest.assign(manifest_size, 0);
      LI_CLIPBOARD_FILE_MANIFEST_HEADER header {
        .entryCount = static_cast<std::uint32_t>(files.size()),
        .fileCount = file_count,
        .totalFileBytes = total_bytes,
      };
      if (!LiEncodeClipboardFileManifestHeader(manifest.data(), manifest.size(), &header)) {
        error_message = "invalid_manifest_header";
        return false;
      }

      std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
      for (const auto &file : files) {
        LI_CLIPBOARD_FILE_MANIFEST_ENTRY manifest_entry {
          .type = file.type,
          .pathLength = static_cast<std::uint32_t>(file.relative_path.size()),
          .size = file.size,
          .modifiedTimeMs = file.modified_time_ms,
          .path = reinterpret_cast<const std::uint8_t *>(file.relative_path.data()),
        };
        std::size_t encoded_length = 0;
        if (!LiEncodeClipboardFileManifestEntry(manifest.data() + offset, manifest.size() - offset, &manifest_entry, &encoded_length)) {
          error_message = "invalid_manifest_entry";
          return false;
        }
        offset += encoded_length;
      }
      return offset == manifest.size() &&
             LiIsValidClipboardFileManifest(manifest.data(), manifest.size());
    }

    bool decode_manifest(const std::vector<std::uint8_t> &manifest, std::vector<file_t> &files, std::string &error_message) {
      LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
      if (!LiIsValidClipboardFileManifest(manifest.data(), manifest.size()) ||
          !LiDecodeClipboardFileManifestHeader(manifest.data(), manifest.size(), &header)) {
        error_message = "invalid_manifest";
        return false;
      }

      std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
      files.reserve(header.entryCount);
      for (std::uint32_t index = 0; index < header.entryCount; index++) {
        LI_CLIPBOARD_FILE_MANIFEST_ENTRY decoded;
        if (!LiDecodeClipboardFileManifestEntry(manifest.data(), manifest.size(), &offset, &decoded)) {
          error_message = "invalid_manifest_entry";
          return false;
        }
        files.push_back({
          .relative_path = std::string(reinterpret_cast<const char *>(decoded.path), decoded.pathLength),
          .type = decoded.type,
          .size = decoded.size,
          .modified_time_ms = decoded.modifiedTimeMs,
        });
      }
      return true;
    }

    fs::path staging_base() {
      std::error_code error;
      auto base = fs::temp_directory_path(error) / "Sunshine" / "clipboard-files";
      if (error) {
        return {};
      }
      fs::create_directories(base, error);
      return error ? fs::path {} : base;
    }

    void remove_stale_staging_locked(const fs::path &base) {
      std::error_code error;
      const auto cutoff = fs::file_time_type::clock::now() -
                          std::chrono::seconds(completed_ttl_seconds);
      fs::directory_iterator iterator(base, fs::directory_options::skip_permission_denied, error);
      const fs::directory_iterator end;
      while (!error && iterator != end) {
        const auto path = iterator->path();
        const auto id = path.filename().string();
        const auto modified = iterator->last_write_time(error);
        if (!error &&
            iterator->is_directory(error) &&
            !entries.contains(id) &&
            modified < cutoff) {
          std::error_code remove_error;
          fs::remove_all(path, remove_error);
        }
        if (error) {
          break;
        }
        iterator.increment(error);
      }
    }

    void remove_staging(const fs::path &path) {
      if (path.empty()) {
        return;
      }
      const auto base = staging_base();
      if (base.empty() || path.parent_path() != base) {
        return;
      }
      std::error_code error;
      fs::remove_all(path, error);
    }

    void erase_entry_locked(const std::string &id) {
      auto position = entries.find(id);
      if (position == entries.end()) {
        return;
      }
      if (!position->second.idempotency_key.empty()) {
        idempotency_entries.erase(idempotency_map_key(position->second.origin_id, position->second.idempotency_key));
      }
      const auto staging_root = position->second.staging_root;
      entries.erase(position);
      remove_staging(staging_root);
    }

    void sweep_locked(clock_t::time_point now) {
      for (auto position = entries.begin(); position != entries.end();) {
        if (position->second.expires_at > now) {
          ++position;
          continue;
        }
        const auto id = position->first;
        ++position;
        erase_entry_locked(id);
      }
    }

    std::string unique_id_locked() {
      std::string id = make_id();
      while (entries.contains(id)) {
        id = make_id();
      }
      return id;
    }

    reference_result_t existing_idempotent_locked(const digest_t &manifest_sha256, std::size_t manifest_size, std::uint64_t origin_id, const std::string &idempotency_key) {
      const auto map_key = idempotency_map_key(origin_id, idempotency_key);
      auto mapped = idempotency_entries.find(map_key);
      if (mapped == idempotency_entries.end()) {
        return {};
      }
      auto existing = entries.find(mapped->second);
      if (existing == entries.end()) {
        idempotency_entries.erase(mapped);
        return {};
      }
      if (existing->second.manifest_sha256 != manifest_sha256 ||
          existing->second.manifest.size() != manifest_size) {
        return {.error = "idempotency_conflict"};
      }
      return {
        .ok = true,
        .id = existing->first,
        .manifest_size = existing->second.manifest.size(),
        .manifest_sha256 = existing->second.manifest_sha256,
      };
    }
  }  // namespace

  reference_result_t register_sources(const std::vector<fs::path> &paths, std::uint64_t origin_id, std::string idempotency_key) {
    if (origin_id == 0 || idempotency_key.empty() || idempotency_key.size() > 128) {
      return {.error = "invalid_identity"};
    }

    try {
      std::vector<file_t> files;
      std::vector<fs::path> top_level_paths;
      std::string error;
      if (!enumerate_sources(paths, files, top_level_paths, error)) {
        return {.error = std::move(error)};
      }

      std::vector<std::uint8_t> manifest;
      if (!encode_manifest(files, manifest, error)) {
        return {.error = std::move(error)};
      }
      const auto digest = calculate_sha256(manifest.data(), manifest.size());
      const auto now = clock_t::now();

      std::lock_guard lock(store_mutex);
      sweep_locked(now);
      auto existing = existing_idempotent_locked(digest, manifest.size(), origin_id, idempotency_key);
      if (existing.ok || !existing.error.empty()) {
        return existing;
      }

      const auto id = unique_id_locked();
      entry_t entry {
        .manifest = std::move(manifest),
        .manifest_sha256 = digest,
        .origin_id = origin_id,
        .idempotency_key = idempotency_key,
        .files = std::move(files),
        .top_level_paths = std::move(top_level_paths),
        .upload = false,
        .complete = true,
        .expires_at = now + std::chrono::seconds(pending_ttl_seconds),
      };
      const auto manifest_size = entry.manifest.size();
      idempotency_entries.emplace(idempotency_map_key(origin_id, idempotency_key), id);
      entries.emplace(id, std::move(entry));
      return {
        .ok = true,
        .id = id,
        .manifest_size = manifest_size,
        .manifest_sha256 = digest,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  reference_result_t begin_upload(std::vector<std::uint8_t> manifest, std::uint64_t origin_id, std::string idempotency_key) {
    if (origin_id == 0 || idempotency_key.empty() || idempotency_key.size() > 128) {
      return {.error = "invalid_identity"};
    }

    try {
      std::vector<file_t> files;
      std::string error_message;
      if (!decode_manifest(manifest, files, error_message)) {
        return {.error = std::move(error_message)};
      }
      const auto digest = calculate_sha256(manifest.data(), manifest.size());
      const auto now = clock_t::now();

      std::lock_guard lock(store_mutex);
      sweep_locked(now);
      auto existing = existing_idempotent_locked(digest, manifest.size(), origin_id, idempotency_key);
      if (existing.ok || !existing.error.empty()) {
        return existing;
      }

      const auto base = staging_base();
      if (base.empty()) {
        return {.error = "staging_directory_unavailable"};
      }
      remove_stale_staging_locked(base);

      LI_CLIPBOARD_FILE_MANIFEST_HEADER manifest_header;
      if (!LiDecodeClipboardFileManifestHeader(
            manifest.data(),
            manifest.size(),
            &manifest_header
          )) {
        return {.error = "invalid_manifest_header"};
      }
      std::uint64_t staged_bytes = 0;
      for (const auto &[entry_id, entry] : entries) {
        (void) entry_id;
        if (entry.upload) {
          if (staged_bytes > max_staged_bytes - entry.total_file_bytes) {
            staged_bytes = max_staged_bytes;
            break;
          }
          staged_bytes += entry.total_file_bytes;
        }
      }
      std::error_code space_error;
      const auto space = fs::space(base, space_error);
      if (manifest_header.totalFileBytes > max_staged_bytes ||
          staged_bytes > max_staged_bytes - manifest_header.totalFileBytes ||
          space_error ||
          space.available < minimum_free_bytes ||
          manifest_header.totalFileBytes > space.available - minimum_free_bytes) {
        return {.error = "staging_quota_exceeded"};
      }

      const auto id = unique_id_locked();
      const auto staging_root = base / id;
      std::error_code fs_error;
      fs::create_directory(staging_root, fs_error);
      if (fs_error) {
        return {.error = fs_error.message()};
      }

      std::vector<fs::path> top_level_paths;
      for (auto &file : files) {
        file.path = staging_root / path_from_utf8(file.relative_path);
        const bool top_level = file.relative_path.find('/') == std::string::npos;
        if (top_level) {
          top_level_paths.push_back(file.path);
        }
        if (file.type == LI_CLIPBOARD_FILE_TYPE_DIRECTORY) {
          fs::create_directories(file.path, fs_error);
        } else {
          fs::create_directories(file.path.parent_path(), fs_error);
          if (!fs_error) {
            std::ofstream output(file.path, std::ios::binary | std::ios::trunc);
            if (!output) {
              fs_error = std::make_error_code(std::errc::io_error);
            }
          }
        }
        if (fs_error) {
          remove_staging(staging_root);
          return {.error = fs_error.message()};
        }
      }

      entry_t entry {
        .manifest = std::move(manifest),
        .manifest_sha256 = digest,
        .origin_id = origin_id,
        .idempotency_key = idempotency_key,
        .files = std::move(files),
        .top_level_paths = std::move(top_level_paths),
        .staging_root = staging_root,
        .total_file_bytes = manifest_header.totalFileBytes,
        .upload = true,
        .complete = false,
        .expires_at = now + std::chrono::seconds(pending_ttl_seconds),
      };
      const auto manifest_size = entry.manifest.size();
      idempotency_entries.emplace(idempotency_map_key(origin_id, idempotency_key), id);
      entries.emplace(id, std::move(entry));
      return {
        .ok = true,
        .id = id,
        .manifest_size = manifest_size,
        .manifest_sha256 = digest,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  operation_result_t write_chunk(const std::string &id, std::uint64_t origin_id, std::uint32_t file_index, std::uint64_t offset, const std::vector<std::uint8_t> &bytes, const digest_t &expected_sha256) {
    if (bytes.empty() || bytes.size() > max_chunk_bytes) {
      return {.error = "bad_chunk_size"};
    }

    try {
      if (calculate_sha256(bytes.data(), bytes.size()) != expected_sha256) {
        return {.error = "chunk_hash_mismatch"};
      }

      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now());
      auto position = entries.find(id);
      if (position == entries.end()) {
        return {.error = "not_found"};
      }
      auto &entry = position->second;
      if (!entry.upload || entry.complete || entry.origin_id != origin_id ||
          file_index >= entry.files.size()) {
        return {.error = "invalid_upload"};
      }

      auto &file = entry.files[file_index];
      if (file.type != LI_CLIPBOARD_FILE_TYPE_REGULAR ||
          offset > file.size ||
          bytes.size() > file.size - offset) {
        return {.error = "bad_file_range"};
      }

      if (offset < file.received_bytes) {
        if (bytes.size() > file.received_bytes - offset) {
          return {.error = "non_sequential_chunk"};
        }
        std::ifstream existing(file.path, std::ios::binary);
        std::vector<std::uint8_t> previous(bytes.size());
        existing.seekg(static_cast<std::streamoff>(offset));
        existing.read(reinterpret_cast<char *>(previous.data()), static_cast<std::streamsize>(previous.size()));
        return existing && previous == bytes ?
                 operation_result_t {.ok = true} :
                 operation_result_t {.error = "retry_content_mismatch"};
      }
      if (offset != file.received_bytes) {
        return {.error = "non_sequential_chunk"};
      }

      std::fstream output(file.path, std::ios::binary | std::ios::in | std::ios::out);
      if (!output) {
        return {.error = "open_failed"};
      }
      output.seekp(static_cast<std::streamoff>(offset));
      output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.flush();
      if (!output) {
        return {.error = "write_failed"};
      }
      file.received_bytes += bytes.size();
      entry.expires_at = clock_t::now() + std::chrono::seconds(pending_ttl_seconds);
      return {.ok = true};
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  operation_result_t complete_upload(const std::string &id, std::uint64_t origin_id) {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
    auto position = entries.find(id);
    if (position == entries.end()) {
      return {.error = "not_found"};
    }
    auto &entry = position->second;
    if (!entry.upload || entry.origin_id != origin_id) {
      return {.error = "invalid_upload"};
    }
    if (entry.complete) {
      return {.ok = true};
    }
    if (std::any_of(entry.files.begin(), entry.files.end(), [](const file_t &file) {
          return file.type == LI_CLIPBOARD_FILE_TYPE_REGULAR &&
                 file.received_bytes != file.size;
        })) {
      return {.error = "upload_incomplete"};
    }

    for (auto position = entry.files.rbegin(); position != entry.files.rend(); ++position) {
      if (position->modified_time_ms == 0) {
        continue;
      }
      const auto system_time = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(position->modified_time_ms)
      );
      const auto file_time = std::chrono::time_point_cast<fs::file_time_type::duration>(
        system_time - std::chrono::system_clock::now() + fs::file_time_type::clock::now()
      );
      std::error_code error;
      fs::last_write_time(position->path, file_time, error);
    }

    entry.complete = true;
    entry.expires_at = clock_t::now() + std::chrono::seconds(completed_ttl_seconds);
    return {.ok = true};
  }

  manifest_result_t get_manifest(const std::string &id) {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
    auto position = entries.find(id);
    if (position == entries.end() || !position->second.complete) {
      return {};
    }
    position->second.expires_at = clock_t::now() +
                                  std::chrono::seconds(position->second.upload ? completed_ttl_seconds : pending_ttl_seconds);
    return {
      .found = true,
      .bytes = position->second.manifest,
      .sha256 = position->second.manifest_sha256,
      .origin_id = position->second.origin_id,
    };
  }

  chunk_result_t read_chunk(const std::string &id, std::uint32_t file_index, std::uint64_t offset, std::size_t length) {
    if (length == 0 || length > max_chunk_bytes) {
      return {.error = "bad_chunk_size"};
    }

    try {
      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now());
      auto position = entries.find(id);
      if (position == entries.end() || !position->second.complete ||
          file_index >= position->second.files.size()) {
        return {.error = "not_found"};
      }
      auto &entry = position->second;
      auto &file = entry.files[file_index];
      if (file.type != LI_CLIPBOARD_FILE_TYPE_REGULAR ||
          offset > file.size ||
          length > file.size - offset) {
        return {.error = "bad_file_range"};
      }

      std::error_code fs_error;
      if (!fs::is_regular_file(file.path, fs_error) ||
          fs::file_size(file.path, fs_error) != file.size ||
          fs_error) {
        return {.error = "source_changed"};
      }

      std::ifstream input(file.path, std::ios::binary);
      std::vector<std::uint8_t> bytes(length);
      input.seekg(static_cast<std::streamoff>(offset));
      input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(length));
      if (!input || static_cast<std::size_t>(input.gcount()) != length) {
        return {.error = "read_failed"};
      }
      entry.expires_at = clock_t::now() +
                         std::chrono::seconds(entry.upload ? completed_ttl_seconds : pending_ttl_seconds);
      return {
        .ok = true,
        .bytes = bytes,
        .sha256 = calculate_sha256(bytes.data(), bytes.size()),
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  resolve_result_t resolve_upload(const std::string &id, std::uint64_t origin_id, std::size_t manifest_size, const digest_t &manifest_sha256) {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
    auto position = entries.find(id);
    if (position == entries.end()) {
      return {.error = "not_found"};
    }
    auto &entry = position->second;
    if (!entry.upload || !entry.complete ||
        entry.origin_id != origin_id ||
        entry.manifest.size() != manifest_size ||
        entry.manifest_sha256 != manifest_sha256) {
      return {.error = "reference_mismatch"};
    }
    entry.expires_at = clock_t::now() + std::chrono::seconds(completed_ttl_seconds);
    return {.ok = true, .paths = entry.top_level_paths};
  }

  void sweep_expired() {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
  }

#ifdef SUNSHINE_TESTS
  void clear_for_tests() {
    std::lock_guard lock(store_mutex);
    for (const auto &[id, entry] : entries) {
      remove_staging(entry.staging_root);
    }
    entries.clear();
    idempotency_entries.clear();
  }
#endif
}  // namespace clipboard_file_store
