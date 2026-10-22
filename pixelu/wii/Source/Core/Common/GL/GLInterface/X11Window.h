// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// X11 support for the GLX and EGL contexts. Only GLX.cpp and EGL.cpp include this header (it
// brings in Xlib and its macros); the context headers keep X11 out of the rest of the program.

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <memory>
#include <mutex>

#include "Common/CommonTypes.h"

namespace GLX11
{
// The Xlib functions the GL contexts call. libX11 is opened at run time like libGL and libEGL,
// so a build without a display server (Vulkan or headless use) needs none of them installed.
struct XlibFunctions
{
  decltype(&XOpenDisplay) open_display;
  decltype(&XCloseDisplay) close_display;
  decltype(&XDisplayString) display_string;
  decltype(&XDefaultScreen) default_screen;
  decltype(&XSetErrorHandler) set_error_handler;
  decltype(&XGetErrorText) get_error_text;
  decltype(&XGetWindowAttributes) get_window_attributes;
  decltype(&XCreateColormap) create_colormap;
  decltype(&XFreeColormap) free_colormap;
  decltype(&XCreateWindow) create_window;
  decltype(&XDestroyWindow) destroy_window;
  decltype(&XMapWindow) map_window;
  decltype(&XMoveResizeWindow) move_resize_window;
  decltype(&XGetVisualInfo) get_visual_info;
  decltype(&XFree) free;
  decltype(&XSync) sync;
  decltype(&XFlush) flush;
};

// libX11.so.6's functions, or nullptr when it cannot be loaded.
const XlibFunctions* Xlib();

// While one of these lives, X errors on our connections raised by this thread are expected (for
// example while asking for context versions newest first) and only logged at info level.
class ExpectErrors
{
public:
  ExpectErrors();
  ~ExpectErrors();
  ExpectErrors(const ExpectErrors&) = delete;
  ExpectErrors& operator=(const ExpectErrors&) = delete;
};

// What the GL context draws into on X11: its own connection to the frontend's X server and a
// child window of the frontend's render window.
//
// The connection is private so that nothing here needs the frontend's Xlib display to be
// thread-safe (XInitThreads): the video thread and the shader-compile threads only ever use this
// connection, and every call on it is made with lock() held. Window ids are global to the
// server, so the child can sit in the frontend's window although another connection owns that.
//
// The child window has the visual the GL config asks for (the frontend's window may have any
// visual) and is kept at the size of its parent by FollowParent(). It selects no input events,
// so key and mouse events go on to the frontend's window as before.
class Target
{
public:
  ~Target();

  // Connects to the server of `host_display` (an Xlib Display*), for drawing into `parent`.
  // nullptr when libX11 is missing or the server cannot be reached.
  static std::unique_ptr<Target> Open(void* host_display, Window parent);

  Display* GetDisplay() const { return m_display; }
  int GetScreen() const { return m_screen; }
  Window GetWindow() const { return m_window; }
  std::mutex& Lock() { return m_lock; }

  // Makes the child window with this visual (caller holds the lock). False on failure.
  bool CreateChild(const XVisualInfo& visual);

  // The visual with this id on our screen, or false.
  bool FindVisual(VisualID id, XVisualInfo* out) const;

  // Resizes the child to its parent and reports the size (caller holds the lock). False when
  // the parent cannot be read (for example because it is gone).
  bool FollowParent(u32* width, u32* height);

private:
  Target() = default;

  Display* m_display = nullptr;
  int m_screen = 0;
  Window m_parent = 0;
  Window m_window = 0;
  Colormap m_colormap = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  std::mutex m_lock;
};
}  // namespace GLX11
