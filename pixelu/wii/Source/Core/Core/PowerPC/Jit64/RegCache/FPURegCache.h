// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"

class Jit64;

// Paired-single registers, each held as a full XMM register (ps0 in the low
// lane, ps1 in the high lane). There are no immediates.
class FPURegCache final : public RegCache
{
public:
  explicit FPURegCache(Jit64& jit);

  bool IsImm(preg_t preg) const override;
  u32 Imm32(preg_t preg) const override;
  s32 SImm32(preg_t preg) const override;

protected:
  Gen::OpArg R(preg_t preg) const override;
  Gen::OpArg GetDefaultLocation(preg_t preg) const override;
  void StoreRegister(preg_t preg, const Gen::OpArg& newLoc,
                     IgnoreDiscardedRegisters ignore_discarded_registers) override;
  void LoadRegister(preg_t preg, Gen::X64Reg newLoc) override;
  void DiscardImm(preg_t preg) override;
  std::span<const Gen::X64Reg> GetAllocationOrder() const override;
  BitSet32 GetRegUtilization() const override;
  BitSet32 CountRegsIn(preg_t preg, u32 lookahead) const override;
};
