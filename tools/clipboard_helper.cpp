#define WIN32_LEAN_AND_MEAN

#include "src/platform/windows/clipboard_helper_ipc.h"
#include "src/platform/windows/clipboard_virtual_files.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <windows.h>

extern "C" {
#include "third-party/moonlight-common-c/src/Clipboard.h"
}

namespace {
  namespace ipc = platf::windows::clipboard_helper;

  HANDLE service_read_pipe {};
  HANDLE service_write_pipe {};
  std::mutex service_pipe_mutex;

  bool read_exact(HANDLE pipe, void *data, std::size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(data);
    std::size_t offset = 0;
    while (offset < size) {
      const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(size - offset, UINT32_MAX)
      );
      DWORD received = 0;
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
      const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(size - offset, UINT32_MAX)
      );
      DWORD written = 0;
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

  bool append_bytes(
    std::vector<std::uint8_t> &output,
    const void *data,
    std::size_t size
  ) {
    if (size > ipc::max_response_bytes ||
        output.size() > ipc::max_response_bytes - size) {
      return false;
    }
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    output.insert(output.end(), bytes, bytes + size);
    return true;
  }

  bool append_path(
    std::vector<std::uint8_t> &output,
    const std::wstring &path
  ) {
    if (path.empty() ||
        path.size() > ipc::max_path_chars ||
        std::find(path.begin(), path.end(), L'\0') != path.end()) {
      return false;
    }
    const auto characters = static_cast<std::uint32_t>(path.size());
    return append_bytes(output, &characters, sizeof(characters)) &&
           append_bytes(
             output,
             path.data(),
             path.size() * sizeof(wchar_t)
           );
  }

  std::vector<std::uint8_t> read_clipboard() {
    ipc::response_header header {
      .magic = ipc::protocol_magic,
      .version = ipc::protocol_version,
      .result = ipc::status::clipboard_error,
    };
    std::vector<std::uint8_t> response(sizeof(header));

    const HRESULT initialize_result = OleInitialize(nullptr);
    if (FAILED(initialize_result)) {
      header.detail = static_cast<std::uint32_t>(initialize_result);
      std::memcpy(response.data(), &header, sizeof(header));
      return response;
    }

    IDataObject *object = nullptr;
    const HRESULT clipboard_result = OleGetClipboard(&object);
    if (FAILED(clipboard_result) || !object) {
      header.detail = static_cast<std::uint32_t>(
        FAILED(clipboard_result) ? clipboard_result : E_UNEXPECTED
      );
      std::memcpy(response.data(), &header, sizeof(header));
      OleUninitialize();
      return response;
    }

    FORMATETC format {
      CF_HDROP,
      nullptr,
      DVASPECT_CONTENT,
      -1,
      TYMED_HGLOBAL,
    };
    STGMEDIUM medium {};
    const HRESULT data_result = object->GetData(&format, &medium);
    if (FAILED(data_result)) {
      header.result = data_result == DV_E_FORMATETC ?
                        ipc::status::no_files :
                        ipc::status::clipboard_error;
      header.detail = static_cast<std::uint32_t>(data_result);
      std::memcpy(response.data(), &header, sizeof(header));
      object->Release();
      OleUninitialize();
      return response;
    }

    if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
      header.result = ipc::status::invalid_data;
      header.detail = static_cast<std::uint32_t>(DV_E_TYMED);
    } else {
      const auto drop = static_cast<HDROP>(medium.hGlobal);
      const UINT count = DragQueryFileW(
        drop,
        0xFFFFFFFF,
        nullptr,
        0
      );
      if (count == 0 || count > LI_CLIPBOARD_MAX_FILE_ENTRIES) {
        header.result = count == 0 ?
                          ipc::status::no_files :
                          ipc::status::invalid_data;
      } else {
        header.result = ipc::status::success;
        header.path_count = count;
        for (UINT index = 0; index < count; ++index) {
          const UINT length = DragQueryFileW(drop, index, nullptr, 0);
          if (length == 0 || length > ipc::max_path_chars) {
            header.result = ipc::status::invalid_data;
            break;
          }
          std::wstring path(length + 1, L'\0');
          if (DragQueryFileW(
                drop,
                index,
                path.data(),
                length + 1
              ) != length) {
            header.result = ipc::status::invalid_data;
            break;
          }
          path.resize(length);
          if (!append_path(response, path)) {
            header.result = ipc::status::invalid_data;
            break;
          }
        }
      }
    }

    if (header.result != ipc::status::success) {
      response.resize(sizeof(header));
      header.path_count = 0;
    }
    std::memcpy(response.data(), &header, sizeof(header));
    ReleaseStgMedium(&medium);
    object->Release();
    OleUninitialize();
    return response;
  }

  HANDLE parse_handle(
    int argc,
    wchar_t **argv,
    std::wstring_view option
  ) {
    for (int index = 1; index + 1 < argc; ++index) {
      if (std::wstring_view(argv[index]) != option) {
        continue;
      }
      wchar_t *end = nullptr;
      const auto value = std::wcstoull(argv[index + 1], &end, 10);
      if (!end || *end != L'\0' || value == 0) {
        return nullptr;
      }
      return reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(value)
      );
    }
    return nullptr;
  }

  bool has_option(
    int argc,
    wchar_t **argv,
    std::wstring_view option
  ) {
    return std::any_of(
      argv + 1,
      argv + argc,
      [option](const wchar_t *argument) {
        return std::wstring_view(argument) == option;
      }
    );
  }

  int publish_virtual_files(int argc, wchar_t **argv) {
    service_read_pipe = parse_handle(
      argc,
      argv,
      L"--service-read-handle"
    );
    service_write_pipe = parse_handle(
      argc,
      argv,
      L"--service-write-handle"
    );
    HANDLE stop_event = parse_handle(argc, argv, L"--stop-event");
    HANDLE parent_process = parse_handle(
      argc,
      argv,
      L"--parent-handle"
    );
    if (!service_read_pipe || !service_write_pipe ||
        !stop_event || !parent_process) {
      return ERROR_INVALID_PARAMETER;
    }

    ipc::publish_request_header request {};
    if (!read_exact(service_read_pipe, &request, sizeof(request)) ||
        request.magic != ipc::protocol_magic ||
        request.version != ipc::protocol_version ||
        request.type != ipc::message_type::publish_request ||
        request.manifest_size == 0 ||
        request.manifest_size > LI_CLIPBOARD_MAX_FILE_MANIFEST_BYTES ||
        request.transfer_id_size == 0 ||
        request.transfer_id_size > ipc::max_transfer_id_bytes ||
        request.origin_id == 0 ||
        request.item_id == 0) {
      return ERROR_INVALID_DATA;
    }

    std::vector<std::uint8_t> manifest(request.manifest_size);
    std::string transfer_id(request.transfer_id_size, '\0');
    if (!read_exact(
          service_read_pipe,
          manifest.data(),
          manifest.size()
        ) ||
        !read_exact(
          service_read_pipe,
          transfer_id.data(),
          transfer_id.size()
        ) ||
        !LiIsValidClipboardFileManifest(
          manifest.data(),
          manifest.size()
        )) {
      return ERROR_INVALID_DATA;
    }

    ipc::publish_ready_message ready {
      .magic = ipc::protocol_magic,
      .version = ipc::protocol_version,
      .type = ipc::message_type::publish_ready,
      .result = ipc::status::clipboard_error,
    };
    if (platf::windows::set_virtual_file_clipboard(
          manifest,
          transfer_id,
          request.origin_id,
          request.item_id
        )) {
      ready.result = ipc::status::success;
    } else {
      ready.detail = static_cast<std::uint32_t>(E_FAIL);
    }
    if (!write_exact(service_write_pipe, &ready, sizeof(ready)) ||
        ready.result != ipc::status::success) {
      return ERROR_WRITE_FAULT;
    }

    const HANDLE wait_handles[] {stop_event, parent_process};
    WaitForMultipleObjects(
      static_cast<DWORD>(std::size(wait_handles)),
      wait_handles,
      FALSE,
      INFINITE
    );
    CloseHandle(parent_process);
    CloseHandle(stop_event);
    CloseHandle(service_write_pipe);
    CloseHandle(service_read_pipe);
    service_write_pipe = nullptr;
    service_read_pipe = nullptr;
    return ERROR_SUCCESS;
  }
}  // namespace

bool platf::windows::request_virtual_file_chunk_from_service(
  std::uint32_t file_index,
  std::uint64_t offset,
  std::size_t length,
  std::vector<std::uint8_t> &bytes
) {
  if (!service_read_pipe || !service_write_pipe ||
      length == 0 ||
      length > LI_CLIPBOARD_MAX_FILE_CHUNK_BYTES ||
      length > (std::numeric_limits<std::uint32_t>::max)()) {
    return false;
  }

  std::lock_guard lock(service_pipe_mutex);
  ipc::chunk_request_message request {
    .magic = ipc::protocol_magic,
    .version = ipc::protocol_version,
    .type = ipc::message_type::chunk_request,
    .file_index = file_index,
    .offset = offset,
    .length = static_cast<std::uint32_t>(length),
  };
  if (!write_exact(service_write_pipe, &request, sizeof(request))) {
    return false;
  }

  ipc::chunk_response_header response {};
  if (!read_exact(service_read_pipe, &response, sizeof(response)) ||
      response.magic != ipc::protocol_magic ||
      response.version != ipc::protocol_version ||
      response.type != ipc::message_type::chunk_response ||
      response.result != ipc::status::success ||
      response.length != request.length) {
    return false;
  }
  bytes.resize(response.length);
  return read_exact(service_read_pipe, bytes.data(), bytes.size());
}

int wmain(int argc, wchar_t **argv) {
  if (has_option(argc, argv, L"--publish-virtual-files")) {
    return publish_virtual_files(argc, argv);
  }

  HANDLE pipe = parse_handle(argc, argv, L"--write-handle");
  if (!pipe) {
    return ERROR_INVALID_PARAMETER;
  }
  const auto response = read_clipboard();
  const bool written = write_exact(
    pipe,
    response.data(),
    response.size()
  );
  CloseHandle(pipe);
  return written ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
