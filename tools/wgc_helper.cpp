/**
 * @file tools/wgc_helper.cpp
 * @brief User-session Windows.Graphics.Capture helper for service-hosted Sunshine.
 */
#define WIN32_LEAN_AND_MEAN

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
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

  struct diagnostic_log_t {
    handle_guard file;
    std::mutex mutex;

    void open(const wchar_t *path) {
      std::lock_guard lock {mutex};
      file.reset(CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    }

    void write(std::string_view message) {
      std::lock_guard lock {mutex};
      if (!file) {
        return;
      }

      SYSTEMTIME time {};
      GetLocalTime(&time);

      std::ostringstream line;
      line << '['
           << time.wYear << '-'
           << (time.wMonth < 10 ? "0" : "") << time.wMonth << '-'
           << (time.wDay < 10 ? "0" : "") << time.wDay << ' '
           << (time.wHour < 10 ? "0" : "") << time.wHour << ':'
           << (time.wMinute < 10 ? "0" : "") << time.wMinute << ':'
           << (time.wSecond < 10 ? "0" : "") << time.wSecond << '.'
           << time.wMilliseconds
           << "] " << message << "\r\n";

      const auto text = line.str();
      DWORD written = 0;
      WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
  };

  diagnostic_log_t g_log;

  void atomic_max(std::atomic<std::uint64_t> &target, std::uint64_t value) {
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
  }

  std::uint64_t qpc_now() {
    LARGE_INTEGER counter {};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
  }

  std::uint64_t qpc_delta_us(std::uint64_t start, std::uint64_t end) {
    static const auto frequency = []() {
      LARGE_INTEGER value {};
      QueryPerformanceFrequency(&value);
      return static_cast<std::uint64_t>(value.QuadPart);
    }();

    return frequency ? (end - start) * 1000000ull / frequency : 0;
  }

  std::uint64_t qpc_delta_us_checked(std::uint64_t start, std::uint64_t end) {
    if (!start || !end || end < start) {
      return 0;
    }
    return qpc_delta_us(start, end);
  }

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

  bool send_no_frame(HANDLE pipe, std::uint64_t request_id) {
    ipc::no_frame_message message {};
    message.header = ipc::make_header(ipc::message_type::no_frame, sizeof(message));
    message.request_id = request_id;
    return write_exact(pipe, &message, sizeof(message));
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
    winrt::event_token frame_arrived_token {};
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
      std::uint64_t sequence {};
      std::uint64_t sent_qpc {};
      bool ready {};
      bool writing {};
      bool in_flight {};

      void reset() {
        texture = nullptr;
        mutex = nullptr;
        shared_handle.reset();
        desc = {};
        qpc_timestamp = 0;
        sequence = 0;
        sent_qpc = 0;
        ready = false;
        writing = false;
        in_flight = false;
      }
    };

    std::array<frame_slot, 1> slots;
    int latest_slot = -1;
    std::uint64_t next_sequence = 1;
    std::uint64_t last_sent_sequence = 0;
    std::uint64_t last_capture_qpc = 0;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::mutex d3d_mutex;
    std::thread capture_thread;
    bool frame_pending {};
    bool frame_arrived_registered {};
    bool stopping {};
    bool fatal_error {};
    ipc::error_code fatal_code {};
    DWORD fatal_detail {};
    std::atomic<std::uint64_t> captured_frames {};
    std::atomic<std::uint64_t> empty_polls {};
    std::atomic<std::uint64_t> frame_requests {};
    std::atomic<std::uint64_t> frame_sends {};
    std::atomic<std::uint64_t> frame_releases {};
    std::atomic<std::uint64_t> no_frame_replies {};
    std::atomic<std::uint64_t> slot_waits {};
    std::atomic<std::uint64_t> request_wait_us {};
    std::atomic<std::uint64_t> request_wait_max_us {};
    std::atomic<std::uint64_t> frame_age_us {};
    std::atomic<std::uint64_t> frame_age_max_us {};
    std::atomic<std::uint64_t> capture_delta_max_us {};
    std::atomic<std::uint64_t> release_held_us {};
    std::atomic<std::uint64_t> release_held_max_us {};
    std::atomic<std::uint64_t> copy_us {};
    std::atomic<std::uint64_t> copy_max_us {};
    std::mutex stats_log_mutex;
    std::chrono::steady_clock::time_point last_stats_log {};

    ~wgc_session() {
      stop_capture_thread();
      if (capture_session) {
        capture_session.Close();
      }
      if (frame_pool) {
        if (frame_arrived_registered) {
          frame_pool.FrameArrived(frame_arrived_token);
          frame_arrived_registered = false;
        }
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
      {
        std::ostringstream line;
        line << "init display=";
        std::wstring display {message.display_name};
        line << std::string(display.begin(), display.end())
             << " format=" << static_cast<unsigned>(message.format)
             << " cursorVisible=" << message.cursor_visible;
        g_log.write(line.str());
      }

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
        frame_arrived_token = frame_pool.FrameArrived([this](auto const &, auto const &) {
          {
            std::lock_guard lock {frame_mutex};
            frame_pending = true;
          }
          frame_cv.notify_one();
        });
        frame_arrived_registered = true;
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

      g_log.write("capture started");
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

    void release_slot(const ipc::frame_release_message &release) {
      std::lock_guard lock {state_mutex};
      for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        auto &slot = slots[i];
        if (slot.in_flight && reinterpret_cast<std::uint64_t>(slot.shared_handle.get()) == release.shared_handle) {
          const auto release_received_qpc = qpc_now();
          const auto held_us = slot.sent_qpc ? qpc_delta_us(slot.sent_qpc, release_received_qpc) : 0;
          release_held_us.fetch_add(held_us, std::memory_order_relaxed);
          atomic_max(release_held_max_us, held_us);
          if (slot.sequence <= 120 || slot.sequence % 120 == 0 || held_us > 50000 || release.sequence != slot.sequence) {
            std::ostringstream line;
            line << "trace seq=" << slot.sequence
                 << " releaseSeq=" << release.sequence
                 << " slot=" << i
                 << " helperSendToMainRecvUs=" << qpc_delta_us_checked(slot.sent_qpc, release.main_received_qpc)
                 << " mainRecvToAcquireUs=" << qpc_delta_us_checked(release.main_received_qpc, release.main_acquired_qpc)
                 << " mainAcquireToReleaseUs=" << qpc_delta_us_checked(release.main_acquired_qpc, release.main_released_qpc)
                 << " mainReleaseToHelperRecvUs=" << qpc_delta_us_checked(release.main_released_qpc, release_received_qpc)
                 << " helperHeldUs=" << held_us
                 << " frameAgeAtSendUs=" << qpc_delta_us_checked(slot.qpc_timestamp, slot.sent_qpc)
                 << slot_summary_locked();
            g_log.write(line.str());
          }
          slot.in_flight = false;
          slot.sent_qpc = 0;
          frame_releases.fetch_add(1, std::memory_order_relaxed);
          state_cv.notify_all();
          return;
        }
      }
      std::ostringstream line;
      line << "release unknown seq=" << std::dec << release.sequence
           << " handle=0x" << std::hex << release.shared_handle << slot_summary_locked();
      g_log.write(line.str());
    }

    void set_fatal_error(ipc::error_code code, DWORD detail = 0) {
      std::lock_guard lock {state_mutex};
      if (!fatal_error) {
        fatal_error = true;
        fatal_code = code;
        fatal_detail = detail;
      }
      {
        std::ostringstream line;
        line << "fatal code=" << static_cast<unsigned>(code) << " detail=0x" << std::hex << detail;
        g_log.write(line.str());
      }
      {
        std::lock_guard frame_lock {frame_mutex};
        frame_pending = true;
      }
      frame_cv.notify_all();
      state_cv.notify_all();
    }

    void maybe_log_stats() {
      std::lock_guard lock {stats_log_mutex};
      const auto now = std::chrono::steady_clock::now();
      if (last_stats_log.time_since_epoch().count() != 0 && now - last_stats_log < 2s) {
        return;
      }
      last_stats_log = now;

      const auto requests = frame_requests.load(std::memory_order_relaxed);
      const auto sends = frame_sends.load(std::memory_order_relaxed);
      const auto copies = captured_frames.load(std::memory_order_relaxed);
      const auto releases = frame_releases.load(std::memory_order_relaxed);
      const auto wait_avg = requests ? request_wait_us.load(std::memory_order_relaxed) / requests : 0;
      const auto age_avg = sends ? frame_age_us.load(std::memory_order_relaxed) / sends : 0;
      const auto copy_avg = copies ? copy_us.load(std::memory_order_relaxed) / copies : 0;
      const auto release_avg = releases ? release_held_us.load(std::memory_order_relaxed) / releases : 0;

      std::ostringstream line;
      line << "stats captured=" << copies
           << " sent=" << sends
           << " requests=" << requests
           << " noFrame=" << no_frame_replies.load(std::memory_order_relaxed)
           << " releases=" << releases
           << " emptyPolls=" << empty_polls.exchange(0, std::memory_order_relaxed)
           << " slotWaits=" << slot_waits.load(std::memory_order_relaxed)
           << " waitAvgUs=" << wait_avg
           << " waitMaxUs=" << request_wait_max_us.exchange(0, std::memory_order_relaxed)
           << " ageAvgUs=" << age_avg
           << " ageMaxUs=" << frame_age_max_us.exchange(0, std::memory_order_relaxed)
           << " captureDeltaMaxUs=" << capture_delta_max_us.exchange(0, std::memory_order_relaxed)
           << " copyAvgUs=" << copy_avg
           << " copyMaxUs=" << copy_max_us.exchange(0, std::memory_order_relaxed)
           << " releaseHeldAvgUs=" << release_avg
           << " releaseHeldMaxUs=" << release_held_max_us.exchange(0, std::memory_order_relaxed);
      g_log.write(line.str());
    }

    std::string slot_summary_locked() const {
      std::ostringstream line;
      line << " latest=" << latest_slot;
      for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto &slot = slots[i];
        line << " s" << i
             << "{seq=" << slot.sequence
             << ",r=" << slot.ready
             << ",w=" << slot.writing
             << ",f=" << slot.in_flight
             << "}";
      }
      return line.str();
    }

    int select_writable_slot_locked() {
      const auto &slot = slots[0];
      return !slot.in_flight &&
                 !slot.writing &&
                 (!slot.texture || slot.mutex) ?
               0 :
               -1;
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

        {
          std::unique_lock lock {frame_mutex};
          if (!frame_pending) {
            frame_cv.wait(lock, [&]() {
              return frame_pending;
            });
          }
          frame_pending = false;
        }

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
          empty_polls.fetch_add(1, std::memory_order_relaxed);
          continue;
        }

        int slot_index = -1;
        {
          std::unique_lock lock {state_mutex};
          if (select_writable_slot_locked() < 0) {
            slot_waits.fetch_add(1, std::memory_order_relaxed);
            state_cv.wait(lock, [&]() {
              return stopping || fatal_error || select_writable_slot_locked() >= 0;
            });
          }
          if (stopping || fatal_error) {
            return;
          }
          slot_index = select_writable_slot_locked();
          if (slot_index < 0) {
            continue;
          }
          slots[slot_index].writing = true;
          slots[slot_index].ready = false;
        }

        auto access = newest_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> src;
        HRESULT surface_status = access ? access->GetInterface(IID_ID3D11Texture2D, src.put_void()) : E_NOINTERFACE;
        if (!access || FAILED(surface_status)) {
          {
            std::lock_guard lock {state_mutex};
            slots[slot_index].writing = false;
          }
          set_fatal_error(ipc::error_code::frame_surface, static_cast<DWORD>(surface_status));
          return;
        }

        D3D11_TEXTURE2D_DESC desc {};
        src->GetDesc(&desc);

        {
          std::lock_guard lock {state_mutex};
          if (stopping || fatal_error) {
            slots[slot_index].writing = false;
            return;
          }
        }

        bool ok = true;
        const auto copy_start = qpc_now();
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
        const auto copy_time_us = qpc_delta_us(copy_start, qpc_now());
        copy_us.fetch_add(copy_time_us, std::memory_order_relaxed);
        atomic_max(copy_max_us, copy_time_us);

        {
          std::lock_guard lock {state_mutex};
          slots[slot_index].writing = false;
          if (!ok) {
            fatal_error = true;
            fatal_code = ipc::error_code::shared_texture;
            fatal_detail = static_cast<DWORD>(last_shared_texture_status);
          } else {
            const auto frame_qpc = newest_frame.SystemRelativeTime().count();
            const auto capture_delta_us = last_capture_qpc ? qpc_delta_us(last_capture_qpc, frame_qpc) : 0;
            slots[slot_index].qpc_timestamp = newest_frame.SystemRelativeTime().count();
            slots[slot_index].sequence = next_sequence++;
            slots[slot_index].ready = true;
            latest_slot = slot_index;
            last_capture_qpc = frame_qpc;
            captured_frames.fetch_add(1, std::memory_order_relaxed);
            atomic_max(capture_delta_max_us, capture_delta_us);
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
      {
        std::lock_guard frame_lock {frame_mutex};
        frame_pending = true;
      }
      frame_cv.notify_all();
      state_cv.notify_all();
      if (capture_thread.joinable()) {
        capture_thread.join();
      }
    }

    bool send_requested_frame(HANDLE pipe, const ipc::frame_request_message &request) {
      frame_requests.fetch_add(1, std::memory_order_relaxed);
      const auto request_start = qpc_now();
      const auto timeout = std::chrono::milliseconds(request.timeout_ms);
      int slot_index = -1;
      ipc::frame_message message {};

      auto frame_available_locked = [&]() {
        return latest_slot >= 0 &&
               slots[latest_slot].ready &&
               !slots[latest_slot].writing &&
               !slots[latest_slot].in_flight &&
               slots[latest_slot].mutex &&
               slots[latest_slot].sequence > last_sent_sequence;
      };

      {
        std::unique_lock lock {state_mutex};
        if (fatal_error) {
          send_error(pipe, fatal_code, fatal_detail);
          return false;
        }
        if (slots[0].writing) {
          state_cv.wait(lock, [&]() {
            return stopping || fatal_error || !slots[0].writing;
          });
          if (fatal_error) {
            send_error(pipe, fatal_code, fatal_detail);
            return false;
          }
          if (stopping) {
            return false;
          }
        }
        if (!frame_available_locked()) {
          lock.unlock();
          no_frame_replies.fetch_add(1, std::memory_order_relaxed);
          const auto wait_time_us = qpc_delta_us(request_start, qpc_now());
          request_wait_us.fetch_add(wait_time_us, std::memory_order_relaxed);
          atomic_max(request_wait_max_us, wait_time_us);
          maybe_log_stats();
          return send_no_frame(pipe, request.request_id);
        }

        slot_index = latest_slot;
        auto &slot = slots[slot_index];
        if (!slot.mutex || !slot.shared_handle) {
          send_error(pipe, ipc::error_code::shared_texture, detail_code(0xB0, HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE)));
          return false;
        }

        const HRESULT acquire_status = slot.mutex->AcquireSync(0, 0);
        if (mutex_timeout(acquire_status)) {
          lock.unlock();
          no_frame_replies.fetch_add(1, std::memory_order_relaxed);
          const auto wait_time_us = qpc_delta_us(request_start, qpc_now());
          request_wait_us.fetch_add(wait_time_us, std::memory_order_relaxed);
          atomic_max(request_wait_max_us, wait_time_us);
          maybe_log_stats();
          return send_no_frame(pipe, request.request_id);
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
        last_sent_sequence = slot.sequence;
        slot.sent_qpc = qpc_now();

        message.header = ipc::make_header(ipc::message_type::frame, sizeof(message));
        message.shared_handle = reinterpret_cast<std::uint64_t>(slot.shared_handle.get());
        message.width = slot.desc.Width;
        message.height = slot.desc.Height;
        message.format = slot.desc.Format;
        message.qpc_timestamp = slot.qpc_timestamp;
        message.sequence = slot.sequence;
        message.helper_send_qpc = slot.sent_qpc;
        message.request_id = request.request_id;
      }
      const auto send_qpc = qpc_now();
      const auto wait_time_us = qpc_delta_us(request_start, send_qpc);
      const auto age_time_us = message.qpc_timestamp <= send_qpc ? qpc_delta_us(message.qpc_timestamp, send_qpc) : 0;
      request_wait_us.fetch_add(wait_time_us, std::memory_order_relaxed);
      atomic_max(request_wait_max_us, wait_time_us);
      frame_age_us.fetch_add(age_time_us, std::memory_order_relaxed);
      atomic_max(frame_age_max_us, age_time_us);
      frame_sends.fetch_add(1, std::memory_order_relaxed);
      if (wait_time_us > 20000 || age_time_us > 50000) {
        std::ostringstream line;
        line << "slow send waitUs=" << wait_time_us
             << " ageUs=" << age_time_us
             << " timeoutMs=" << timeout.count()
             << " slot=" << slot_index;
        g_log.write(line.str());
      }
      if (!write_exact(pipe, &message, sizeof(message))) {
        return false;
      }
      maybe_log_stats();

      return true;
    }
  };

  int run_helper(const wchar_t *pipe_name) {
    g_log.write("helper starting");
    handle_guard pipe {CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!pipe) {
      g_log.write("failed to open pipe");
      return static_cast<int>(GetLastError());
    }

    ipc::init_message init {};
    if (!read_exact(pipe.get(), &init, sizeof(init)) ||
        !ipc::valid_header(init.header, ipc::message_type::init, sizeof(init))) {
      g_log.write("invalid init message");
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
        g_log.write("shutdown requested");
        session.stop_capture_thread();
        return 0;
      }

      if (ipc::valid_header(header, ipc::message_type::frame_request, sizeof(ipc::frame_request_message))) {
        ipc::frame_request_message request {};
        request.header = header;
        if (!read_exact(pipe.get(), reinterpret_cast<std::uint8_t *>(&request) + sizeof(header), sizeof(request) - sizeof(header))) {
          session.stop_capture_thread();
          return ERROR_BROKEN_PIPE;
        }
        try {
          if (!session.send_requested_frame(pipe.get(), request)) {
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
        session.release_slot(release);
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

  const wchar_t *pipe_name = nullptr;
  const wchar_t *log_path = nullptr;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (wcscmp(argv[i], L"--pipe") == 0) {
      pipe_name = argv[i + 1];
    } else if (wcscmp(argv[i], L"--log") == 0) {
      log_path = argv[i + 1];
    } else {
      return ERROR_INVALID_PARAMETER;
    }
  }

  if (!pipe_name) {
    return ERROR_INVALID_PARAMETER;
  }
  if (log_path) {
    g_log.open(log_path);
  }

  return run_helper(pipe_name);
}
