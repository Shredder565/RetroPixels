// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <utility>

namespace Common
{
// Runs a callable when the guard leaves scope, unless Dismiss() was called first.
// Exit() runs it early (at most once).
template <typename Callable>
class ScopeGuard final
{
public:
  ScopeGuard(Callable&& on_exit) : m_action(std::forward<Callable>(on_exit)) {}
  ScopeGuard(ScopeGuard&& other) : m_action(std::move(other.m_action)) { other.m_action.reset(); }
  ~ScopeGuard() { Exit(); }

  ScopeGuard(const ScopeGuard&) = delete;
  void operator=(const ScopeGuard&) = delete;

  void Dismiss() { m_action.reset(); }

  void Exit()
  {
    if (!m_action.has_value())
      return;
    // Disarm before calling, so a re-entrant Exit() does nothing. (Lambdas are not assignable,
    // hence move-construct + reset instead of swap.)
    Callable action(std::move(*m_action));
    m_action.reset();
    action();
  }

private:
  std::optional<Callable> m_action;
};
}  // namespace Common
