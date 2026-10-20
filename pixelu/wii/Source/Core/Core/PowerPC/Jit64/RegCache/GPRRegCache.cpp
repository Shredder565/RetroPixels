// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/RegCache/GPRRegCache.h"

#include <array>

#include "Common/x64Reg.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"

using namespace Gen;

namespace
{
// Callee-saved registers come first so values survive C++ calls without
// spills. RSI and RDI are callee-saved on Windows only; RCX doubles as
// RSCRATCH_EXTRA and comes last.
#ifdef _WIN32
constexpr std::array<X64Reg, 11> GPR_ALLOCATION_ORDER = {RSI, RDI, R13, R14, R15, R8,
                                                         R9,  R10, R11, R12, RCX};
#else
constexpr std::array<X64Reg, 11> GPR_ALLOCATION_ORDER = {R12, R13, R14, R15, RSI, RDI,
                                                         R8,  R9,  R10, R11, RCX};
#endif
}  // namespace

GPRRegCache::GPRRegCache(Jit64& jit) : RegCache{jit}
{
}

bool GPRRegCache::IsImm(preg_t preg) const
{
  return m_jit.GetConstantPropagation().HasGPR(preg);
}

u32 GPRRegCache::Imm32(preg_t preg) const
{
  const auto& constants = m_jit.GetConstantPropagation();
  ASSERT(constants.HasGPR(preg));
  return constants.GetGPR(preg);
}

s32 GPRRegCache::SImm32(preg_t preg) const
{
  return static_cast<s32>(Imm32(preg));
}

OpArg GPRRegCache::R(preg_t preg) const
{
  const PPCCachedReg& reg = m_regs[preg];
  if (reg.IsInHostRegister())
    return Gen::R(reg.GetHostRegister());

  const auto& constants = m_jit.GetConstantPropagation();
  if (constants.HasGPR(preg))
    return Gen::Imm32(constants.GetGPR(preg));

  ASSERT_MSG(DYNA_REC, reg.IsInDefaultLocation(), "GPR {} has no valid location", preg);
  return reg.GetDefaultLocation();
}

void GPRRegCache::StoreRegister(preg_t preg, const OpArg& new_loc,
                                IgnoreDiscardedRegisters ignore_discarded_registers)
{
  const PPCCachedReg& reg = m_regs[preg];
  const auto& constants = m_jit.GetConstantPropagation();
  if (reg.IsInHostRegister())
  {
    m_emitter->MOV(32, new_loc, Gen::R(reg.GetHostRegister()));
    return;
  }
  if (constants.HasGPR(preg))
  {
    m_emitter->MOV(32, new_loc, Gen::Imm32(constants.GetGPR(preg)));
    return;
  }
  ASSERT_MSG(DYNA_REC, ignore_discarded_registers == IgnoreDiscardedRegisters::Yes,
             "GPR {} has neither a host register nor a known value", preg);
}

void GPRRegCache::LoadRegister(preg_t preg, X64Reg new_loc)
{
  const auto& constants = m_jit.GetConstantPropagation();
  if (constants.HasGPR(preg))
  {
    m_emitter->MOV(32, Gen::R(new_loc), Gen::Imm32(constants.GetGPR(preg)));
    return;
  }

  const PPCCachedReg& reg = m_regs[preg];
  ASSERT_MSG(DYNA_REC, reg.IsInDefaultLocation(), "GPR {} is not in ppcState", preg);
  m_emitter->MOV(32, Gen::R(new_loc), reg.GetDefaultLocation());
}

void GPRRegCache::DiscardImm(preg_t preg)
{
  m_jit.GetConstantPropagation().ClearGPR(preg);
}

OpArg GPRRegCache::GetDefaultLocation(preg_t preg) const
{
  return PPCSTATE_GPR(preg);
}

std::span<const X64Reg> GPRRegCache::GetAllocationOrder() const
{
  return GPR_ALLOCATION_ORDER;
}

void GPRRegCache::SetImmediate32(preg_t preg, u32 imm_value, bool dirty)
{
  if (dirty)
    DiscardRegister(preg);
  m_jit.GetConstantPropagation().SetGPR(preg, imm_value);
}

BitSet32 GPRRegCache::GetRegUtilization() const
{
  const auto& op = *m_jit.js.op;
  return op.gprWillBeRead | op.gprWillBeWritten;
}

// Registers read by the following instructions, up to and including the
// first one that reads `preg`.
BitSet32 GPRRegCache::CountRegsIn(preg_t preg, u32 lookahead) const
{
  BitSet32 seen;
  for (u32 ahead = 1; ahead < lookahead; ++ahead)
  {
    const BitSet32 inputs = m_jit.js.op[ahead].regsIn;
    seen |= inputs;
    if (inputs[preg])
      break;
  }
  return seen;
}
