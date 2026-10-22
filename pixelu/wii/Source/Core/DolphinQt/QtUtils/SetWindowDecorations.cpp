// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#if defined(_WIN32)

#include "DolphinQt/QtUtils/SetWindowDecorations.h"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#include <dwmapi.h>

#include "DolphinQt/Settings.h"

namespace
{
// DWMWA_USE_IMMERSIVE_DARK_MODE; spelled out because older SDK headers lack the name.
constexpr DWORD DARK_TITLE_BAR_ATTRIBUTE = 20;

void ApplyDarkTitleBar(QWidget* window)
{
  if (!Settings::Instance().IsThemeDark())
    return;

  const BOOL enable = TRUE;
  DwmSetWindowAttribute(reinterpret_cast<HWND>(window->winId()), DARK_TITLE_BAR_ATTRIBUTE, &enable,
                        static_cast<DWORD>(sizeof(enable)));
}

// Installed on each top-level window: refreshes the title bar when the window is shown and when
// the application palette changes.
class TitleBarUpdater final : public QObject
{
public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override
  {
    const QEvent::Type type = event->type();
    if (type == QEvent::Show || type == QEvent::ApplicationPaletteChange)
      ApplyDarkTitleBar(static_cast<QWidget*>(watched));
    return QObject::eventFilter(watched, event);
  }
};

// Installed on the application: whenever a widget gets a native window and is a top-level
// window, hooks the TitleBarUpdater onto it. (Filtering only native-window changes keeps this
// cheap compared with inspecting every Show event of every widget.)
class TitleBarHookInstaller final : public QObject
{
public:
  using QObject::QObject;

  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (event->type() == QEvent::WinIdChange)
    {
      auto* const widget = qobject_cast<QWidget*>(watched);
      if (widget != nullptr && widget->isWindow())
        widget->installEventFilter(m_updater);
    }
    return QObject::eventFilter(watched, event);
  }

private:
  QObject* const m_updater = new TitleBarUpdater{this};
};
}  // namespace

namespace QtUtils
{
void InstallWindowDecorationFilter(QApplication* app)
{
  app->installEventFilter(new TitleBarHookInstaller{app});
}
}  // namespace QtUtils

#endif
