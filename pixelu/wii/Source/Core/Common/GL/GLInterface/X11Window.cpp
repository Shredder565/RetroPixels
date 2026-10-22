// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLInterface/X11Window.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"

namespace GLX11
{
namespace
{
template <typename Fn>
bool Resolve(const Common::DynamicLibrary& library, const char* name, Fn* out)
{
  *out = reinterpret_cast<Fn>(library.GetSymbolAddress(name));
  if (*out == nullptr)
    ERROR_LOG_FMT(VIDEO, "X11: {} is missing from libX11", name);
  return *out != nullptr;
}

XlibFunctions LoadXlib(bool* ok)
{
  // Opened once and kept for the life of the program.
  static Common::DynamicLibrary library;
  XlibFunctions fns{};
  *ok = false;
  if (!library.Open(Common::DynamicLibrary::GetVersionedFilename("X11", 6).c_str()))
  {
    ERROR_LOG_FMT(VIDEO, "X11: libX11.so.6 could not be loaded");
    return fns;
  }

  bool all = true;
  all &= Resolve(library, "XOpenDisplay", &fns.open_display);
  all &= Resolve(library, "XCloseDisplay", &fns.close_display);
  all &= Resolve(library, "XDisplayString", &fns.display_string);
  all &= Resolve(library, "XDefaultScreen", &fns.default_screen);
  all &= Resolve(library, "XSetErrorHandler", &fns.set_error_handler);
  all &= Resolve(library, "XGetErrorText", &fns.get_error_text);
  all &= Resolve(library, "XGetWindowAttributes", &fns.get_window_attributes);
  all &= Resolve(library, "XCreateColormap", &fns.create_colormap);
  all &= Resolve(library, "XFreeColormap", &fns.free_colormap);
  all &= Resolve(library, "XCreateWindow", &fns.create_window);
  all &= Resolve(library, "XDestroyWindow", &fns.destroy_window);
  all &= Resolve(library, "XMapWindow", &fns.map_window);
  all &= Resolve(library, "XMoveResizeWindow", &fns.move_resize_window);
  all &= Resolve(library, "XGetVisualInfo", &fns.get_visual_info);
  all &= Resolve(library, "XFree", &fns.free);
  all &= Resolve(library, "XSync", &fns.sync);
  all &= Resolve(library, "XFlush", &fns.flush);
  *ok = all;
  return fns;
}

// X protocol errors on our own connections are logged and otherwise ignored: the frontend may
// destroy its render window before the video backend lets go of the child, and Xlib's default
// handler would end the program for that. Errors on any other connection go to the handler
// that was installed before ours, so the frontend's error handling is unchanged.
std::mutex s_displays_lock;
std::vector<Display*> s_our_displays;
XErrorHandler s_previous_handler = nullptr;
thread_local int s_expected_errors = 0;

int OnXError(Display* display, XErrorEvent* event)
{
  bool ours;
  {
    std::lock_guard guard(s_displays_lock);
    ours = std::ranges::find(s_our_displays, display) != s_our_displays.end();
  }
  if (!ours)
    return s_previous_handler != nullptr ? s_previous_handler(display, event) : 0;

  std::array<char, 256> text{};
  Xlib()->get_error_text(display, event->error_code, text.data(), static_cast<int>(text.size()));
  if (s_expected_errors > 0)
  {
    INFO_LOG_FMT(VIDEO, "X11: {} (request {}.{}, expected)", text.data(),
                 static_cast<int>(event->request_code), static_cast<int>(event->minor_code));
    return 0;
  }
  WARN_LOG_FMT(VIDEO, "X11: {} (request {}.{}, resource {:#x})", text.data(),
               static_cast<int>(event->request_code), static_cast<int>(event->minor_code),
               event->resourceid);
  return 0;
}

void RegisterDisplay(Display* display)
{
  std::lock_guard guard(s_displays_lock);
  if (s_our_displays.empty())
  {
    // Installed while one of our connections is open, put back when the last one closes, as
    // long as nobody replaced ours in the meantime.
    s_previous_handler = Xlib()->set_error_handler(OnXError);
  }
  s_our_displays.push_back(display);
}

void UnregisterDisplay(Display* display)
{
  std::lock_guard guard(s_displays_lock);
  std::erase(s_our_displays, display);
  if (!s_our_displays.empty())
    return;
  const XErrorHandler current = Xlib()->set_error_handler(s_previous_handler);
  if (current != OnXError)
    Xlib()->set_error_handler(current);
  s_previous_handler = nullptr;
}
}  // namespace

ExpectErrors::ExpectErrors()
{
  ++s_expected_errors;
}

ExpectErrors::~ExpectErrors()
{
  --s_expected_errors;
}

const XlibFunctions* Xlib()
{
  static bool ok = false;
  static const XlibFunctions fns = LoadXlib(&ok);
  return ok ? &fns : nullptr;
}

std::unique_ptr<Target> Target::Open(void* host_display, Window parent)
{
  const XlibFunctions* x = Xlib();
  if (x == nullptr || parent == 0)
    return nullptr;

  // The same server (and default screen) the frontend talks to.
  const char* name =
      host_display != nullptr ? x->display_string(static_cast<Display*>(host_display)) : nullptr;
  Display* const display = x->open_display(name);
  if (display == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "X11: could not connect to display '{}'", name != nullptr ? name : "");
    return nullptr;
  }
  RegisterDisplay(display);

  std::unique_ptr<Target> target(new Target());
  target->m_display = display;
  target->m_parent = parent;

  // The screen is the parent window's, which need not be the default one.
  XWindowAttributes attributes{};
  if (x->get_window_attributes(display, parent, &attributes) == 0 || attributes.screen == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "X11: render window {:#x} cannot be read", parent);
    return nullptr;
  }
  target->m_screen = x->default_screen(display);
  for (int i = 0; i < ScreenCount(display); ++i)
  {
    if (ScreenOfDisplay(display, i) == attributes.screen)
      target->m_screen = i;
  }
  target->m_width = static_cast<u32>(std::max(attributes.width, 1));
  target->m_height = static_cast<u32>(std::max(attributes.height, 1));
  return target;
}

Target::~Target()
{
  if (m_display == nullptr)
    return;

  const XlibFunctions* x = Xlib();
  {
    std::lock_guard guard(m_lock);
    if (m_window != 0)
      x->destroy_window(m_display, m_window);
    if (m_colormap != 0)
      x->free_colormap(m_display, m_colormap);
    // Errors from the lines above (a parent destroyed first takes the child with it) arrive
    // here, while the connection is still known as ours.
    x->sync(m_display, False);
  }
  UnregisterDisplay(m_display);
  x->close_display(m_display);
}

bool Target::FindVisual(VisualID id, XVisualInfo* out) const
{
  XVisualInfo wanted{};
  wanted.visualid = id;
  wanted.screen = m_screen;
  int count = 0;
  XVisualInfo* const found =
      Xlib()->get_visual_info(m_display, VisualIDMask | VisualScreenMask, &wanted, &count);
  if (found == nullptr || count == 0)
    return false;
  *out = found[0];
  Xlib()->free(found);
  return true;
}

bool Target::CreateChild(const XVisualInfo& visual)
{
  const XlibFunctions* x = Xlib();
  m_colormap = x->create_colormap(m_display, m_parent, visual.visual, AllocNone);

  XSetWindowAttributes attributes{};
  attributes.colormap = m_colormap;
  attributes.background_pixmap = None;
  attributes.border_pixel = 0;
  // Nothing selected: input events propagate to the frontend's window.
  attributes.event_mask = 0;
  attributes.do_not_propagate_mask = 0;

  m_window = x->create_window(m_display, m_parent, 0, 0, m_width, m_height, 0, visual.depth,
                              InputOutput, visual.visual,
                              CWColormap | CWBackPixmap | CWBorderPixel | CWEventMask, &attributes);
  if (m_window == 0)
  {
    ERROR_LOG_FMT(VIDEO, "X11: could not create the render child window");
    return false;
  }
  x->map_window(m_display, m_window);
  x->sync(m_display, False);
  return true;
}

bool Target::FollowParent(u32* width, u32* height)
{
  const XlibFunctions* x = Xlib();
  XWindowAttributes attributes{};
  if (x->get_window_attributes(m_display, m_parent, &attributes) == 0)
    return false;

  const u32 new_width = static_cast<u32>(std::max(attributes.width, 1));
  const u32 new_height = static_cast<u32>(std::max(attributes.height, 1));
  if (m_window != 0 && (new_width != m_width || new_height != m_height))
  {
    x->move_resize_window(m_display, m_window, 0, 0, new_width, new_height);
    // The resize must reach the server before the next frame is sized for it.
    x->sync(m_display, False);
  }
  m_width = new_width;
  m_height = new_height;
  *width = m_width;
  *height = m_height;
  return true;
}
}  // namespace GLX11
