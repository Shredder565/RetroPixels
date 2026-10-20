// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QEvent>
#include <QWidget>

class QMouseEvent;
class QTimer;

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
  void HandleCursorTimer();
  void OnHandleChanged(void* handle);
  void OnHideCursorChanged();
  void OnNeverHideCursorChanged();
  void OnLockCursorChanged();
  void OnKeepOnTopChanged(bool top);
  void UpdateCursor();
  void PassEventToPresenter(const QEvent* event);
  void SetPresenterKeyMap();
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  bool IsEmbedDocked() const;
  void CheckEmbedDocked();
  void KeepEmbedOffscreen();
  void ShowEmbedFallback();

  static constexpr int MOUSE_HIDE_DELAY = 3000;
  QTimer* m_mouse_timer;
  QPoint m_last_mouse{};
  int m_last_window_width = 0;
  int m_last_window_height = 0;
  float m_last_window_scale = 0;
  bool m_cursor_locked = false;
  bool m_lock_cursor_on_next_activation = false;
  bool m_dont_lock_cursor_on_show = false;
  bool m_waiting_for_message_box = false;
  bool m_should_unpause_on_focus = false;
  // PixelU embed mode state (see the notes in RenderWidget.cpp)
  QTimer* m_embed_poll_timer = nullptr;
  QTimer* m_embed_fallback_timer = nullptr;
  bool m_embed_pending = false;
  bool m_embed_had_appwindow = false;
};
