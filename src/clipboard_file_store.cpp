#include "clipboard_file_store.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "logging.h"
#include "utility.h"

#ifdef _WIN32
  #include <windows.h>
  #include <wtsapi32.h>
#endif

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

namespace clipboard_file_store {
  static_assert(max_chunk_bytes == LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES);

  namespace {
    namespace fs = std::filesystem;
    using clock_t = std::chrono::steady_clock;

    struct file_t {
      std::string relative_path;
      std::uint8_t type {};
      std::uint64_t size {};
      std::uint64_t modified_time_ms {};
      fs::path path;
      std::optional<fs::file_time_type> modified_time;
    };

    struct pending_request_t {
      std::string id;
      request_kind_e kind {request_kind_e::chunk};
      std::uint32_t file_index {};
      std::uint64_t offset {};
      std::size_t length {};
      std::mutex mutex;
      std::condition_variable ready;
      bool complete {};
      std::vector<std::uint8_t> bytes;
      std::string error;
    };

    struct entry_t {
      std::vector<fs::path> local_paths;
      std::vector<std::uint8_t> manifest;
      digest_t manifest_sha256 {};
      std::uint64_t authorized_origin_id {};
      std::string idempotency_key;
      std::vector<file_t> files;
      bool remote_source {};
      bool manifest_building {};
      clock_t::time_point expires_at;
      std::deque<std::string> queued_requests;
      std::unordered_map<std::string, std::shared_ptr<pending_request_t>> requests;
    };

    std::mutex store_mutex;
    std::condition_variable request_available;
    std::condition_variable manifest_available;
    std::unordered_map<std::string, entry_t> entries;
    std::unordered_map<std::string, std::string> idempotency_entries;

#ifdef _WIN32
    std::optional<bool> is_local_system_process() {
      HANDLE token {};
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return std::nullopt;
      }
      auto token_guard = util::fail_guard([token]() {
        CloseHandle(token);
      });

      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      std::vector<std::uint8_t> buffer(size);
      if (size == 0 ||
          !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        return std::nullopt;
      }

      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_sid {};
      DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
      if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid.data(),
            &system_sid_size
          )) {
        return std::nullopt;
      }

      const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      return EqualSid(user->User.Sid, system_sid.data());
    }
#endif

    template<typename Function>
    auto with_current_user_file_access(Function &&function)
      -> std::optional<std::invoke_result_t<Function>> {
#ifdef _WIN32
      const auto system_process = is_local_system_process();
      if (!system_process) {
        BOOST_LOG(error) << "Failed to determine the Sunshine process identity";
        return std::nullopt;
      }
      if (*system_process) {
        const DWORD session_id = WTSGetActiveConsoleSessionId();
        HANDLE token {};
        if (session_id == 0xFFFFFFFF ||
            !WTSQueryUserToken(session_id, &token)) {
          BOOST_LOG(error) << "Failed to query the interactive user for file access: "
                           << GetLastError();
          return std::nullopt;
        }
        auto token_guard = util::fail_guard([token]() {
          CloseHandle(token);
        });
        if (!ImpersonateLoggedOnUser(token)) {
          BOOST_LOG(error) << "Failed to impersonate the interactive user for file access: "
                           << GetLastError();
          return std::nullopt;
        }
        auto impersonation_guard = util::fail_guard([]() {
          if (!RevertToSelf()) {
            BOOST_LOG(fatal) << "Failed to revert file access impersonation: "
                             << GetLastError();
            DebugBreak();
          }
        });
        return std::invoke(std::forward<Function>(function));
      }
#endif
      return std::invoke(std::forward<Function>(function));
    }

    std::string idempotency_map_key(
      std::uint64_t authorized_origin_id,
      const std::string &key
    ) {
      return std::to_string(authorized_origin_id) + ':' + key;
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
      for (std::size_t index = 0; index < raw.size(); ++index) {
        id[slots[index]] = hex[raw[index] >> 4];
        id[slots[index] + 1] = hex[raw[index] & 0x0F];
      }
      return id;
    }

    std::string path_to_utf8(const fs::path &path) {
      const auto value = path.generic_u8string();
      return {reinterpret_cast<const char *>(value.data()), value.size()};
    }

    std::uint64_t modified_time_ms(fs::file_time_type file_time) {
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

      const auto file_modified_time = fs::last_write_time(source, error);
      const bool has_modified_time = !error;
      error.clear();

      file_t file {
        .relative_path = relative_path,
        .type = static_cast<std::uint8_t>(
          fs::is_directory(source) ?
            LI_CLIPBOARD_FILE_TYPE_DIRECTORY :
            LI_CLIPBOARD_FILE_TYPE_REGULAR
        ),
        .modified_time_ms = has_modified_time ?
                              modified_time_ms(file_modified_time) :
                              0,
        .path = source,
        .modified_time = has_modified_time ?
                           std::make_optional(file_modified_time) :
                           std::nullopt,
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

    bool enumerate_sources(const std::vector<fs::path> &paths, std::vector<file_t> &files, std::string &error_message) {
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
      return offset == manifest.size();
    }

    void cancel_requests_locked(entry_t &entry, const std::string &error) {
      for (const auto &[id, request] : entry.requests) {
        std::lock_guard request_lock(request->mutex);
        if (!request->complete) {
          request->complete = true;
          request->error = error;
          request->ready.notify_all();
        }
      }
      entry.queued_requests.clear();
      entry.requests.clear();
    }

    void erase_entry_locked(const std::string &id) {
      auto position = entries.find(id);
      if (position == entries.end()) {
        return;
      }
      if (!position->second.idempotency_key.empty()) {
        idempotency_entries.erase(
          idempotency_map_key(
            position->second.authorized_origin_id,
            position->second.idempotency_key
          )
        );
      }
      cancel_requests_locked(position->second, "source_released");
      entries.erase(position);
      request_available.notify_all();
      manifest_available.notify_all();
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

    reference_result_t existing_idempotent_locked(
      std::uint64_t authorized_origin_id,
      const std::string &idempotency_key,
      bool remote_source
    ) {
      const auto map_key =
        idempotency_map_key(authorized_origin_id, idempotency_key);
      auto mapped = idempotency_entries.find(map_key);
      if (mapped == idempotency_entries.end()) {
        return {};
      }
      auto existing = entries.find(mapped->second);
      if (existing == entries.end()) {
        idempotency_entries.erase(mapped);
        return {};
      }
      if (existing->second.remote_source != remote_source) {
        return {.error = "idempotency_conflict"};
      }
      existing->second.expires_at = clock_t::now() + std::chrono::seconds(source_ttl_seconds);
      return {
        .ok = true,
        .id = existing->first,
      };
    }

    chunk_result_t read_local_chunk(
      const std::string &id,
      std::uint64_t authorized_origin_id,
      std::uint32_t file_index,
      std::uint64_t offset,
      std::size_t length
    ) {
      try {
        file_t file;
        {
          std::lock_guard lock(store_mutex);
          sweep_locked(clock_t::now());
          auto position = entries.find(id);
          if (position == entries.end() ||
              position->second.remote_source ||
              position->second.authorized_origin_id !=
                authorized_origin_id ||
              file_index >= position->second.files.size()) {
            return {.error = "not_found"};
          }
          auto &entry = position->second;
          file = entry.files[file_index];
          if (file.type != LI_CLIPBOARD_FILE_TYPE_REGULAR ||
              offset > file.size ||
              length > file.size - offset) {
            return {.error = "bad_file_range"};
          }
          entry.expires_at =
            clock_t::now() + std::chrono::seconds(source_ttl_seconds);
        }

        std::error_code fs_error;
        if (!fs::is_regular_file(file.path, fs_error) ||
            fs::file_size(file.path, fs_error) != file.size ||
            (file.modified_time &&
             fs::last_write_time(file.path, fs_error) !=
               *file.modified_time) ||
            fs_error) {
          return {.error = "source_changed"};
        }

        std::ifstream input(file.path, std::ios::binary);
        std::vector<std::uint8_t> bytes(length);
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(
          reinterpret_cast<char *>(bytes.data()),
          static_cast<std::streamsize>(length)
        );
        if (!input ||
            static_cast<std::size_t>(input.gcount()) != length) {
          return {.error = "read_failed"};
        }
        if (fs::file_size(file.path, fs_error) != file.size ||
            (file.modified_time &&
             fs::last_write_time(file.path, fs_error) !=
               *file.modified_time) ||
            fs_error) {
          return {.error = "source_changed"};
        }
        return {
          .ok = true,
          .bytes = bytes,
          .sha256 = calculate_sha256(bytes.data(), bytes.size()),
        };
      } catch (const std::exception &error) {
        return {.error = error.what()};
      }
    }

    struct remote_payload_result_t {
      bool ok {};
      std::vector<std::uint8_t> bytes;
      std::string error;
    };

    remote_payload_result_t request_remote_payload(
      const std::string &id,
      std::uint64_t authorized_origin_id,
      request_kind_e kind,
      std::uint32_t file_index,
      std::uint64_t offset,
      std::size_t length
    ) {
      if (kind == request_kind_e::chunk &&
          (length == 0 || length > max_chunk_bytes)) {
        return {.error = "bad_chunk_size"};
      }

      std::shared_ptr<pending_request_t> request;
      try {
        std::lock_guard lock(store_mutex);
        sweep_locked(clock_t::now());
        auto position = entries.find(id);
        if (position == entries.end() ||
            !position->second.remote_source ||
            position->second.authorized_origin_id !=
              authorized_origin_id) {
          return {.error = "not_found"};
        }
        auto &entry = position->second;
        if (kind == request_kind_e::chunk) {
          if (file_index >= entry.files.size()) {
            return {.error = "not_found"};
          }
          const auto &file = entry.files[file_index];
          if (file.type != LI_CLIPBOARD_FILE_TYPE_REGULAR ||
              offset > file.size ||
              length > file.size - offset) {
            return {.error = "bad_file_range"};
          }
        }

        request = std::make_shared<pending_request_t>();
        request->id = make_id();
        request->kind = kind;
        request->file_index = file_index;
        request->offset = offset;
        request->length = length;
        entry.requests.emplace(request->id, request);
        entry.queued_requests.push_back(request->id);
        entry.expires_at =
          clock_t::now() + std::chrono::seconds(source_ttl_seconds);
        request_available.notify_all();
      } catch (const std::exception &error) {
        return {.error = error.what()};
      }

      std::unique_lock request_lock(request->mutex);
      const bool completed = request->ready.wait_for(
        request_lock,
        std::chrono::seconds(request_timeout_seconds),
        [&request]() {
          return request->complete;
        }
      );
      if (!completed) {
        request->complete = true;
        request->error = "request_timeout";
      }
      auto bytes = std::move(request->bytes);
      auto error = std::move(request->error);
      request_lock.unlock();

      {
        std::lock_guard lock(store_mutex);
        auto position = entries.find(id);
        if (position != entries.end()) {
          position->second.requests.erase(request->id);
          auto queued = std::find(
            position->second.queued_requests.begin(),
            position->second.queued_requests.end(),
            request->id
          );
          if (queued != position->second.queued_requests.end()) {
            position->second.queued_requests.erase(queued);
          }
        }
      }

      if (!completed || !error.empty()) {
        return {.error = error.empty() ? "invalid_response" : std::move(error)};
      }
      if ((kind == request_kind_e::manifest &&
           (bytes.empty() ||
            bytes.size() > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES ||
            !LiIsValidClipboardFileManifest(bytes.data(), bytes.size()))) ||
          (kind == request_kind_e::chunk && bytes.size() != length)) {
        return {.error = "invalid_response"};
      }
      return {.ok = true, .bytes = std::move(bytes)};
    }
  }  // namespace

  reference_result_t register_local_offer(
    const std::vector<fs::path> &paths,
    std::uint64_t authorized_origin_id,
    std::string idempotency_key
  ) {
    if (authorized_origin_id == 0 ||
        idempotency_key.empty() ||
        idempotency_key.size() > 128 ||
        paths.empty() ||
        paths.size() > LI_CLIPBOARD_MAX_FILE_ENTRIES) {
      return {.error = "invalid_identity"};
    }

    try {
      const auto now = clock_t::now();

      std::lock_guard lock(store_mutex);
      sweep_locked(now);
      auto existing = existing_idempotent_locked(
        authorized_origin_id,
        idempotency_key,
        false
      );
      if (existing.ok || !existing.error.empty()) {
        return existing;
      }

      const auto id = unique_id_locked();
      entry_t entry {
        .local_paths = paths,
        .authorized_origin_id = authorized_origin_id,
        .idempotency_key = idempotency_key,
        .remote_source = false,
        .expires_at = now + std::chrono::seconds(source_ttl_seconds),
      };
      const auto map_key =
        idempotency_map_key(authorized_origin_id, idempotency_key);
      entries.emplace(id, std::move(entry));
      idempotency_entries[map_key] = id;
      return {
        .ok = true,
        .id = id,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  reference_result_t register_remote_offer(
    std::uint64_t authorized_origin_id,
    std::string idempotency_key
  ) {
    if (authorized_origin_id == 0 ||
        idempotency_key.empty() ||
        idempotency_key.size() > 128) {
      return {.error = "invalid_identity"};
    }

    try {
      const auto now = clock_t::now();

      std::lock_guard lock(store_mutex);
      sweep_locked(now);
      auto existing = existing_idempotent_locked(
        authorized_origin_id,
        idempotency_key,
        true
      );
      if (existing.ok || !existing.error.empty()) {
        return existing;
      }

      const auto id = unique_id_locked();
      entry_t entry {
        .authorized_origin_id = authorized_origin_id,
        .idempotency_key = idempotency_key,
        .remote_source = true,
        .expires_at = now + std::chrono::seconds(source_ttl_seconds),
      };
      const auto map_key =
        idempotency_map_key(authorized_origin_id, idempotency_key);
      entries.emplace(id, std::move(entry));
      idempotency_entries[map_key] = id;
      return {
        .ok = true,
        .id = id,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  operation_result_t resolve_remote_offer(
    const std::string &id,
    std::uint64_t authorized_origin_id
  ) {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
    auto position = entries.find(id);
    if (position == entries.end()) {
      return {.error = "not_found"};
    }
    auto &entry = position->second;
    if (!entry.remote_source ||
        entry.authorized_origin_id != authorized_origin_id) {
      return {.error = "reference_mismatch"};
    }
    entry.expires_at = clock_t::now() + std::chrono::seconds(source_ttl_seconds);
    return {.ok = true};
  }

  request_result_t poll_remote_request(
    const std::string &id,
    std::uint64_t authorized_origin_id,
    int timeout_seconds
  ) {
    if (timeout_seconds < 0 || timeout_seconds > poll_timeout_seconds) {
      return {.error = "bad_timeout"};
    }

    std::unique_lock lock(store_mutex);
    sweep_locked(clock_t::now());
    auto valid_source = [&]() {
      auto position = entries.find(id);
      return position != entries.end() &&
             position->second.remote_source &&
             position->second.authorized_origin_id ==
               authorized_origin_id;
    };
    if (!valid_source()) {
      return {.error = "not_found"};
    }

    const auto has_request_or_released = [&]() {
      auto position = entries.find(id);
      return position == entries.end() ||
             !position->second.remote_source ||
             position->second.authorized_origin_id !=
               authorized_origin_id ||
             !position->second.queued_requests.empty();
    };
    if (!request_available.wait_for(
          lock,
          std::chrono::seconds(timeout_seconds),
          has_request_or_released
        )) {
      return {};
    }
    if (!valid_source()) {
      return {.error = "not_found"};
    }

    auto &entry = entries.at(id);
    if (entry.queued_requests.empty()) {
      return {};
    }
    const auto request_id = std::move(entry.queued_requests.front());
    entry.queued_requests.pop_front();
    const auto request = entry.requests.at(request_id);
    entry.expires_at = clock_t::now() + std::chrono::seconds(source_ttl_seconds);
    return {
      .found = true,
      .request_id = request_id,
      .kind = request->kind,
      .file_index = request->file_index,
      .offset = request->offset,
      .length = request->length,
    };
  }

  operation_result_t fulfill_remote_request(
    const std::string &id,
    std::uint64_t authorized_origin_id,
    const std::string &request_id,
    const std::vector<std::uint8_t> &bytes,
    const digest_t &expected_sha256
  ) {
    std::shared_ptr<pending_request_t> request;
    {
      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now());
      auto position = entries.find(id);
      if (position == entries.end() ||
          !position->second.remote_source ||
          position->second.authorized_origin_id !=
            authorized_origin_id) {
        return {.error = "not_found"};
      }
      auto request_position = position->second.requests.find(request_id);
      if (request_position == position->second.requests.end()) {
        return {.error = "request_not_found"};
      }
      request = request_position->second;
      position->second.expires_at =
        clock_t::now() + std::chrono::seconds(source_ttl_seconds);
    }

    if (request->kind == request_kind_e::manifest) {
      if (bytes.empty() ||
          bytes.size() > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES ||
          !LiIsValidClipboardFileManifest(bytes.data(), bytes.size())) {
        return {.error = "invalid_manifest"};
      }
    } else if (bytes.size() != request->length) {
      return {.error = "wrong_chunk_size"};
    }
    try {
      if (calculate_sha256(bytes.data(), bytes.size()) != expected_sha256) {
        return {.error = "digest_mismatch"};
      }
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }

    std::lock_guard request_lock(request->mutex);
    if (request->complete) {
      return {.error = "request_already_completed"};
    }
    request->bytes = bytes;
    request->complete = true;
    request->ready.notify_all();
    return {.ok = true};
  }

  operation_result_t fail_remote_request(
    const std::string &id,
    std::uint64_t authorized_origin_id,
    const std::string &request_id,
    std::string error
  ) {
    if (error.empty()) {
      return {.error = "missing_error"};
    }

    std::shared_ptr<pending_request_t> request;
    {
      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now());
      auto position = entries.find(id);
      if (position == entries.end() ||
          !position->second.remote_source ||
          position->second.authorized_origin_id !=
            authorized_origin_id) {
        return {.error = "not_found"};
      }
      auto request_position = position->second.requests.find(request_id);
      if (request_position == position->second.requests.end()) {
        return {.error = "request_not_found"};
      }
      request = request_position->second;
      position->second.expires_at =
        clock_t::now() + std::chrono::seconds(source_ttl_seconds);
    }

    std::lock_guard request_lock(request->mutex);
    if (request->complete) {
      return {.error = "request_already_completed"};
    }
    request->error = std::move(error);
    request->complete = true;
    request->ready.notify_all();
    return {.ok = true};
  }

  operation_result_t release_remote_source(
    const std::string &id,
    std::uint64_t authorized_origin_id
  ) {
    if (id.empty() || authorized_origin_id == 0) {
      return {.error = "invalid_identity"};
    }

    std::lock_guard lock(store_mutex);
    auto position = entries.find(id);
    if (position == entries.end()) {
      // Releasing a source is intentionally idempotent.
      return {.ok = true};
    }
    if (!position->second.remote_source ||
        position->second.authorized_origin_id !=
          authorized_origin_id) {
      return {.error = "not_found"};
    }
    erase_entry_locked(id);
    return {.ok = true};
  }

  static chunk_result_t request_remote_chunk(
    const std::string &id,
    std::uint64_t authorized_origin_id,
    std::uint32_t file_index,
    std::uint64_t offset,
    std::size_t length
  ) {
    auto payload = request_remote_payload(
      id,
      authorized_origin_id,
      request_kind_e::chunk,
      file_index,
      offset,
      length
    );
    if (!payload.ok) {
      return {.error = std::move(payload.error)};
    }
    try {
      return {
        .ok = true,
        .bytes = payload.bytes,
        .sha256 = calculate_sha256(
          payload.bytes.data(),
          payload.bytes.size()
        ),
      };
    } catch (const std::exception &exception) {
      return {.error = exception.what()};
    }
  }

  manifest_result_t get_manifest(
    const std::string &id,
    std::uint64_t authorized_origin_id
  ) {
    std::vector<fs::path> local_paths;
    bool remote_source = false;
    {
      std::unique_lock lock(store_mutex);
      for (;;) {
        sweep_locked(clock_t::now());
        auto position = entries.find(id);
        if (position == entries.end() ||
            position->second.authorized_origin_id !=
              authorized_origin_id) {
          return {.error = "not_found"};
        }
        auto &entry = position->second;
        entry.expires_at =
          clock_t::now() + std::chrono::seconds(source_ttl_seconds);
        if (!entry.manifest.empty()) {
          return {
            .ok = true,
            .bytes = entry.manifest,
            .sha256 = entry.manifest_sha256,
          };
        }
        if (!entry.manifest_building) {
          entry.manifest_building = true;
          local_paths = entry.local_paths;
          remote_source = entry.remote_source;
          break;
        }
        manifest_available.wait(lock, [&]() {
          auto current = entries.find(id);
          return current == entries.end() ||
                 !current->second.manifest_building;
        });
      }
    }

    std::vector<file_t> files;
    std::vector<std::uint8_t> manifest;
    std::string error;
    if (remote_source) {
      auto payload = request_remote_payload(
        id,
        authorized_origin_id,
        request_kind_e::manifest,
        0,
        0,
        0
      );
      if (!payload.ok) {
        error = std::move(payload.error);
      } else {
        manifest = std::move(payload.bytes);
        if (!decode_manifest(manifest, files, error)) {
          manifest.clear();
        }
      }
    } else {
      const auto enumerated = with_current_user_file_access([&]() {
        return enumerate_sources(local_paths, files, error);
      });
      if (!enumerated) {
        error = "user_file_access_unavailable";
      } else if (*enumerated &&
                 !encode_manifest(files, manifest, error)) {
        manifest.clear();
      }
    }

    digest_t digest {};
    if (!manifest.empty()) {
      try {
        digest = calculate_sha256(manifest.data(), manifest.size());
      } catch (const std::exception &exception) {
        error = exception.what();
        manifest.clear();
      }
    }
    if (manifest.empty() && error.empty()) {
      error = "manifest_unavailable";
    }

    {
      std::lock_guard lock(store_mutex);
      auto position = entries.find(id);
      if (position == entries.end() ||
          position->second.authorized_origin_id !=
            authorized_origin_id) {
        manifest_available.notify_all();
        return {.error = "not_found"};
      }
      auto &entry = position->second;
      if (!manifest.empty()) {
        entry.manifest = manifest;
        entry.manifest_sha256 = digest;
        entry.files = std::move(files);
      }
      entry.manifest_building = false;
      entry.expires_at =
        clock_t::now() + std::chrono::seconds(source_ttl_seconds);
      manifest_available.notify_all();
    }

    if (manifest.empty()) {
      return {.error = std::move(error)};
    }
    return {
      .ok = true,
      .bytes = std::move(manifest),
      .sha256 = digest,
    };
  }

  chunk_result_t read_chunk(
    const std::string &id,
    std::uint64_t authorized_origin_id,
    std::uint32_t file_index,
    std::uint64_t offset,
    std::size_t length
  ) {
    if (length == 0 || length > max_chunk_bytes) {
      return {.error = "bad_chunk_size"};
    }

    bool remote_source = false;
    {
      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now());
      auto position = entries.find(id);
      if (position == entries.end() ||
          position->second.authorized_origin_id !=
            authorized_origin_id ||
          position->second.manifest.empty()) {
        return {.error = "not_found"};
      }
      remote_source = position->second.remote_source;
    }
    if (remote_source) {
      return request_remote_chunk(
        id,
        authorized_origin_id,
        file_index,
        offset,
        length
      );
    }

    auto result = with_current_user_file_access([&]() {
      return read_local_chunk(
        id,
        authorized_origin_id,
        file_index,
        offset,
        length
      );
    });
    if (!result) {
      return {.error = "user_file_access_unavailable"};
    }
    return std::move(*result);
  }

  void sweep_expired() {
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now());
  }

  void release_origin(std::uint64_t authorized_origin_id) {
    if (authorized_origin_id == 0) {
      return;
    }
    std::lock_guard lock(store_mutex);
    for (auto position = entries.begin(); position != entries.end();) {
      if (position->second.authorized_origin_id !=
          authorized_origin_id) {
        ++position;
        continue;
      }
      const auto id = position->first;
      ++position;
      erase_entry_locked(id);
    }
  }

  void release_all_remote_sources() {
    std::lock_guard lock(store_mutex);
    for (auto position = entries.begin(); position != entries.end();) {
      if (!position->second.remote_source) {
        ++position;
        continue;
      }
      const auto id = position->first;
      ++position;
      erase_entry_locked(id);
    }
  }

#ifdef SUNSHINE_TESTS
  void clear_for_tests() {
    std::lock_guard lock(store_mutex);
    while (!entries.empty()) {
      erase_entry_locked(entries.begin()->first);
    }
    idempotency_entries.clear();
  }
#endif
}  // namespace clipboard_file_store
