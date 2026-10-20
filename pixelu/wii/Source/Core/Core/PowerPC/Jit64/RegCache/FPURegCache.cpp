// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/RegCache/FPURegCache.h"

#include <array>

#include "Common/x64Reg.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"

using namespace Gen;

namespace
{
// XMM0 and XMM1 are the routines' scratch registers and are never allocated.
// XMM6-15 come first since they are callee-saved on Windows.
constexpr std::array<X64Reg, 14> FPR_ALLOCATION_ORDER = {
    XMM6, XMM7, XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, XMM2, XMM3, XMM4, XMM5};
}  // namespace

FPURegCache::FPURegCache(Jit64& jit) : RegCache{jit}
{
}

bool FPURegCache::IsImm(preg_t) const
{
  return false;
}

u32 FPURegCache::Imm32(preg_t) const
{
  ASSERT_MSG(DYNA_REC, false, "FPRs never hold immediates");
  return 0;
}

s32 FPURegCache::SImm32(preg_t) const
{
  ASSERT_MSG(DYNA_REC, false, "FPRs never hold immediates");
  return 0;
}

OpArg FPURegCache::R(preg_t preg) const
{
  const PPCCachedReg& reg = m_regs[preg];
  if (reg.IsInHostRegister())
    return Gen::R(reg.GetHostRegister());

  ASSERT_MSG(DYNA_REC, reg.IsInDefaultLocation(), "FPR {} has no valid location", preg);
  return reg.GetDefaultLocation();
}

void FPURegCache::StoreRegister(preg_t preg, const OpArg& newLoc,
                                IgnoreDiscardedRegisters ignore_discarded_registers)
{
  const PPCCachedReg& reg = m_regs[preg];
  if (!reg.IsInHostRegister())
  {
    ASSERT_MSG(DYNA_REC, ignore_discarded_registers == IgnoreDiscardedRegisters::Yes,
               "FPR {} has no host register to store", preg);
    return;
  }
  m_emitter->MOVAPD(newLoc, reg.GetHostRegister());
}

void FPURegCache::LoadRegister(preg_t preg, X64Reg newLoc)
{
  const PPCCachedReg& reg = m_regs[preg];
  ASSERT_MSG(DYNA_REC, reg.IsInDefaultLocation(), "FPR {} is not in ppcState", preg);
  m_emitter->MOVAPD(newLoc, reg.GetDefaultLocation());
}

void FPURegCache::DiscardImm(preg_t)
{
  // Nothing to forget: FPRs have no constants.
}

std::span<const X64Reg> FPURegCache::GetAllocationOrder() const
{
  return FPR_ALLOCATION_ORDER;
}

OpArg FPURegCache::GetDefaultLocation(preg_t preg) const
{
  return PPCSTATE_PS0(preg);
}

BitSet32 FPURegCache::GetRegUtilization() const
{
  return m_jit.js.op->fprInXmm;
}

// Floating-point registers read by the following instructions, up to and
// including the first one that reads `preg`.
BitSet32 FPURegCache::CountRegsIn(preg_t preg, u32 lookahead) const
{
  BitSet32 seen;
  for (u32 ahead = 1; ahead < lookahead; ++ahead)
  {
    const BitSet32 inputs = m_jit.js.op[ahead].fregsIn;
    seen |= inputs;
    if (inputs[preg])
      break;
  }
  return seen;
}
