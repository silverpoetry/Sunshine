#define WIN32_LEAN_AND_MEAN

#include "src/platform/windows/clipboard_helper_ipc.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
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

  HANDLE parse_pipe_handle(int argc, wchar_t **argv) {
    for (int index = 1; index + 1 < argc; ++index) {
      if (std::wstring_view(argv[index]) != L"--write-handle") {
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
}  // namespace

int wmain(int argc, wchar_t **argv) {
  HANDLE pipe = parse_pipe_handle(argc, argv);
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
