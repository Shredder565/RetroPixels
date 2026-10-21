// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QObject>

namespace QtUtils
{
// Swallows key presses and releases sent to the object it is created for. It installs itself on
// that object (its parent) and goes away a moment after ScheduleRemoval is called.
class BlockKeyboardInputFilter : public QObject
{
  Q_OBJECT
public:
  BlockKeyboardInputFilter(QObject* parent);
  void ScheduleRemoval();

private:
  bool eventFilter(QObject* object, QEvent* event) override;
};

// Blocks keyboard input to `obj` until `removal_signal` of `removal_signal_object` fires.
template <typename T>
void InstallKeyboardBlocker(QObject* obj, T* removal_signal_object, void (T::*removal_signal)())
{
  auto* const blocker = new QtUtils::BlockKeyboardInputFilter{obj};
  removal_signal_object->connect(removal_signal_object, removal_signal, blocker,
                                 &QtUtils::BlockKeyboardInputFilter::ScheduleRemoval);
}
}  // namespace QtUtils
