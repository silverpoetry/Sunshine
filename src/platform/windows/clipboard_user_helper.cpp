#include "clipboard_user_helper.h"

#include "clipboard_helper_ipc.h"
#include "src/logging.h"
#include "src/utility.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
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
          &write_pipe,
          sizeof(write_pipe),
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
    auto command = L"\"" + executable +
                   L"\" --write-handle " +
                   std::to_wstring(
                     reinterpret_cast<std::uintptr_t>(write_pipe)
                   );

    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.lpDesktop =
      const_cast<wchar_t *>(L"winsta0\\default");
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process {};
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
}  // namespace platf::windows
