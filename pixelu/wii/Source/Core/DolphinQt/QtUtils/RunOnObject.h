// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <concepts>
#include <functional>
#include <optional>

#include <QMetaObject>

#include "Common/OneShotEvent.h"

// Runs `functor` on the thread `object` lives on (directly, if that is the calling thread),
// waits for it, and returns its result. Returns nullopt if `object` was destroyed before the
// call could be made.
auto RunOnObject(QObject* object, std::invocable<> auto&& functor)
{
  using Result = decltype(functor());
  std::optional<Result> outcome;
  Common::OneShotEvent finished;

  // The task carries a ScopedSetter, so `finished` is set when the task is destroyed: after it
  // ran, or when Qt throws it away undelivered. Only then may we stop waiting, because the task
  // refers to `outcome` and `functor` on this stack frame.
  auto task = [&outcome, &functor, done = Common::ScopedSetter{&finished}] {
    outcome.emplace(functor());
  };
  QMetaObject::invokeMethod(object, std::move(task));

  finished.Wait();
  return outcome;
}

// Same, calling a member function of `obj` (for example RunOnObject(action, &QAction::isChecked)).
template <std::derived_from<QObject> Receiver>
auto RunOnObject(Receiver* obj, auto Receiver::* func)
{
  return RunOnObject(obj, [obj, func] { return std::invoke(func, obj); });
}
