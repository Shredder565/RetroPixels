// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Which native windowing system the handles in WindowSystemInfo belong to. Picks the GL context
// type / Vulkan surface extension in the video backends.
enum class WindowSystemType
{
  Headless,
  Windows,
  MacOS,
  Android,
  X11,
  Wayland,
  FBDev,
  Haiku,
};

// Native handles the frontend passes to the video backend and input code.
struct WindowSystemInfo
{
  WindowSystemInfo() = default;
  WindowSystemInfo(WindowSystemType type_, void* display_connection_, void* render_window_,
                   void* render_surface_)
      : type(type_), display_connection(display_connection_), render_window(render_window_),
        render_surface(render_surface_)
  {
  }

  WindowSystemType type = WindowSystemType::Headless;

  // Display server connection (X11 Display*, wl_display*); unused on Windows.
  void* display_connection = nullptr;

  // Top-level native window (an HWND on Windows). nullptr means render headless.
  void* render_window = nullptr;

  // The native surface rendering goes to. Usually the same as render_window, but kept separate
  // because input wants the window while the backend may swap the surface during startup.
  void* render_surface = nullptr;

  // Surface DPI scale factor (1.0 = 96 DPI).
  float render_surface_scale = 1.0f;
};
