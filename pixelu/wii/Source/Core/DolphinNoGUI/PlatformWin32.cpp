// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The Windows platform of PixelU-Wii: one plain window the game is drawn into. RetroPixels finds
// it as the largest visible window of the process and may dock it into its own window
// (SetParent); it stops the game with WM_CLOSE. F11 and Alt+Enter toggle fullscreen (borderless,
// covering the window's monitor) when the window is not docked.

#include "DolphinNoGUI/Platform.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <windows.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Present.h"

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"PixelU-Wii";
constexpr wchar_t WINDOW_TITLE[] = L"PixelU Wii";
// The icon resource in PixelU-Wii.rc.
constexpr int IDI_ICON1 = 101;

// Posted to the window by other threads; handled on MainLoop's thread, which owns the window.
constexpr UINT WM_PIXELU_FULLSCREEN = WM_APP + 1;
constexpr UINT WM_PIXELU_PENDING = WM_APP + 2;

class Win32Platform final : public Platform
{
public:
  ~Win32Platform() override;

  bool Init() override;
  WindowSystemInfo GetWindowSystemInfo() const override;
  void MainLoop() override;
  void JobsQueued() override;
  void SetTitle(std::string_view title) override;
  void RequestWindowSize(int width, int height) override;
  void ToggleFullscreen() override;

private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);
  void SetFullscreen(bool fullscreen);
  void ApplyPending();
  bool Docked() const { return GetParent(m_hwnd) != nullptr; }

  HWND m_hwnd = nullptr;
  HANDLE m_wake = nullptr;
  bool m_fullscreen = false;
  bool m_hide_cursor = false;
  WINDOWPLACEMENT m_windowed{sizeof(WINDOWPLACEMENT)};
  LONG_PTR m_windowed_style = 0;

  std::mutex m_pending_mutex;
  std::optional<std::string> m_pending_title;
  std::optional<std::pair<int, int>> m_pending_size;
};

Win32Platform::~Win32Platform()
{
  if (m_hwnd != nullptr)
    DestroyWindow(m_hwnd);
  if (m_wake != nullptr)
    CloseHandle(m_wake);
}

bool Win32Platform::Init()
{
  m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const HINSTANCE instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_ICON1));
  window_class.hIconSm = window_class.hIcon;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = WINDOW_CLASS;
  if (RegisterClassExW(&window_class) == 0)
    return false;

  // The configured size is the picture's (the client area); the frame comes on top.
  const int width = std::max(Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH), 64);
  const int height = std::max(Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT), 64);
  constexpr DWORD style = WS_OVERLAPPEDWINDOW;
  RECT frame{0, 0, width, height};
  AdjustWindowRectEx(&frame, style, FALSE, 0);

  // -1 (the default) means no position was saved: Windows places the window.
  const int saved_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  const int saved_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  const bool positioned = saved_x >= 0 && saved_y >= 0;

  m_hwnd = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, style,
                           positioned ? saved_x : CW_USEDEFAULT,
                           positioned ? saved_y : CW_USEDEFAULT, frame.right - frame.left,
                           frame.bottom - frame.top, nullptr, nullptr, instance, this);
  if (m_hwnd == nullptr)
    return false;

  m_hide_cursor = Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never;
  ShowWindow(m_hwnd, SW_SHOWNORMAL);
  if (Config::Get(Config::MAIN_FULLSCREEN))
    SetFullscreen(true);
  UpdateWindow(m_hwnd);
  return true;
}

WindowSystemInfo Win32Platform::GetWindowSystemInfo() const
{
  WindowSystemInfo wsi(WindowSystemType::Windows, nullptr, m_hwnd, m_hwnd);
  wsi.render_surface_scale = static_cast<float>(GetDpiForWindow(m_hwnd)) / 96.0f;
  return wsi;
}

LRESULT CALLBACK Win32Platform::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
  if (message == WM_NCCREATE)
  {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  auto* const platform = reinterpret_cast<Win32Platform*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (platform == nullptr || platform->m_hwnd == nullptr)
    return DefWindowProcW(hwnd, message, wparam, lparam);
  return platform->Handle(message, wparam, lparam);
}

LRESULT Win32Platform::Handle(UINT message, WPARAM wparam, LPARAM lparam)
{
  switch (message)
  {
  case WM_CLOSE:
    // The game winds down first; the window goes when MainLoop returns.
    RequestShutdown();
    SetEvent(m_wake);
    return 0;

  case WM_SIZE:
    // The presenter reads the new size from the window.
    if (g_presenter)
      g_presenter->ResizeSurface();
    return 0;

  case WM_SETCURSOR:
    if (m_hide_cursor && LOWORD(lparam) == HTCLIENT)
    {
      SetCursor(nullptr);
      return TRUE;
    }
    break;

  case WM_ERASEBKGND:
    // The backend draws every pixel; skipping the erase avoids a flash of black on resize.
    return TRUE;

  case WM_KEYDOWN:
  case WM_SYSKEYDOWN:
  {
    const bool alt = (lparam & (1 << 29)) != 0;
    const bool repeat = (lparam & (1 << 30)) != 0;
    if (wparam == VK_F11 || (alt && wparam == VK_RETURN))
    {
      if (!repeat)
        ToggleFullscreen();
      return 0;
    }
    break;
  }

  case WM_SYSCHAR:
    // Alt+Enter would otherwise beep.
    if (wparam == VK_RETURN)
      return 0;
    break;

  case WM_PIXELU_FULLSCREEN:
    if (!Docked())
      SetFullscreen(!m_fullscreen);
    return 0;

  case WM_PIXELU_PENDING:
    ApplyPending();
    return 0;

  default:
    break;
  }
  return DefWindowProcW(m_hwnd, message, wparam, lparam);
}

void Win32Platform::SetFullscreen(bool fullscreen)
{
  if (fullscreen == m_fullscreen)
    return;

  if (fullscreen)
  {
    // Remember the window as it is, then cover the monitor it is on without a frame.
    GetWindowPlacement(m_hwnd, &m_windowed);
    m_windowed_style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    const RECT& r = monitor.rcMonitor;
    SetWindowPos(m_hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
  else
  {
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, m_windowed_style);
    SetWindowPlacement(m_hwnd, &m_windowed);
    SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
  }
  m_fullscreen = fullscreen;
}

void Win32Platform::ApplyPending()
{
  std::optional<std::string> title;
  std::optional<std::pair<int, int>> size;
  {
    std::lock_guard lock(m_pending_mutex);
    title.swap(m_pending_title);
    size.swap(m_pending_size);
  }
  if (title)
    SetWindowTextW(m_hwnd, UTF8ToWString(*title).c_str());
  if (size && !m_fullscreen && !Docked() && Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE))
  {
    RECT frame{0, 0, size->first, size->second};
    AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE)));
    SetWindowPos(m_hwnd, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void Win32Platform::MainLoop()
{
  Core::System& system = Core::System::GetInstance();
  bool stop_sent = false;

  while (true)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT)
        RequestShutdown();
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    if (IsShutdownRequested() && !stop_sent)
    {
      // The session winds down on its own threads; the window keeps pumping meanwhile, which
      // the video backend needs while it lets go of the window.
      Core::Stop(system);
      stop_sent = true;
    }

    Core::HostDispatchJobs(system);
    if (Core::IsUninitialized(system))
      break;

    // Window messages, queued host jobs, or 10 ms, whichever comes first.
    MsgWaitForMultipleObjects(1, &m_wake, FALSE, 10, QS_ALLINPUT);
  }

  // The game screen goes away with the game.
  ShowWindow(m_hwnd, SW_HIDE);
}

void Win32Platform::JobsQueued()
{
  if (m_wake != nullptr)
    SetEvent(m_wake);
}

void Win32Platform::SetTitle(std::string_view title)
{
  {
    std::lock_guard lock(m_pending_mutex);
    m_pending_title = std::string(title);
  }
  PostMessageW(m_hwnd, WM_PIXELU_PENDING, 0, 0);
}

void Win32Platform::RequestWindowSize(int width, int height)
{
  if (width <= 0 || height <= 0)
    return;
  {
    std::lock_guard lock(m_pending_mutex);
    m_pending_size = std::make_pair(width, height);
  }
  PostMessageW(m_hwnd, WM_PIXELU_PENDING, 0, 0);
}

void Win32Platform::ToggleFullscreen()
{
  PostMessageW(m_hwnd, WM_PIXELU_FULLSCREEN, 0, 0);
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateWin32()
{
  return std::make_unique<Win32Platform>();
}
