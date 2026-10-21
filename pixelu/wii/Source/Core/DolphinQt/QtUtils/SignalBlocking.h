// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSignalBlocker>

// SignalBlocking(widget)->setValue(x) changes a widget without emitting its change signals: the
// returned proxy blocks the object's signals until the end of the full expression.
template <typename T>
class SignalBlockerProxy
{
public:
  explicit SignalBlockerProxy(T* object) : m_target(object), m_block(object) {}

  SignalBlockerProxy(SignalBlockerProxy&&) = default;
  SignalBlockerProxy& operator=(SignalBlockerProxy&&) = default;
  SignalBlockerProxy(const SignalBlockerProxy&) = delete;
  SignalBlockerProxy& operator=(const SignalBlockerProxy&) = delete;
  ~SignalBlockerProxy() = default;

  T* operator->() const { return m_target; }

private:
  T* m_target;
  QSignalBlocker m_block;
};

template <typename T>
SignalBlockerProxy<T> SignalBlocking(T* object)
{
  return SignalBlockerProxy<T>(object);
}
