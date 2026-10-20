// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace Common
{
// A callable wrapper like std::function that also accepts move-only callables (lambdas that
// own a unique_ptr, for example). It cannot be copied.
template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)>
{
public:
  using result_type = R;

  MoveOnlyFunction() = default;

  template <std::invocable<Args...> F>
  requires(!std::same_as<std::decay_t<F>, MoveOnlyFunction>)
  MoveOnlyFunction(F&& f) : m_target{std::make_unique<Holder<std::decay_t<F>>>(std::forward<F>(f))}
  {
  }

  result_type operator()(Args... args) const { return m_target->Call(std::forward<Args>(args)...); }
  explicit operator bool() const { return static_cast<bool>(m_target); }
  void swap(MoveOnlyFunction& other) { std::swap(m_target, other.m_target); }

private:
  struct Callable
  {
    virtual ~Callable() = default;
    virtual result_type Call(Args... args) = 0;
  };

  template <typename F>
  struct Holder final : Callable
  {
    template <typename G>
    explicit Holder(G&& g) : function(std::forward<G>(g))
    {
    }
    result_type Call(Args... args) override
    {
      return std::invoke(function, std::forward<Args>(args)...);
    }
    F function;
  };

  std::unique_ptr<Callable> m_target;
};

// A stateless function object type that calls the function given as template argument; lets a
// function pointer be used where a type is needed (a unique_ptr deleter, for example).
template <auto Invocable>
struct InvokerOf
{
  template <typename... Args>
  constexpr auto operator()(Args... args) const
  {
    return std::invoke(Invocable, std::forward<Args>(args)...);
  }
};
}  // namespace Common
