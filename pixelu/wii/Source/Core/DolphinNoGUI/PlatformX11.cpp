// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The X11 platform of dolphin-emu-nogui: one plain window the game is drawn into (also under
// XWayland). The video backends and the keyboard/mouse backend get {X11, Display*, window}.
// Closing the window (the window manager's close button, or WM_DELETE_WINDOW from RetroPixels)
// stops the game; F11 and Alt+Return toggle fullscreen through the window manager.

#include "DolphinNoGUI/Platform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Present.h"

// Xlib last: its macros (None, Bool, Status...) would clash with the headers above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

namespace
{
constexpr char WINDOW_TITLE[] = "PixelU Wii";

class X11Platform final : public Platform
{
public:
  ~X11Platform() override;

  bool Init() override;
  WindowSystemInfo GetWindowSystemInfo() const override;
  void MainLoop() override;
  void JobsQueued() override;
  void SetTitle(std::string_view title) override;
  void RequestWindowSize(int width, int height) override;

private:
  Atom Intern(const char* name) const { return XInternAtom(m_display, name, False); }
  void SetUtf8Title(const std::string& title);
  void HideCursor();
  void HandleEvent(XEvent& event);
  void ApplyPending();
  void ToggleFullscreen();
  void WaitForWork();

  Display* m_display = nullptr;
  Window m_window = 0;
  Atom m_wm_protocols = 0;
  Atom m_wm_delete = 0;
  int m_wake_fd = -1;

  int m_width = 0;
  int m_height = 0;
  bool m_fullscreen = false;

  // Written by other threads (Host_UpdateTitle, Host_RequestRenderWindowSize), applied by
  // MainLoop's thread, which owns the connection.
  std::mutex m_pending_mutex;
  std::optional<std::string> m_pending_title;
  std::optional<std::pair<int, int>> m_pending_size;
};

X11Platform::~X11Platform()
{
  if (m_display != nullptr)
  {
    if (m_window != 0)
      XDestroyWindow(m_display, m_window);
    XCloseDisplay(m_display);
  }
  if (m_wake_fd != -1)
    close(m_wake_fd);
}

bool X11Platform::Init()
{
  // The Vulkan backend uses this connection from the GPU thread while MainLoop reads events.
  XInitThreads();
  m_display = XOpenDisplay(nullptr);
  if (m_display == nullptr)
  {
    std::fprintf(stderr, "Cannot open the X display \"%s\" (is DISPLAY set?).\n",
                 XDisplayName(nullptr));
    return false;
  }
  m_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

  const int screen = DefaultScreen(m_display);
  const Window root = RootWindow(m_display, screen);
  m_width = std::max(Config::Get(Config::MAIN_RENDER_WINDOW_WIDTH), 64);
  m_height = std::max(Config::Get(Config::MAIN_RENDER_WINDOW_HEIGHT), 64);
  // -1 (the default) means no position was saved: the window manager places it, and
  // without one it starts at the top left corner.
  const int saved_x = Config::Get(Config::MAIN_RENDER_WINDOW_XPOS);
  const int saved_y = Config::Get(Config::MAIN_RENDER_WINDOW_YPOS);
  const bool positioned = saved_x >= 0 && saved_y >= 0;
  const int x = positioned ? saved_x : 0;
  const int y = positioned ? saved_y : 0;

  XSetWindowAttributes attributes{};
  attributes.background_pixel = BlackPixel(m_display, screen);
  attributes.event_mask = StructureNotifyMask | KeyPressMask | FocusChangeMask;
  m_window = XCreateWindow(m_display, root, x, y, static_cast<unsigned>(m_width),
                           static_cast<unsigned>(m_height), 0, CopyFromParent, InputOutput,
                           CopyFromParent, CWBackPixel | CWEventMask, &attributes);
  if (m_window == 0)
    return false;

  // Who we are, for window managers and for RetroPixels (it finds engine windows by pid).
  SetUtf8Title(WINDOW_TITLE);
  XClassHint class_hint{};
  std::string res_name = "pixelu-wii";
  std::string res_class = "PixelU-Wii";
  class_hint.res_name = res_name.data();
  class_hint.res_class = res_class.data();
  XSetClassHint(m_display, m_window, &class_hint);

  const long pid = static_cast<long>(getpid());
  XChangeProperty(m_display, m_window, Intern("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
                  reinterpret_cast<const unsigned char*>(&pid), 1);
  std::array<char, 256> host{};
  if (gethostname(host.data(), host.size() - 1) == 0)
  {
    XChangeProperty(m_display, m_window, XA_WM_CLIENT_MACHINE, XA_STRING, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(host.data()),
                    static_cast<int>(std::strlen(host.data())));
  }

  m_wm_protocols = Intern("WM_PROTOCOLS");
  m_wm_delete = Intern("WM_DELETE_WINDOW");
  XSetWMProtocols(m_display, m_window, &m_wm_delete, 1);

  // Where it was asked to be, when the configuration has a position.
  XSizeHints size_hints{};
  size_hints.flags = PSize | (positioned ? USPosition : 0);
  size_hints.x = x;
  size_hints.y = y;
  size_hints.width = m_width;
  size_hints.height = m_height;
  XSetWMNormalHints(m_display, m_window, &size_hints);

  // Fullscreen from the start: the window manager reads _NET_WM_STATE when the window maps.
  m_fullscreen = Config::Get(Config::MAIN_FULLSCREEN);
  if (m_fullscreen)
  {
    const Atom state = Intern("_NET_WM_STATE_FULLSCREEN");
    XChangeProperty(m_display, m_window, Intern("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&state), 1);
  }

  if (Config::Get(Config::MAIN_SHOW_CURSOR) == Config::ShowCursor::Never)
    HideCursor();

  XMapRaised(m_display, m_window);
  XFlush(m_display);
  return true;
}

WindowSystemInfo X11Platform::GetWindowSystemInfo() const
{
  void* const window = reinterpret_cast<void*>(m_window);
  return WindowSystemInfo{WindowSystemType::X11, m_display, window, window};
}

void X11Platform::SetUtf8Title(const std::string& title)
{
  XStoreName(m_display, m_window, title.c_str());
  XChangeProperty(m_display, m_window, Intern("_NET_WM_NAME"), Intern("UTF8_STRING"), 8,
                  PropModeReplace, reinterpret_cast<const unsigned char*>(title.data()),
                  static_cast<int>(title.size()));
}

void X11Platform::HideCursor()
{
  // An empty 1x1 bitmap as both shape and mask.
  const char blank = 0;
  const Pixmap pixmap = XCreateBitmapFromData(m_display, m_window, &blank, 1, 1);
  XColor black{};
  const Cursor cursor = XCreatePixmapCursor(m_display, pixmap, pixmap, &black, &black, 0, 0);
  XDefineCursor(m_display, m_window, cursor);
  XFreeCursor(m_display, cursor);
  XFreePixmap(m_display, pixmap);
}

void X11Platform::ToggleFullscreen()
{
  // EWMH: a _NET_WM_STATE client message to the root window, 2 = toggle.
  XEvent event{};
  event.xclient.type = ClientMessage;
  event.xclient.window = m_window;
  event.xclient.message_type = Intern("_NET_WM_STATE");
  event.xclient.format = 32;
  event.xclient.data.l[0] = 2;
  event.xclient.data.l[1] = static_cast<long>(Intern("_NET_WM_STATE_FULLSCREEN"));
  event.xclient.data.l[3] = 1;  // a normal application
  XSendEvent(m_display, DefaultRootWindow(m_display), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &event);
  m_fullscreen = !m_fullscreen;
}

void X11Platform::HandleEvent(XEvent& event)
{
  switch (event.type)
  {
  case ClientMessage:
    if (event.xclient.message_type == m_wm_protocols &&
        static_cast<Atom>(event.xclient.data.l[0]) == m_wm_delete)
    {
      RequestShutdown();
    }
    break;

  case ConfigureNotify:
    if (event.xconfigure.window != m_window)
      break;
    if (event.xconfigure.width != m_width || event.xconfigure.height != m_height)
    {
      m_width = event.xconfigure.width;
      m_height = event.xconfigure.height;
      // The presenter reads the new size from the window (an X11 window has one).
      if (g_presenter)
        g_presenter->ResizeSurface();
    }
    break;

  case KeyPress:
  {
    const KeySym key = XLookupKeysym(&event.xkey, 0);
    const bool alt = (event.xkey.state & Mod1Mask) != 0;
    if (key == XK_F11 || (alt && key == XK_Return))
      ToggleFullscreen();
    break;
  }

  default:
    break;
  }
}

void X11Platform::ApplyPending()
{
  std::optional<std::string> title;
  std::optional<std::pair<int, int>> size;
  {
    std::lock_guard lock(m_pending_mutex);
    title.swap(m_pending_title);
    size.swap(m_pending_size);
  }
  if (title)
    SetUtf8Title(*title);
  if (size && !m_fullscreen && Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE))
    XResizeWindow(m_display, m_window, static_cast<unsigned>(size->first),
                  static_cast<unsigned>(size->second));
  if (title || size)
    XFlush(m_display);
}

void X11Platform::WaitForWork()
{
  // X events, queued host jobs, or 10 ms, whichever comes first.
  std::array<pollfd, 2> fds{};
  fds[0] = {ConnectionNumber(m_display), POLLIN, 0};
  fds[1] = {m_wake_fd, POLLIN, 0};
  const nfds_t count = m_wake_fd != -1 ? 2 : 1;
  if (poll(fds.data(), count, 10) > 0 && (fds[1].revents & POLLIN) != 0)
  {
    std::uint64_t wakes = 0;
    [[maybe_unused]] const ssize_t got = read(m_wake_fd, &wakes, sizeof(wakes));
  }
}

void X11Platform::MainLoop()
{
  Core::System& system = Core::System::GetInstance();
  bool stop_sent = false;

  while (true)
  {
    while (XPending(m_display) > 0)
    {
      XEvent event;
      XNextEvent(m_display, &event);
      HandleEvent(event);
    }
    ApplyPending();

    if (IsShutdownRequested() && !stop_sent)
    {
      // As in the headless platform: the session winds down on its own threads.
      Core::Stop(system);
      stop_sent = true;
    }

    Core::HostDispatchJobs(system);
    if (Core::IsUninitialized(system))
      break;

    WaitForWork();
  }

  // The game screen goes away with the game, as the Qt program's does.
  XUnmapWindow(m_display, m_window);
  XFlush(m_display);
}

void X11Platform::JobsQueued()
{
  if (m_wake_fd == -1)
    return;
  const std::uint64_t one = 1;
  [[maybe_unused]] const ssize_t written = write(m_wake_fd, &one, sizeof(one));
}

void X11Platform::SetTitle(std::string_view title)
{
  {
    std::lock_guard lock(m_pending_mutex);
    m_pending_title = std::string(title);
  }
  JobsQueued();
}

void X11Platform::RequestWindowSize(int width, int height)
{
  if (width <= 0 || height <= 0)
    return;
  {
    std::lock_guard lock(m_pending_mutex);
    m_pending_size = std::make_pair(width, height);
  }
  JobsQueued();
}
}  // namespace

std::unique_ptr<Platform> Platform::CreateX11()
{
  return std::make_unique<X11Platform>();
}
