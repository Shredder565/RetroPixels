// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QObject>

// Turns QFileOpenEvents (sent by the OS when a file is opened with the application) into the
// fileOpened() signal. It watches the object it is created for, which also becomes its parent.
class FileOpenEventFilter : public QObject
{
  Q_OBJECT
public:
  explicit FileOpenEventFilter(QObject* event_source);

signals:
  void fileOpened(const QString& file_name);

private:
  bool eventFilter(QObject* object, QEvent* event) override;
};
