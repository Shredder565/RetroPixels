// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Core/PowerPC/PowerPC.h"

namespace JitCommon
{
// What compile-time evaluation of one instruction found out.
struct ConstantPropagationResult final
{
  constexpr ConstantPropagationResult() = default;

  constexpr ConstantPropagationResult(s8 gpr_, u32 gpr_value_, bool compute_rc_ = false)
      : gpr_value(gpr_value_), gpr(gpr_), instruction_fully_executed(true), compute_rc(compute_rc_)
  {
  }

  // The value written to `gpr` (meaningless when gpr is negative).
  u32 gpr_value = 0;

  // The GPR the instruction writes, or -1 when nothing was evaluated or the
  // instruction writes no GPR.
  s8 gpr = -1;

  // True when the instruction's whole effect is described here, so no code has
  // to be emitted for it.
  bool instruction_fully_executed = false;

  // True when CR0 must be set from gpr_value (record forms).
  bool compute_rc = false;

  // The value written to XER[CA], when the instruction writes it.
  std::optional<bool> carry = std::nullopt;

  // The value written to XER[OV] (and ORed into SO), when the instruction writes it.
  std::optional<bool> overflow = std::nullopt;
};

// Tracks which GPRs hold values known at compile time and evaluates integer
// instructions whose inputs are known.
class ConstantPropagation final
{
public:
  // `flags` are the instruction's GekkoOPInfo flags.
  ConstantPropagationResult EvaluateInstruction(UGeckoInstruction inst, u64 flags) const;

  // Records the GPR value described by `result`, if any.
  void Apply(ConstantPropagationResult result);

  template <typename... Args>
  bool HasGPR(Args... gprs) const
  {
    return HasGPRs(BitSet32{static_cast<int>(gprs)...});
  }

  bool HasGPRs(BitSet32 gprs) const { return (m_known & gprs) == gprs; }

  u32 GetGPR(size_t gpr) const { return m_values[gpr]; }

  void SetGPR(size_t gpr, u32 value)
  {
    m_values[gpr] = value;
    m_known[gpr] = true;
  }

  template <typename... Args>
  void ClearGPR(Args... gprs)
  {
    ClearGPRs(BitSet32{static_cast<int>(gprs)...});
  }

  void ClearGPRs(BitSet32 gprs) { m_known &= ~gprs; }

  void Clear() { m_known = BitSet32{}; }

private:
  std::optional<u32> Known(u32 gpr) const
  {
    if (m_known[gpr])
      return m_values[gpr];
    return std::nullopt;
  }

  ConstantPropagationResult EvaluatePrimary(UGeckoInstruction inst) const;
  ConstantPropagationResult EvaluateExtended(UGeckoInstruction inst, u64 flags) const;
  ConstantPropagationResult EvaluateArithmetic(UGeckoInstruction inst, u64 flags) const;
  ConstantPropagationResult EvaluateLogical(UGeckoInstruction inst) const;
  ConstantPropagationResult EvaluateUnary(UGeckoInstruction inst, u64 flags) const;

  std::array<u32, 32> m_values{};
  BitSet32 m_known{};
};

}  // namespace JitCommon
