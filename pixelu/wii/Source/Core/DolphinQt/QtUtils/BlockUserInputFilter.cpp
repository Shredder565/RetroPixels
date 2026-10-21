// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/BlockUserInputFilter.h"

#include <chrono>

#include <QEvent>
#include <QTimer>

namespace QtUtils
{
namespace
{
// The filter stays for a short while after removal is requested, so that the key that ended the
// blocking (for example Return or Space on a button) cannot immediately trigger it again.
constexpr std::chrono::milliseconds LINGER_TIME{100};
}  // namespace

BlockKeyboardInputFilter::BlockKeyboardInputFilter(QObject* parent) : QObject{parent}
{
  parent->installEventFilter(this);
}

void BlockKeyboardInputFilter::ScheduleRemoval()
{
  auto* const countdown = new QTimer(this);
  countdown->setSingleShot(true);
  connect(countdown, &QTimer::timeout, [this] { delete this; });
  countdown->start(LINGER_TIME);
}

bool BlockKeyboardInputFilter::eventFilter(QObject*, QEvent* event)
{
  switch (event->type())
  {
  case QEvent::KeyPress:
  case QEvent::KeyRelease:
    return true;
  default:
    return false;
  }
}

}  // namespace QtUtils
