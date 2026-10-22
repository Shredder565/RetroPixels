// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"

using namespace Gen;

namespace
{
// lfs/lfsu/lfd/lfdu (48..51), stfs/stfsu/stfd/stfdu (52..55) and their
// X-forms under opcode 31.
struct FloatAccessForm
{
  bool indexed;
  bool update;
  bool single;
};

FloatAccessForm DecodeFloatAccess(UGeckoInstruction inst)
{
  FloatAccessForm form{};
  form.indexed = inst.OPCD == 31;
  if (form.indexed)
  {
    form.update = (inst.SUBOP10 & 0x20) != 0;
    form.single = (inst.SUBOP10 & 0x40) == 0;
  }
  else
  {
    // An update with a zero displacement leaves rA as it is.
    form.update = (inst.OPCD & 1) != 0 && inst.SIMM_16 != 0;
    form.single = (inst.OPCD & 2) == 0;
  }
  return form;
}
}  // namespace

void Jit64::lfXXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreFloatingOff);
  const FloatAccessForm form = DecodeFloatAccess(inst);

  const u32 d = inst.RD;
  const u32 a = inst.RA;
  const u32 b = inst.RB;

  FALLBACK_IF(!form.indexed && a == 0);

  FlushRegistersBeforeSlowAccess();

  // With memchecks an update must not reach rA before the load succeeds,
  // so the address is formed in RSCRATCH2 and written back at the end.
  const bool deferred_update = form.update && jo.memcheck;

  s32 displacement = 0;
  RCOpArg address = gpr.Bind(a, form.update ? RCMode::ReadWrite : RCMode::Read);
  RegCache::Realize(address);

  if (deferred_update)
  {
    MOV(32, R(RSCRATCH2), address);
    address = RCOpArg::R(RSCRATCH2);
  }

  if (!form.indexed)
  {
    const s32 imm = static_cast<s16>(inst.SIMM_16);
    if (form.update)
      ADD(32, address, Imm32(static_cast<u32>(imm)));
    else
      displacement = imm;
  }
  else
  {
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RegCache::Realize(Rb);
    if (form.update)
    {
      ADD(32, address, Rb);
    }
    else
    {
      MOV_sum(32, RSCRATCH2, a != 0 ? address.Location() : Imm32(0), Rb);
      address = RCOpArg::R(RSCRATCH2);
    }
  }

  // lfd replaces only ps0, so ps1 must be kept; lfs writes both.
  const RCMode mode = form.single ? RCMode::Write : RCMode::ReadWrite;
  RCX64Reg Rd = (jo.memcheck && form.single) ? fpr.RevertableBind(d, mode) : fpr.Bind(d, mode);
  RegCache::Realize(Rd);

  BitSet32 live = CallerSavedRegistersInUse();
  if (deferred_update)
    live[RSCRATCH2] = true;
  SafeLoadToReg(RSCRATCH, address, form.single ? 32 : 64, displacement, live, false);

  if (!form.single)
  {
    MOVQ_xmm(XMM0, R(RSCRATCH));
    MOVSD(Rd, R(XMM0));
  }
  else
  {
    ConvertSingleToDouble(Rd, RSCRATCH, true);
  }

  if (deferred_update)
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Ra);
    MOV(32, Ra, address);
  }
}

void Jit64::stfXXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreFloatingOff);
  const FloatAccessForm form = DecodeFloatAccess(inst);

  const u32 s = inst.RS;
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const s32 imm = static_cast<s16>(inst.SIMM_16);
  const int bits = form.single ? 32 : 64;

  FALLBACK_IF(form.update && jo.memcheck && form.indexed && a == b);

  FlushRegistersBeforeSlowAccess();

  // The value to store goes to RSCRATCH first.
  if (!form.single)
  {
    RCOpArg Rs = fpr.Use(s, RCMode::Read);
    RegCache::Realize(Rs);
    if (Rs.IsSimpleReg())
      MOVQ_xmm(R(RSCRATCH), Rs.GetSimpleReg());
    else
      MOV(64, R(RSCRATCH), Rs);
  }
  else if (js.fpr_is_store_safe[s] && js.op->fprIsSingle[s])
  {
    // The double is exactly a single, so a plain conversion is exact.
    RCOpArg Rs = fpr.Use(s, RCMode::Read);
    RegCache::Realize(Rs);
    CVTSD2SS(XMM0, Rs);
    MOVD_xmm(R(RSCRATCH), XMM0);
  }
  else
  {
    // Bit-exact double-to-single conversion (the routine reads XMM0).
    RCX64Reg Rs = fpr.Bind(s, RCMode::Read);
    RegCache::Realize(Rs);
    MOVAPD(XMM0, Rs);
    CALL(asm_routines.cdts);
  }

  if (!form.indexed && (a == 0 || gpr.IsImm(a)))
  {
    const u32 address = (a != 0 ? gpr.Imm32(a) : 0) + static_cast<u32>(imm);
    const bool may_fault =
        WriteToConstAddress(bits, R(RSCRATCH), address, CallerSavedRegistersInUse());
    if (!form.update)
      return;
    if (jo.memcheck && may_fault)
    {
      RCOpArg Ra = gpr.RevertableBind(a, RCMode::Write);
      RegCache::Realize(Ra);
      MemoryExceptionCheck();
      MOV(32, Ra, Imm32(address));
    }
    else
    {
      gpr.SetImmediate32(a, address);
    }
    return;
  }

  s32 displacement = 0;
  RCOpArg Ra = form.update ? gpr.Bind(a, RCMode::ReadWrite) : gpr.Use(a, RCMode::Read);
  RegCache::Realize(Ra);
  if (form.indexed)
  {
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RegCache::Realize(Rb);
    MOV_sum(32, RSCRATCH2, a != 0 ? Ra.Location() : Imm32(0), Rb);
  }
  else if (form.update)
  {
    MOV_sum(32, RSCRATCH2, Ra, Imm32(static_cast<u32>(imm)));
  }
  else
  {
    displacement = imm;
    MOV(32, R(RSCRATCH2), Ra);
  }

  BitSet32 live = CallerSavedRegistersInUse();
  if (form.update)
    live[RSCRATCH2] = true;
  SafeWriteRegToReg(RSCRATCH, RSCRATCH2, bits, displacement, live);

  if (form.update)
    MOV(32, Ra, R(RSCRATCH2));
}

// Stores the low word of the double's bit pattern, unconverted.
void Jit64::stfiwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreFloatingOff);

  const u32 a = inst.RA;

  FlushRegistersBeforeSlowAccess();

  RCOpArg Ra = a != 0 ? gpr.Use(a, RCMode::Read) : RCOpArg::Imm32(0);
  RCOpArg Rb = gpr.Use(inst.RB, RCMode::Read);
  RCOpArg Rs = fpr.Use(inst.RS, RCMode::Read);
  RegCache::Realize(Ra, Rb, Rs);

  MOV_sum(32, RSCRATCH2, Ra, Rb);
  if (Rs.IsSimpleReg())
    MOVD_xmm(R(RSCRATCH), Rs.GetSimpleReg());
  else
    MOV(32, R(RSCRATCH), Rs);
  SafeWriteRegToReg(RSCRATCH, RSCRATCH2, 32, 0, CallerSavedRegistersInUse());
}
