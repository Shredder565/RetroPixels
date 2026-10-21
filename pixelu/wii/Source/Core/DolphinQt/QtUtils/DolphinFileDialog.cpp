// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/DolphinFileDialog.h"

#include <QFileDialog>
#include <QObject>

namespace DolphinFileDialog
{
namespace
{
// Runs one QFileDialog helper with hotkeys switched off around it.
template <typename Fn>
auto WithoutHotkeys(Fn&& show_dialog)
{
  const HotkeyDisabler no_hotkeys;
  return show_dialog();
}
}  // namespace

QString getExistingDirectory(QWidget* parent, const QString& caption, const QString& dir,
                             QFileDialog::Options options)
{
  return WithoutHotkeys(
      [&] { return QFileDialog::getExistingDirectory(parent, caption, dir, options); });
}

QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir,
                        const QString& filter, QString* selectedFilter,
                        QFileDialog::Options options)
{
  return WithoutHotkeys([&] {
    return QFileDialog::getOpenFileName(parent, caption, dir, filter, selectedFilter, options);
  });
}

QStringList getOpenFileNames(QWidget* parent, const QString& caption, const QString& dir,
                             const QString& filter, QString* selectedFilter,
                             QFileDialog::Options options)
{
  return WithoutHotkeys([&] {
    return QFileDialog::getOpenFileNames(parent, caption, dir, filter, selectedFilter, options);
  });
}

QString getSaveFileName(QWidget* parent, const QString& caption, const QString& dir,
                        const QString& filter, QString* selectedFilter,
                        QFileDialog::Options options)
{
  return WithoutHotkeys([&] {
    return QFileDialog::getSaveFileName(parent, caption, dir, filter, selectedFilter, options);
  });
}

}  // namespace DolphinFileDialog
