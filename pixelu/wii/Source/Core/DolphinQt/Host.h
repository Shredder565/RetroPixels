// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>

#include <QObject>

// The Qt side of the core's Host_* interface (Core/Host.h). The core calls those functions from
// its own threads, so everything the GUI has to react to is passed on as a signal; the state the
// core polls (focus, fullscreen) is kept in atomics.
class Host final : public QObject
{
  Q_OBJECT

public:
  ~Host() override;

  static Host* GetInstance();

  bool GetRenderFocus();
  bool GetRenderFullFocus();
  bool GetRenderFullscreen();
  bool GetGBAFocus();
  bool GetTASInputFocus() const;

  void SetMainWindowHandle(void* handle);
  void SetRenderHandle(void* handle);
  void SetRenderFocus(bool focus);
  void SetRenderFullFocus(bool focus);
  void SetRenderFullscreen(bool fullscreen);
  void SetTASInputFocus(bool focus);
  void ResizeSurface(int new_width, int new_height);

signals:
  void RequestTitle(const QString& title);
  void RequestStop();
  void RequestRenderSize(int w, int h);
  void UpdateDisasmDialog();
  void JitCacheInvalidation();
  void JitProfileDataWiped();
  void PPCSymbolsChanged();
  void PPCBreakpointsChanged();

private:
  Host();

  // Native window handles (HWND on Windows) of the render surface and the main window.
  std::atomic<void*> m_surface{nullptr};
  std::atomic<void*> m_main_window{nullptr};

  // Whether the render surface lives inside the main window, as of the last SetRenderHandle.
  std::atomic<bool> m_surface_in_main{false};

  std::atomic<bool> m_has_focus{false};
  std::atomic<bool> m_has_full_focus{false};
  std::atomic<bool> m_is_fullscreen{false};
  std::atomic<bool> m_tas_has_focus{false};
};
