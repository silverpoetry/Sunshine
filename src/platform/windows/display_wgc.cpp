/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// standard includes
#include <algorithm>
#include <filesystem>
#include <thread>
#include <vector>

// platform includes
#include <dxgi1_2.h>
#include <sddl.h>
#include <userenv.h>
#include <WtsApi32.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"
#include "wgc_helper_ipc.h"

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
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
  // compare with __declspec(uuid(...)) for the struct above.
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  namespace {
    namespace helper_ipc = wgc_helper;

    bool is_local_system_process() {
      HANDLE token {};
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
      }
      auto token_guard = util::fail_guard([&]() {
        CloseHandle(token);
      });

      DWORD size = 0;
      GetTokenInformation(token, TokenUser, nullptr, 0, &size);
      std::vector<std::uint8_t> buffer(size);
      if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        return false;
      }

      PSID system_sid {};
      if (!ConvertStringSidToSidW(L"S-1-5-18", &system_sid)) {
        return false;
      }
      auto sid_guard = util::fail_guard([&]() {
        LocalFree(system_sid);
      });

      auto token_user = reinterpret_cast<TOKEN_USER *>(buffer.data());
      return EqualSid(token_user->User.Sid, system_sid);
    }

    bool pipe_io(HANDLE pipe, void *data, DWORD size, bool write) {
      auto bytes = static_cast<std::uint8_t *>(data);
      DWORD total = 0;
      while (total < size) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
          return false;
        }
        auto event_guard = util::fail_guard([&]() {
          CloseHandle(overlapped.hEvent);
        });

        DWORD chunk = 0;
        BOOL ok;
        if (write) {
          ok = WriteFile(pipe, bytes + total, size - total, &chunk, &overlapped);
        } else {
          ok = ReadFile(pipe, bytes + total, size - total, &chunk, &overlapped);
        }

        if (!ok) {
          const auto error = GetLastError();
          if (error != ERROR_IO_PENDING) {
            return false;
          }
          if (WaitForSingleObject(overlapped.hEvent, INFINITE) != WAIT_OBJECT_0 ||
              !GetOverlappedResult(pipe, &overlapped, &chunk, FALSE)) {
            return false;
          }
        }

        if (!chunk) {
          return false;
        }
        total += chunk;
      }
      return true;
    }

    bool read_exact(HANDLE pipe, void *data, DWORD size) {
      return pipe_io(pipe, data, size, false);
    }

    bool write_exact(HANDLE pipe, const void *data, DWORD size) {
      return pipe_io(pipe, const_cast<void *>(data), size, true);
    }

    enum class pipe_read_e {
      ok,
      timeout,
      error,
    };

    pipe_read_e read_exact_timeout(HANDLE pipe, void *data, DWORD size, std::chrono::milliseconds timeout) {
      auto bytes = static_cast<std::uint8_t *>(data);
      DWORD total = 0;
      const auto deadline = std::chrono::steady_clock::now() + timeout;

      while (total < size) {
        OVERLAPPED overlapped {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
          return pipe_read_e::error;
        }
        auto event_guard = util::fail_guard([&]() {
          CloseHandle(overlapped.hEvent);
        });

        DWORD chunk = 0;
        if (!ReadFile(pipe, bytes + total, size - total, &chunk, &overlapped)) {
          const auto error = GetLastError();
          if (error != ERROR_IO_PENDING) {
            return pipe_read_e::error;
          }

          const auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            CancelIo(pipe);
            return pipe_read_e::timeout;
          }

          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
          if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(remaining.count())) != WAIT_OBJECT_0) {
            CancelIo(pipe);
            return pipe_read_e::timeout;
          }
          if (!GetOverlappedResult(pipe, &overlapped, &chunk, FALSE)) {
            return pipe_read_e::error;
          }
        }

        if (!chunk) {
          return pipe_read_e::error;
        }
        total += chunk;
      }

      return pipe_read_e::ok;
    }

    std::wstring helper_path() {
      wchar_t module_path[MAX_PATH];
      GetModuleFileNameW(nullptr, module_path, _countof(module_path));
      std::wstring path {module_path};
      auto slash = path.find_last_of(L"\\/");
      if (slash == std::wstring::npos) {
        return L"tools\\sunshine-wgc-helper.exe";
      }
      path.resize(slash + 1);
      path += L"tools\\sunshine-wgc-helper.exe";
      return path;
    }

    std::wstring helper_log_path() {
      wchar_t module_path[MAX_PATH];
      GetModuleFileNameW(nullptr, module_path, _countof(module_path));
      std::filesystem::path path {module_path};
      path = path.parent_path() / L"config" / L"wgc-helper.log";
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      return path.wstring();
    }

    bool connect_pipe_with_timeout(HANDLE pipe, std::chrono::milliseconds timeout) {
      OVERLAPPED overlapped {};
      overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!overlapped.hEvent) {
        return false;
      }
      auto event_guard = util::fail_guard([&]() {
        CloseHandle(overlapped.hEvent);
      });

      if (ConnectNamedPipe(pipe, &overlapped)) {
        return true;
      }

      const auto error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) {
        return true;
      }
      if (error != ERROR_IO_PENDING) {
        return false;
      }

      if (WaitForSingleObject(overlapped.hEvent, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        CancelIo(pipe);
        return false;
      }

      DWORD transferred = 0;
      return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
    }

    std::string helper_exit_status(HANDLE process) {
      if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        return "still running"s;
      }

      DWORD exit_code = 0;
      if (!GetExitCodeProcess(process, &exit_code)) {
        return "unknown"s;
      }

      return "exited 0x"s + util::hex(exit_code).to_string();
    }

    HANDLE query_console_user_token() {
      const auto session_id = WTSGetActiveConsoleSessionId();
      if (session_id == 0xFFFFFFFF) {
        return nullptr;
      }

      HANDLE token {};
      if (!WTSQueryUserToken(session_id, &token)) {
        return nullptr;
      }
      return token;
    }
  }  // namespace

  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    stop_helper();
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    if (dispatcher_queue_controller) {
      dispatcher_queue_controller->ShutdownQueueAsync(nullptr);
      dispatcher_queue_controller = nullptr;
    }
    if (coremessaging_module) {
      FreeLibrary(coremessaging_module);
      coremessaging_module = nullptr;
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    const bool service_context = is_local_system_process();
    BOOST_LOG(warning) << "Initializing WGC capture [service_context=" << service_context << ']';
    if (service_context) {
      if (init_helper(display, config) == 0) {
        helper_active = true;
        BOOST_LOG(warning) << "Using user-session WGC helper for service-hosted capture"sv;
        return 0;
      }
      BOOST_LOG(warning) << "WGC helper initialization failed; trying in-process WGC capture"sv;
    }

    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;

    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(debug) << "WGC: WinRT apartment initialization returned [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }

    MSG msg;
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    DispatcherQueueOptions options {};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_NONE;

    coremessaging_module = LoadLibraryW(L"coremessaging.dll");
    if (!coremessaging_module) {
      BOOST_LOG(error) << "Failed to load coremessaging.dll for WGC dispatcher queue";
      return -1;
    }

    auto create_dispatcher_queue_controller = reinterpret_cast<decltype(&CreateDispatcherQueueController)>(GetProcAddress(coremessaging_module, "CreateDispatcherQueueController"));
    if (!create_dispatcher_queue_controller) {
      BOOST_LOG(error) << "Failed to find CreateDispatcherQueueController for WGC dispatcher queue";
      return -1;
    }

    status = create_dispatcher_queue_controller(options, dispatcher_queue_controller.put());
    if (FAILED(status)) {
      BOOST_LOG(error) << "Failed to create WGC dispatcher queue [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr ||
        FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(4ms);  // 250Hz
      } else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may be capped to 60fps on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    BOOST_LOG(warning) << "Using in-process WGC capture"sv;
    return 0;
  }

  int wgc_capture_t::init_helper(display_base_t *display, const ::video::config_t &config) {
    auto pipe_name = std::wstring {L"\\\\.\\pipe\\SunshineWgcHelper-"} + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    PSECURITY_DESCRIPTOR pipe_sd {};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GA;;;AU)",
          SDDL_REVISION_1,
          &pipe_sd,
          nullptr
        )) {
      BOOST_LOG(warning) << "Failed to create WGC helper pipe security descriptor [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
      return -1;
    }
    auto pipe_sd_guard = util::fail_guard([&]() {
      LocalFree(pipe_sd);
    });

    SECURITY_ATTRIBUTES pipe_security {};
    pipe_security.nLength = sizeof(pipe_security);
    pipe_security.lpSecurityDescriptor = pipe_sd;

    helper_pipe = CreateNamedPipeW(
      pipe_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1,
      1024 * 1024,
      1024 * 1024,
      0,
      &pipe_security
    );
    if (!helper_pipe || helper_pipe == INVALID_HANDLE_VALUE) {
      helper_pipe = nullptr;
      return -1;
    }

    auto token = query_console_user_token();
    if (!token) {
      stop_helper();
      return -1;
    }
    auto token_guard = util::fail_guard([&]() {
      CloseHandle(token);
    });

    PVOID environment_block {};
    if (!CreateEnvironmentBlock(&environment_block, token, FALSE)) {
      stop_helper();
      return -1;
    }
    auto env_guard = util::fail_guard([&]() {
      DestroyEnvironmentBlock(environment_block);
    });

    auto exe = helper_path();
    auto log_path = helper_log_path();
    auto command = L"\""s + exe + L"\" --pipe \"" + pipe_name + L"\" --log \"" + log_path + L"\"";

    STARTUPINFOW startup_info {};
    startup_info.cb = sizeof(startup_info);
    startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

    PROCESS_INFORMATION process_info {};
    if (!CreateProcessAsUserW(
          token,
          exe.c_str(),
          command.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
          environment_block,
          nullptr,
          &startup_info,
          &process_info
        )) {
      BOOST_LOG(warning) << "Failed to launch WGC helper [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
      stop_helper();
      return -1;
    }

    helper_process = process_info.hProcess;
    helper_thread = process_info.hThread;

    if (!connect_pipe_with_timeout(helper_pipe, 5s)) {
      BOOST_LOG(warning) << "WGC helper did not connect to the pipe in time; helper is "sv << helper_exit_status(helper_process);
      stop_helper();
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc {};
    display->output->GetDesc(&output_desc);

    helper_ipc::init_message message {};
    message.header = helper_ipc::make_header(helper_ipc::message_type::init, sizeof(message));
    wcsncpy_s(message.display_name, output_desc.DeviceName, _TRUNCATE);
    message.format = config.dynamicRange ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    message.cursor_visible = !config.nativeCursor;
    helper_cursor_visible = message.cursor_visible;

    if (!write_exact(helper_pipe, &message, sizeof(message))) {
      BOOST_LOG(warning) << "Failed to initialize WGC helper pipe"sv;
      stop_helper();
      return -1;
    }

    display->capture_format = message.format;
    helper_device = display->device.get();
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    if (helper_active) {
      release_frame();
      return next_helper_frame(timeout, out, out_time);
    }

    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (helper_active) {
      if (helper_mutex && helper_frame_locked) {
        const auto locked_handle = helper_remote_texture_handle;
        if (FAILED(helper_mutex->ReleaseSync(0))) {
          helper_frame_locked = false;
          return capture_e::error;
        }
        helper_frame_locked = false;
        if (helper_pipe && locked_handle) {
          helper_ipc::frame_release_message message {};
          message.header = helper_ipc::make_header(helper_ipc::message_type::frame_release, sizeof(message));
          message.shared_handle = reinterpret_cast<std::uint64_t>(locked_handle);
          message.sequence = helper_frame_sequence;
          message.helper_send_qpc = helper_frame_send_qpc;
          message.main_received_qpc = helper_frame_received_qpc;
          message.main_acquired_qpc = helper_frame_acquired_qpc;
          message.main_released_qpc = static_cast<std::uint64_t>(qpc_counter());
          if (!write_exact(helper_pipe, &message, sizeof(message))) {
            return capture_e::error;
          }
        }
      }
      return capture_e::ok;
    }

    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  bool wgc_capture_t::is_helper_active() const {
    return helper_active;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    if (helper_active) {
      if (helper_cursor_visible == x || !helper_pipe) {
        return 0;
      }

      helper_ipc::cursor_message message {};
      message.header = helper_ipc::make_header(helper_ipc::message_type::cursor, sizeof(message));
      message.cursor_visible = x;
      if (!write_exact(helper_pipe, &message, sizeof(message))) {
        return -1;
      }
      helper_cursor_visible = x;
      return 0;
    }

    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  capture_e wgc_capture_t::next_helper_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    if (WaitForSingleObject(helper_process, 0) == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      GetExitCodeProcess(helper_process, &exit_code);
      BOOST_LOG(warning) << "WGC helper exited [0x"sv << util::hex(exit_code).to_string_view() << ']';
      return capture_e::error;
    }

    helper_ipc::frame_request_message request {};
    request.header = helper_ipc::make_header(helper_ipc::message_type::frame_request, sizeof(request));
    // The helper owns a mailbox of already-captured frames. Requests must be
    // non-blocking so frame release messages are never stuck behind a wait.
    request.timeout_ms = 0;
    request.request_id = ++helper_next_request_id;
    if (!write_exact(helper_pipe, &request, sizeof(request))) {
      return capture_e::error;
    }

    auto wait_time = 50ms;

    auto ensure_helper_texture = [&](const helper_ipc::frame_message &message) -> bool {
      if (helper_remote_texture_handle == reinterpret_cast<HANDLE>(message.shared_handle)) {
        return true;
      }
      helper_texture.reset();
      helper_mutex.reset();
      helper_remote_texture_handle = nullptr;

      HANDLE duplicated_handle {};
      if (!helper_process ||
          !DuplicateHandle(
            helper_process,
            reinterpret_cast<HANDLE>(message.shared_handle),
            GetCurrentProcess(),
            &duplicated_handle,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
          )) {
        return false;
      }
      helper_remote_texture_handle = reinterpret_cast<HANDLE>(message.shared_handle);

      winrt::com_ptr<ID3D11Device1> device1;
      winrt::com_ptr<ID3D11Resource> resource;
      if (!helper_device ||
          FAILED(helper_device->QueryInterface(IID_ID3D11Device1, device1.put_void())) ||
          FAILED(device1->OpenSharedResource1(duplicated_handle, IID_ID3D11Resource, resource.put_void())) ||
          FAILED(resource->QueryInterface(IID_ID3D11Texture2D, (void **) &helper_texture))) {
        CloseHandle(duplicated_handle);
        return false;
      }
      CloseHandle(duplicated_handle);
      if (FAILED(helper_texture->QueryInterface(IID_IDXGIKeyedMutex, (void **) &helper_mutex))) {
        helper_texture.reset();
        helper_mutex.reset();
        return false;
      }
      return true;
    };

    auto release_helper_message = [&](const helper_ipc::frame_message &message, std::uint64_t received_qpc, std::uint64_t acquired_qpc) -> bool {
      helper_ipc::frame_release_message release {};
      release.header = helper_ipc::make_header(helper_ipc::message_type::frame_release, sizeof(release));
      release.shared_handle = message.shared_handle;
      release.sequence = message.sequence;
      release.helper_send_qpc = message.helper_send_qpc;
      release.main_received_qpc = received_qpc;
      release.main_acquired_qpc = acquired_qpc;
      release.main_released_qpc = static_cast<std::uint64_t>(qpc_counter());
      return write_exact(helper_pipe, &release, sizeof(release));
    };

    while (true) {
      helper_ipc::message_header header {};
      switch (read_exact_timeout(helper_pipe, &header, sizeof(header), wait_time)) {
        case pipe_read_e::ok:
          break;
        case pipe_read_e::timeout:
          return capture_e::timeout;
        case pipe_read_e::error:
          return capture_e::error;
      }

      if (header.type == helper_ipc::message_type::no_frame && header.size == sizeof(helper_ipc::no_frame_message)) {
        helper_ipc::no_frame_message message {};
        message.header = header;
        switch (read_exact_timeout(helper_pipe, reinterpret_cast<std::uint8_t *>(&message) + sizeof(header), sizeof(message) - sizeof(header), wait_time)) {
          case pipe_read_e::ok:
            break;
          case pipe_read_e::timeout:
            return capture_e::timeout;
          case pipe_read_e::error:
            return capture_e::error;
        }
        if (message.request_id == request.request_id) {
          return capture_e::timeout;
        }
        continue;
      }

      if (header.type == helper_ipc::message_type::error) {
        helper_ipc::error_message message {};
        message.header = header;
        read_exact(helper_pipe, reinterpret_cast<std::uint8_t *>(&message) + sizeof(header), sizeof(message) - sizeof(header));
        BOOST_LOG(warning) << "WGC helper reported an error [0x"sv << util::hex(message.code).to_string_view()
                           << "] detail [0x"sv << util::hex(message.detail).to_string_view() << ']';
        stop_helper();
        return capture_e::error;
      }

      if (header.type != helper_ipc::message_type::frame || header.size != sizeof(helper_ipc::frame_message)) {
        return capture_e::error;
      }

      helper_ipc::frame_message message {};
      message.header = header;
      switch (read_exact_timeout(helper_pipe, reinterpret_cast<std::uint8_t *>(&message) + sizeof(header), sizeof(message) - sizeof(header), wait_time)) {
        case pipe_read_e::ok:
          break;
        case pipe_read_e::timeout:
          return capture_e::timeout;
        case pipe_read_e::error:
          return capture_e::error;
      }
      const auto received_qpc = static_cast<std::uint64_t>(qpc_counter());

      if (!ensure_helper_texture(message) || !helper_mutex) {
        return capture_e::error;
      }
      const HRESULT acquire_status = helper_mutex->AcquireSync(1, 1000);
      if (acquire_status == WAIT_TIMEOUT || acquire_status == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
        return capture_e::timeout;
      }
      if (FAILED(acquire_status)) {
        return capture_e::error;
      }
      const auto acquired_qpc = static_cast<std::uint64_t>(qpc_counter());

      if (message.request_id != request.request_id) {
        if (FAILED(helper_mutex->ReleaseSync(0)) || !release_helper_message(message, received_qpc, acquired_qpc)) {
          return capture_e::error;
        }
        continue;
      }

      helper_frame_locked = true;
      helper_frame_sequence = message.sequence;
      helper_frame_send_qpc = message.helper_send_qpc;
      helper_frame_received_qpc = received_qpc;
      helper_frame_acquired_qpc = acquired_qpc;
      *out = helper_texture.get();
      (*out)->AddRef();
      out_time = message.qpc_timestamp;
      return capture_e::ok;
    }
  }

  void wgc_capture_t::stop_helper() {
    if (helper_pipe) {
      helper_ipc::message_header message = helper_ipc::make_header(helper_ipc::message_type::shutdown, sizeof(message));
      write_exact(helper_pipe, &message, sizeof(message));
      FlushFileBuffers(helper_pipe);
      DisconnectNamedPipe(helper_pipe);
      CloseHandle(helper_pipe);
      helper_pipe = nullptr;
    }

    if (helper_process) {
      if (WaitForSingleObject(helper_process, 1000) != WAIT_OBJECT_0) {
        TerminateProcess(helper_process, ERROR_PROCESS_ABORTED);
      }
      CloseHandle(helper_process);
      helper_process = nullptr;
    }

    if (helper_thread) {
      CloseHandle(helper_thread);
      helper_thread = nullptr;
    }

    helper_texture.reset();
    helper_mutex.reset();
    helper_texture_name.clear();
    helper_remote_texture_handle = nullptr;
    helper_device = nullptr;
    helper_active = false;
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      if (dup.is_helper_active()) {
        dup.release_frame();
      }
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      if (dup.is_helper_active()) {
        dup.release_frame();
      }
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());
    if (dup.is_helper_active()) {
      dup.release_frame();
    }

    if (!pull_free_image_cb(img_out)) {
      if (dup.is_helper_active()) {
        dup.release_frame();
      }
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
