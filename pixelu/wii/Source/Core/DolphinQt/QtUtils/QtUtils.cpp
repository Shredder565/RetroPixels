// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/QtUtils.h"

#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QScreen>

#if defined(QT_DBUS_LIB)
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#endif

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace
{
// Opens the folder containing the file in the system's file manager.
void OpenContainingFolder(std::string_view file_path)
{
  // SplitPath leaves the separator at the end of the folder path. That matters on Windows: given
  // a folder path without it, Explorer may instead run a .bat or .exe that sits next to the folder
  // and has the folder's name.
  std::string folder;
  SplitPath(file_path, &folder, nullptr, nullptr);
  if (folder.empty())
    return;

  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
}

#if defined(QT_DBUS_LIB)
// Asks the freedesktop file manager service to show the file. Returns whether that worked.
bool ShowWithFileManager1(std::string_view file_path)
{
  const QString dbus_name = QString::fromLatin1("org.freedesktop.DBus");
  QDBusInterface bus{dbus_name, QString::fromLatin1("/org/freedesktop/DBus"), dbus_name};
  bus.call(QString::fromLatin1("StartServiceByName"),
           QString::fromLatin1("org.freedesktop.FileManager1"), 0u);

  const QString fm_name = QString::fromLatin1("org.freedesktop.FileManager1");
  QDBusInterface file_manager{fm_name, QString::fromLatin1("/org/freedesktop/FileManager1"),
                              fm_name};
  if (!file_manager.isValid())
  {
    WARN_LOG_FMT(COMMON, "Invalid DBus interface: {}",
                 file_manager.lastError().message().toStdString());
    return false;
  }

  const QStringList uris{QUrl::fromLocalFile(QString::fromUtf8(file_path)).toString()};
  const QDBusReply<void> reply =
      file_manager.call(QString::fromLatin1("ShowItems"), uris, QString{});
  if (reply.isValid())
    return true;

  WARN_LOG_FMT(COMMON, "DBus call failed: {} - {}", reply.error().name().toStdString(),
               reply.error().message().toStdString());
  return false;
}
#endif
}  // namespace

namespace QtUtils
{
void ShowFourDigitYear(QDateTimeEdit* widget)
{
  // A two-digit year would always be read as 20xx, so widen "yy" to "yyyy" unless it already is.
  QString format = widget->displayFormat();
  if (format.contains(QStringLiteral("yyyy")))
    return;
  widget->setDisplayFormat(format.replace(QStringLiteral("yy"), QStringLiteral("yyyy")));
}

QWidget* CreateIconWarning(QWidget* parent, QStyle::StandardPixmap standard_pixmap, QLabel* label)
{
  const int icon_px = QFontMetrics(parent->font()).height() * 5 / 4;

  auto* const icon_label = new QLabel{};
  icon_label->setPixmap(parent->style()->standardIcon(standard_pixmap).pixmap(icon_px, icon_px));

  auto* const container = new QWidget;
  auto* const row = new QHBoxLayout{container};
  row->addWidget(icon_label);
  row->addWidget(label, 1);
  return container;
}

void AdjustSizeWithinScreen(QWidget* widget)
{
  const QSize limit = widget->screen()->availableSize() * 9 / 10;
  widget->resize(widget->sizeHint().boundedTo(limit));
  CenterOnParentWindow(widget);
}

void CenterOnParentWindow(QWidget* const widget)
{
  const QWidget* const parent = widget->parentWidget();
  if (parent == nullptr)
    return;

  const QRect outer = parent->window()->geometry();
  const QSize own_size = widget->geometry().size();
  const QPoint top_left = outer.topLeft() + QPoint{(outer.width() - own_size.width()) / 2,
                                                   (outer.height() - own_size.height()) / 2};
  widget->setGeometry(QRect(top_left, own_size));
}

void ShowFileInFolder(std::string_view file_path)
{
  bool shown = false;
#if defined(QT_DBUS_LIB)
  shown = ShowWithFileManager1(file_path);
#elif defined(_WIN32)
  // explorer.exe /select , <path> opens the folder with the file selected.
  const QStringList explorer_args{QString::fromLatin1("/select"), QString::fromLatin1(","),
                                  QDir::toNativeSeparators(QString::fromUtf8(file_path))};
  shown = QProcess{}.startDetached(QString::fromLatin1("explorer.exe"), explorer_args);
#elif defined(__APPLE__)
  shown = QProcess{}.startDetached(QString::fromLatin1("open"),
                                   {QString::fromLatin1("-R"), QString::fromUtf8(file_path)});
#endif

  // Fallback: no way to select the file here, or that failed.
  if (!shown)
    OpenContainingFolder(file_path);
}

}  // namespace QtUtils
