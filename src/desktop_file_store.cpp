#include "desktop_file_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "logging.h"

#ifdef _WIN32
  #include <shlobj.h>
  #include <windows.h>
  #include <wtsapi32.h>
#endif

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

namespace desktop_file_store {
  static_assert(max_chunk_bytes == LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES);

  namespace {
    namespace fs = std::filesystem;
    using clock_t = std::chrono::steady_clock;

    constexpr std::uint64_t minimum_free_bytes = 512ULL * 1024ULL * 1024ULL;

    struct file_t {
      std::string relative_path;
      std::uint8_t type {};
      std::uint64_t size {};
      std::uint64_t modified_time_ms {};
      fs::path staged_path;
      std::uint64_t received_bytes {};
    };

    struct entry_t {
      std::vector<std::uint8_t> manifest;
      digest_t manifest_sha256 {};
      digest_t token_sha256 {};
      std::vector<std::uint8_t> user_sid;
      std::string idempotency_key;
      std::vector<file_t> files;
      std::vector<std::size_t> top_level_indices;
      fs::path desktop;
      fs::path staging_root;
      std::uint64_t total_file_bytes {};
      bool complete {};
      clock_t::time_point expires_at;
    };

    std::mutex store_mutex;
    std::unordered_map<std::string, entry_t> entries;
    std::unordered_map<std::string, std::string> idempotency_entries;
#ifdef SUNSHINE_TESTS
    std::optional<fs::path> test_desktop;
#endif

#ifdef _WIN32
    std::vector<std::uint8_t> token_user_sid(HANDLE token) {
      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      if (size == 0) {
        return {};
      }

      std::vector<std::uint8_t> token_user(size);
      if (!GetTokenInformation(
            token,
            TokenUser,
            token_user.data(),
            size,
            &size
          )) {
        return {};
      }

      const auto *user =
        reinterpret_cast<const TOKEN_USER *>(token_user.data());
      const DWORD sid_size = GetLengthSid(user->User.Sid);
      std::vector<std::uint8_t> sid(sid_size);
      if (sid_size == 0 ||
          !CopySid(sid_size, sid.data(), user->User.Sid)) {
        return {};
      }
      return sid;
    }

    bool local_system_token(HANDLE token) {
      auto sid = token_user_sid(token);
      if (sid.empty()) {
        return false;
      }

      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_sid {};
      DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
      return CreateWellKnownSid(
               WinLocalSystemSid,
               nullptr,
               system_sid.data(),
               &system_sid_size
             ) &&
             EqualSid(sid.data(), system_sid.data());
    }
#endif

    class interactive_user_context_t {
    public:
      interactive_user_context_t() {
#ifdef _WIN32
        HANDLE process_token {};
        if (!OpenProcessToken(
              GetCurrentProcess(),
              TOKEN_QUERY,
              &process_token
            )) {
          BOOST_LOG(error) << "Failed to open the Sunshine process token for desktop file transfer: "
                           << GetLastError();
          return;
        }

        if (!local_system_token(process_token)) {
          token_ = process_token;
        } else {
          CloseHandle(process_token);
          const DWORD session_id = WTSGetActiveConsoleSessionId();
          if (session_id == 0xFFFFFFFF ||
              !WTSQueryUserToken(session_id, &token_)) {
            BOOST_LOG(error) << "Failed to query the interactive user for desktop file transfer: "
                             << GetLastError();
            token_ = nullptr;
            return;
          }
          if (!ImpersonateLoggedOnUser(token_)) {
            BOOST_LOG(error) << "Failed to impersonate the interactive user for desktop file transfer: "
                             << GetLastError();
            CloseHandle(token_);
            token_ = nullptr;
            return;
          }
          impersonating_ = true;
        }

        user_sid_ = token_user_sid(token_);
        if (user_sid_.empty()) {
          BOOST_LOG(error) << "Failed to read the interactive user identity for desktop file transfer";
          reset();
          return;
        }
#endif
        valid_ = true;
      }

      interactive_user_context_t(const interactive_user_context_t &) = delete;
      interactive_user_context_t &operator=(
        const interactive_user_context_t &
      ) = delete;

      ~interactive_user_context_t() {
        reset();
      }

      explicit operator bool() const {
        return valid_;
      }

      const std::vector<std::uint8_t> &user_sid() const {
        return user_sid_;
      }

#ifdef _WIN32
      HANDLE known_folder_token() const {
        return impersonating_ ? token_ : nullptr;
      }
#endif

    private:
      void reset() {
#ifdef _WIN32
        if (impersonating_) {
          if (!RevertToSelf()) {
            BOOST_LOG(fatal) << "Failed to revert desktop file transfer impersonation: "
                             << GetLastError();
            DebugBreak();
          }
          impersonating_ = false;
        }
        if (token_ != nullptr) {
          CloseHandle(token_);
          token_ = nullptr;
        }
#endif
        valid_ = false;
      }

      bool valid_ {};
      std::vector<std::uint8_t> user_sid_;
#ifdef _WIN32
      HANDLE token_ {};
      bool impersonating_ {};
#endif
    };

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

    bool digest_equal(const digest_t &first, const digest_t &second) {
      return CRYPTO_memcmp(first.data(), second.data(), first.size()) == 0;
    }

    std::string digest_hex(const digest_t &digest) {
      static constexpr char alphabet[] = "0123456789abcdef";
      std::string encoded(digest.size() * 2, '\0');
      for (std::size_t index = 0; index < digest.size(); index++) {
        encoded[index * 2] = alphabet[digest[index] >> 4];
        encoded[index * 2 + 1] = alphabet[digest[index] & 0x0F];
      }
      return encoded;
    }

    std::string make_id() {
      std::array<std::uint8_t, 16> raw {};
      if (RAND_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed");
      }
      raw[6] = static_cast<std::uint8_t>((raw[6] & 0x0F) | 0x40);
      raw[8] = static_cast<std::uint8_t>((raw[8] & 0x3F) | 0x80);

      static constexpr char alphabet[] = "0123456789abcdef";
      static constexpr int slots[] = {
        0,
        2,
        4,
        6,
        9,
        11,
        14,
        16,
        19,
        21,
        24,
        26,
        28,
        30,
        32,
        34
      };
      std::string id(36, '-');
      for (std::size_t index = 0; index < raw.size(); index++) {
        id[slots[index]] = alphabet[raw[index] >> 4];
        id[slots[index] + 1] = alphabet[raw[index] & 0x0F];
      }
      return id;
    }

    bool canonical_id(std::string_view value) {
      if (value.size() != 36) {
        return false;
      }
      for (std::size_t index = 0; index < value.size(); index++) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
          if (value[index] != '-') {
            return false;
          }
        } else if (!((value[index] >= '0' && value[index] <= '9') ||
                     (value[index] >= 'a' && value[index] <= 'f'))) {
          return false;
        }
      }
      return true;
    }

    fs::path path_from_utf8(const std::string &value) {
      return fs::path(std::u8string(
        reinterpret_cast<const char8_t *>(value.data()),
        value.size()
      ));
    }

    fs::path desktop_directory(
      const interactive_user_context_t &user_context
    ) {
#ifdef SUNSHINE_TESTS
      if (test_desktop) {
        return *test_desktop;
      }
#endif
#ifdef _WIN32
      PWSTR raw_path = nullptr;
      const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_Desktop,
        KF_FLAG_CREATE,
        user_context.known_folder_token(),
        &raw_path
      );
      if (FAILED(result) || raw_path == nullptr) {
        if (raw_path != nullptr) {
          CoTaskMemFree(raw_path);
        }
        return {};
      }
      fs::path path(raw_path);
      CoTaskMemFree(raw_path);
      return path;
#else
      return {};
#endif
    }

    std::string path_to_utf8(const fs::path &path) {
      const auto utf8 = path.u8string();
      return std::string(
        reinterpret_cast<const char *>(utf8.data()),
        utf8.size()
      );
    }

    fs::path staging_base(const fs::path &desktop) {
      std::error_code error;
      fs::path base = desktop / ".moonlight-transfers";
      fs::create_directories(base, error);
      if (error) {
        return {};
      }
#ifdef _WIN32
      const DWORD attributes = GetFileAttributesW(base.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(base.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
      }
#endif
      return base;
    }

    bool decode_manifest(const std::vector<std::uint8_t> &manifest, const fs::path &staging_root, std::vector<file_t> &files, std::vector<std::size_t> &top_level_indices, std::uint64_t &total_file_bytes, std::string &error) {
      LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
      if (!LiIsValidClipboardFileManifest(manifest.data(), manifest.size()) ||
          !LiDecodeClipboardFileManifestHeader(
            manifest.data(),
            manifest.size(),
            &header
          )) {
        error = "invalid_manifest";
        return false;
      }

      std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
      files.reserve(header.entryCount);
      for (std::uint32_t index = 0; index < header.entryCount; index++) {
        LI_CLIPBOARD_FILE_MANIFEST_ENTRY decoded;
        if (!LiDecodeClipboardFileManifestEntry(
              manifest.data(),
              manifest.size(),
              &offset,
              &decoded
            )) {
          error = "invalid_manifest_entry";
          return false;
        }
        std::string relative_path(
          reinterpret_cast<const char *>(decoded.path),
          decoded.pathLength
        );
        if (relative_path.find('/') == std::string::npos) {
          top_level_indices.push_back(index);
        }
        files.push_back({
          .relative_path = relative_path,
          .type = decoded.type,
          .size = decoded.size,
          .modified_time_ms = decoded.modifiedTimeMs,
          .staged_path = staging_root / path_from_utf8(relative_path),
        });
      }
      total_file_bytes = header.totalFileBytes;
      return offset == manifest.size();
    }

    std::string idempotency_map_key(const digest_t &token_sha256, const std::string &key) {
      return digest_hex(token_sha256) + ':' + key;
    }

    void remove_staging(
      const fs::path &path,
      const fs::path &desktop
    ) {
      const auto base = desktop.empty() ? fs::path {} : staging_base(desktop);
      if (path.empty() || base.empty() || path.parent_path() != base ||
          !canonical_id(path.filename().string())) {
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
      idempotency_entries.erase(idempotency_map_key(
        position->second.token_sha256,
        position->second.idempotency_key
      ));
      const auto staging_root = position->second.staging_root;
      const auto desktop = position->second.desktop;
      const bool complete = position->second.complete;
      entries.erase(position);
      if (!complete) {
        remove_staging(staging_root, desktop);
      }
    }

    void sweep_locked(
      clock_t::time_point now,
      const std::vector<std::uint8_t> &user_sid
    ) {
      for (auto position = entries.begin(); position != entries.end();) {
        if (position->second.expires_at > now ||
            position->second.user_sid != user_sid) {
          ++position;
          continue;
        }
        const auto id = position->first;
        ++position;
        erase_entry_locked(id);
      }
    }

    void remove_stale_staging_locked(const fs::path &base) {
      std::error_code error;
      const auto cutoff = fs::file_time_type::clock::now() -
                          std::chrono::seconds(completed_ttl_seconds);
      fs::directory_iterator iterator(
        base,
        fs::directory_options::skip_permission_denied,
        error
      );
      const fs::directory_iterator end;
      while (!error && iterator != end) {
        const auto path = iterator->path();
        const auto id = path.filename().string();
        std::error_code item_error;
        const bool removable = canonical_id(id) &&
                               iterator->is_directory(item_error) &&
                               !item_error &&
                               !entries.contains(id) &&
                               iterator->last_write_time(item_error) < cutoff &&
                               !item_error &&
                               fs::exists(path / ".sunshine-transfer", item_error) &&
                               !item_error;
        if (removable) {
          std::error_code remove_error;
          fs::remove_all(path, remove_error);
        }
        iterator.increment(error);
      }
    }

    std::string unique_id_locked() {
      std::string id = make_id();
      while (entries.contains(id)) {
        id = make_id();
      }
      return id;
    }

    fs::path collision_free_path(const fs::path &desktop, const fs::path &requested, bool directory) {
      fs::path candidate = desktop / requested.filename();
      std::error_code error;
      if (!fs::exists(candidate, error) && !error) {
        return candidate;
      }
      const auto stem = requested.stem();
      const auto extension = directory ? fs::path {} : requested.extension();
      for (unsigned suffix = 2; suffix != 0; suffix++) {
        fs::path name = stem;
        name += " (" + std::to_string(suffix) + ")";
        name += extension;
        candidate = desktop / name;
        error.clear();
        if (!fs::exists(candidate, error) && !error) {
          return candidate;
        }
      }
      return {};
    }
  }  // namespace

  begin_result_t begin(std::vector<std::uint8_t> manifest, std::string token, std::string idempotency_key) {
    if (manifest.empty() ||
        manifest.size() > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES ||
        token.size() != 64 ||
        idempotency_key.empty() ||
        idempotency_key.size() > 128) {
      return {.error = "invalid_request"};
    }

    try {
      interactive_user_context_t user_context;
      if (!user_context) {
        return {.error = "desktop_unavailable"};
      }
      const auto manifest_sha256 = calculate_sha256(
        manifest.data(),
        manifest.size()
      );
      const auto token_sha256 = calculate_sha256(token.data(), token.size());
      const auto now = clock_t::now();

      std::lock_guard lock(store_mutex);
      sweep_locked(now, user_context.user_sid());
      const auto map_key = idempotency_map_key(
        token_sha256,
        idempotency_key
      );
      auto mapped = idempotency_entries.find(map_key);
      if (mapped != idempotency_entries.end()) {
        auto existing = entries.find(mapped->second);
        if (existing != entries.end()) {
          if (!digest_equal(
                existing->second.manifest_sha256,
                manifest_sha256
              ) ||
              existing->second.manifest.size() != manifest.size() ||
              existing->second.user_sid != user_context.user_sid()) {
            return {.error = "idempotency_conflict"};
          }
          existing->second.expires_at = now +
                                        std::chrono::seconds(
                                          existing->second.complete ?
                                            completed_ttl_seconds :
                                            pending_ttl_seconds
                                        );
          return {
            .ok = true,
            .id = existing->first,
            .manifest_size = existing->second.manifest.size(),
            .manifest_sha256 = existing->second.manifest_sha256,
          };
        }
        idempotency_entries.erase(mapped);
      }

      const auto desktop = desktop_directory(user_context);
      const auto base = desktop.empty() ? fs::path {} : staging_base(desktop);
      if (base.empty()) {
        return {.error = "desktop_unavailable"};
      }
      remove_stale_staging_locked(base);

      const auto id = unique_id_locked();
      const auto staging_root = base / id;
      std::vector<file_t> files;
      std::vector<std::size_t> top_level_indices;
      std::uint64_t total_file_bytes = 0;
      std::string error_message;
      if (!decode_manifest(
            manifest,
            staging_root,
            files,
            top_level_indices,
            total_file_bytes,
            error_message
          )) {
        return {.error = std::move(error_message)};
      }

      std::uint64_t staged_bytes = 0;
      for (const auto &[entry_id, entry] : entries) {
        (void) entry_id;
        if (!entry.complete) {
          if (staged_bytes >
              LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES -
                entry.total_file_bytes) {
            staged_bytes = LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES;
            break;
          }
          staged_bytes += entry.total_file_bytes;
        }
      }
      std::error_code space_error;
      const auto space = fs::space(base, space_error);
      if (space_error ||
          space.available < minimum_free_bytes ||
          total_file_bytes > space.available - minimum_free_bytes ||
          staged_bytes >
            LI_CLIPBOARD_MAX_FILE_TRANSFER_BYTES - total_file_bytes) {
        return {.error = "staging_quota_exceeded"};
      }

      std::error_code fs_error;
      fs::create_directory(staging_root, fs_error);
      if (fs_error) {
        return {.error = fs_error.message()};
      }
      {
        std::ofstream marker(
          staging_root / ".sunshine-transfer",
          std::ios::binary | std::ios::trunc
        );
        marker << "Sunshine desktop file transfer\n";
        if (!marker) {
          remove_staging(staging_root, desktop);
          return {.error = "staging_marker_failed"};
        }
      }

      for (const auto &file : files) {
        if (file.type == LI_CLIPBOARD_FILE_TYPE_DIRECTORY) {
          fs::create_directories(file.staged_path, fs_error);
        } else {
          fs::create_directories(file.staged_path.parent_path(), fs_error);
          if (!fs_error) {
            std::ofstream output(
              file.staged_path,
              std::ios::binary | std::ios::trunc
            );
            if (!output) {
              fs_error = std::make_error_code(std::errc::io_error);
            }
          }
        }
        if (fs_error) {
          remove_staging(staging_root, desktop);
          return {.error = fs_error.message()};
        }
      }

      entry_t entry {
        .manifest = std::move(manifest),
        .manifest_sha256 = manifest_sha256,
        .token_sha256 = token_sha256,
        .user_sid = user_context.user_sid(),
        .idempotency_key = std::move(idempotency_key),
        .files = std::move(files),
        .top_level_indices = std::move(top_level_indices),
        .desktop = desktop,
        .staging_root = staging_root,
        .total_file_bytes = total_file_bytes,
        .complete = false,
        .expires_at = now + std::chrono::seconds(pending_ttl_seconds),
      };
      const auto manifest_size = entry.manifest.size();
      idempotency_entries.emplace(
        idempotency_map_key(entry.token_sha256, entry.idempotency_key),
        id
      );
      entries.emplace(id, std::move(entry));
      return {
        .ok = true,
        .id = id,
        .manifest_size = manifest_size,
        .manifest_sha256 = manifest_sha256,
      };
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  operation_result_t write_chunk(const std::string &id, const std::string &token, std::uint32_t file_index, std::uint64_t offset, const std::vector<std::uint8_t> &bytes, const digest_t &expected_sha256) {
    if (token.size() != 64 ||
        bytes.empty() || bytes.size() > max_chunk_bytes) {
      return {.error = "bad_chunk_size"};
    }

    try {
      interactive_user_context_t user_context;
      if (!user_context) {
        return {.error = "interactive_user_unavailable"};
      }
      const auto token_sha256 = calculate_sha256(token.data(), token.size());
      if (!digest_equal(
            calculate_sha256(bytes.data(), bytes.size()),
            expected_sha256
          )) {
        return {.error = "chunk_hash_mismatch"};
      }

      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now(), user_context.user_sid());
      auto position = entries.find(id);
      if (position == entries.end()) {
        return {.error = "not_found"};
      }
      auto &entry = position->second;
      if (entry.complete ||
          !digest_equal(entry.token_sha256, token_sha256) ||
          entry.user_sid != user_context.user_sid() ||
          file_index >= entry.files.size()) {
        return {.error = "invalid_transfer"};
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
        std::ifstream existing(file.staged_path, std::ios::binary);
        std::vector<std::uint8_t> previous(bytes.size());
        existing.seekg(static_cast<std::streamoff>(offset));
        existing.read(
          reinterpret_cast<char *>(previous.data()),
          static_cast<std::streamsize>(previous.size())
        );
        return existing && previous == bytes ?
                 operation_result_t {.ok = true} :
                 operation_result_t {.error = "retry_content_mismatch"};
      }
      if (offset != file.received_bytes) {
        return {.error = "non_sequential_chunk"};
      }

      std::fstream output(
        file.staged_path,
        std::ios::binary | std::ios::in | std::ios::out
      );
      if (!output) {
        return {.error = "open_failed"};
      }
      output.seekp(static_cast<std::streamoff>(offset));
      output.write(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
      );
      output.flush();
      if (!output) {
        return {.error = "write_failed"};
      }
      file.received_bytes += bytes.size();
      entry.expires_at = clock_t::now() +
                         std::chrono::seconds(pending_ttl_seconds);
      return {.ok = true};
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  operation_result_t complete(const std::string &id, const std::string &token) {
    if (token.size() != 64) {
      return {.error = "invalid_transfer"};
    }
    try {
      interactive_user_context_t user_context;
      if (!user_context) {
        return {.error = "interactive_user_unavailable"};
      }
      const auto token_sha256 = calculate_sha256(token.data(), token.size());
      std::lock_guard lock(store_mutex);
      sweep_locked(clock_t::now(), user_context.user_sid());
      auto position = entries.find(id);
      if (position == entries.end()) {
        return {.error = "not_found"};
      }
      auto &entry = position->second;
      if (!digest_equal(entry.token_sha256, token_sha256) ||
          entry.user_sid != user_context.user_sid()) {
        return {.error = "invalid_transfer"};
      }
      if (entry.complete) {
        return {.ok = true};
      }
      if (std::any_of(
            entry.files.begin(),
            entry.files.end(),
            [](const file_t &file) {
              return file.type == LI_CLIPBOARD_FILE_TYPE_REGULAR &&
                     file.received_bytes != file.size;
            }
          )) {
        return {.error = "upload_incomplete"};
      }

      for (auto file = entry.files.rbegin();
           file != entry.files.rend();
           ++file) {
        if (file->modified_time_ms == 0) {
          continue;
        }
        const auto system_time = std::chrono::system_clock::time_point(
          std::chrono::milliseconds(file->modified_time_ms)
        );
        const auto file_time =
          std::chrono::time_point_cast<fs::file_time_type::duration>(
            system_time -
            std::chrono::system_clock::now() +
            fs::file_time_type::clock::now()
          );
        std::error_code ignored;
        fs::last_write_time(file->staged_path, file_time, ignored);
      }

      const auto &desktop = entry.desktop;
      if (desktop.empty()) {
        return {.error = "desktop_unavailable"};
      }
      std::vector<std::pair<fs::path, fs::path>> moved;
      for (const auto index : entry.top_level_indices) {
        const auto &file = entry.files.at(index);
        const auto destination = collision_free_path(
          desktop,
          file.staged_path,
          file.type == LI_CLIPBOARD_FILE_TYPE_DIRECTORY
        );
        if (destination.empty()) {
          return {.error = "destination_name_unavailable"};
        }
        std::error_code move_error;
        fs::rename(file.staged_path, destination, move_error);
        if (move_error) {
          for (auto rollback = moved.rbegin();
               rollback != moved.rend();
               ++rollback) {
            std::error_code ignored;
            fs::rename(rollback->second, rollback->first, ignored);
          }
          return {.error = move_error.message()};
        }
        moved.emplace_back(file.staged_path, destination);
      }

      std::error_code cleanup_error;
      fs::remove_all(entry.staging_root, cleanup_error);
      entry.staging_root.clear();
      entry.complete = true;
      entry.expires_at = clock_t::now() +
                         std::chrono::seconds(completed_ttl_seconds);
      for (const auto &[staged, destination] : moved) {
        (void) staged;
        BOOST_LOG(info) << "Desktop file upload committed to: "
                        << path_to_utf8(destination);
      }
      return {.ok = true};
    } catch (const std::exception &error) {
      return {.error = error.what()};
    }
  }

  void sweep_expired() {
    interactive_user_context_t user_context;
    if (!user_context) {
      return;
    }
    std::lock_guard lock(store_mutex);
    sweep_locked(clock_t::now(), user_context.user_sid());
  }

#ifdef SUNSHINE_TESTS
  void clear_for_tests() {
    std::lock_guard lock(store_mutex);
    for (const auto &[id, entry] : entries) {
      (void) id;
      if (!entry.complete) {
        remove_staging(entry.staging_root, entry.desktop);
      }
    }
    entries.clear();
    idempotency_entries.clear();
  }

  void set_desktop_for_tests(fs::path desktop) {
    std::lock_guard lock(store_mutex);
    test_desktop = std::move(desktop);
  }
#endif
}  // namespace desktop_file_store
