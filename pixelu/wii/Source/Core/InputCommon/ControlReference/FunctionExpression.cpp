// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControlReference/FunctionExpression.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace ciface::ExpressionParser
{
namespace
{
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<ControlState>;

ControlState ToSeconds(Clock::duration span)
{
  return std::chrono::duration_cast<Seconds>(span).count();
}

Clock::duration FromSeconds(ControlState seconds)
{
  return std::chrono::duration_cast<Clock::duration>(Seconds(seconds));
}

bool IsHigh(ControlState value)
{
  return value > CONDITION_THRESHOLD;
}

bool IsLow(ControlState value)
{
  return value < CONDITION_THRESHOLD;
}

// A function taking between min_args and max_args arguments. `usage` is the argument list
// shown in the "Expected arguments" error.
class Builtin : public FunctionExpression
{
protected:
  Builtin(u32 min_args, u32 max_args, const char* usage)
      : m_min_args(min_args), m_max_args(max_args), m_usage(usage)
  {
  }

  ControlState Arg(u32 index) { return GetArg(index).GetValue(); }

private:
  ArgumentValidation ValidateArguments() final
  {
    const u32 count = GetArgCount();
    if (count < m_min_args || count > m_max_args)
      return ExpectedArguments{m_usage};
    return ArgumentsAreValid{};
  }

  const u32 m_min_args;
  const u32 m_max_args;
  const char* const m_usage;
};

constexpr const char* ONE_EXPRESSION = "expression";

// --- Pure arithmetic ---------------------------------------------------------------------------

// One-argument math functions: abs, sin, cos, ...
class UnaryMath final : public Builtin
{
public:
  using Operation = ControlState (*)(ControlState);

  explicit UnaryMath(Operation operation) : Builtin(1, 1, ONE_EXPRESSION), m_operation(operation)
  {
  }

  ControlState GetValue() override { return m_operation(Arg(0)); }

private:
  const Operation m_operation;
};

// "not(x)" is 1 - x, and writes to it write 1 - value to x.
class Not final : public Builtin
{
public:
  Not() : Builtin(1, 1, ONE_EXPRESSION) {}

  ControlState GetValue() override { return 1.0 - Arg(0); }
  void SetValue(ControlState value) override { GetArg(0).SetValue(1.0 - value); }
};

class Negate final : public Builtin
{
public:
  Negate() : Builtin(1, 1, ONE_EXPRESSION) {}
  ControlState GetValue() override { return 0.0 - Arg(0); }
};

class Identity final : public Builtin
{
public:
  Identity() : Builtin(1, 1, ONE_EXPRESSION) {}
  ControlState GetValue() override { return Arg(0); }
};

// The two-argument functions keep both argument reads inside one call so they are evaluated in
// the order the compiler picks for function arguments, as mappings have always seen it.
class Atan2 final : public Builtin
{
public:
  Atan2() : Builtin(2, 2, "y, x") {}
  ControlState GetValue() override { return std::atan2(Arg(0), Arg(1)); }
};

class Pow final : public Builtin
{
public:
  Pow() : Builtin(2, 2, "base, exponent") {}
  ControlState GetValue() override { return std::pow(Arg(0), Arg(1)); }
};

class Min final : public Builtin
{
public:
  Min() : Builtin(2, 2, "a, b") {}
  ControlState GetValue() override { return std::min(Arg(0), Arg(1)); }
};

class Max final : public Builtin
{
public:
  Max() : Builtin(2, 2, "a, b") {}
  ControlState GetValue() override { return std::max(Arg(0), Arg(1)); }
};

class Clamp final : public Builtin
{
public:
  Clamp() : Builtin(3, 3, "value, min, max") {}
  ControlState GetValue() override { return std::clamp(Arg(0), Arg(1), Arg(2)); }
};

// "deadzone(x, d)": cuts off |x| <= d and rescales the rest back to the full range.
class Deadzone final : public Builtin
{
public:
  Deadzone() : Builtin(2, 2, "input, amount") {}

  ControlState GetValue() override
  {
    const ControlState input = Arg(0);
    const ControlState amount = Arg(1);
    const ControlState outside = std::max(0.0, std::abs(input) - amount);
    return std::copysign(outside / (1.0 - amount), input);
  }
};

// "if(c, a, b)" (also "c ? a : b"). Only the chosen branch is evaluated, and an assignment to
// the conditional assigns to that branch.
class Conditional final : public Builtin
{
public:
  Conditional() : Builtin(3, 3, "condition, true_expression, false_expression") {}

  ControlState GetValue() override { return GetLValue()->GetValue(); }

  Expression* GetLValue() override { return &GetArg(IsHigh(Arg(0)) ? 1 : 2); }
};

// --- Stateful functions --------------------------------------------------------------------------

// "toggle(x, [reset])": flips on each press of x; a high reset forces it off.
class Toggle final : public Builtin
{
public:
  Toggle() : Builtin(1, 2, "toggle_state_input, [clear_state_input]") {}

  ControlState GetValue() override
  {
    const ControlState input = Arg(0);
    if (IsLow(input))
    {
      m_armed = true;
    }
    else if (m_armed && IsHigh(input))
    {
      m_armed = false;
      m_on = !m_on;
    }

    if (GetArgCount() == 2 && IsHigh(Arg(1)))
      m_on = false;

    return m_on;
  }

private:
  bool m_armed = false;
  bool m_on = false;
};

// "timer(period)": a saw wave rising from 0 to 1 once per period.
class Timer final : public Builtin
{
public:
  Timer() : Builtin(1, 1, "seconds") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();
    const Clock::duration since_start = now - m_cycle_start;
    const ControlState period = Arg(0);

    ControlState phase = ToSeconds(since_start) / period;
    if (std::isinf(phase) || phase < 0.0)
    {
      // A zero or negative period: restart and stay at 0.
      m_cycle_start = now;
      return 0.0;
    }

    if (phase >= 1.0)
    {
      const ControlState whole_cycles = std::floor(phase);
      m_cycle_start += FromSeconds(period * whole_cycles);
      phase -= whole_cycles;
    }
    return phase;
  }

private:
  Clock::time_point m_cycle_start = Clock::now();
};

// "smooth(x, up, [down])": follows x, moving by at most 1 per `up` seconds when rising and 1
// per `down` seconds (default: `up`) when falling.
class Smooth final : public Builtin
{
public:
  Smooth() : Builtin(2, 3, "input, seconds_up, seconds_down = seconds_up") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();
    const Clock::duration since_last = now - m_last_poll;
    m_last_poll = now;

    const ControlState target = Arg(0);
    const ControlState rise_time = Arg(1);
    const ControlState fall_time = GetArgCount() == 3 ? Arg(2) : rise_time;

    const ControlState duration = target < m_current ? fall_time : rise_time;
    const ControlState step_limit = ToSeconds(since_last) / duration;

    if (std::isinf(step_limit))
    {
      m_current = target;
    }
    else
    {
      const ControlState distance = target - m_current;
      m_current += std::copysign(std::min(step_limit, std::abs(distance)), distance);
    }
    return m_current;
  }

private:
  ControlState m_current = 0.0;
  Clock::time_point m_last_poll = Clock::now();
};

// "hold(x, seconds)": goes high once x has been held for the given time.
class Hold final : public Builtin
{
public:
  Hold() : Builtin(2, 2, "input, seconds") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();

    if (IsLow(Arg(0)))
    {
      m_fired = false;
      m_pressed_at = Clock::now();
    }
    else if (!m_fired && ToSeconds(now - m_pressed_at) >= Arg(1))
    {
      m_fired = true;
    }
    return m_fired;
  }

private:
  bool m_fired = false;
  Clock::time_point m_pressed_at = Clock::now();
};

// "tap(x, seconds, [count])": high while x is held for the count-th (default 2nd) press of a
// series that started less than `seconds` ago.
class Tap final : public Builtin
{
public:
  Tap() : Builtin(2, 3, "input, seconds, taps = 2") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();
    const ControlState since_first = ToSeconds(now - m_series_start);

    const ControlState input = Arg(0);
    const ControlState window = Arg(1);
    const bool expired = since_first > window;
    const u32 wanted = GetArgCount() == 3 ? static_cast<u32>(Arg(2) + 0.5) : 2;

    if (IsLow(input))
    {
      m_released = true;
      if (expired && m_count > 0)
        m_count = 0;
      return 0.0;
    }

    if (m_released)
    {
      if (m_count == 0)
        m_series_start = now;
      m_released = false;
      ++m_count;
    }
    return m_count == wanted;
  }

private:
  bool m_released = true;
  u32 m_count = 0;
  Clock::time_point m_series_start = Clock::now();
};

// "relative(x, speed, [limit], [$shared])": integrates x * speed over time into a value between
// 0 and `limit` (default 1; a negative limit integrates downwards and reports the negated value).
// A shared variable lets two mappings (e.g. up and down) drive the same value.
class Relative final : public Builtin
{
public:
  Relative() : Builtin(2, 4, "input, speed, [max_abs_value, [shared_state]]") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();
    const bool shared = GetArgCount() >= 4;

    if (shared)
      m_value = Arg(3);

    const ControlState elapsed = ToSeconds(now - m_last_poll);
    m_last_poll = now;

    const ControlState input = Arg(0);
    const ControlState speed = Arg(1);
    const ControlState limit = GetArgCount() >= 3 ? Arg(2) : 1.0;

    const ControlState wanted_move = input * elapsed * speed;
    const ControlState room_below = std::abs(0.0 - m_value);
    const ControlState room_above = std::abs(limit - m_value);
    const ControlState direction = std::copysign(1.0, limit);

    m_value += std::min(std::max(wanted_move, -room_below), room_above) * direction;

    if (shared)
      GetArg(3).SetValue(m_value);

    return std::max(0.0, m_value * direction);
  }

private:
  ControlState m_value = 0.0;
  Clock::time_point m_last_poll = Clock::now();
};

// "pulse(x, seconds)": each press of x keeps the output high for the given time (presses during
// a pulse extend it).
class Pulse final : public Builtin
{
public:
  Pulse() : Builtin(2, 2, "input, seconds") {}

  ControlState GetValue() override
  {
    const Clock::time_point now = Clock::now();
    const ControlState input = Arg(0);

    if (IsLow(input))
    {
      m_released = true;
    }
    else if (m_released)
    {
      m_released = false;
      const Clock::duration length = FromSeconds(Arg(1));
      if (!m_active)
      {
        m_active = true;
        m_ends_at = now + length;
      }
      else
      {
        m_ends_at += length;
      }
    }

    if (m_active && now >= m_ends_at)
      m_active = false;
    return m_active;
  }

private:
  bool m_released = false;
  bool m_active = false;
  Clock::time_point m_ends_at = Clock::now();
};

// --- Name lookup ---------------------------------------------------------------------------------

using Factory = std::unique_ptr<FunctionExpression> (*)();

template <typename T>
std::unique_ptr<FunctionExpression> Create()
{
  return std::make_unique<T>();
}

template <ControlState (*Operation)(ControlState)>
std::unique_ptr<FunctionExpression> CreateMath()
{
  return std::make_unique<UnaryMath>(Operation);
}

ControlState Abs(ControlState v)
{
  return std::abs(v);
}
ControlState Sin(ControlState v)
{
  return std::sin(v);
}
ControlState Cos(ControlState v)
{
  return std::cos(v);
}
ControlState Tan(ControlState v)
{
  return std::tan(v);
}
ControlState Asin(ControlState v)
{
  return std::asin(v);
}
ControlState Acos(ControlState v)
{
  return std::acos(v);
}
ControlState Atan(ControlState v)
{
  return std::atan(v);
}
ControlState Sqrt(ControlState v)
{
  return std::sqrt(v);
}

struct NamedFunction
{
  std::string_view name;
  Factory create;
};

constexpr std::array<NamedFunction, 25> FUNCTIONS{{
    {"abs", CreateMath<Abs>},
    {"acos", CreateMath<Acos>},
    {"asin", CreateMath<Asin>},
    {"atan", CreateMath<Atan>},
    {"atan2", Create<Atan2>},
    {"clamp", Create<Clamp>},
    {"cos", CreateMath<Cos>},
    {"deadzone", Create<Deadzone>},
    {"hold", Create<Hold>},
    {"if", Create<Conditional>},
    {"max", Create<Max>},
    {"min", Create<Min>},
    {"minus", Create<Negate>},
    {"not", Create<Not>},
    {"plus", Create<Identity>},
    {"pow", Create<Pow>},
    {"pulse", Create<Pulse>},
    {"relative", Create<Relative>},
    {"sin", CreateMath<Sin>},
    {"smooth", Create<Smooth>},
    {"sqrt", CreateMath<Sqrt>},
    {"tan", CreateMath<Tan>},
    {"tap", Create<Tap>},
    {"timer", Create<Timer>},
    {"toggle", Create<Toggle>},
}};
}  // namespace

std::unique_ptr<FunctionExpression> MakeFunctionExpression(std::string_view name)
{
  const auto entry = std::ranges::find(FUNCTIONS, name, &NamedFunction::name);
  if (entry == FUNCTIONS.end())
    return nullptr;
  return entry->create();
}

int FunctionExpression::CountNumControls() const
{
  int count = 0;
  for (const auto& argument : m_args)
    count += argument->CountNumControls();
  return count;
}

void FunctionExpression::UpdateReferences(ControlEnvironment& env)
{
  for (const auto& argument : m_args)
    argument->UpdateReferences(env);
}

void FunctionExpression::SetArguments(std::vector<std::unique_ptr<Expression>>&& args)
{
  m_args = std::move(args);
}

Expression& FunctionExpression::GetArg(u32 number)
{
  return *m_args[number];
}

u32 FunctionExpression::GetArgCount() const
{
  return static_cast<u32>(m_args.size());
}

void FunctionExpression::SetValue(ControlState)
{
}

}  // namespace ciface::ExpressionParser
