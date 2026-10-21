// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/SignalDaemon.h"

#include <array>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

#include <QSocketNotifier>

namespace
{
// [0] is read by the GUI thread, [1] written by the signal handler. -1 until the first
// SignalDaemon is made; the pipe then lives as long as the process.
std::array<int, 2> s_pipe{-1, -1};

bool OpenPipe()
{
  if (s_pipe[0] != -1)
    return true;
  std::array<int, 2> ends{};
  if (pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) != 0)
    return false;
  s_pipe = ends;
  return true;
}
}  // namespace

SignalDaemon::SignalDaemon(QObject* parent) : QObject(parent)
{
  if (!OpenPipe())
    return;
  m_notifier = new QSocketNotifier(s_pipe[0], QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this, &SignalDaemon::Drain);
}

SignalDaemon::~SignalDaemon() = default;

void SignalDaemon::HandleInterrupt(int)
{
  const int saved_errno = errno;
  if (s_pipe[1] != -1)
  {
    const char byte = 1;
    // A full pipe already holds a wake-up; losing this byte changes nothing.
    [[maybe_unused]] const ssize_t written = write(s_pipe[1], &byte, 1);
  }
  errno = saved_errno;
}

void SignalDaemon::Drain()
{
  // Several signals may have arrived since the last wake-up; they count as one request.
  std::array<char, 64> buffer{};
  bool got_any = false;
  while (read(s_pipe[0], buffer.data(), buffer.size()) > 0)
    got_any = true;
  if (got_any)
    emit InterruptReceived();
}
