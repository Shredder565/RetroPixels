// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Breakpoint and watchpoint conditions. A condition is an arithmetic
// expression (parsed and evaluated by the bundled "expr" library) over the
// CPU's registers, plus helper functions that read and write guest memory.
// It counts as true when it evaluates to anything but zero.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/BitSet.h"

struct expr;
struct expr_var_list;

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

struct ExprDeleter
{
  void operator()(expr* expression) const;
};

using ExprPointer = std::unique_ptr<expr, ExprDeleter>;

struct ExprVarListDeleter
{
  void operator()(expr_var_list* vars) const;
};

using ExprVarListPointer = std::unique_ptr<expr_var_list, ExprVarListDeleter>;

class Expression
{
public:
  // Returns nothing if the text does not parse.
  static std::optional<Expression> TryParse(std::string_view text);

  // Loads the registers the expression names, evaluates it, and writes back
  // any register the expression assigned to.
  double Evaluate(Core::System& system) const;

  std::string GetText() const;

  // The registers the expression mentions, so the JIT can make sure they are
  // up to date in memory before a watchpoint check.
  BitSet32 GetGPRsUsed()
  {
    ComputeRegistersUsed();
    return m_gprs_used;
  }

  BitSet32 GetFPRsUsed()
  {
    ComputeRegistersUsed();
    return m_fprs_used;
  }

  // What a variable of the expression stands for.
  enum class RegisterKind
  {
    None,  // not a register name: starts at 0 on every evaluation
    GPR,
    FPR,
    SPR,
    PC,
    MSR,
  };

  struct Register
  {
    RegisterKind kind = RegisterKind::None;
    int index = 0;
  };

private:
  Expression(std::string_view text, ExprPointer ex, ExprVarListPointer vars);

  void LoadVariables(Core::System& system) const;
  void StoreVariables(Core::System& system) const;
  void LogResult(double result) const;

  void ComputeRegistersUsed();

  std::string m_text;
  ExprPointer m_expr;
  ExprVarListPointer m_vars;
  // One entry per variable, in the order of the library's variable list.
  std::vector<Register> m_registers;

  BitSet32 m_gprs_used;
  BitSet32 m_fprs_used;
  bool m_registers_used_known = false;
};

// No condition means "always".
inline bool EvaluateCondition(Core::System& system, const std::optional<Expression>& condition)
{
  if (!condition)
    return true;
  return condition->Evaluate(system) != 0.0;
}
