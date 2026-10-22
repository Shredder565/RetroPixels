// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QObject>

class QSocketNotifier;

// Turns SIGINT and SIGTERM (Unix) into a Qt signal on the GUI thread. A signal handler may only
// do async-signal-safe things, so HandleInterrupt() just writes one byte into a pipe; the read
// end is watched by a QSocketNotifier, which emits InterruptReceived() from the event loop.
// One SignalDaemon per program: the pipe is shared by every instance.
class SignalDaemon final : public QObject
{
  Q_OBJECT

public:
  explicit SignalDaemon(QObject* parent);
  ~SignalDaemon() override;

  // For sigaction(): async-signal-safe.
  static void HandleInterrupt(int signal_number);

signals:
  void InterruptReceived();

private:
  void Drain();

  QSocketNotifier* m_notifier = nullptr;
};
