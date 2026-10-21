// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HotkeyManager.h"

#include <QFileDialog>
#include <QObject>
#include <QString>

// Wrappers around QFileDialog's static helpers. While one of these dialogs is open, emulator
// hotkeys are switched off so that typing a file name cannot trigger them.
namespace DolphinFileDialog
{
// Turns hotkeys off for as long as it exists.
class HotkeyDisabler final
{
public:
  HotkeyDisabler() { HotkeyManagerEmu::Enable(false); }
  ~HotkeyDisabler() { HotkeyManagerEmu::Enable(true); }
};

QString getExistingDirectory(QWidget* parent = nullptr, const QString& caption = QString(),
                             const QString& dir = QString(),
                             QFileDialog::Options options = QFileDialog::ShowDirsOnly);

QString getOpenFileName(QWidget* parent = nullptr, const QString& caption = QString(),
                        const QString& dir = QString(), const QString& filter = QString(),
                        QString* selectedFilter = nullptr,
                        QFileDialog::Options options = QFileDialog::Options());

QStringList getOpenFileNames(QWidget* parent = nullptr, const QString& caption = QString(),
                             const QString& dir = QString(), const QString& filter = QString(),
                             QString* selectedFilter = nullptr,
                             QFileDialog::Options options = QFileDialog::Options());

QString getSaveFileName(QWidget* parent = nullptr, const QString& caption = QString(),
                        const QString& dir = QString(), const QString& filter = QString(),
                        QString* selectedFilter = nullptr,
                        QFileDialog::Options options = QFileDialog::Options());

}  // namespace DolphinFileDialog
