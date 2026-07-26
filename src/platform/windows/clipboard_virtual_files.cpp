#include "clipboard_virtual_files.h"

#include "../../clipboard_file_store.h"
#include "clipboard_user_helper.h"
#include "src/utility.h"
#include "utf_utils.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <objidl.h>
#include <shldisp.h>
#include <shlobj.h>
#include <thread>
#include <variant>
#include <windows.h>

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

namespace platf::windows {
  namespace {
    constexpr std::uint32_t clipboard_identity_magic = 0x32434C4D;  // "MLC2"

#pragma pack(push, 1)

    struct clipboard_identity_t {
      std::uint32_t magic;
      std::uint8_t mime_type;
      std::uint8_t reserved[3];
      std::uint64_t origin_id;
      std::uint64_t item_id;
    };

#pragma pack(pop)

    struct file_entry_t {
      std::wstring path;
      std::uint8_t type {};
      std::uint64_t size {};
      std::uint64_t modified_time_ms {};
    };

    HRESULT read_file_clipboard_paths(std::vector<std::filesystem::path> &paths) {
      IDataObject *object = nullptr;
      const auto clipboard_result = OleGetClipboard(&object);
      if (FAILED(clipboard_result)) {
        return clipboard_result;
      }
      if (!object) {
        return E_UNEXPECTED;
      }
      auto release_object = util::fail_guard([object]() {
        object->Release();
      });

      FORMATETC format {
        CF_HDROP,
        nullptr,
        DVASPECT_CONTENT,
        -1,
        TYMED_HGLOBAL,
      };
      STGMEDIUM medium {};
      const auto data_result = object->GetData(&format, &medium);
      if (FAILED(data_result)) {
        return data_result;
      }
      auto release_medium = util::fail_guard([&medium]() {
        ReleaseStgMedium(&medium);
      });

      if (medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) {
        return DV_E_TYMED;
      }

      const auto drop = static_cast<HDROP>(medium.hGlobal);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      if (count == 0 || count > LI_CLIPBOARD_MAX_FILE_ENTRIES) {
        return DV_E_FORMATETC;
      }

      std::vector<std::filesystem::path> result;
      result.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
          return DV_E_FORMATETC;
        }
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, index, path.data(), length + 1) != length) {
          return DV_E_FORMATETC;
        }
        path.resize(length);
        result.emplace_back(std::move(path));
      }

      paths = std::move(result);
      return S_OK;
    }

    HGLOBAL copy_to_global_memory(const void *source, std::size_t size) {
      HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
      if (!memory) {
        return nullptr;
      }
      void *destination = GlobalLock(memory);
      if (!destination) {
        GlobalFree(memory);
        return nullptr;
      }
      std::memcpy(destination, source, size);
      GlobalUnlock(memory);
      return memory;
    }

    FILETIME unix_time_to_file_time(std::uint64_t milliseconds) {
      constexpr std::uint64_t windows_epoch = UINT64_C(116444736000000000);
      constexpr std::uint64_t ticks_per_millisecond = 10000;
      const std::uint64_t ticks =
        milliseconds <= (UINT64_MAX - windows_epoch) / ticks_per_millisecond ?
          windows_epoch + milliseconds * ticks_per_millisecond :
          windows_epoch;
      ULARGE_INTEGER value {};
      value.QuadPart = ticks;
      return {value.LowPart, value.HighPart};
    }

    bool decode_manifest(const std::vector<std::uint8_t> &manifest, std::vector<file_entry_t> &files) {
      LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
      if (!LiIsValidClipboardFileManifest(manifest.data(), manifest.size()) ||
          !LiDecodeClipboardFileManifestHeader(
            manifest.data(),
            manifest.size(),
            &header
          )) {
        return false;
      }

      files.reserve(header.entryCount);
      std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
      try {
        for (std::uint32_t index = 0; index < header.entryCount; ++index) {
          LI_CLIPBOARD_FILE_MANIFEST_ENTRY decoded;
          if (!LiDecodeClipboardFileManifestEntry(
                manifest.data(),
                manifest.size(),
                &offset,
                &decoded
              )) {
            return false;
          }
          std::string utf8(
            reinterpret_cast<const char *>(decoded.path),
            decoded.pathLength
          );
          auto path = utf_utils::from_utf8(utf8);
          std::replace(path.begin(), path.end(), L'/', L'\\');
          if (path.empty() || path.size() >= MAX_PATH) {
            return false;
          }
          files.push_back({
            .path = std::move(path),
            .type = decoded.type,
            .size = decoded.size,
            .modified_time_ms = decoded.modifiedTimeMs,
          });
        }
      } catch (...) {
        return false;
      }
      return offset == manifest.size() && !files.empty();
    }

    class virtual_file_stream_t final: public IStream {
    public:
      virtual_file_stream_t(std::string transfer_id, std::uint64_t origin_id, std::uint32_t file_index, std::uint64_t size, std::wstring name):
          transfer_id_(std::move(transfer_id)),
          origin_id_(origin_id),
          file_index_(file_index),
          size_(size),
          name_(std::move(name)) {
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) {
          return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
          *object = static_cast<IStream *>(this);
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
          delete this;
        }
        return remaining;
      }

      HRESULT STDMETHODCALLTYPE Read(void *destination, ULONG requested, ULONG *bytes_read) override {
        if (bytes_read) {
          *bytes_read = 0;
        }
        if (requested != 0 && !destination) {
          return STG_E_INVALIDPOINTER;
        }
        if (requested == 0) {
          return S_OK;
        }

        auto *output = static_cast<std::uint8_t *>(destination);
        ULONG total_read = 0;
        while (total_read < requested && position_ < size_) {
          const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>({
              requested - total_read,
              size_ - position_,
              clipboard_file_store::max_chunk_bytes,
            })
          );
          auto result = clipboard_file_store::request_remote_chunk(
            transfer_id_,
            origin_id_,
            file_index_,
            position_,
            length
          );
          if (!result.ok || result.bytes.size() != length) {
            if (bytes_read) {
              *bytes_read = total_read;
            }
            return STG_E_READFAULT;
          }
          std::memcpy(output + total_read, result.bytes.data(), result.bytes.size());
          position_ += result.bytes.size();
          total_read += static_cast<ULONG>(result.bytes.size());
        }

        if (bytes_read) {
          *bytes_read = total_read;
        }
        return total_read == requested ? S_OK : S_FALSE;
      }

      HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
        return STG_E_ACCESSDENIED;
      }

      HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *new_position) override {
        std::int64_t base;
        switch (origin) {
          case STREAM_SEEK_SET:
            base = 0;
            break;
          case STREAM_SEEK_CUR:
            if (position_ > static_cast<std::uint64_t>(INT64_MAX)) {
              return STG_E_INVALIDFUNCTION;
            }
            base = static_cast<std::int64_t>(position_);
            break;
          case STREAM_SEEK_END:
            if (size_ > static_cast<std::uint64_t>(INT64_MAX)) {
              return STG_E_INVALIDFUNCTION;
            }
            base = static_cast<std::int64_t>(size_);
            break;
          default:
            return STG_E_INVALIDFUNCTION;
        }
        if ((move.QuadPart < 0 && base < -move.QuadPart) ||
            (move.QuadPart > 0 && base > INT64_MAX - move.QuadPart)) {
          return STG_E_INVALIDFUNCTION;
        }
        const auto target = base + move.QuadPart;
        if (target < 0) {
          return STG_E_INVALIDFUNCTION;
        }
        position_ = static_cast<std::uint64_t>(target);
        if (new_position) {
          new_position->QuadPart = position_;
        }
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_ACCESSDENIED;
      }

      HRESULT STDMETHODCALLTYPE CopyTo(IStream *destination, ULARGE_INTEGER count, ULARGE_INTEGER *bytes_read, ULARGE_INTEGER *bytes_written) override {
        if (!destination) {
          return STG_E_INVALIDPOINTER;
        }
        if (bytes_read) {
          bytes_read->QuadPart = 0;
        }
        if (bytes_written) {
          bytes_written->QuadPart = 0;
        }

        std::vector<std::uint8_t> buffer(
          std::min<std::uint64_t>(clipboard_file_store::max_chunk_bytes, 1024 * 1024)
        );
        std::uint64_t remaining = count.QuadPart;
        while (remaining != 0) {
          const auto requested = static_cast<ULONG>(
            std::min<std::uint64_t>(remaining, buffer.size())
          );
          ULONG read = 0;
          const auto read_result = Read(buffer.data(), requested, &read);
          if (FAILED(read_result)) {
            return read_result;
          }
          if (read == 0) {
            return S_FALSE;
          }
          ULONG written = 0;
          const auto write_result = destination->Write(buffer.data(), read, &written);
          if (FAILED(write_result) || written != read) {
            return STG_E_WRITEFAULT;
          }
          remaining -= read;
          if (bytes_read) {
            bytes_read->QuadPart += read;
          }
          if (bytes_written) {
            bytes_written->QuadPart += written;
          }
          if (read_result == S_FALSE) {
            return S_FALSE;
          }
        }
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Revert() override {
        return STG_E_REVERTED;
      }

      HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
      }

      HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
      }

      HRESULT STDMETHODCALLTYPE Stat(STATSTG *status, DWORD flags) override {
        if (!status) {
          return STG_E_INVALIDPOINTER;
        }
        *status = {};
        status->type = STGTY_STREAM;
        status->cbSize.QuadPart = size_;
        status->grfMode = STGM_READ;
        if ((flags & STATFLAG_NONAME) == 0) {
          const auto bytes = (name_.size() + 1) * sizeof(wchar_t);
          status->pwcsName = static_cast<wchar_t *>(CoTaskMemAlloc(bytes));
          if (!status->pwcsName) {
            return STG_E_INSUFFICIENTMEMORY;
          }
          std::memcpy(status->pwcsName, name_.c_str(), bytes);
        }
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Clone(IStream **stream) override {
        if (!stream) {
          return E_POINTER;
        }
        auto clone = new (std::nothrow) virtual_file_stream_t(
          transfer_id_,
          origin_id_,
          file_index_,
          size_,
          name_
        );
        if (!clone) {
          return E_OUTOFMEMORY;
        }
        clone->position_ = position_;
        *stream = clone;
        return S_OK;
      }

    private:
      std::atomic_ulong references_ {1};
      std::string transfer_id_;
      std::uint64_t origin_id_;
      std::uint32_t file_index_;
      std::uint64_t size_;
      std::wstring name_;
      std::uint64_t position_ {};
    };

    class format_enumerator_t final: public IEnumFORMATETC {
    public:
      explicit format_enumerator_t(std::vector<FORMATETC> formats):
          formats_(std::move(formats)) {
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) {
          return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC) {
          *object = static_cast<IEnumFORMATETC *>(this);
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
          delete this;
        }
        return remaining;
      }

      HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC *formats, ULONG *fetched) override {
        if (!formats || (count != 1 && !fetched)) {
          return E_INVALIDARG;
        }
        ULONG copied = 0;
        while (copied < count && position_ < formats_.size()) {
          formats[copied] = formats_[position_];
          formats[copied].ptd = nullptr;
          ++copied;
          ++position_;
        }
        if (fetched) {
          *fetched = copied;
        }
        return copied == count ? S_OK : S_FALSE;
      }

      HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const auto remaining = formats_.size() - position_;
        const auto skipped = std::min<std::size_t>(count, remaining);
        position_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
      }

      HRESULT STDMETHODCALLTYPE Reset() override {
        position_ = 0;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **enumerator) override {
        if (!enumerator) {
          return E_POINTER;
        }
        auto clone = new (std::nothrow) format_enumerator_t(formats_);
        if (!clone) {
          return E_OUTOFMEMORY;
        }
        clone->position_ = position_;
        *enumerator = clone;
        return S_OK;
      }

    private:
      std::atomic_ulong references_ {1};
      std::vector<FORMATETC> formats_;
      std::size_t position_ {};
    };

    class virtual_file_data_object_t final:
        public IDataObject,
        public IDataObjectAsyncCapability {
    public:
      virtual_file_data_object_t(std::vector<file_entry_t> files, std::string transfer_id, std::uint64_t origin_id, std::uint64_t item_id):
          files_(std::move(files)),
          transfer_id_(std::move(transfer_id)),
          origin_id_(origin_id),
          identity_ {
            clipboard_identity_magic,
            LI_CLIPBOARD_MIME_FILE_MANIFEST,
            {},
            origin_id,
            item_id,
          },
          descriptor_format_(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
          contents_format_(RegisterClipboardFormatW(L"FileContents")),
          preferred_drop_effect_format_(RegisterClipboardFormatW(L"Preferred DropEffect")),
          identity_format_(RegisterClipboardFormatW(L"MoonlightClipboardIdentityV2")) {
      }

      bool valid() const {
        return !files_.empty() &&
               !transfer_id_.empty() &&
               origin_id_ != 0 &&
               descriptor_format_ != 0 &&
               contents_format_ != 0 &&
               preferred_drop_effect_format_ != 0 &&
               identity_format_ != 0;
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) {
          return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
          *object = static_cast<IDataObject *>(this);
          AddRef();
          return S_OK;
        }
        if (iid == IID_IDataObjectAsyncCapability) {
          *object = static_cast<IDataObjectAsyncCapability *>(this);
          AddRef();
          return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) {
          delete this;
        }
        return remaining;
      }

      HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override {
        if (!format || !medium) {
          return E_POINTER;
        }
        *medium = {};
        const auto query = QueryGetData(format);
        if (FAILED(query)) {
          return query;
        }

        if (format->cfFormat == descriptor_format_) {
          return get_descriptors(*medium);
        }
        if (format->cfFormat == contents_format_) {
          const auto index = static_cast<std::size_t>(format->lindex);
          const auto &file = files_[index];
          auto stream = new (std::nothrow) virtual_file_stream_t(
            transfer_id_,
            origin_id_,
            static_cast<std::uint32_t>(index),
            file.size,
            file.path
          );
          if (!stream) {
            return E_OUTOFMEMORY;
          }
          medium->tymed = TYMED_ISTREAM;
          medium->pstm = stream;
          return S_OK;
        }
        if (format->cfFormat == preferred_drop_effect_format_) {
          const DWORD effect = DROPEFFECT_COPY;
          medium->hGlobal = copy_to_global_memory(&effect, sizeof(effect));
        } else if (format->cfFormat == identity_format_) {
          medium->hGlobal = copy_to_global_memory(&identity_, sizeof(identity_));
        }
        if (!medium->hGlobal) {
          return E_OUTOFMEMORY;
        }
        medium->tymed = TYMED_HGLOBAL;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override {
        return DATA_E_FORMATETC;
      }

      HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
        if (!format) {
          return E_POINTER;
        }
        if (format->dwAspect != DVASPECT_CONTENT) {
          return DV_E_DVASPECT;
        }
        if (format->cfFormat == descriptor_format_) {
          return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_TYMED;
        }
        if (format->cfFormat == contents_format_) {
          if ((format->tymed & TYMED_ISTREAM) == 0) {
            return DV_E_TYMED;
          }
          if (format->lindex < 0 ||
              static_cast<std::size_t>(format->lindex) >= files_.size() ||
              files_[format->lindex].type != LI_CLIPBOARD_FILE_TYPE_REGULAR) {
            return DV_E_LINDEX;
          }
          return S_OK;
        }
        if (format->cfFormat == preferred_drop_effect_format_ ||
            format->cfFormat == identity_format_) {
          return (format->tymed & TYMED_HGLOBAL) != 0 ? S_OK : DV_E_TYMED;
        }
        return DV_E_FORMATETC;
      }

      HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *output) override {
        if (!output) {
          return E_POINTER;
        }
        output->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
      }

      HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override {
        return E_NOTIMPL;
      }

      HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **enumerator) override {
        if (!enumerator) {
          return E_POINTER;
        }
        if (direction != DATADIR_GET) {
          *enumerator = nullptr;
          return E_NOTIMPL;
        }
        std::vector<FORMATETC> formats {
          {static_cast<CLIPFORMAT>(descriptor_format_), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
          {static_cast<CLIPFORMAT>(contents_format_), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
          {static_cast<CLIPFORMAT>(preferred_drop_effect_format_), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
          {static_cast<CLIPFORMAT>(identity_format_), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        *enumerator = new (std::nothrow) format_enumerator_t(std::move(formats));
        return *enumerator ? S_OK : E_OUTOFMEMORY;
      }

      HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override {
        async_mode_ = enabled != FALSE;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL *enabled) override {
        if (!enabled) {
          return E_POINTER;
        }
        *enabled = async_mode_ ? TRUE : FALSE;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override {
        in_operation_ = true;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE InOperation(BOOL *in_operation) override {
        if (!in_operation) {
          return E_POINTER;
        }
        *in_operation = in_operation_ ? TRUE : FALSE;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx *, DWORD) override {
        in_operation_ = false;
        return S_OK;
      }

    private:
      HRESULT get_descriptors(STGMEDIUM &medium) const {
        const auto size =
          offsetof(FILEGROUPDESCRIPTORW, fgd) + files_.size() * sizeof(FILEDESCRIPTORW);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (!memory) {
          return E_OUTOFMEMORY;
        }
        auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(memory));
        if (!group) {
          GlobalFree(memory);
          return E_OUTOFMEMORY;
        }
        group->cItems = static_cast<UINT>(files_.size());
        for (std::size_t index = 0; index < files_.size(); ++index) {
          const auto &file = files_[index];
          auto &descriptor = group->fgd[index];
          descriptor.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI | FD_UNICODE;
          descriptor.dwFileAttributes =
            file.type == LI_CLIPBOARD_FILE_TYPE_DIRECTORY ?
              FILE_ATTRIBUTE_DIRECTORY :
              FILE_ATTRIBUTE_NORMAL;
          if (file.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
            descriptor.dwFlags |= FD_FILESIZE;
            descriptor.nFileSizeHigh = static_cast<DWORD>(file.size >> 32);
            descriptor.nFileSizeLow = static_cast<DWORD>(file.size);
          }
          if (file.modified_time_ms != 0) {
            descriptor.dwFlags |= FD_WRITESTIME;
            descriptor.ftLastWriteTime = unix_time_to_file_time(file.modified_time_ms);
          }
          std::copy(file.path.begin(), file.path.end(), descriptor.cFileName);
          descriptor.cFileName[file.path.size()] = L'\0';
        }
        GlobalUnlock(memory);
        medium.tymed = TYMED_HGLOBAL;
        medium.hGlobal = memory;
        return S_OK;
      }

      std::atomic_ulong references_ {1};
      std::vector<file_entry_t> files_;
      std::string transfer_id_;
      std::uint64_t origin_id_;
      clipboard_identity_t identity_;
      UINT descriptor_format_;
      UINT contents_format_;
      UINT preferred_drop_effect_format_;
      UINT identity_format_;
      std::atomic_bool async_mode_ {true};
      std::atomic_bool in_operation_ {};
    };

    class ole_clipboard_broker_t {
    public:
      ole_clipboard_broker_t():
          wake_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          thread_([this]() {
            run();
          }) {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this]() {
          return initialized_;
        });
      }

      ~ole_clipboard_broker_t() {
        {
          std::lock_guard lock(mutex_);
          stopping_ = true;
        }
        SetEvent(wake_event_);
        if (thread_.joinable()) {
          thread_.join();
        }
        CloseHandle(wake_event_);
      }

      bool set(IDataObject *object) {
        if (!object || !ole_available_) {
          if (object) {
            object->Release();
          }
          return false;
        }
        auto job = std::make_shared<set_job_t>();
        job->object = object;
        auto result = job->result.get_future();
        {
          std::lock_guard lock(mutex_);
          jobs_.emplace_back(job);
          if (!SetEvent(wake_event_)) {
            jobs_.pop_back();
            object->Release();
            return false;
          }
        }
        return SUCCEEDED(result.get());
      }

      bool get_file_paths(std::vector<std::filesystem::path> &paths) {
        if (!ole_available_) {
          return false;
        }
        auto job = std::make_shared<get_file_paths_job_t>();
        auto result = job->result.get_future();
        {
          std::lock_guard lock(mutex_);
          jobs_.emplace_back(job);
          if (!SetEvent(wake_event_)) {
            jobs_.pop_back();
            return false;
          }
        }

        auto value = result.get();
        if (FAILED(value.status) || value.paths.empty()) {
          return false;
        }
        paths = std::move(value.paths);
        return true;
      }

    private:
      struct set_job_t {
        IDataObject *object {};
        std::promise<HRESULT> result;
      };

      struct get_file_paths_result_t {
        HRESULT status {};
        std::vector<std::filesystem::path> paths;
      };

      struct get_file_paths_job_t {
        std::promise<get_file_paths_result_t> result;
      };

      using job_t = std::variant<
        std::shared_ptr<set_job_t>,
        std::shared_ptr<get_file_paths_job_t>>;

      void run() {
        if (!wake_event_) {
          std::lock_guard lock(mutex_);
          initialized_ = true;
          ready_.notify_all();
          return;
        }

        const auto initialize_result = OleInitialize(nullptr);
        {
          std::lock_guard lock(mutex_);
          ole_available_ = SUCCEEDED(initialize_result);
          initialized_ = true;
        }
        ready_.notify_all();
        if (!ole_available_) {
          return;
        }

        for (;;) {
          const DWORD wait_result = MsgWaitForMultipleObjects(
            1,
            &wake_event_,
            FALSE,
            INFINITE,
            QS_ALLINPUT
          );
          if (wait_result == WAIT_OBJECT_0) {
            std::deque<job_t> jobs;
            bool stopping;
            {
              std::lock_guard lock(mutex_);
              jobs.swap(jobs_);
              stopping = stopping_;
              ResetEvent(wake_event_);
            }
            for (const auto &job : jobs) {
              std::visit(
                util::overloaded {
                  [](const std::shared_ptr<set_job_t> &set_job) {
                    const auto result = OleSetClipboard(set_job->object);
                    set_job->object->Release();
                    set_job->result.set_value(result);
                  },
                  [](const std::shared_ptr<get_file_paths_job_t> &get_job) {
                    get_file_paths_result_t result;
                    result.status = read_file_clipboard_paths(result.paths);
                    get_job->result.set_value(std::move(result));
                  },
                },
                job
              );
            }
            if (stopping) {
              break;
            }
          } else if (wait_result == WAIT_OBJECT_0 + 1) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
              TranslateMessage(&message);
              DispatchMessageW(&message);
            }
          } else {
            break;
          }
        }
        OleUninitialize();
      }

      HANDLE wake_event_;
      std::thread thread_;
      std::mutex mutex_;
      std::condition_variable ready_;
      std::deque<job_t> jobs_;
      bool initialized_ {};
      bool ole_available_ {};
      bool stopping_ {};
    };

    ole_clipboard_broker_t &clipboard_broker() {
      static ole_clipboard_broker_t broker;
      return broker;
    }
  }  // namespace

  bool get_file_clipboard_paths(std::vector<std::filesystem::path> &paths) {
    paths.clear();
    if (get_user_file_clipboard_paths(paths)) {
      return true;
    }
    return clipboard_broker().get_file_paths(paths);
  }

  bool set_virtual_file_clipboard(const std::vector<std::uint8_t> &manifest, const std::string &transfer_id, std::uint64_t origin_id, std::uint64_t item_id) {
    std::vector<file_entry_t> files;
    if (transfer_id.empty() ||
        origin_id == 0 ||
        item_id == 0 ||
        !decode_manifest(manifest, files)) {
      return false;
    }
    auto *object = new (std::nothrow) virtual_file_data_object_t(
      std::move(files),
      transfer_id,
      origin_id,
      item_id
    );
    if (!object) {
      return false;
    }
    if (!object->valid()) {
      object->Release();
      return false;
    }
    return clipboard_broker().set(object);
  }
}  // namespace platf::windows
