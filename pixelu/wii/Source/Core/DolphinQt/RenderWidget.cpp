// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/RenderWidget.h"

#include <cmath>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>
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

// PixelU: docking into RetroPixels.
//
// When RetroPixels runs us (environment PIXELU_EMBED=1) it looks for this window and makes it a
// child of its own window. Until then the window must not flash up on screen or in the taskbar,
// so in embed mode it is created as a tool window and shown far off-screen (it has to be shown:
// RetroPixels looks for a visible window, and the renderer needs a real surface). While waiting,
// anything that moves it gets undone; once docked, RetroPixels alone places and sizes it. If no
// dock happens within 20 seconds the window becomes an ordinary, centred window after all.
// Outside embed mode, and on other platforms, all of this is inert.
namespace
{
[[maybe_unused]] constexpr int PARKING_SPOT = -32000;
[[maybe_unused]] constexpr int DOCK_POLL_MS = 100;
[[maybe_unused]] constexpr int DOCK_TIMEOUT_MS = 20000;

// How long the cursor may sit still before "hide on movement" hides it.
constexpr int CURSOR_IDLE_MS = 3000;

#ifdef _WIN32
HWND NativeHandle(const QWidget* widget)
{
  // internalWinId() never creates the native window; it is 0 until one exists.
  return reinterpret_cast<HWND>(widget->internalWinId());
}
#endif

Config::ShowCursor CursorMode()
{
  return Settings::Instance().GetCursorVisibility();
}

bool LockCursorEnabled()
{
  return Settings::Instance().GetLockCursor();
}

Core::System& TheSystem()
{
  return Core::System::GetInstance();
}

// Narrows a value computed in float the way an implicit float-to-int conversion does.
int ToInt(float value)
{
  return static_cast<int>(value);
}
}  // namespace

RenderWidget::RenderWidget(QWidget* parent) : QWidget(parent)
{
  setWindowTitle(QStringLiteral("PixelU Wii"));
  setWindowIcon(Resources::GetAppIcon());
  setWindowRole(QStringLiteral("renderer"));
  setAcceptDrops(true);

  QPalette black_background;
  black_background.setColor(QPalette::Window, Qt::black);
  setPalette(black_background);

  Host* const host = Host::GetInstance();
  Settings* const settings = &Settings::Instance();

  connect(host, &Host::RequestTitle, this, &RenderWidget::setWindowTitle);
  connect(host, &Host::RequestRenderSize, this, [this](int width, int height) {
    // Auto-size only applies to a normal window, and (PixelU) not once RetroPixels has docked us.
    if (!Config::Get(Config::MAIN_RENDER_WINDOW_AUTOSIZE) || isFullScreen() || isMaximized())
      return;
    if (IsDockedByHost())
      return;

    // The core asks in physical pixels; Qt sizes are logical.
    const qreal scale = window()->windowHandle()->screen()->devicePixelRatio();
    resize(static_cast<int>(width / scale), static_cast<int>(height / scale));

    // PixelU: a resize keeps the window's corner where it is, which must stay off-screen.
    ParkOffscreen();
  });

  connect(settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Running)
      InstallOverlayKeyMap();
  });

  // These have to reach Host right away, even when emitted from another thread's call stack: a
  // queued call could arrive after the core has already acted on the stale state.
  connect(this, &RenderWidget::StateChanged, host, &Host::SetRenderFullscreen,
          Qt::DirectConnection);
  connect(this, &RenderWidget::HandleChanged, this, &RenderWidget::ApplyNewHandle,
          Qt::DirectConnection);
  connect(this, &RenderWidget::SizeChanged, host, &Host::ResizeSurface, Qt::DirectConnection);
  connect(this, &RenderWidget::FocusChanged, host, &Host::SetRenderFocus, Qt::DirectConnection);

  m_cursor_idle_timer = new QTimer(this);
  connect(m_cursor_idle_timer, &QTimer::timeout, this, &RenderWidget::HideIdleCursor);
  m_cursor_idle_timer->setSingleShot(true);
  setMouseTracking(true);

  connect(settings, &Settings::CursorVisibilityChanged, this, &RenderWidget::RefreshCursorShape);
  connect(settings, &Settings::LockCursorChanged, this, [this] {
    SetCursorLocked(false);
    RefreshCursorShape();
  });
  RefreshCursorShape();
  SetCursorLocked(false);
  RefreshCursorShape();

  connect(settings, &Settings::KeepWindowOnTopChanged, this, &RenderWidget::ApplyAlwaysOnTop);
  ApplyAlwaysOnTop(settings->IsKeepWindowOnTopEnabled());

  m_cursor_idle_timer->start(CURSOR_IDLE_MS);

  // The renderer draws straight into this widget's own native window; Qt paints nothing.
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_PaintOnScreen);
}

QPaintEngine* RenderWidget::paintEngine() const
{
  return nullptr;
}

// --- Drag and drop: dropping a file loads it as a save state ---------------------------------

void RenderWidget::dragEnterEvent(QDragEnterEvent* event)
{
  const QMimeData* const data = event->mimeData();
  if (data->hasUrls() && data->urls().size() == 1)
    event->acceptProposedAction();
}

void RenderWidget::dropEvent(QDropEvent* event)
{
  const QList<QUrl> urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  const QFileInfo dropped(urls.front().toLocalFile());
  const QString path = dropped.filePath();

  if (!dropped.exists() || !dropped.isReadable())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
    return;
  }
  if (!dropped.isFile())
    return;

  State::LoadAs(TheSystem(), path.toStdString());
}

// --- Native window ---------------------------------------------------------------------------

void RenderWidget::ApplyNewHandle(void* handle)
{
#ifdef _WIN32
  if (handle != nullptr)
  {
    // Windows 11 rounds window corners, which would cut into the picture.
    const DWM_WINDOW_CORNER_PREFERENCE square = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(static_cast<HWND>(handle), DWMWA_WINDOW_CORNER_PREFERENCE, &square,
                          sizeof(square));
  }
#endif
  Host::GetInstance()->SetRenderHandle(handle);
}

void RenderWidget::ApplyAlwaysOnTop(bool on_top)
{
  const bool shown = isVisible();

  Qt::WindowFlags flags = windowFlags();
  flags.setFlag(Qt::WindowStaysOnTopHint, on_top);
  setWindowFlags(flags);

  // Changing window flags hides the window; bring it back without treating that as a fresh show
  // (which would grab the cursor).
  m_suppress_lock_on_show = true;
  if (shown)
    show();
  m_suppress_lock_on_show = false;

  RefreshCursorShape();
}

void RenderWidget::showFullScreen()
{
  QWidget::showFullScreen();

  const qreal scale = window()->windowHandle()->screen()->devicePixelRatio();
  emit SizeChanged(static_cast<int>(width() * scale), static_cast<int>(height() * scale));
}

// --- Cursor ----------------------------------------------------------------------------------

void RenderWidget::RefreshCursorShape()
{
  const bool never_show = CursorMode() == Config::ShowCursor::Never;

  bool hide;
  if (LockCursorEnabled())
  {
    // With cursor locking the cursor disappears only while it is actually locked.
    hide = m_cursor_locked && never_show;
  }
  else
  {
    // Without locking, a "never show" cursor is hidden while this window is in use. A window in
    // the background keeps a visible cursor unless it stays on top or takes background input.
    const bool on_top = windowFlags().testFlag(Qt::WindowStaysOnTopHint);
    hide = never_show &&
           (on_top || Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT) || isActiveWindow());
  }
  setCursor(hide ? Qt::BlankCursor : Qt::ArrowCursor);
}

void RenderWidget::HideIdleCursor()
{
  if (!isActiveWindow())
    return;
  if (CursorMode() != Config::ShowCursor::OnMovement)
    return;
  // A cursor that is free to leave the window (lock enabled but not locked) stays visible.
  if (LockCursorEnabled() && !m_cursor_locked)
    return;
  setCursor(Qt::BlankCursor);
}

void RenderWidget::SetCursorLocked(bool locked, bool follow_aspect_ratio)
{
  // The area the cursor is confined to, in physical screen pixels: the widget's rectangle in
  // global coordinates, scaled about the origin of its screen.
  QRect area = geometry();
  if (QWidget* const parent = parentWidget())
    area.moveTopLeft(parent->mapToGlobal(area.topLeft()));

  const qreal scale = devicePixelRatioF();
  QPoint screen_origin(0, 0);
  if (const QWindow* const handle = window()->windowHandle(); handle && handle->screen())
    screen_origin = handle->screen()->geometry().topLeft();
  area.moveTopLeft((area.topLeft() - screen_origin) * scale + screen_origin);
  area.setSize(area.size() * scale);

  // Leave out the black bars when the picture is narrower or shorter than the window. The half
  // extent is floored on one side and rounded up (from a quarter pixel on) on the other.
  if (follow_aspect_ratio)
  {
    const Common::Vec2 input_scale = g_controller_interface.GetWindowInputScale();
    if (input_scale.x > 1.f)
    {
      const float half = static_cast<float>(area.width()) / (input_scale.x * 2.f);
      const float half_up = std::ceil(std::round(half * 2.f) / 2.f);
      const float center = static_cast<float>(area.center().x());
      area.setLeft(ToInt(center - std::floor(half)));
      area.setRight(ToInt(center + half_up));
    }
    if (input_scale.y > 1.f)
    {
      const float half = static_cast<float>(area.height()) / (input_scale.y * 2.f);
      const float half_up = std::ceil(std::round(half * 2.f) / 2.f);
      const float center = static_cast<float>(area.center().y());
      area.setTop(ToInt(center - std::floor(half)));
      area.setBottom(ToInt(center + half_up));
    }
  }

  if (!locked)
  {
#ifdef _WIN32
    ClipCursor(nullptr);
#endif
    if (!m_cursor_locked)
      return;
    m_cursor_locked = false;

    if (!LockCursorEnabled())
      return;

    // Put the cursor in the middle of the game, unless focus went elsewhere (a dialog, another
    // application), in which case it stays where the user left it.
    if (isActiveWindow())
      cursor().setPos(area.left() + area.width() / 2, area.top() + area.height() / 2);

    // Make the freed cursor visible again.
    setCursor(Qt::ArrowCursor);
    Host::GetInstance()->SetRenderFullFocus(false);
    return;
  }

  // Confining the cursor is only implemented for Windows (the option is hidden elsewhere).
  bool confined = false;
#ifdef _WIN32
  RECT clip;
  clip.left = area.left();
  clip.top = area.top();
  clip.right = area.right();
  clip.bottom = area.bottom();
  confined = ClipCursor(&clip) != FALSE;
#endif
  if (!confined)
    return;

  m_cursor_locked = true;
  if (CursorMode() != Config::ShowCursor::Constantly)
    setCursor(Qt::BlankCursor);
  Host::GetInstance()->SetRenderFullFocus(true);
}

void RenderWidget::SetCursorLockedOnNextActivation(bool locked)
{
  m_lock_when_activated = LockCursorEnabled() && locked;
}

void RenderWidget::SetWaitingForMessageBox(bool waiting_for_message_box)
{
  if (waiting_for_message_box == m_message_box_open)
    return;
  m_message_box_open = waiting_for_message_box;

  // A lock that was postponed because of the message box can happen now.
  if (m_message_box_open || !m_lock_when_activated || !isActiveWindow())
    return;
  if (LockCursorEnabled())
    SetCursorLocked(true);
  m_lock_when_activated = false;
}

// --- Events ----------------------------------------------------------------------------------

void RenderWidget::ReportSizeIfChanged(QSize logical_size)
{
  const float scale = static_cast<float>(window()->windowHandle()->screen()->devicePixelRatio());
  const int width = ToInt(static_cast<float>(logical_size.width()) * scale);
  const int height = ToInt(static_cast<float>(logical_size.height()) * scale);

  const bool same = width == m_reported_width && height == m_reported_height &&
                    scale == m_reported_scale;
  if (same)
    return;

  m_reported_width = width;
  m_reported_height = height;
  m_reported_scale = scale;
  emit SizeChanged(width, height);
}

bool RenderWidget::event(QEvent* event)
{
  ForwardToOverlay(event);

  switch (event->type())
  {
  case QEvent::KeyPress:
  {
    const int key = static_cast<QKeyEvent*>(event)->key();
    if (key == Qt::Key_Escape)
      emit EscapePressed();
    // Qt would try to move focus on Tab, which makes the window flicker; there is nothing in
    // here to move focus to anyway.
    if (key == Qt::Key_Tab)
      return true;
    break;
  }

  case QEvent::WindowBlocked:
    // A modal window opened (and may move the cursor).
    SetCursorLocked(false);
    break;

  case QEvent::MouseButtonPress:
    // Keep keyboard input away from other widgets once the game is clicked.
    setFocus();
    // Clicking locks the cursor too; activation alone is sometimes reported too late.
    if (isActiveWindow() && LockCursorEnabled())
      SetCursorLocked(true);
    break;

  case QEvent::MouseMove:
    if (CursorMode() == Config::ShowCursor::OnMovement)
    {
      setCursor(Qt::ArrowCursor);
      m_cursor_idle_timer->start(CURSOR_IDLE_MS);
    }
    break;

  case QEvent::WinIdChange:
    emit HandleChanged(reinterpret_cast<void*>(winId()));
    break;

  case QEvent::Show:
    // Lock the cursor when the window appears, now or when it is next activated; not when the
    // window was only re-shown after an always-on-top change.
    if (LockCursorEnabled() && CursorMode() != Config::ShowCursor::Constantly &&
        !m_suppress_lock_on_show)
    {
      if (isActiveWindow())
        SetCursorLocked(true);
      else
        SetCursorLockedOnNextActivation();
    }
    break;

  case QEvent::WindowActivate:
    // (On Windows this follows keyboard and mouse focus rather than the highlighted window.)
    if (m_resume_when_activated && Core::GetState(TheSystem()) == Core::State::Paused)
      Core::SetState(TheSystem(), Core::State::Running);
    m_resume_when_activated = false;

    RefreshCursorShape();

    // A lock postponed until now, unless a message box is about to take focus again.
    if (m_lock_when_activated && !m_message_box_open)
    {
      if (LockCursorEnabled())
        SetCursorLocked(true);
      m_lock_when_activated = false;
    }

    emit FocusChanged(true);
    break;

  case QEvent::WindowDeactivate:
    SetCursorLocked(false);
    RefreshCursorShape();

    // Pause on focus loss. Not when this thread stands in for the CPU or GPU thread (while a
    // panic alert from that thread is up): pausing would wait for ourselves.
    if (Config::Get(Config::MAIN_PAUSE_ON_FOCUS_LOST) &&
        Core::GetState(TheSystem()) == Core::State::Running && !Core::IsCPUThread() &&
        !Core::IsGPUThread())
    {
      m_resume_when_activated = true;
      Core::SetState(TheSystem(), Core::State::Paused);
    }

    emit FocusChanged(false);
    break;

  case QEvent::Move:
    // PixelU: undo moves while waiting to be docked.
    ParkOffscreen();
    SetCursorLocked(m_cursor_locked);
    break;

  case QEvent::Resize:
    SetCursorLocked(m_cursor_locked);
    ReportSizeIfChanged(static_cast<QResizeEvent*>(event)->size());
    break;

  case QEvent::Paint:
  {
    // Paint events are also when a DPI change becomes visible, so the size report is re-checked
    // here (QTBUG-95925). Historically this read the paint event as if it were a resize event,
    // which yields the paint rectangle's top-left corner as the "size"; that is kept as it was.
    SetCursorLocked(m_cursor_locked);
    const QRect painted = static_cast<QPaintEvent*>(event)->rect();
    ReportSizeIfChanged(QSize(painted.left(), painted.top()));
    break;
  }

  case QEvent::ParentChange:
    // Moved between the main window and a window of its own.
    SetCursorLocked(false);
    break;

  case QEvent::WindowStateChange:
    // Fullscreen changes can swallow other events; re-establish the lock.
    SetCursorLocked(m_cursor_locked || (isFullScreen() && LockCursorEnabled()));
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

// --- On-screen UI input ------------------------------------------------------------------------

void RenderWidget::ForwardToOverlay(const QEvent* event)
{
  if (!Core::IsRunning(TheSystem()))
    return;

  const QEvent::Type type = event->type();
  if (type == QEvent::KeyPress || type == QEvent::KeyRelease)
  {
    // The UI's key table has 512 entries; Qt key codes are folded into it by their low nine bits,
    // which keeps them distinct. Typed text is passed along separately.
    const auto* const key_event = static_cast<const QKeyEvent*>(event);
    const bool pressed = type == QEvent::KeyPress;
    const u32 key = static_cast<u32>(key_event->key() & 0x1FF);

    QByteArray text;
    if (pressed)
      text = key_event->text().toUtf8();
    g_presenter->SetKey(key, pressed, text.isEmpty() ? nullptr : text.constData());
  }
  else if (type == QEvent::MouseMove)
  {
    // Qt reports logical coordinates; the UI is drawn in physical pixels.
    const float scale = static_cast<float>(devicePixelRatio());
    const QPoint position = static_cast<const QMouseEvent*>(event)->pos();
    g_presenter->SetMousePos(static_cast<float>(position.x()) * scale,
                             static_cast<float>(position.y()) * scale);
  }
  else if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease)
  {
    const auto buttons = static_cast<const QMouseEvent*>(event)->buttons();
    g_presenter->SetMousePress(static_cast<u32>(buttons));
  }
}

void RenderWidget::InstallOverlayKeyMap()
{
  // Qt key codes for the keys the on-screen UI needs, in DolphinKeyMap order.
  static constexpr DolphinKeyMap OVERLAY_KEYS = {
      Qt::Key_Tab,      Qt::Key_Left,   Qt::Key_Right,  Qt::Key_Up,
      Qt::Key_Down,     Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_Home,
      Qt::Key_End,      Qt::Key_Insert, Qt::Key_Delete, Qt::Key_Backspace,
      Qt::Key_Space,    Qt::Key_Return, Qt::Key_Escape, Qt::Key_Enter,
      Qt::Key_A,        Qt::Key_C,      Qt::Key_V,      Qt::Key_X,
      Qt::Key_Y,        Qt::Key_Z,
  };
  g_presenter->SetKeyMap(OVERLAY_KEYS);
}

// --- PixelU embedding ------------------------------------------------------------------------

bool RenderWidget::IsEmbedMode()
{
#ifdef _WIN32
  static const bool embedded = qEnvironmentVariable("PIXELU_EMBED") == QStringLiteral("1");
  return embedded;
#else
  return false;
#endif
}

// Docked means RetroPixels has adopted the window: it is a child window, or its parent is
// something other than the desktop.
bool RenderWidget::IsDockedByHost() const
{
#ifdef _WIN32
  if (!IsEmbedMode())
    return false;
  const HWND hwnd = NativeHandle(this);
  if (hwnd == nullptr)
    return false;
  const bool child_style = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0;
  return child_style || GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow();
#else
  return false;
#endif
}

void RenderWidget::ShowForEmbedding(const QByteArray& geometry)
{
#ifdef _WIN32
  // Shown again (another boot) after RetroPixels already docked us: its layout stands.
  if (IsDockedByHost())
  {
    show();
    return;
  }

  // Keep the saved size but not the saved position, and do not take focus from RetroPixels.
  restoreGeometry(geometry);
  setAttribute(Qt::WA_ShowWithoutActivating);
  move(PARKING_SPOT, PARKING_SPOT);

  // Create the native window while hidden (WA_NativeWindow makes winId() do that) so that it is
  // a tool window, without a taskbar button, from the moment it first appears.
  const HWND hwnd = reinterpret_cast<HWND>(winId());
  const LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  m_had_app_window_style = (ex_style & WS_EX_APPWINDOW) != 0;
  const LONG_PTR tool_style = (ex_style & ~static_cast<LONG_PTR>(WS_EX_APPWINDOW)) |
                              static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, tool_style);

  showNormal();

  // Showing may have placed it elsewhere.
  m_waiting_for_dock = true;
  ParkOffscreen();

  if (m_dock_poll == nullptr)
  {
    m_dock_poll = new QTimer(this);
    m_dock_poll->setInterval(DOCK_POLL_MS);
    connect(m_dock_poll, &QTimer::timeout, this, &RenderWidget::PollDocking);

    m_dock_deadline = new QTimer(this);
    m_dock_deadline->setSingleShot(true);
    m_dock_deadline->setInterval(DOCK_TIMEOUT_MS);
    connect(m_dock_deadline, &QTimer::timeout, this, &RenderWidget::GiveUpDocking);
  }
  m_dock_poll->start();
  m_dock_deadline->start();
#else
  showNormal();
  restoreGeometry(geometry);
#endif
}

void RenderWidget::PollDocking()
{
  if (m_waiting_for_dock && !IsDockedByHost())
  {
    ParkOffscreen();
    return;
  }

  // Docked, or no longer waiting: nothing left to watch.
  m_waiting_for_dock = false;
  if (m_dock_poll != nullptr)
    m_dock_poll->stop();
  if (m_dock_deadline != nullptr)
    m_dock_deadline->stop();
}

void RenderWidget::ParkOffscreen()
{
#ifdef _WIN32
  if (!m_waiting_for_dock || IsDockedByHost())
    return;
  const HWND hwnd = NativeHandle(this);
  if (hwnd == nullptr)
    return;

  RECT placed;
  if (!GetWindowRect(hwnd, &placed))
    return;
  if (placed.left == PARKING_SPOT && placed.top == PARKING_SPOT)
    return;
  SetWindowPos(hwnd, nullptr, PARKING_SPOT, PARKING_SPOT, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

void RenderWidget::GiveUpDocking()
{
#ifdef _WIN32
  if (!m_waiting_for_dock)
    return;
  m_waiting_for_dock = false;
  if (m_dock_poll != nullptr)
    m_dock_poll->stop();

  if (IsDockedByHost())
    return;
  const HWND hwnd = NativeHandle(this);
  if (hwnd == nullptr)
    return;

  // The taskbar picks up the style change only when the window is shown again: hide it (it is
  // off-screen anyway), turn it back into a normal window, centre it and show it.
  ShowWindow(hwnd, SW_HIDE);
  LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  ex_style &= ~static_cast<LONG_PTR>(WS_EX_TOOLWINDOW);
  if (m_had_app_window_style)
    ex_style |= static_cast<LONG_PTR>(WS_EX_APPWINDOW);
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

  setAttribute(Qt::WA_ShowWithoutActivating, false);
  if (const QScreen* const screen = QGuiApplication::primaryScreen())
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
