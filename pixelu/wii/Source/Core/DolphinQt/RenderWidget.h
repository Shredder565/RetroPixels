// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QByteArray>
#include <QEvent>
#include <QWidget>

class QMouseEvent;
class QTimer;

// The native window the game is drawn into, either on its own or inside the main window. It
// passes keyboard and mouse input on to the on-screen UI, hides and confines the cursor as
// configured, and reports focus, size and fullscreen changes to Host.
class RenderWidget final : public QWidget
{
  Q_OBJECT

public:
  explicit RenderWidget(QWidget* parent = nullptr);

  bool event(QEvent* event) override;
  void showFullScreen();
  QPaintEngine* paintEngine() const override;
  bool IsCursorLocked() const { return m_cursor_locked; }
  void SetCursorLockedOnNextActivation(bool locked = true);
  void SetWaitingForMessageBox(bool waiting_for_message_box);
  void SetCursorLocked(bool locked, bool follow_aspect_ratio = true);

  // PixelU: true when RetroPixels started us with PIXELU_EMBED=1 (Windows only).
  static bool IsEmbedMode();
  // PixelU: show the render window parked off-screen, waiting for RetroPixels to dock it.
  void ShowForEmbedding(const QByteArray& geometry);

signals:
  void EscapePressed();
  void Closed();
  void HandleChanged(void* handle);
  void StateChanged(bool fullscreen);
  void SizeChanged(int new_width, int new_height);
  void FocusChanged(bool focus);

private:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

  void ApplyNewHandle(void* handle);
  void ApplyAlwaysOnTop(bool on_top);
  void RefreshCursorShape();
  void HideIdleCursor();
  void ReportSizeIfChanged(QSize logical_size);

  void ForwardToOverlay(const QEvent* event);
  void InstallOverlayKeyMap();

  // PixelU embedding (see RenderWidget.cpp).
  bool IsDockedByHost() const;
  void ParkOffscreen();
  void PollDocking();
  void GiveUpDocking();

  QTimer* m_cursor_idle_timer = nullptr;

  // Last size reported through SizeChanged, in physical pixels, and the scale it was taken at.
  int m_reported_width = 0;
  int m_reported_height = 0;
  float m_reported_scale = 0;

  bool m_cursor_locked = false;
  bool m_lock_when_activated = false;
  bool m_suppress_lock_on_show = false;
  bool m_message_box_open = false;
  bool m_resume_when_activated = false;

  QTimer* m_dock_poll = nullptr;
  QTimer* m_dock_deadline = nullptr;
  bool m_waiting_for_dock = false;
  bool m_had_app_window_style = false;
};
