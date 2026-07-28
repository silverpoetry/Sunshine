#include "clipboard_user_helper.h"

#include "clipboard_helper_ipc.h"
#include "src/clipboard_file_store.h"
#include "src/logging.h"
#include "src/utility.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <userenv.h>
#include <windows.h>
#include <wtsapi32.h>

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

namespace platf::windows {
  namespace {
    using namespace std::chrono_literals;
    namespace ipc = clipboard_helper;

    struct pipe_result_t {
      bool ok {};
      std::vector<std::uint8_t> bytes;
    };

    bool is_local_system_process() {
      HANDLE token {};
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }
      auto token_guard = util::fail_guard([token]() {
        CloseHandle(token);
      });

      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      std::vector<std::uint8_t> buffer(size);
      if (size == 0 ||
          !GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        return false;
      }

      std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> system_sid {};
      DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
      if (!CreateWellKnownSid(
            WinLocalSystemSid,
            nullptr,
            system_sid.data(),
            &system_sid_size
          )) {
        return false;
      }

      const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
      return EqualSid(user->User.Sid, system_sid.data());
    }

    std::wstring helper_path() {
      std::wstring path(MAX_PATH, L'\0');
      const DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size())
      );
      if (length == 0 || length >= path.size()) {
        return {};
      }
      path.resize(length);
      const auto slash = path.find_last_of(L"\\/");
      if (slash == std::wstring::npos) {
        return L"tools\\sunshine-clipboard-helper.exe";
      }
      path.resize(slash + 1);
      path += L"tools\\sunshine-clipboard-helper.exe";
      return path;
    }

    bool launch_helper_as_interactive_user(
      std::wstring arguments,
      std::span<const HANDLE> inherited_handles,
      PROCESS_INFORMATION &process
    ) {
      if (inherited_handles.empty()) {
        return false;
      }

      const DWORD session_id = WTSGetActiveConsoleSessionId();
      HANDLE user_token {};
      if (session_id == 0xFFFFFFFF ||
          !WTSQueryUserToken(session_id, &user_token)) {
        BOOST_LOG(warning) << "Failed to query the interactive user for clipboard helper: "
                           << GetLastError();
        return false;
      }
      auto token_guard = util::fail_guard([user_token]() {
        CloseHandle(user_token);
      });

      PVOID environment {};
      if (!CreateEnvironmentBlock(&environment, user_token, FALSE)) {
        BOOST_LOG(warning) << "Failed to create clipboard helper environment: "
                           << GetLastError();
        return false;
      }
      auto environment_guard = util::fail_guard([environment]() {
        DestroyEnvironmentBlock(environment);
      });

      SIZE_T attribute_size = 0;
      InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
      auto *attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attribute_size)
      );
      if (!attributes ||
          !InitializeProcThreadAttributeList(
            attributes,
            1,
            0,
            &attribute_size
          )) {
        if (attributes) {
          HeapFree(GetProcessHeap(), 0, attributes);
        }
        BOOST_LOG(warning) << "Failed to create clipboard helper process attributes: "
                           << GetLastError();
        return false;
      }
      auto attributes_guard = util::fail_guard([attributes]() {
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
      });
      if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE *>(inherited_handles.data()),
            inherited_handles.size_bytes(),
            nullptr,
            nullptr
          )) {
        BOOST_LOG(warning) << "Failed to restrict clipboard helper handle inheritance: "
                           << GetLastError();
        return false;
      }

      const auto executable = helper_path();
      if (executable.empty()) {
        BOOST_LOG(warning) << "Failed to resolve clipboard helper path";
        return false;
      }
      auto command = L"\"" + executable + L"\" " + std::move(arguments);
      STARTUPINFOEXW startup {};
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.lpDesktop =
        const_cast<wchar_t *>(L"winsta0\\default");
      startup.lpAttributeList = attributes;
      if (!CreateProcessAsUserW(
            user_token,
            executable.c_str(),
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW |
              CREATE_UNICODE_ENVIRONMENT |
              EXTENDED_STARTUPINFO_PRESENT,
            environment,
            nullptr,
            &startup.StartupInfo,
            &process
          )) {
        BOOST_LOG(warning) << "Failed to launch clipboard helper: "
                           << GetLastError();
        return false;
      }
      return true;
    }

    pipe_result_t read_pipe_response(HANDLE pipe) {
      auto pipe_guard = util::fail_guard([pipe]() {
        CloseHandle(pipe);
      });

      pipe_result_t result;
      std::array<std::uint8_t, 64 * 1024> buffer {};
      for (;;) {
        DWORD received = 0;
        if (!ReadFile(
              pipe,
              buffer.data(),
              static_cast<DWORD>(buffer.size()),
              &received,
              nullptr
            )) {
          if (GetLastError() == ERROR_BROKEN_PIPE) {
            result.ok = true;
          }
          return result;
        }
        if (received == 0) {
          result.ok = true;
          return result;
        }
        if (result.bytes.size() >
            ipc::max_response_bytes - received) {
          return result;
        }
        result.bytes.insert(
          result.bytes.end(),
          buffer.begin(),
          buffer.begin() + received
        );
      }
    }

    bool decode_response(
      const std::vector<std::uint8_t> &bytes,
      std::vector<std::filesystem::path> &paths
    ) {
      if (bytes.size() < sizeof(ipc::response_header)) {
        return false;
      }

      ipc::response_header header {};
      std::memcpy(&header, bytes.data(), sizeof(header));
      if (header.magic != ipc::protocol_magic ||
          header.version != ipc::protocol_version) {
        BOOST_LOG(warning) << "Clipboard helper returned an invalid response header";
        return false;
      }
      if (header.result == ipc::status::no_files) {
        return false;
      }
      if (header.result != ipc::status::success) {
        BOOST_LOG(warning) << "Clipboard helper returned status "
                           << static_cast<std::uint32_t>(header.result)
                           << " with detail 0x"
                           << util::hex(header.detail).to_string_view();
        return false;
      }
      if (header.path_count == 0 ||
          header.path_count > LI_CLIPBOARD_MAX_FILE_ENTRIES) {
        return false;
      }

      std::size_t offset = sizeof(header);
      std::vector<std::filesystem::path> decoded;
      decoded.reserve(header.path_count);
      for (std::uint32_t index = 0; index < header.path_count; ++index) {
        std::uint32_t characters = 0;
        if (bytes.size() - offset < sizeof(characters)) {
          return false;
        }
        std::memcpy(&characters, bytes.data() + offset, sizeof(characters));
        offset += sizeof(characters);
        if (characters == 0 ||
            characters > ipc::max_path_chars ||
            characters > (bytes.size() - offset) / sizeof(wchar_t)) {
          return false;
        }

        std::wstring path(characters, L'\0');
        std::memcpy(
          path.data(),
          bytes.data() + offset,
          characters * sizeof(wchar_t)
        );
        offset += characters * sizeof(wchar_t);
        if (std::find(path.begin(), path.end(), L'\0') != path.end()) {
          return false;
        }
        decoded.emplace_back(std::move(path));
      }
      if (offset != bytes.size()) {
        return false;
      }
      paths = std::move(decoded);
      return true;
    }

    bool read_exact(HANDLE pipe, void *data, std::size_t size) {
      auto *bytes = static_cast<std::uint8_t *>(data);
      std::size_t offset = 0;
      while (offset < size) {
        DWORD received = 0;
        const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(size - offset, UINT32_MAX)
        );
        if (!ReadFile(
              pipe,
              bytes + offset,
              chunk,
              &received,
              nullptr
            ) ||
            received == 0) {
          return false;
        }
        offset += received;
      }
      return true;
    }

    bool write_exact(HANDLE pipe, const void *data, std::size_t size) {
      const auto *bytes = static_cast<const std::uint8_t *>(data);
      std::size_t offset = 0;
      while (offset < size) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(
          std::min<std::size_t>(size - offset, UINT32_MAX)
        );
        if (!WriteFile(
              pipe,
              bytes + offset,
              chunk,
              &written,
              nullptr
            ) ||
            written == 0) {
          return false;
        }
        offset += written;
      }
      return true;
    }

    class virtual_clipboard_session_t {
    public:
      virtual_clipboard_session_t() = default;
      virtual_clipboard_session_t(const virtual_clipboard_session_t &) = delete;
      virtual_clipboard_session_t &operator=(
        const virtual_clipboard_session_t &
      ) = delete;

      ~virtual_clipboard_session_t() {
        stop();
      }

      bool start(
        const std::vector<std::uint8_t> &manifest,
        std::string transfer_id,
        std::uint64_t origin_id,
        std::uint64_t item_id
      ) {
        if (manifest.empty() ||
            manifest.size() > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES ||
            transfer_id.empty() ||
            transfer_id.size() > ipc::max_transfer_id_bytes ||
            origin_id == 0 ||
            item_id == 0) {
          return false;
        }
        transfer_id_ = std::move(transfer_id);
        origin_id_ = origin_id;

        SECURITY_ATTRIBUTES security {
          .nLength = sizeof(SECURITY_ATTRIBUTES),
          .lpSecurityDescriptor = nullptr,
          .bInheritHandle = TRUE,
        };
        HANDLE helper_read {};
        HANDLE service_write {};
        HANDLE service_read {};
        HANDLE helper_write {};
        if (!CreatePipe(
              &helper_read,
              &service_write,
              &security,
              0
            ) ||
            !CreatePipe(
              &service_read,
              &helper_write,
              &security,
              0
            )) {
          if (helper_read) {
            CloseHandle(helper_read);
          }
          if (service_write) {
            CloseHandle(service_write);
          }
          if (service_read) {
            CloseHandle(service_read);
          }
          if (helper_write) {
            CloseHandle(helper_write);
          }
          BOOST_LOG(warning) << "Failed to create virtual clipboard helper pipes: "
                             << GetLastError();
          return false;
        }
        auto helper_read_guard = util::fail_guard([helper_read]() {
          CloseHandle(helper_read);
        });
        auto helper_write_guard = util::fail_guard([helper_write]() {
          CloseHandle(helper_write);
        });
        auto service_read_guard = util::fail_guard([service_read]() {
          CloseHandle(service_read);
        });
        auto service_write_guard = util::fail_guard([service_write]() {
          CloseHandle(service_write);
        });
        if (!SetHandleInformation(
              service_read,
              HANDLE_FLAG_INHERIT,
              0
            ) ||
            !SetHandleInformation(
              service_write,
              HANDLE_FLAG_INHERIT,
              0
            )) {
          BOOST_LOG(warning) << "Failed to protect virtual clipboard service pipe handles: "
                             << GetLastError();
          return false;
        }

        HANDLE stop_event = CreateEventW(
          &security,
          TRUE,
          FALSE,
          nullptr
        );
        if (!stop_event) {
          BOOST_LOG(warning) << "Failed to create virtual clipboard stop event: "
                             << GetLastError();
          return false;
        }
        auto stop_guard = util::fail_guard([stop_event]() {
          CloseHandle(stop_event);
        });

        HANDLE parent_process {};
        if (!DuplicateHandle(
              GetCurrentProcess(),
              GetCurrentProcess(),
              GetCurrentProcess(),
              &parent_process,
              SYNCHRONIZE,
              TRUE,
              0
            )) {
          BOOST_LOG(warning) << "Failed to create virtual clipboard parent process handle: "
                             << GetLastError();
          return false;
        }
        auto parent_guard = util::fail_guard([parent_process]() {
          CloseHandle(parent_process);
        });

        const HANDLE inherited_handles[] {
          helper_read,
          helper_write,
          stop_event,
          parent_process,
        };
        auto arguments =
          L"--publish-virtual-files --service-read-handle " +
          std::to_wstring(
            reinterpret_cast<std::uintptr_t>(helper_read)
          ) +
          L" --service-write-handle " +
          std::to_wstring(
            reinterpret_cast<std::uintptr_t>(helper_write)
          ) +
          L" --stop-event " +
          std::to_wstring(
            reinterpret_cast<std::uintptr_t>(stop_event)
          ) +
          L" --parent-handle " +
          std::to_wstring(
            reinterpret_cast<std::uintptr_t>(parent_process)
          );

        PROCESS_INFORMATION process {};
        if (!launch_helper_as_interactive_user(
              std::move(arguments),
              inherited_handles,
              process
            )) {
          return false;
        }

        CloseHandle(process.hThread);
        process_ = process.hProcess;
        stop_event_ = stop_event;
        stop_guard.disable();
        service_read_ = service_read;
        service_read_guard.disable();
        service_write_ = service_write;
        service_write_guard.disable();
        CloseHandle(helper_read);
        helper_read_guard.disable();
        CloseHandle(helper_write);
        helper_write_guard.disable();

        auto ready = ready_promise_.get_future();
        worker_ = std::thread([this]() {
          service_messages();
        });

        ipc::publish_request_header request {
          .magic = ipc::protocol_magic,
          .version = ipc::protocol_version,
          .type = ipc::message_type::publish_request,
          .manifest_size =
            static_cast<std::uint32_t>(manifest.size()),
          .transfer_id_size =
            static_cast<std::uint32_t>(transfer_id_.size()),
          .origin_id = origin_id,
          .item_id = item_id,
        };
        if (!write_exact(service_write_, &request, sizeof(request)) ||
            !write_exact(
              service_write_,
              manifest.data(),
              manifest.size()
            ) ||
            !write_exact(
              service_write_,
              transfer_id_.data(),
              transfer_id_.size()
            )) {
          BOOST_LOG(warning) << "Failed to initialize virtual clipboard helper";
          return false;
        }

        if (ready.wait_for(5s) == std::future_status::timeout ||
            !ready.get()) {
          BOOST_LOG(warning) << "Virtual clipboard helper failed to publish the clipboard";
          return false;
        }
        BOOST_LOG(debug) << "Clipboard file trace transfer="
                        << transfer_id_
                        << " phase=published helper_pid="
                        << GetProcessId(process_);
        return true;
      }

    private:
#pragma pack(push, 1)
      struct message_prefix_t {
        std::uint32_t magic;
        std::uint32_t version;
        ipc::message_type type;
      };
#pragma pack(pop)

      void report_ready(bool ready) {
        bool expected = false;
        if (ready_reported_.compare_exchange_strong(
              expected,
              true
            )) {
          ready_promise_.set_value(ready);
        }
      }

      void service_messages() {
        for (;;) {
          message_prefix_t prefix {};
          if (!read_exact(service_read_, &prefix, sizeof(prefix)) ||
              prefix.magic != ipc::protocol_magic ||
              prefix.version != ipc::protocol_version) {
            break;
          }

          if (prefix.type == ipc::message_type::publish_ready) {
            ipc::publish_ready_message ready {};
            std::memcpy(&ready, &prefix, sizeof(prefix));
            if (!read_exact(
                  service_read_,
                  reinterpret_cast<std::uint8_t *>(&ready) +
                    sizeof(prefix),
                  sizeof(ready) - sizeof(prefix)
                )) {
              break;
            }
            const bool published =
              ready.result == ipc::status::success;
            published_.store(published);
            report_ready(published);
            continue;
          }

          if (prefix.type != ipc::message_type::chunk_request) {
            break;
          }
          ipc::chunk_request_message request {};
          std::memcpy(&request, &prefix, sizeof(prefix));
          if (!read_exact(
                service_read_,
                reinterpret_cast<std::uint8_t *>(&request) +
                  sizeof(prefix),
                sizeof(request) - sizeof(prefix)
              ) ||
              request.length == 0 ||
              request.length > LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES) {
            break;
          }
          const bool first_chunk =
            !chunk_request_observed_.exchange(true);
          const auto request_started =
            std::chrono::steady_clock::now();
          if (first_chunk) {
            BOOST_LOG(debug) << "Clipboard file trace transfer="
                            << transfer_id_
                            << " phase=first_chunk_requested file="
                            << request.file_index
                            << " offset="
                            << request.offset
                            << " bytes="
                            << request.length;
          }

          auto result = clipboard_file_store::request_remote_chunk(
            transfer_id_,
            origin_id_,
            request.file_index,
            request.offset,
            request.length
          );
          if (first_chunk) {
            const auto elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - request_started
              ).count();
            if (result.ok) {
              BOOST_LOG(debug) << "Clipboard file trace transfer="
                              << transfer_id_
                              << " phase=first_chunk_completed duration_ms="
                              << elapsed
                              << " bytes="
                              << result.bytes.size();
            } else {
              BOOST_LOG(warning) << "Clipboard file trace transfer="
                                 << transfer_id_
                                 << " phase=first_chunk_failed duration_ms="
                                 << elapsed
                                 << " error="
                                 << result.error;
            }
          }
          const bool valid =
            result.ok && result.bytes.size() == request.length;
          ipc::chunk_response_header response {
            .magic = ipc::protocol_magic,
            .version = ipc::protocol_version,
            .type = ipc::message_type::chunk_response,
            .result = valid ?
                        ipc::status::success :
                        ipc::status::transfer_error,
            .length = valid ? request.length : 0,
          };
          if (!write_exact(
                service_write_,
                &response,
                sizeof(response)
              ) ||
              (valid &&
               !write_exact(
                 service_write_,
                 result.bytes.data(),
                 result.bytes.size()
               ))) {
            break;
          }
        }
        report_ready(false);
        if (!stopping_.load() && published_.load()) {
          WaitForSingleObject(process_, 100);
          DWORD exit_code = STILL_ACTIVE;
          GetExitCodeProcess(process_, &exit_code);
          BOOST_LOG(warning) << "Interactive clipboard helper exited unexpectedly with code "
                             << exit_code;
        }
      }

      void stop() {
        stopping_.store(true);
        if (stop_event_) {
          SetEvent(stop_event_);
        }
        if (service_write_) {
          CloseHandle(service_write_);
        }
        if (process_) {
          if (WaitForSingleObject(process_, 2000) != WAIT_OBJECT_0) {
            TerminateProcess(process_, ERROR_TIMEOUT);
            WaitForSingleObject(process_, INFINITE);
          }
        }
        if (worker_.joinable()) {
          worker_.join();
        }
        if (service_read_) {
          CloseHandle(service_read_);
        }
        if (stop_event_) {
          CloseHandle(stop_event_);
        }
        if (process_) {
          CloseHandle(process_);
        }
        service_write_ = nullptr;
        service_read_ = nullptr;
        stop_event_ = nullptr;
        process_ = nullptr;
      }

      std::string transfer_id_;
      std::uint64_t origin_id_ {};
      HANDLE service_read_ {};
      HANDLE service_write_ {};
      HANDLE stop_event_ {};
      HANDLE process_ {};
      std::thread worker_;
      std::promise<bool> ready_promise_;
      std::atomic_bool ready_reported_ {};
      std::atomic_bool published_ {};
      std::atomic_bool stopping_ {};
      std::atomic_bool chunk_request_observed_ {};
    };

    std::mutex virtual_clipboard_mutex;
    std::unique_ptr<virtual_clipboard_session_t>
      virtual_clipboard_session;
  }  // namespace

  bool get_user_file_clipboard_paths(
    std::vector<std::filesystem::path> &paths
  ) {
    paths.clear();
    if (!is_local_system_process()) {
      return false;
    }

    HANDLE read_pipe {};
    HANDLE write_pipe {};
    SECURITY_ATTRIBUTES pipe_security {
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = nullptr,
      .bInheritHandle = TRUE,
    };
    if (!CreatePipe(&read_pipe, &write_pipe, &pipe_security, 0)) {
      BOOST_LOG(warning) << "Failed to create clipboard helper pipe: "
                         << GetLastError();
      return false;
    }
    auto read_pipe_guard = util::fail_guard([read_pipe]() {
      CloseHandle(read_pipe);
    });
    auto write_pipe_guard = util::fail_guard([write_pipe]() {
      CloseHandle(write_pipe);
    });
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
      BOOST_LOG(warning) << "Failed to protect clipboard helper read handle: "
                         << GetLastError();
      return false;
    }

    auto arguments =
      L"--write-handle " +
      std::to_wstring(
        reinterpret_cast<std::uintptr_t>(write_pipe)
      );
    const HANDLE inherited_handles[] {write_pipe};
    PROCESS_INFORMATION process {};
    if (!launch_helper_as_interactive_user(
          std::move(arguments),
          inherited_handles,
          process
        )) {
      return false;
    }
    auto process_guard = util::fail_guard([process]() {
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    });

    CloseHandle(write_pipe);
    write_pipe_guard.disable();

    const HANDLE reader_pipe = read_pipe;
    auto reader = std::async(std::launch::async, [reader_pipe]() {
      return read_pipe_response(reader_pipe);
    });
    read_pipe_guard.disable();

    if (reader.wait_for(5s) == std::future_status::timeout) {
      BOOST_LOG(warning) << "Clipboard helper timed out";
      TerminateProcess(process.hProcess, ERROR_TIMEOUT);
    }
    const auto pipe_result = reader.get();
    const DWORD process_wait = WaitForSingleObject(
      process.hProcess,
      1000
    );
    if (process_wait != WAIT_OBJECT_0) {
      TerminateProcess(process.hProcess, ERROR_TIMEOUT);
      WaitForSingleObject(process.hProcess, INFINITE);
      BOOST_LOG(warning) << "Clipboard helper did not exit cleanly";
      return false;
    }

    DWORD exit_code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(process.hProcess, &exit_code) ||
        exit_code != ERROR_SUCCESS ||
        !pipe_result.ok) {
      BOOST_LOG(warning) << "Clipboard helper failed with exit code "
                         << exit_code;
      return false;
    }
    if (!decode_response(pipe_result.bytes, paths)) {
      return false;
    }
    BOOST_LOG(info) << "Clipboard helper read "
                    << paths.size()
                    << " top-level file item(s)";
    return true;
  }

  user_virtual_clipboard_result set_user_virtual_file_clipboard(
    const std::vector<std::uint8_t> &manifest,
    const std::string &transfer_id,
    std::uint64_t origin_id,
    std::uint64_t item_id
  ) {
    if (!is_local_system_process()) {
      return user_virtual_clipboard_result::not_required;
    }

    auto next_session =
      std::make_unique<virtual_clipboard_session_t>();
    if (!next_session->start(
          manifest,
          transfer_id,
          origin_id,
          item_id
        )) {
      return user_virtual_clipboard_result::failure;
    }

    std::unique_ptr<virtual_clipboard_session_t> previous_session;
    {
      std::lock_guard lock(virtual_clipboard_mutex);
      previous_session = std::move(virtual_clipboard_session);
      virtual_clipboard_session = std::move(next_session);
    }
    previous_session.reset();
    return user_virtual_clipboard_result::success;
  }
}  // namespace platf::windows
