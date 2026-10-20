// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/RenderWidget.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/State.h"
#include "Core/System.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/Present.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

// PixelU: embedding support for RetroPixels.
//
// RetroPixels starts us with PIXELU_EMBED=1, finds this render window and docks it inside its
// own window (SetParent + WS_CHILD). Left alone, the render window would appear on screen as a
// normal window (frame, taskbar button) for about a second before that happens. In embed mode:
//  - before it is first shown it gets WS_EX_TOOLWINDOW (no taskbar button or Alt+Tab entry) and
//    is parked off-screen at (-32000,-32000) at its normal size. It stays "visible", so
//    RetroPixels can find it and the renderer gets an ordinary surface.
//  - while it waits to be docked, nothing is allowed to move it back on screen.
//  - once it is docked, Dolphin never moves or resizes it (RetroPixels is in charge).
//  - if it is not docked within 20 seconds, it turns back into the normal window it would have
//    been, centred on the primary screen, so the game is never stuck out of sight.
// Without PIXELU_EMBED=1, or on non-Windows builds, none of this does anything.
namespace
{
[[maybe_unused]] constexpr int EMBED_OFFSCREEN_POS = -32000;
[[maybe_unused]] constexpr int EMBED_POLL_INTERVAL_MS = 100;
[[maybe_unused]] constexpr int EMBED_FALLBACK_MS = 20000;
}  // namespace

RenderWidget::RenderWidget(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(QStringLiteral("PixelU Wii"));
  setWindowIcon(Resources::GetAppIcon());
  setWindowRole(QStringLiteral("renderer"));
  setAcceptDrops(true);

  QPalette p;
  p.setColor(QPalette::Window, Qt::black);
  setPalette(p);

  connect(Host::GetInstance(), &Host::RequestTitle, this, &RenderWidget::setWindowTitle);
  connect(Host::GetInstance(), &Host::RequestRenderSize, this, [this](int w, int h) {
    if (!Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE) || isFullScreen() || isMaximized())
      return;

    // PixelU: once RetroPixels has docked us, it decides our size and position.
    if (IsEmbedDocked())
      return;

    const auto dpr = window()->windowHandle()->screen()->devicePixelRatio();

    resize(w / dpr, h / dpr);

    // PixelU: resizing keeps the top-left corner, but make sure we stay parked off-screen.
    KeepEmbedOffscreen();
  });

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Running)
      SetPresenterKeyMap();
  });

  // We have to use Qt::DirectConnection here because we don't want those signals to get queued
  // (which results in them not getting called)
  connect(this, &RenderWidget::StateChanged, Host::GetInstance(), &Host::SetRenderFullscreen,
          Qt::DirectConnection);
  connect(this, &RenderWidget::HandleChanged, this, &RenderWidget::OnHandleChanged,
          Qt::DirectConnection);
  connect(this, &RenderWidget::SizeChanged, Host::GetInstance(), &Host::ResizeSurface,
          Qt::DirectConnection);
  connect(this, &RenderWidget::FocusChanged, Host::GetInstance(), &Host::SetRenderFocus,
          Qt::DirectConnection);

  m_mouse_timer = new QTimer(this);
  connect(m_mouse_timer, &QTimer::timeout, this, &RenderWidget::HandleCursorTimer);
  m_mouse_timer->setSingleShot(true);
  setMouseTracking(true);

  connect(&Settings::Instance(), &Settings::CursorVisibilityChanged, this,
          &RenderWidget::OnHideCursorChanged);
  connect(&Settings::Instance(), &Settings::LockCursorChanged, this,
          &RenderWidget::OnLockCursorChanged);
  OnHideCursorChanged();
  OnLockCursorChanged();
  connect(&Settings::Instance(), &Settings::KeepWindowOnTopChanged, this,
          &RenderWidget::OnKeepOnTopChanged);
  OnKeepOnTopChanged(Settings::Instance().IsKeepWindowOnTopEnabled());
  m_mouse_timer->start(MOUSE_HIDE_DELAY);

  // We need a native window to render into.
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
}

QPaintEngine* RenderWidget::paintEngine() const
{
  return nullptr;
}

void RenderWidget::dragEnterEvent(QDragEnterEvent* event)
{
  if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1)
    event->acceptProposedAction();
}

void RenderWidget::dropEvent(QDropEvent* event)
{
  const auto& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  const auto& url = urls[0];
  QFileInfo file_info(url.toLocalFile());

  auto path = file_info.filePath();

  if (!file_info.exists() || !file_info.isReadable())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
    return;
  }

  if (!file_info.isFile())
  {
    return;
  }

  State::LoadAs(Core::System::GetInstance(), path.toStdString());
}

void RenderWidget::OnHandleChanged(void* handle)
{
  if (handle)
  {
#ifdef _WIN32
    // Remove rounded corners from the render window on Windows 11
    const DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(static_cast<HWND>(handle), DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner_preference, sizeof(corner_preference));
#endif
  }
  Host::GetInstance()->SetRenderHandle(handle);
}

bool RenderWidget::IsEmbedMode()
{
#ifdef _WIN32
  static const bool embed_mode =
      qEnvironmentVariable("PIXELU_EMBED") == QStringLiteral("1");
  return embed_mode;
#else
  return false;
#endif
}

// True when RetroPixels has taken the window: it is a child window, or has a parent other than
// the desktop.
bool RenderWidget::IsEmbedDocked() const
{
#ifdef _WIN32
  if (!IsEmbedMode())
    return false;

  // internalWinId() is 0 if the native window doesn't exist yet (it never creates one).
  const HWND hwnd = reinterpret_cast<HWND>(internalWinId());
  if (!hwnd)
    return false;

  if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0)
    return true;

  return GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow();
#else
  return false;
#endif
}

void RenderWidget::ShowForEmbedding(const QByteArray& geometry)
{
#ifdef _WIN32
  // Already docked (e.g. shown again for a second boot): leave RetroPixels' placement alone.
  if (IsEmbedDocked())
  {
    show();
    return;
  }

  // Use the saved size (if any), but not the saved position: park the window off-screen.
  // Don't steal focus from RetroPixels with a window nobody can see.
  restoreGeometry(geometry);
  setAttribute(Qt::WA_ShowWithoutActivating);
  move(EMBED_OFFSCREEN_POS, EMBED_OFFSCREEN_POS);

  // Create the native window now, while it is still hidden, so we can change its style before
  // Windows ever shows it. (WA_NativeWindow is set, so winId() creates it.)
  const HWND hwnd = reinterpret_cast<HWND>(winId());
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  m_embed_had_appwindow = (ex_style & WS_EX_APPWINDOW) != 0;
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex_style & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);

  showNormal();

  // In case Qt or Windows placed it somewhere else while showing it.
  m_embed_pending = true;
  KeepEmbedOffscreen();

  if (!m_embed_poll_timer)
  {
    m_embed_poll_timer = new QTimer(this);
    m_embed_poll_timer->setInterval(EMBED_POLL_INTERVAL_MS);
    connect(m_embed_poll_timer, &QTimer::timeout, this, &RenderWidget::CheckEmbedDocked);

    m_embed_fallback_timer = new QTimer(this);
    m_embed_fallback_timer->setSingleShot(true);
    m_embed_fallback_timer->setInterval(EMBED_FALLBACK_MS);
    connect(m_embed_fallback_timer, &QTimer::timeout, this, &RenderWidget::ShowEmbedFallback);
  }
  m_embed_poll_timer->start();
  m_embed_fallback_timer->start();
#else
  showNormal();
  restoreGeometry(geometry);
#endif
}

// Runs every 100 ms while we wait to be docked.
void RenderWidget::CheckEmbedDocked()
{
  if (m_embed_pending && !IsEmbedDocked())
  {
    KeepEmbedOffscreen();
    return;
  }

  // Docked (or no longer waiting): stop watching and cancel the fallback.
  m_embed_pending = false;
  if (m_embed_poll_timer)
    m_embed_poll_timer->stop();
  if (m_embed_fallback_timer)
    m_embed_fallback_timer->stop();
}

// While waiting to be docked, put the window back at the off-screen spot if it moved.
// Does nothing outside embed mode, once docked, or after the fallback has shown the window.
void RenderWidget::KeepEmbedOffscreen()
{
#ifdef _WIN32
  if (!m_embed_pending || IsEmbedDocked())
    return;

  const HWND hwnd = reinterpret_cast<HWND>(internalWinId());
  if (!hwnd)
    return;

  RECT rect;
  if (GetWindowRect(hwnd, &rect) &&
      (rect.left != EMBED_OFFSCREEN_POS || rect.top != EMBED_OFFSCREEN_POS))
  {
    SetWindowPos(hwnd, nullptr, EMBED_OFFSCREEN_POS, EMBED_OFFSCREEN_POS, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
#endif
}

// Not docked within 20 seconds: show it as the normal window it would otherwise have been.
void RenderWidget::ShowEmbedFallback()
{
#ifdef _WIN32
  if (!m_embed_pending)
    return;
  m_embed_pending = false;
  if (m_embed_poll_timer)
    m_embed_poll_timer->stop();

  if (IsEmbedDocked())
    return;

  const HWND hwnd = reinterpret_cast<HWND>(internalWinId());
  if (!hwnd)
    return;

  // The taskbar only notices a WS_EX_TOOLWINDOW change when the window is shown again, so hide
  // it briefly (it is off-screen anyway), restore the normal style, centre it, show it again.
  ShowWindow(hwnd, SW_HIDE);
  LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW;
  if (m_embed_had_appwindow)
    ex_style |= WS_EX_APPWINDOW;
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

  setAttribute(Qt::WA_ShowWithoutActivating, false);
  if (QScreen* screen = QGuiApplication::primaryScreen())
  {
    QRect frame = frameGeometry();
    frame.moveCenter(screen->availableGeometry().center());
    move(frame.topLeft());
  }

  ShowWindow(hwnd, SW_SHOW);
  raise();
  activateWindow();
#endif
}

void RenderWidget::OnHideCursorChanged()
{
  UpdateCursor();
}

void RenderWidget::OnLockCursorChanged()
{
  SetCursorLocked(false);
  UpdateCursor();
}

// Calling this at any time will set the cursor (image) to the correct state
void RenderWidget::UpdateCursor()
{
  if (!Settings::Instance().GetLockCursor())
  {
    // Only hide if the cursor is automatically locking (it will hide on lock).
    // "Unhide" the cursor if we lost focus, otherwise it will disappear when hovering
    // on top of the game window in the background
    const bool keep_on_top = (windowFlags() & Qt::WindowStaysOnTopHint) != 0;
    const bool should_hide =
        (Settings::Instance().GetCursorVisibility() == Config::ShowCursor::Never) &&
        (keep_on_top || Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) || isActiveWindow());
    setCursor(should_hide ? Qt::BlankCursor : Qt::ArrowCursor);
  }
  else
  {
    setCursor((m_cursor_locked &&
               Settings::Instance().GetCursorVisibility() == Config::ShowCursor::Never) ?
                  Qt::BlankCursor :
                  Qt::ArrowCursor);
  }
}

void RenderWidget::OnKeepOnTopChanged(bool top)
{
  const bool was_visible = isVisible();

  setWindowFlags(top ? windowFlags() | Qt::WindowStaysOnTopHint :
                       windowFlags() & ~Qt::WindowStaysOnTopHint);

  m_dont_lock_cursor_on_show = true;
  if (was_visible)
    show();
  m_dont_lock_cursor_on_show = false;

  UpdateCursor();
}

void RenderWidget::HandleCursorTimer()
{
  if (!isActiveWindow())
    return;
  if ((!Settings::Instance().GetLockCursor() || m_cursor_locked) &&
      Settings::Instance().GetCursorVisibility() == Config::ShowCursor::OnMovement)
  {
    setCursor(Qt::BlankCursor);
  }
}

void RenderWidget::showFullScreen()
{
  QWidget::showFullScreen();

  QScreen* screen = window()->windowHandle()->screen();

  const auto dpr = screen->devicePixelRatio();

  emit SizeChanged(width() * dpr, height() * dpr);
}

// Lock the cursor within the window/widget internal borders, including the aspect ratio if wanted
void RenderWidget::SetCursorLocked(bool locked, bool follow_aspect_ratio)
{
  // It seems like QT doesn't scale the window frame correctly with some DPIs
  // so it might happen that the locked cursor can be on the frame of the window,
  // being able to resize it, but that is a minor problem.
  // As a hack, if necessary, we could always scale down the size by 2 pixel, to a min of 1 given
  // that the size can be 0 already. We probably shouldn't scale axes already scaled by aspect ratio
  QRect render_rect = geometry();
  if (parentWidget())
  {
    render_rect.moveTopLeft(parentWidget()->mapToGlobal(render_rect.topLeft()));
  }
  auto scale = devicePixelRatioF();  // Seems to always be rounded on Win. Should we round results?
  QPoint screen_offset = QPoint(0, 0);
  if (window()->windowHandle() && window()->windowHandle()->screen())
  {
    screen_offset = window()->windowHandle()->screen()->geometry().topLeft();
  }
  render_rect.moveTopLeft(((render_rect.topLeft() - screen_offset) * scale) + screen_offset);
  render_rect.setSize(render_rect.size() * scale);

  if (follow_aspect_ratio)
  {
    // TODO: SetCursorLocked() should be re-called every time this value is changed?
    // This might cause imprecisions of one pixel (but it won't cause the cursor to go over borders)
    Common::Vec2 aspect_ratio = g_controller_interface.GetWindowInputScale();
    if (aspect_ratio.x > 1.f)
    {
      const float new_half_width = float(render_rect.width()) / (aspect_ratio.x * 2.f);
      // Only ceil if it was >= 0.25
      const float ceiled_new_half_width = std::ceil(std::round(new_half_width * 2.f) / 2.f);
      const int x_center = render_rect.center().x();
      // Make a guess on which one to floor and ceil.
      // For more precision, we should have kept the rounding point scale from above as well.
      render_rect.setLeft(x_center - std::floor(new_half_width));
      render_rect.setRight(x_center + ceiled_new_half_width);
    }
    if (aspect_ratio.y > 1.f)
    {
      const float new_half_height = render_rect.height() / (aspect_ratio.y * 2.f);
      const float ceiled_new_half_height = std::ceil(std::round(new_half_height * 2.f) / 2.f);
      const int y_center = render_rect.center().y();
      render_rect.setTop(y_center - std::floor(new_half_height));
      render_rect.setBottom(y_center + ceiled_new_half_height);
    }
  }

  if (locked)
  {
#ifdef _WIN32
    RECT rect;
    rect.left = render_rect.left();
    rect.right = render_rect.right();
    rect.top = render_rect.top();
    rect.bottom = render_rect.bottom();

    if (ClipCursor(&rect))
#else
    // TODO: Implement on other platforms. XGrabPointer on Linux X11 should be equivalent to
    // ClipCursor on Windows, though XFixesCreatePointerBarrier and XFixesDestroyPointerBarrier
    // may also work. On Wayland zwp_pointer_constraints_v1::confine_pointer and
    // zwp_pointer_constraints_v1::destroy provide this functionality.
    // More info:
    // https://stackoverflow.com/a/36269507
    // https://tronche.com/gui/x/xlib/input/XGrabPointer.html
    // https://www.x.org/releases/X11R7.7/doc/fixesproto/fixesproto.txt
    // https://wayland.app/protocols/pointer-constraints-unstable-v1

    // The setting is hidden in the UI if not implemented
    if (false)
#endif
    {
      m_cursor_locked = true;

      if (Settings::Instance().GetCursorVisibility() != Config::ShowCursor::Constantly)
      {
        setCursor(Qt::BlankCursor);
      }

      Host::GetInstance()->SetRenderFullFocus(true);
    }
  }
  else
  {
#ifdef _WIN32
    ClipCursor(nullptr);
#endif

    if (m_cursor_locked)
    {
      m_cursor_locked = false;

      if (!Settings::Instance().GetLockCursor())
      {
        return;
      }

      // Center the mouse in the window if it's still active
      // Leave it where it was otherwise, e.g. a prompt has opened or we alt tabbed.
      if (isActiveWindow())
      {
        cursor().setPos(render_rect.left() + render_rect.width() / 2,
                        render_rect.top() + render_rect.height() / 2);
      }

      // Show the cursor or the user won't know the mouse is now unlocked
      setCursor(Qt::ArrowCursor);

      Host::GetInstance()->SetRenderFullFocus(false);
    }
  }
}

void RenderWidget::SetCursorLockedOnNextActivation(bool locked)
{
  if (Settings::Instance().GetLockCursor())
  {
    m_lock_cursor_on_next_activation = locked;
    return;
  }
  m_lock_cursor_on_next_activation = false;
}

void RenderWidget::SetWaitingForMessageBox(bool waiting_for_message_box)
{
  if (m_waiting_for_message_box == waiting_for_message_box)
  {
    return;
  }
  m_waiting_for_message_box = waiting_for_message_box;
  if (!m_waiting_for_message_box && m_lock_cursor_on_next_activation && isActiveWindow())
  {
    if (Settings::Instance().GetLockCursor())
    {
      SetCursorLocked(true);
    }
    m_lock_cursor_on_next_activation = false;
  }
}

bool RenderWidget::event(QEvent* event)
{
  PassEventToPresenter(event);

  switch (event->type())
  {
  case QEvent::KeyPress:
  {
    QKeyEvent* ke = static_cast<QKeyEvent*>(event);
    if (ke->key() == Qt::Key_Escape)
      emit EscapePressed();

    // The render window might flicker on some platforms because Qt tries to change focus to a new
    // element when there is none (?) Handling this event before it reaches QWidget fixes the issue.
    if (ke->key() == Qt::Key_Tab)
      return true;

    break;
  }
  // Needed in case a new window open and it moves the mouse
  case QEvent::WindowBlocked:
    SetCursorLocked(false);
    break;
  case QEvent::MouseButtonPress:

    // Grab focus to stop unwanted keyboard input UI interaction.
    setFocus();

    if (isActiveWindow())
    {
      // Lock the cursor with any mouse button click (behave the same as window focus change).
      // This event is occasionally missed because isActiveWindow is laggy
      if (Settings::Instance().GetLockCursor())
      {
        SetCursorLocked(true);
      }
    }
    break;
  case QEvent::MouseMove:
    // Unhide on movement
    if (Settings::Instance().GetCursorVisibility() == Config::ShowCursor::OnMovement)
    {
      setCursor(Qt::ArrowCursor);
      m_mouse_timer->start(MOUSE_HIDE_DELAY);
    }
    break;
  case QEvent::WinIdChange:
    emit HandleChanged(reinterpret_cast<void*>(winId()));
    break;
  case QEvent::Show:
    // Don't do if "stay on top" changed (or was true)
    if (Settings::Instance().GetLockCursor() &&
        Settings::Instance().GetCursorVisibility() != Config::ShowCursor::Constantly &&
        !m_dont_lock_cursor_on_show)
    {
      // Auto lock when this window is shown (it was hidden)
      if (isActiveWindow())
        SetCursorLocked(true);
      else
        SetCursorLockedOnNextActivation();
    }
    break;
  // Note that this event in Windows is not always aligned to the window that is highlighted,
  // it's the window that has keyboard and mouse focus
  case QEvent::WindowActivate:
    if (m_should_unpause_on_focus &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Paused)
    {
      Core::SetState(Core::System::GetInstance(), Core::State::Running);
    }

    m_should_unpause_on_focus = false;

    UpdateCursor();

    // Avoid "race conditions" with message boxes
    if (m_lock_cursor_on_next_activation && !m_waiting_for_message_box)
    {
      if (Settings::Instance().GetLockCursor())
      {
        SetCursorLocked(true);
      }
      m_lock_cursor_on_next_activation = false;
    }

    emit FocusChanged(true);
    break;
  case QEvent::WindowDeactivate:
    SetCursorLocked(false);

    UpdateCursor();

    if (Config::Get(Config::MAIN_PAUSE_ON_FOCUS_LOST) &&
        Core::GetState(Core::System::GetInstance()) == Core::State::Running)
    {
      // If we are declared as the CPU or GPU thread, it means that the real CPU or GPU thread
      // is waiting for us to finish showing a panic alert (with that panic alert likely being
      // the cause of this event), so trying to pause the core would cause a deadlock
      if (!Core::IsCPUThread() && !Core::IsGPUThread())
      {
        m_should_unpause_on_focus = true;
        Core::SetState(Core::System::GetInstance(), Core::State::Paused);
      }
    }

    emit FocusChanged(false);
    break;
  case QEvent::Move:
    // PixelU: if anything moved us while we wait to be docked, park us off-screen again.
    KeepEmbedOffscreen();
    SetCursorLocked(m_cursor_locked);
    break;

  // According to https://bugreports.qt.io/browse/QTBUG-95925 the recommended practice for
  // handling DPI change is responding to paint events
  case QEvent::Paint:
  case QEvent::Resize:
  {
    SetCursorLocked(m_cursor_locked);

    const QResizeEvent* se = static_cast<QResizeEvent*>(event);
    QSize new_size = se->size();

    QScreen* screen = window()->windowHandle()->screen();

    const float dpr = screen->devicePixelRatio();
    const int width = new_size.width() * dpr;
    const int height = new_size.height() * dpr;

    if (m_last_window_width != width || m_last_window_height != height ||
        m_last_window_scale != dpr)
    {
      m_last_window_width = width;
      m_last_window_height = height;
      m_last_window_scale = dpr;
      emit SizeChanged(width, height);
    }
    break;
  }
  // Happens when we add/remove the widget from the main window instead of the dedicated one
  case QEvent::ParentChange:
    SetCursorLocked(false);
    break;
  case QEvent::WindowStateChange:
    // Lock the mouse again when fullscreen changes (we might have missed some events)
    SetCursorLocked(m_cursor_locked || (isFullScreen() && Settings::Instance().GetLockCursor()));
    emit StateChanged(isFullScreen());
    break;
  case QEvent::Close:
    emit Closed();
    break;
  default:
    break;
  }
  return QWidget::event(event);
}

void RenderWidget::PassEventToPresenter(const QEvent* event)
{
  if (!Core::IsRunning(Core::System::GetInstance()))
    return;

  switch (event->type())
  {
  case QEvent::KeyPress:
  case QEvent::KeyRelease:
  {
    // As the imgui KeysDown array is only 512 elements wide, and some Qt keys which
    // we need to track (e.g. alt) are above this value, we mask the lower 9 bits.
    // Even masked, the key codes are still unique, so conflicts aren't an issue.
    // The actual text input goes through AddInputCharactersUTF8().
    const QKeyEvent* key_event = static_cast<const QKeyEvent*>(event);
    const bool is_down = event->type() == QEvent::KeyPress;
    const u32 key = static_cast<u32>(key_event->key() & 0x1FF);

    const char* chars = nullptr;
    QByteArray utf8;

    if (is_down)
    {
      utf8 = key_event->text().toUtf8();

      if (utf8.size())
        chars = utf8.constData();
    }

    // Pass the key onto Presenter (for the imgui UI)
    g_presenter->SetKey(key, is_down, chars);
  }
  break;

  case QEvent::MouseMove:
  {
    // Qt multiplies all coordinates by the scaling factor in highdpi mode, giving us "scaled" mouse
    // coordinates (as if the screen was standard dpi). We need to update the mouse position in
    // native coordinates, as the UI (and game) is rendered at native resolution.
    const float scale = devicePixelRatio();
    float x = static_cast<const QMouseEvent*>(event)->pos().x() * scale;
    float y = static_cast<const QMouseEvent*>(event)->pos().y() * scale;

    g_presenter->SetMousePos(x, y);
  }
  break;

  case QEvent::MouseButtonPress:
  case QEvent::MouseButtonRelease:
  {
    const u32 button_mask = static_cast<u32>(static_cast<const QMouseEvent*>(event)->buttons());
    g_presenter->SetMousePress(button_mask);
  }
  break;

  default:
    break;
  }
}

void RenderWidget::SetPresenterKeyMap()
{
  static constexpr DolphinKeyMap key_map = {
      Qt::Key_Tab,    Qt::Key_Left,      Qt::Key_Right, Qt::Key_Up,     Qt::Key_Down,
      Qt::Key_PageUp, Qt::Key_PageDown,  Qt::Key_Home,  Qt::Key_End,    Qt::Key_Insert,
      Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Space, Qt::Key_Return, Qt::Key_Escape,
      Qt::Key_Enter,  // Keypad enter
      Qt::Key_A,      Qt::Key_C,         Qt::Key_V,     Qt::Key_X,      Qt::Key_Y,
      Qt::Key_Z,
  };

  g_presenter->SetKeyMap(key_map);
}
