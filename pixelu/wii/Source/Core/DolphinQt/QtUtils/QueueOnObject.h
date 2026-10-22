// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <utility>

// Qt widgets may only be used from the thread they live on. These helpers hand `func` to the
// event loop of the thread that `obj` lives on, so code on any thread can safely act on it. If
// `obj` is destroyed before the call is delivered, `func` never runs.

// Returns straight away; `func` runs later on `obj`'s thread.
template <typename T, typename F>
void QueueOnObject(T* obj, F&& func)
{
  QMetaObject::invokeMethod(obj, std::forward<F>(func), Qt::QueuedConnection);
}

// Waits until `func` has run on `obj`'s thread. Never call it from that thread itself.
template <typename T, typename F>
void QueueOnObjectBlocking(T* obj, F&& func)
{
  QMetaObject::invokeMethod(obj, std::forward<F>(func), Qt::BlockingQueuedConnection);
}
