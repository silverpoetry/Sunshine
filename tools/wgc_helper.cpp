/**
 * @file tools/wgc_helper.cpp
 * @brief User-session Windows.Graphics.Capture helper for service-hosted Sunshine.
 */
#define WIN32_LEAN_AND_MEAN

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <d3d11_4.h>
#include <dispatcherqueue.h>
#include <dxgi1_6.h>
#include <shellapi.h>
#include <Windows.h>
#include <sddl.h>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.capture.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

#include "src/platform/windows/wgc_helper_ipc.h"

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt

#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace {
  using namespace std::chrono_literals;
  namespace ipc = platf::dxgi::wgc_helper;

  template<class T>
  using com_ptr = winrt::com_ptr<T>;

  constexpr DWORD detail_code(DWORD stage, HRESULT status) {
    return (stage << 16) | (static_cast<DWORD>(status) & 0xFFFF);
  }

  bool mutex_timeout(HRESULT status) {
    return status == WAIT_TIMEOUT || status == HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }

  struct handle_guard {
    HANDLE handle {};

    handle_guard() = default;
    explicit handle_guard(HANDLE handle):
        handle {handle} {}
    ~handle_guard() {
      reset();
    }

    handle_guard(const handle_guard &) = delete;
    handle_guard &operator=(const handle_guard &) = delete;

    handle_guard(handle_guard &&other) noexcept:
        handle {other.handle} {
      other.handle = nullptr;
    }

    handle_guard &operator=(handle_guard &&other) noexcept {
      if (this != &other) {
        reset();
        handle = other.handle;
        other.handle = nullptr;
      }
      return *this;
    }

    void reset(HANDLE new_handle = nullptr) {
      if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
      handle = new_handle;
    }

    HANDLE get() const {
      return handle;
    }

    explicit operator bool() const {
      return handle && handle != INVALID_HANDLE_VALUE;
    }
  };

  bool read_exact(HANDLE pipe, void *data, DWORD size) {
    auto bytes = static_cast<std::uint8_t *>(data);
    DWORD total = 0;
    while (total < size) {
      DWORD chunk = 0;
      if (!ReadFile(pipe, bytes + total, size - total, &chunk, nullptr) || !chunk) {
        return false;
      }
      total += chunk;
    }
    return true;
  }

  bool write_exact(HANDLE pipe, const void *data, DWORD size) {
    const auto bytes = static_cast<const std::uint8_t *>(data);
    DWORD total = 0;
    while (total < size) {
      DWORD chunk = 0;
      if (!WriteFile(pipe, bytes + total, size - total, &chunk, nullptr)) {
        return false;
      }
      total += chunk;
    }
    return true;
  }

  int send_error(HANDLE pipe, ipc::error_code code, DWORD detail = 0) {
    ipc::error_message message {};
    message.header = ipc::make_header(ipc::message_type::error, sizeof(message));
    message.code = static_cast<std::uint32_t>(code);
    message.detail = detail;
    write_exact(pipe, &message, sizeof(message));
    return static_cast<int>(code);
  }

  bool make_device(com_ptr<ID3D11Device> &device, com_ptr<ID3D11DeviceContext> &context) {
    D3D_FEATURE_LEVEL feature_levels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL feature_level {};
    return SUCCEEDED(D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      feature_levels,
      std::size(feature_levels),
      D3D11_SDK_VERSION,
      device.put(),
      &feature_level,
      context.put()
    ));
  }

  HMONITOR find_monitor(const wchar_t *display_name) {
    com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, factory.put_void()))) {
      return nullptr;
    }

    com_ptr<IDXGIAdapter1> adapter;
    for (UINT adapter_index = 0; factory->EnumAdapters1(adapter_index, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++adapter_index) {
      com_ptr<IDXGIOutput> output;
      for (UINT output_index = 0; adapter->EnumOutputs(output_index, output.put()) != DXGI_ERROR_NOT_FOUND; ++output_index) {
        DXGI_OUTPUT_DESC desc {};
        if (SUCCEEDED(output->GetDesc(&desc)) && wcscmp(desc.DeviceName, display_name) == 0) {
          return desc.Monitor;
        }
        output = nullptr;
      }
      adapter = nullptr;
    }

    return nullptr;
  }

  struct wgc_session {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D11Device1> device1;
    winrt::IDirect3DDevice uwp_device {nullptr};
    winrt::GraphicsCaptureItem item {nullptr};
    winrt::Direct3D11CaptureFramePool frame_pool {nullptr};
    winrt::GraphicsCaptureSession capture_session {nullptr};
    winrt::com_ptr<::ABI::Windows::System::IDispatcherQueueController> dispatcher_queue_controller;
    HMODULE coremessaging_module {};
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT last_shared_texture_status = S_OK;

    struct frame_slot {
      com_ptr<ID3D11Texture2D> texture;
      com_ptr<IDXGIKeyedMutex> mutex;
      handle_guard shared_handle;
      D3D11_TEXTURE2D_DESC desc {};
      std::uint64_t qpc_timestamp {};
      bool ready {};
      bool writing {};
      bool in_flight {};

      void reset() {
        texture = nullptr;
        mutex = nullptr;
        shared_handle.reset();
        desc = {};
        qpc_timestamp = 0;
        ready = false;
        writing = false;
        in_flight = false;
      }
    };

    std::array<frame_slot, 2> slots;
    int latest_slot = -1;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::mutex d3d_mutex;
    std::thread capture_thread;
    bool stopping {};
    bool fatal_error {};
    ipc::error_code fatal_code {};
    DWORD fatal_detail {};

    ~wgc_session() {
      stop_capture_thread();
      if (capture_session) {
        capture_session.Close();
      }
      if (frame_pool) {
        frame_pool.Close();
      }
      if (dispatcher_queue_controller) {
        dispatcher_queue_controller->ShutdownQueueAsync(nullptr);
      }
      if (coremessaging_module) {
        FreeLibrary(coremessaging_module);
      }
      for (auto &slot : slots) {
        slot.reset();
      }
    }

    bool init_dispatcher() {
      MSG msg;
      PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

      DispatcherQueueOptions options {};
      options.dwSize = sizeof(options);
      options.threadType = DQTYPE_THREAD_CURRENT;
      options.apartmentType = DQTAT_COM_NONE;

      coremessaging_module = LoadLibraryW(L"coremessaging.dll");
      if (!coremessaging_module) {
        return false;
      }

      auto create_dispatcher_queue_controller = reinterpret_cast<decltype(&CreateDispatcherQueueController)>(GetProcAddress(coremessaging_module, "CreateDispatcherQueueController"));
      return create_dispatcher_queue_controller &&
             SUCCEEDED(create_dispatcher_queue_controller(options, dispatcher_queue_controller.put()));
    }

    ipc::error_code init(const ipc::init_message &message) {
      try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
      } catch (...) {
      }

      if (!init_dispatcher() || !make_device(device, context)) {
        return !coremessaging_module ? ipc::error_code::dispatcher : ipc::error_code::d3d_device;
      }
      if (FAILED(device->QueryInterface(IID_ID3D11Device1, device1.put_void()))) {
        return ipc::error_code::d3d_device;
      }

      com_ptr<IDXGIDevice> dxgi_device;
      winrt::com_ptr<::IInspectable> d3d_comhandle;
      if (FAILED(device->QueryInterface(IID_IDXGIDevice, dxgi_device.put_void())) ||
          FAILED(winrt::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), d3d_comhandle.put()))) {
        return ipc::error_code::dxgi_device;
      }
      uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();

      HMONITOR monitor = find_monitor(message.display_name);
      if (!monitor || !winrt::GraphicsCaptureSession::IsSupported()) {
        return monitor ? ipc::error_code::capture_unsupported : ipc::error_code::monitor_not_found;
      }

      auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      if (!monitor_factory ||
          FAILED(monitor_factory->CreateForMonitor(monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
        return ipc::error_code::capture_item;
      }

      format = message.format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : message.format;
      try {
        frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
          uwp_device,
          static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format),
          2,
          item.Size()
        );
        capture_session = frame_pool.CreateCaptureSession(item);
        if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
          capture_session.IsBorderRequired(false);
        }
        if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
          capture_session.MinUpdateInterval(4ms);
        }
        capture_session.IsCursorCaptureEnabled(message.cursor_visible);
      } catch (...) {
        return ipc::error_code::frame_pool;
      }

      try {
        capture_session.StartCapture();
      } catch (...) {
        return ipc::error_code::start_capture;
      }

      return ipc::error_code {};
    }

    bool set_cursor_visible(bool visible) {
      try {
        capture_session.IsCursorCaptureEnabled(visible);
        return true;
      } catch (...) {
        return false;
      }
    }

    bool ensure_slot_texture(frame_slot &slot, const D3D11_TEXTURE2D_DESC &src_desc) {
      last_shared_texture_status = S_OK;
      if (slot.texture) {
        if (slot.desc.Width == src_desc.Width &&
            slot.desc.Height == src_desc.Height &&
            slot.desc.Format == src_desc.Format) {
          return true;
        }
        slot.reset();
      }

      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = src_desc.Width;
      desc.Height = src_desc.Height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = src_desc.Format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

      HRESULT status = device->CreateTexture2D(&desc, nullptr, slot.texture.put());
      if (FAILED(status)) {
        last_shared_texture_status = detail_code(0xC1, status);
        return false;
      }

      com_ptr<IDXGIResource1> resource;
      status = slot.texture->QueryInterface(IID_IDXGIResource1, resource.put_void());
      if (FAILED(status)) {
        last_shared_texture_status = detail_code(0xC2, status);
        slot.reset();
        return false;
      }

      HANDLE raw_handle {};
      status = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &raw_handle);
      if (FAILED(status)) {
        last_shared_texture_status = detail_code(0xC3, status);
        slot.reset();
        return false;
      }
      slot.shared_handle.reset(raw_handle);
      status = slot.texture->QueryInterface(IID_IDXGIKeyedMutex, slot.mutex.put_void());
      if (FAILED(status)) {
        last_shared_texture_status = detail_code(0xC4, status);
        slot.reset();
        return false;
      }

      slot.desc = desc;
      return true;
    }

    void release_slot(std::uint64_t shared_handle) {
      std::lock_guard lock {state_mutex};
      for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        auto &slot = slots[i];
        if (slot.in_flight && reinterpret_cast<std::uint64_t>(slot.shared_handle.get()) == shared_handle) {
          slot.in_flight = false;
          state_cv.notify_all();
          return;
        }
      }
    }

    void set_fatal_error(ipc::error_code code, DWORD detail = 0) {
      std::lock_guard lock {state_mutex};
      if (!fatal_error) {
        fatal_error = true;
        fatal_code = code;
        fatal_detail = detail;
      }
      state_cv.notify_all();
    }

    int select_writable_slot_locked() {
      if (latest_slot >= 0) {
        const int other = 1 - latest_slot;
        if (!slots[other].in_flight &&
            !slots[other].writing &&
            (!slots[other].texture || slots[other].mutex)) {
          return other;
        }
        return -1;
      }

      for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        if (!slots[i].in_flight && !slots[i].writing && (!slots[i].texture || slots[i].mutex)) {
          return i;
        }
      }

      return -1;
    }

    void capture_loop() {
      try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
      } catch (winrt::hresult_error &e) {
        set_fatal_error(ipc::error_code::winrt_init, static_cast<DWORD>(e.code()));
        return;
      } catch (...) {
        set_fatal_error(ipc::error_code::winrt_init);
        return;
      }

      while (true) {
        {
          std::lock_guard lock {state_mutex};
          if (stopping || fatal_error) {
            return;
          }
        }

        winrt::Direct3D11CaptureFrame newest_frame {nullptr};
        try {
          while (auto frame = frame_pool.TryGetNextFrame()) {
            newest_frame = frame;
          }
        } catch (winrt::hresult_error &e) {
          set_fatal_error(ipc::error_code::generic, static_cast<DWORD>(e.code()));
          return;
        } catch (...) {
          set_fatal_error(ipc::error_code::generic);
          return;
        }

        if (!newest_frame) {
          Sleep(0);
          continue;
        }

        auto access = newest_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> src;
        HRESULT surface_status = access ? access->GetInterface(IID_ID3D11Texture2D, src.put_void()) : E_NOINTERFACE;
        if (!access || FAILED(surface_status)) {
          set_fatal_error(ipc::error_code::frame_surface, static_cast<DWORD>(surface_status));
          return;
        }

        D3D11_TEXTURE2D_DESC desc {};
        src->GetDesc(&desc);

        int slot_index = -1;
        {
          std::unique_lock lock {state_mutex};
          slot_index = select_writable_slot_locked();
          if (slot_index < 0) {
            state_cv.wait(lock, [&]() {
              if (stopping || fatal_error) {
                return true;
              }
              slot_index = select_writable_slot_locked();
              return slot_index >= 0;
            });
          }
          if (stopping || fatal_error) {
            return;
          }
          slots[slot_index].writing = true;
          slots[slot_index].ready = false;
        }

        bool ok = true;
        {
          std::lock_guard d3d_lock {d3d_mutex};
          ok = ensure_slot_texture(slots[slot_index], desc);
          if (ok) {
            const HRESULT acquire_status = slots[slot_index].mutex->AcquireSync(0, 1000);
            if (FAILED(acquire_status)) {
              last_shared_texture_status = detail_code(0xA1, acquire_status);
              ok = false;
            }
          }
          if (ok) {
            context->CopyResource(slots[slot_index].texture.get(), src.get());
            context->Flush();
            const HRESULT release_status = slots[slot_index].mutex->ReleaseSync(0);
            if (FAILED(release_status)) {
              last_shared_texture_status = detail_code(0xA2, release_status);
              ok = false;
            }
          }
        }

        {
          std::lock_guard lock {state_mutex};
          slots[slot_index].writing = false;
          if (!ok) {
            fatal_error = true;
            fatal_code = ipc::error_code::shared_texture;
            fatal_detail = static_cast<DWORD>(last_shared_texture_status);
          } else {
            slots[slot_index].qpc_timestamp = newest_frame.SystemRelativeTime().count();
            slots[slot_index].ready = true;
            latest_slot = slot_index;
          }
        }
        state_cv.notify_all();
      }
    }

    void start_capture_thread() {
      capture_thread = std::thread([this]() {
        capture_loop();
      });
    }

    void stop_capture_thread() {
      {
        std::lock_guard lock {state_mutex};
        stopping = true;
      }
      state_cv.notify_all();
      if (capture_thread.joinable()) {
        capture_thread.join();
      }
    }

    bool send_requested_frame(HANDLE pipe) {
      int slot_index = -1;
      ipc::frame_message message {};

      {
        std::unique_lock lock {state_mutex};
        if (fatal_error) {
          send_error(pipe, fatal_code, fatal_detail);
          return false;
        }

        if (latest_slot < 0 ||
            !slots[latest_slot].ready ||
            slots[latest_slot].writing ||
            slots[latest_slot].in_flight ||
            !slots[latest_slot].mutex) {
          state_cv.wait_for(lock, 2ms, [&]() {
            return fatal_error ||
                   (latest_slot >= 0 &&
                    slots[latest_slot].ready &&
                    !slots[latest_slot].writing &&
                    !slots[latest_slot].in_flight &&
                    slots[latest_slot].mutex);
          });
        }

        if (fatal_error) {
          send_error(pipe, fatal_code, fatal_detail);
          return false;
        }
        if (latest_slot < 0 ||
            !slots[latest_slot].ready ||
            slots[latest_slot].writing ||
            slots[latest_slot].in_flight ||
            !slots[latest_slot].mutex) {
          return true;
        }

        slot_index = latest_slot;
        auto &slot = slots[slot_index];
        if (!slot.mutex || !slot.shared_handle) {
          send_error(pipe, ipc::error_code::shared_texture, detail_code(0xB0, HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)));
          return false;
        }

        const HRESULT acquire_status = slot.mutex->AcquireSync(0, 0);
        if (mutex_timeout(acquire_status)) {
          return true;
        }
        if (FAILED(acquire_status)) {
          send_error(pipe, ipc::error_code::shared_texture, detail_code(0xB1, acquire_status));
          return false;
        }

        const HRESULT release_status = slot.mutex->ReleaseSync(1);
        if (FAILED(release_status)) {
          send_error(pipe, ipc::error_code::shared_texture, detail_code(0xB2, release_status));
          return false;
        }
        slot.in_flight = true;

        message.header = ipc::make_header(ipc::message_type::frame, sizeof(message));
        message.shared_handle = reinterpret_cast<std::uint64_t>(slot.shared_handle.get());
        message.width = slot.desc.Width;
        message.height = slot.desc.Height;
        message.format = slot.desc.Format;
        message.qpc_timestamp = slot.qpc_timestamp;
      }
      if (!write_exact(pipe, &message, sizeof(message))) {
        return false;
      }

      return true;
    }
  };

  int run_helper(const wchar_t *pipe_name) {
    handle_guard pipe {CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!pipe) {
      return static_cast<int>(GetLastError());
    }

    ipc::init_message init {};
    if (!read_exact(pipe.get(), &init, sizeof(init)) ||
        !ipc::valid_header(init.header, ipc::message_type::init, sizeof(init))) {
      return ERROR_INVALID_DATA;
    }

    wgc_session session;
    if (auto error = session.init(init); error != ipc::error_code {}) {
      return send_error(pipe.get(), error);
    }
    session.start_capture_thread();

    while (true) {
      ipc::message_header header {};
      if (!read_exact(pipe.get(), &header, sizeof(header))) {
        session.stop_capture_thread();
        return ERROR_BROKEN_PIPE;
      }

      if (ipc::valid_header(header, ipc::message_type::shutdown, sizeof(header))) {
        session.stop_capture_thread();
        return 0;
      }

      if (ipc::valid_header(header, ipc::message_type::frame_request, sizeof(header))) {
        try {
          if (!session.send_requested_frame(pipe.get())) {
            Sleep(10);
            session.stop_capture_thread();
            return static_cast<int>(ipc::error_code::generic);
          }
        } catch (...) {
          session.stop_capture_thread();
          return send_error(pipe.get(), ipc::error_code::generic);
        }
      } else if (header.type == ipc::message_type::cursor && header.size == sizeof(ipc::cursor_message)) {
        ipc::cursor_message cursor {};
        cursor.header = header;
        if (!read_exact(pipe.get(), reinterpret_cast<std::uint8_t *>(&cursor) + sizeof(header), sizeof(cursor) - sizeof(header))) {
          session.stop_capture_thread();
          return ERROR_BROKEN_PIPE;
        }
        session.set_cursor_visible(cursor.cursor_visible);
      } else if (header.type == ipc::message_type::frame_release && header.size == sizeof(ipc::frame_release_message)) {
        ipc::frame_release_message release {};
        release.header = header;
        if (!read_exact(pipe.get(), reinterpret_cast<std::uint8_t *>(&release) + sizeof(header), sizeof(release) - sizeof(header))) {
          session.stop_capture_thread();
          return ERROR_BROKEN_PIPE;
        }
        session.release_slot(release.shared_handle);
      } else {
        session.stop_capture_thread();
        return ERROR_INVALID_DATA;
      }
    }
  }
}  // namespace

int main() {
  int argc = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return ERROR_INVALID_PARAMETER;
  }
  struct argv_guard_t {
    LPWSTR *argv;
    ~argv_guard_t() {
      LocalFree(argv);
    }
  } argv_guard {argv};

  if (argc != 3 || wcscmp(argv[1], L"--pipe") != 0) {
    return ERROR_INVALID_PARAMETER;
  }

  return run_helper(argv[2]);
}
