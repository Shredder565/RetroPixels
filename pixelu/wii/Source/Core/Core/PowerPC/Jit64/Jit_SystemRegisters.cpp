// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <array>
#include <cstddef>

#include "Common/BitSet.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/x64Emitter.h"

#include "Core/CoreTiming.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace Gen;

// Each CR field is held as a 64-bit value (see ConditionRegister.h):
//   SO  is bit 59 (CR_EMU_SO_BIT)
//   LT  is bit 62 (CR_EMU_LT_BIT)
//   EQ  is set when the low 32 bits are zero
//   GT  is set when the value, read as signed, is positive
// Bit 32 is set whenever a bit was written so that EQ alone does not make
// the value zero (which would read as "not GT" in a different way).

namespace
{
// LDMXCSR values for FPSCR[NI] and FPSCR[RN], indexed by FPSCR & 7. All
// exceptions are masked; NI selects flush-to-zero plus denormals-are-zero.
constexpr std::array<u32, 8> FPSCR_TO_MXCSR{
    0x1F80, 0x7F80, 0x5F80, 0x3F80, 0x9F80, 0xFF80, 0xDF80, 0xBF80,
};

// FPSCR bits whose change requires FEX and VX to be recomputed.
constexpr u32 SUMMARY_INPUTS = FPSCR_ANY_X | FPSCR_ANY_E;

// HID0[ICFI], the flash-invalidate bit of the instruction cache.
constexpr u8 HID0_ICFI_BIT = 31 - 20;

// mtmsr checks these pending exceptions when it turns on MSR[EE].
constexpr u32 MSR_EE = 0x8000;
constexpr u32 DELAYED_EXCEPTIONS =
    EXCEPTION_EXTERNAL_INT | EXCEPTION_PERFORMANCE_MONITOR | EXCEPTION_DECREMENTER;

OpArg CRField(int field)
{
  return PPCSTATE_CR(field);
}

OpArg CRField(u32 field)
{
  return PPCSTATE_CR(static_cast<int>(field));
}

// Splits a 5-bit CR bit number into (field, bit) in the CR helpers' terms.
int CRFieldOf(u32 crbit)
{
  return static_cast<int>(crbit >> 2);
}

int CRBitOf(u32 crbit)
{
  return 3 - static_cast<int>(crbit & 3);
}

u32 SPRIndex(UGeckoInstruction inst)
{
  return (u32{inst.SPRU} << 5) | (u32{inst.SPRL} & 0x1F);
}

CCFlags Inverted(CCFlags cc)
{
  // x86 condition codes come in pairs that differ only in the low bit.
  return static_cast<CCFlags>(static_cast<u32>(cc) ^ 1u);
}

// SPRs that are nothing more than a 32-bit slot in ppcState.spr.
bool IsPlainSPR(u32 index)
{
  switch (index)
  {
  case SPR_DMAU:
  case SPR_SPRG0:
  case SPR_SPRG1:
  case SPR_SPRG2:
  case SPR_SPRG3:
  case SPR_SRR0:
  case SPR_SRR1:
  case SPR_LR:
  case SPR_CTR:
    return true;
  default:
    return index >= SPR_GQR0 && index < SPR_GQR0 + 8;
  }
}

// SPRs that mfspr leaves to the interpreter because reading them has side
// effects or depends on state the JIT does not keep up to date.
bool MfsprNeedsInterpreter(u32 index)
{
  switch (index)
  {
  // Performance counters, in both their supervisor and user aliases.
  case SPR_PMC1:
  case SPR_UPMC1:
  case SPR_PMC2:
  case SPR_UPMC2:
  case SPR_PMC3:
  case SPR_UPMC3:
  case SPR_PMC4:
  case SPR_UPMC4:
  // Timer, breakpoint and gather-pipe registers.
  case SPR_DEC:
  case SPR_IABR:
  case SPR_WPAR:
    return true;
  default:
    return false;
  }
}

bool IsTimeBaseRead(UGeckoInstruction inst)
{
  // mftb is opcode 31/371; mfspr of TBL/TBU (31/339) is handled the same way.
  if (inst.OPCD != 31 || inst.SUBOP10 != 371)
    return false;
  const u32 index = SPRIndex(inst);
  return index == SPR_TU || index == SPR_TL;
}

void InvalidateICache(PowerPC::PowerPCState& ppc_state, JitInterface& jit_interface)
{
  ppc_state.iCache.Reset(jit_interface);
}

// Computes the 64-bit time base into `value`, the same way
// CoreTiming::GetFakeTimeBase does:
//   ticks = global_timer + slice_length - downcount / oc_factor - start_ticks
//   tb    = start_value + ticks / 12
// `globals` must hold &Globals; `tmp` is clobbered. `value` must be RAX and
// `tmp` RDX for the MUL.
void EmitFakeTimeBase(XEmitter& emit, X64Reg globals, X64Reg value, X64Reg tmp)
{
  using CoreTiming::Globals;
  const auto field = [globals](size_t offset) { return MDisp(globals, static_cast<int>(offset)); };

  emit.CVTSI2SS(XMM0, PPCSTATE(downcount));
  emit.MULSS(XMM0, field(offsetof(Globals, last_OC_factor_inverted)));
  emit.CVTSS2SI(tmp, R(XMM0));
  emit.MOV(32, R(value), field(offsetof(Globals, slice_length)));
  emit.SUB(64, R(value), R(tmp));
  emit.ADD(64, R(value), field(offsetof(Globals, global_timer)));
  emit.SUB(64, R(value), field(offsetof(Globals, fake_TB_start_ticks)));
  // The position within the block is deliberately not added: reading a time
  // past a pending interrupt breaks some games.

  // Unsigned divide by 12: high half of value * ceil(2^67 / 12), then >> 3.
  emit.MOV(64, R(tmp), Imm64(0xAAAAAAAAAAAAAAABULL));
  emit.MUL(64, R(tmp));
  emit.MOV(64, R(value), field(offsetof(Globals, fake_TB_start_value)));
  emit.SHR(64, R(tmp), Imm8(3));
  emit.ADD(64, R(value), R(tmp));
}
}  // namespace

// ---------------------------------------------------------------------------
// Condition register bits

// Emits the test for a CR bit and returns the condition that holds when
// the bit is set.
static CCFlags EmitCRBitTest(XEmitter& emit, int field, int bit)
{
  switch (bit)
  {
  case PowerPC::CR_SO_BIT:
    emit.BT(64, CRField(field), Imm8(PowerPC::CR_EMU_SO_BIT));
    return CC_C;
  case PowerPC::CR_EQ_BIT:
    emit.CMP(32, CRField(field), Imm8(0));
    return CC_Z;
  case PowerPC::CR_GT_BIT:
    emit.CMP(64, CRField(field), Imm8(0));
    return CC_G;
  case PowerPC::CR_LT_BIT:
    emit.BT(64, CRField(field), Imm8(PowerPC::CR_EMU_LT_BIT));
    return CC_C;
  default:
    ASSERT_MSG(DYNA_REC, false, "Invalid CR bit");
    return CC_Z;
  }
}

static bool IsValidCRBit(int bit)
{
  return bit >= 0 && bit <= 3;
}

void Jit64::GetCRFieldBit(int field, int bit, X64Reg out, bool negate)
{
  if (!IsValidCRBit(bit))
  {
    ASSERT_MSG(DYNA_REC, false, "Invalid CR bit");
    return;
  }
  const CCFlags set = EmitCRBitTest(*this, field, bit);
  SETcc(negate ? Inverted(set) : set, R(out));
}

FixupBranch Jit64::JumpIfCRFieldBit(int field, int bit, bool jump_if_set)
{
  if (!IsValidCRBit(bit))
  {
    ASSERT_MSG(DYNA_REC, false, "Invalid CR bit");
    return FixupBranch();
  }
  const CCFlags set = EmitCRBitTest(*this, field, bit);
  return J_CC(jump_if_set ? set : Inverted(set), Jump::Near);
}

void Jit64::FixGTBeforeSettingCRFieldBit(X64Reg reg)
{
  // An all-zero field reads as EQ and "not GT". Setting some other bit would
  // make the value positive and so turn GT on; bit 63 keeps it off.
  TEST(64, R(reg), R(reg));
  const FixupBranch nonzero = J_CC(CC_NZ);
  BTS(64, R(reg), Imm8(63));
  SetJumpTarget(nonzero);
}

void Jit64::SetCRFieldBit(int field, int bit, X64Reg in)
{
  // `in` holds 0 or 1 in its low byte.
  const X64Reg value = RSCRATCH2;
  MOV(64, R(value), CRField(field));
  MOVZX(32, 8, in, R(in));

  if (bit != PowerPC::CR_GT_BIT)
    FixGTBeforeSettingCRFieldBit(value);

  if (bit == PowerPC::CR_EQ_BIT)
  {
    // EQ is "low word is zero": keep the high word and store !in below it.
    SHR(64, R(value), Imm8(32));
    SHL(64, R(value), Imm8(32));
    XOR(32, R(in), Imm8(1));
    OR(64, R(value), R(in));
  }
  else if (bit == PowerPC::CR_GT_BIT)
  {
    // GT is "positive": bit 63 holds !in.
    BTR(64, R(value), Imm8(63));
    NOT(32, R(in));
    SHL(64, R(in), Imm8(63));
    OR(64, R(value), R(in));
  }
  else if (bit == PowerPC::CR_SO_BIT || bit == PowerPC::CR_LT_BIT)
  {
    const u8 position = bit == PowerPC::CR_SO_BIT ? u8{PowerPC::CR_EMU_SO_BIT} :
                                                    u8{PowerPC::CR_EMU_LT_BIT};
    BTR(64, R(value), Imm8(position));
    SHL(64, R(in), Imm8(position));
    OR(64, R(value), R(in));
  }

  BTS(64, R(value), Imm8(32));
  MOV(64, CRField(field), R(value));
}

void Jit64::SetCRFieldBit(int field, int bit)
{
  const X64Reg value = RSCRATCH;
  MOV(64, R(value), CRField(field));
  if (bit != PowerPC::CR_GT_BIT)
    FixGTBeforeSettingCRFieldBit(value);

  if (bit == PowerPC::CR_SO_BIT)
  {
    BTS(64, R(value), Imm8(PowerPC::CR_EMU_SO_BIT));
  }
  else if (bit == PowerPC::CR_EQ_BIT)
  {
    SHR(64, R(value), Imm8(32));
    SHL(64, R(value), Imm8(32));
  }
  else if (bit == PowerPC::CR_GT_BIT)
  {
    BTR(64, R(value), Imm8(63));
  }
  else if (bit == PowerPC::CR_LT_BIT)
  {
    BTS(64, R(value), Imm8(PowerPC::CR_EMU_LT_BIT));
  }

  BTS(64, R(value), Imm8(32));
  MOV(64, CRField(field), R(value));
}

void Jit64::ClearCRFieldBit(int field, int bit)
{
  // Clearing never needs bit 32: a cleared EQ makes the low word nonzero by
  // itself, and the other bits cannot make the whole value zero.
  if (bit == PowerPC::CR_EQ_BIT)
  {
    MOV(64, R(RSCRATCH), CRField(field));
    FixGTBeforeSettingCRFieldBit(RSCRATCH);
    OR(64, R(RSCRATCH), Imm8(1));
    MOV(64, CRField(field), R(RSCRATCH));
  }
  else if (bit == PowerPC::CR_GT_BIT)
  {
    BTS(64, CRField(field), Imm8(63));
  }
  else if (bit == PowerPC::CR_SO_BIT)
  {
    BTR(64, CRField(field), Imm8(PowerPC::CR_EMU_SO_BIT));
  }
  else if (bit == PowerPC::CR_LT_BIT)
  {
    BTR(64, CRField(field), Imm8(PowerPC::CR_EMU_LT_BIT));
  }
}

// ---------------------------------------------------------------------------
// FPSCR

void Jit64::UpdateFPExceptionSummary(X64Reg fpscr, X64Reg tmp1, X64Reg tmp2)
{
  // VX = any invalid-operation sticky bit.
  XOR(32, R(tmp1), R(tmp1));  // breaks the dependency on the old tmp1
  TEST(32, R(fpscr), Imm32(FPSCR_VX_ANY));
  SETcc(CC_NZ, R(tmp1));
  SHL(32, R(tmp1), Imm8(MathUtil::IntLog2(FPSCR_VX)));
  AND(32, R(fpscr), Imm32(~u32{FPSCR_VX | FPSCR_FEX}));
  OR(32, R(fpscr), R(tmp1));

  // FEX = any exception bit whose enable bit is set. The enables sit 22
  // bits below the matching exception bits.
  MOV(32, R(tmp1), R(fpscr));
  MOV(32, R(tmp2), R(fpscr));
  SHR(32, R(tmp1), Imm8(22));
  AND(32, R(tmp2), Imm32(FPSCR_ANY_E));
  TEST(32, R(tmp1), R(tmp2));
  // Nothing can be zeroed between TEST and SETcc without touching the
  // flags, so this SETcc writes a partial register.
  SETcc(CC_NZ, R(tmp1));
  SHL(32, R(tmp1), Imm8(MathUtil::IntLog2(FPSCR_FEX)));
  OR(32, R(fpscr), R(tmp1));
}

// Expects FPSCR in RSCRATCH; clobbers RSCRATCH and RSCRATCH2.
void Jit64::UpdateMXCSR()
{
  LEA(64, RSCRATCH2, MConst(FPSCR_TO_MXCSR));
  AND(32, R(RSCRATCH), Imm32(7));
  LDMXCSR(MComplex(RSCRATCH2, RSCRATCH, SCALE_4, 0));
}

void Jit64::mcrfs(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const u8 shift = static_cast<u8>(4 * (7 - inst.CRFS));
  // Copying a field clears its exception bits, but never FEX or VX, which
  // are summaries.
  const u32 cleared = (0xFu << shift) & (FPSCR_FX | FPSCR_ANY_X);
  const bool clears = cleared != 0;

  RCX64Reg scratch_guard;
  X64Reg table = RSCRATCH;
  if (clears)
  {
    scratch_guard = gpr.Scratch();
    RegCache::Realize(scratch_guard);
    table = scratch_guard;
  }

  // The nibble goes to RSCRATCH2; with `clears` RSCRATCH keeps FPSCR.
  if (cpu_info.bBMI1)
  {
    MOV(32, R(RSCRATCH), PPCSTATE(fpscr));
    MOV(32, R(RSCRATCH2), Imm32((4u << 8) | shift));
    BEXTR(32, RSCRATCH2, R(RSCRATCH), RSCRATCH2);
  }
  else
  {
    MOV(32, R(RSCRATCH2), PPCSTATE(fpscr));
    if (clears)
      MOV(32, R(RSCRATCH), R(RSCRATCH2));
    SHR(32, R(RSCRATCH2), Imm8(shift));
    AND(32, R(RSCRATCH2), Imm32(0xF));
  }

  LEA(64, table, MConst(PowerPC::ConditionRegister::s_crTable));
  MOV(64, R(table), MComplex(table, RSCRATCH2, SCALE_8, 0));
  MOV(64, CRField(inst.CRFD), R(table));

  if (!clears)
    return;
  AND(32, R(RSCRATCH), Imm32(~cleared));
  UpdateFPExceptionSummary(RSCRATCH, RSCRATCH2, table);
  MOV(32, PPCSTATE(fpscr), R(RSCRATCH));
}

void Jit64::mffsx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(inst.Rc);

  // The result is FPSCR in the low word with 0xFFF8 in the top bits.
  MOV(32, R(RSCRATCH), PPCSTATE(fpscr));
  RCX64Reg Rd = fpr.Bind(inst.FD, RCMode::Write);
  RegCache::Realize(Rd);
  MOV(64, R(RSCRATCH2), Imm64(0xFFF8000000000000));
  OR(64, R(RSCRATCH), R(RSCRATCH2));
  MOVQ_xmm(XMM0, R(RSCRATCH));
  MOVSD(Rd, R(XMM0));
}

void Jit64::mtfsb0x(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(inst.Rc);

  const u32 bit = 0x80000000u >> inst.CRBD;
  // FEX and VX are computed, never written.
  if (bit == FPSCR_FEX || bit == FPSCR_VX)
    return;

  const bool touches_summary = (bit & SUMMARY_INPUTS) != 0;
  const bool touches_rounding = inst.CRBD >= 29;
  if (!touches_summary && !touches_rounding)
  {
    AND(32, PPCSTATE(fpscr), Imm32(~bit));
    return;
  }

  MOV(32, R(RSCRATCH), PPCSTATE(fpscr));
  AND(32, R(RSCRATCH), Imm32(~bit));
  if (touches_summary)
  {
    RCX64Reg scratch = gpr.Scratch();
    RegCache::Realize(scratch);
    UpdateFPExceptionSummary(RSCRATCH, RSCRATCH2, scratch);
  }
  MOV(32, PPCSTATE(fpscr), R(RSCRATCH));
  if (touches_rounding)
    UpdateMXCSR();
}

void Jit64::mtfsb1x(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  const u32 bit = 0x80000000u >> inst.CRBD;
  if (bit == FPSCR_FEX || bit == FPSCR_VX)
    return;

  MOV(32, R(RSCRATCH), PPCSTATE(fpscr));
  if ((bit & FPSCR_ANY_X) == 0)
  {
    OR(32, R(RSCRATCH), Imm32(bit));
  }
  else
  {
    // Setting an exception bit that was clear also sets FX.
    BTS(32, R(RSCRATCH), Imm32(31 - inst.CRBD));
    const FixupBranch was_set = J_CC(CC_C);
    OR(32, R(RSCRATCH), Imm32(FPSCR_FX));
    SetJumpTarget(was_set);
  }

  if ((bit & SUMMARY_INPUTS) != 0)
  {
    RCX64Reg summary_tmp = gpr.Scratch();
    RegCache::Realize(summary_tmp);
    UpdateFPExceptionSummary(RSCRATCH, RSCRATCH2, summary_tmp);
  }

  MOV(32, PPCSTATE(fpscr), R(RSCRATCH));
  // Bits 29..31 (NI and RN) are mirrored into the host MXCSR.
  const bool touches_rounding = inst.CRBD >= 29;
  if (touches_rounding)
    UpdateMXCSR();
}

void Jit64::mtfsfix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(jo.fp_exceptions);
  FALLBACK_IF(inst.Rc);

  const u32 field = inst.CRFD;
  const u32 imm = (inst.hex >> 12) & 0xF;
  const u32 field_shift = 28 - 4 * field;
  const u32 field_mask = 0xFu << field_shift;

  MOV(32, R(RSCRATCH), PPCSTATE(fpscr));
  AND(32, R(RSCRATCH), Imm32(~field_mask));
  OR(32, R(RSCRATCH), Imm32(imm << field_shift));

  if ((field_mask & (FPSCR_FEX | FPSCR_VX | SUMMARY_INPUTS)) != 0)
  {
    RCX64Reg scratch = gpr.Scratch();
    RegCache::Realize(scratch);
    UpdateFPExceptionSummary(RSCRATCH, RSCRATCH2, scratch);
  }

  MOV(32, PPCSTATE(fpscr), R(RSCRATCH));

  // Field 7 holds NI and RN; their new value is known here.
  if (field == 7)
    LDMXCSR(MConst(FPSCR_TO_MXCSR, imm & 7));
}

void Jit64::mtfsfx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(inst.Rc);
  FALLBACK_IF(jo.fp_exceptions);

  // Each FM bit selects one nibble, FM bit 0 being the lowest nibble.
  u32 written = 0;
  for (u32 nibble = 0; nibble < 8; ++nibble)
  {
    if ((inst.FM >> nibble) & 1)
      written |= 0xFu << (4 * nibble);
  }

  RCOpArg Rb = fpr.Use(inst.FB, RCMode::Read);
  RegCache::Realize(Rb);
  if (Rb.IsSimpleReg())
    MOVQ_xmm(R(RSCRATCH), Rb.GetSimpleReg());
  else
    MOV(32, R(RSCRATCH), Rb);

  if (written != 0xFFFFFFFF)
  {
    MOV(32, R(RSCRATCH2), PPCSTATE(fpscr));
    AND(32, R(RSCRATCH), Imm32(written));
    AND(32, R(RSCRATCH2), Imm32(~written));
    OR(32, R(RSCRATCH), R(RSCRATCH2));
  }

  if ((written & (FPSCR_FEX | FPSCR_VX | SUMMARY_INPUTS)) != 0)
  {
    RCX64Reg scratch = gpr.Scratch();
    RegCache::Realize(scratch);
    UpdateFPExceptionSummary(RSCRATCH, RSCRATCH2, scratch);
  }

  MOV(32, PPCSTATE(fpscr), R(RSCRATCH));

  if (inst.FM & 1)
    UpdateMXCSR();
}

// ---------------------------------------------------------------------------
// SPRs and MSR

void Jit64::mtspr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  const u32 index = SPRIndex(inst);
  const u32 s = inst.RD;

  if (IsPlainSPR(index))
  {
    RCOpArg Rs = gpr.BindOrImm(s, RCMode::Read);
    RegCache::Realize(Rs);
    MOV(32, PPCSTATE_SPR(index), Rs);
    return;
  }

  if (index == SPR_XER)
  {
    // XER is kept split into string count, CA and SO/OV.
    RCX64Reg Rs = gpr.Bind(s, RCMode::Read);
    RegCache::Realize(Rs);

    MOV(32, R(RSCRATCH), Rs);
    AND(32, R(RSCRATCH), Imm32(0xff7f));
    MOV(16, PPCSTATE(xer_stringctrl), R(RSCRATCH));

    MOV(32, R(RSCRATCH), Rs);
    SHR(32, R(RSCRATCH), Imm8(XER_CA_SHIFT));
    AND(8, R(RSCRATCH), Imm8(1));
    MOV(8, PPCSTATE(xer_ca), R(RSCRATCH));

    MOV(32, R(RSCRATCH), Rs);
    SHR(32, R(RSCRATCH), Imm8(XER_OV_SHIFT));
    MOV(8, PPCSTATE(xer_so_ov), R(RSCRATCH));
    return;
  }

  if (index == SPR_HID0)
  {
    // ICFI is a command bit: it is never stored, and writing it as 1 flushes
    // the instruction cache.
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Rs);

    MOV(32, R(RSCRATCH), Rs);
    BTR(32, R(RSCRATCH), Imm8(HID0_ICFI_BIT));
    MOV(32, PPCSTATE_SPR(index), R(RSCRATCH));
    const FixupBranch no_flush = J_CC(CC_NC);
    const BitSet32 live = CallerSavedRegistersInUse();
    ABI_PushRegistersAndAdjustStack(live, 0);
    ABI_CallFunctionPP(InvalidateICache, &m_ppc_state, &m_system.GetJitInterface());
    ABI_PopRegistersAndAdjustStack(live, 0);
    SetJumpTarget(no_flush);
    return;
  }

  FALLBACK_IF(true);
}

void Jit64::mfspr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  const u32 index = SPRIndex(inst);
  const u32 d = inst.RD;

  if (MfsprNeedsInterpreter(index))
    FALLBACK_IF(true);

  if (index == SPR_XER)
  {
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rd);
    MOVZX(32, 16, Rd, PPCSTATE(xer_stringctrl));
    MOVZX(32, 8, RSCRATCH, PPCSTATE(xer_ca));
    SHL(32, R(RSCRATCH), Imm8(XER_CA_SHIFT));
    OR(32, Rd, R(RSCRATCH));
    MOVZX(32, 8, RSCRATCH, PPCSTATE(xer_so_ov));
    SHL(32, R(RSCRATCH), Imm8(XER_OV_SHIFT));
    OR(32, Rd, R(RSCRATCH));
    return;
  }

  if (index != SPR_TL && index != SPR_TU)
  {
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rd);
    MOV(32, Rd, PPCSTATE_SPR(index));
    return;
  }

  // Time base. The MUL fixes RAX and RDX.
  RCX64Reg rdx = gpr.Scratch(RDX);
  RCX64Reg rax = gpr.Scratch(RAX);
  RCX64Reg rcx = gpr.Scratch(RCX);

  MOV(64, rcx, ImmPtr(&m_system.GetCoreTiming().GetGlobals()));
  EmitFakeTimeBase(*this, rcx, rax, rdx);
  MOV(64, PPCSTATE_SPR(SPR_TL), rax);

  // Reading both halves back to back is the usual pattern, so the next
  // mftb is folded in when it targets a different register.
  if (CanMergeNextInstructions(1))
  {
    const UGeckoInstruction next = js.op[1].inst;
    const u32 n = next.RD;
    if (IsTimeBaseRead(next) && n != d)
    {
      const u32 next_index = SPRIndex(next);
      js.downcountAmount++;
      js.skipInstructions = 1;

      RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
      RCX64Reg Rn = gpr.Bind(n, RCMode::Write);
      RegCache::Realize(Rd, Rn);
      if (index == SPR_TL)
        MOV(32, Rd, rax);
      if (next_index == SPR_TL)
        MOV(32, Rn, rax);
      SHR(64, rax, Imm8(32));
      if (index == SPR_TU)
        MOV(32, Rd, rax);
      if (next_index == SPR_TU)
        MOV(32, Rn, rax);
      return;
    }
  }

  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  RegCache::Realize(Rd);
  if (index == SPR_TU)
    SHR(64, rax, Imm8(32));
  MOV(32, Rd, rax);
}

void Jit64::mftb(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  mfspr(inst);
}

void Jit64::mtmsr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  FALLBACK_IF(jo.fp_exceptions);

  OpArg new_msr;
  {
    RCOpArg Rs = gpr.BindOrImm(inst.RS, RCMode::Read);
    RegCache::Realize(Rs);
    MOV(32, PPCSTATE(msr), Rs);
    new_msr = Rs;
  }
  MSRUpdated(new_msr, RSCRATCH2);

  gpr.Flush();
  fpr.Flush();

  // Turning EE on with an interrupt already pending takes the interrupt
  // right after this instruction. A pending CP interrupt is left to the
  // normal check so the GPU stays in sync.
  const u32 next_pc = js.compilerPC + 4;
  TEST(32, PPCSTATE(msr), Imm32(MSR_EE));
  const FixupBranch ee_off = J_CC(CC_Z, Jump::Near);
  TEST(32, PPCSTATE(Exceptions), Imm32(DELAYED_EXCEPTIONS));
  const FixupBranch none_pending = J_CC(CC_Z, Jump::Near);
  MOV(64, R(RSCRATCH), ImmPtr(&m_system.GetProcessorInterface().m_interrupt_cause));
  TEST(32, MatR(RSCRATCH), Imm32(ProcessorInterface::INT_CAUSE_CP));
  const FixupBranch cp_pending = J_CC(CC_NZ, Jump::Near);

  MOV(32, PPCSTATE(pc), Imm32(next_pc));
  WriteExternalExceptionExit();

  SetJumpTarget(cp_pending);
  SetJumpTarget(none_pending);
  SetJumpTarget(ee_off);

  MOV(32, R(RSCRATCH), Imm32(next_pc));
  WriteExitDestInRSCRATCH();
}

void Jit64::mfmsr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  RCX64Reg Rd = gpr.Bind(inst.RD, RCMode::Write);
  RegCache::Realize(Rd);
  MOV(32, Rd, PPCSTATE(msr));
}

// ---------------------------------------------------------------------------
// Whole-CR moves

void Jit64::mfcr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  // The routine returns CR in RSCRATCH and uses RSCRATCH_EXTRA.
  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  CALL(asm_routines.mfcr);

  RCX64Reg Rd = gpr.Bind(inst.RD, RCMode::Write);
  RegCache::Realize(Rd);
  MOV(32, Rd, R(RSCRATCH));
}

void Jit64::mtcrf(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  const u32 crm = inst.CRM;
  if (crm == 0)
    return;

  // CRM bit 7 selects field 0.
  const auto selected = [crm](int field) { return ((crm << field) & 0x80) != 0; };

  if (gpr.IsImm(inst.RS))
  {
    const u32 value = gpr.Imm32(inst.RS);
    for (int field = 0; field < 8; field++)
    {
      if (!selected(field))
        continue;
      const u32 nibble = (value >> (28 - 4 * field)) & 0xF;
      const u64 internal = PowerPC::ConditionRegister::PPCToInternal(static_cast<u8>(nibble));
      const s32 low = static_cast<s32>(internal);
      if (static_cast<s64>(internal) == low)
      {
        MOV(64, CRField(field), Imm32(static_cast<u32>(low)));
      }
      else
      {
        MOV(64, R(RSCRATCH), Imm64(internal));
        MOV(64, CRField(field), R(RSCRATCH));
      }
    }
    return;
  }

  MOV(64, R(RSCRATCH2), ImmPtr(PowerPC::ConditionRegister::s_crTable.data()));
  RCX64Reg Rs = gpr.Bind(inst.RS, RCMode::Read);
  RegCache::Realize(Rs);
  for (int field = 0; field < 8; field++)
  {
    if (!selected(field))
      continue;
    MOV(32, R(RSCRATCH), Rs);
    // Field 7 needs no shift and field 0 no mask.
    if (field != 7)
      SHR(32, R(RSCRATCH), Imm8(static_cast<u8>(28 - 4 * field)));
    if (field != 0)
      AND(32, R(RSCRATCH), Imm8(0xF));
    MOV(64, R(RSCRATCH), MComplex(RSCRATCH2, RSCRATCH, SCALE_8, 0));
    MOV(64, CRField(field), R(RSCRATCH));
  }
}

void Jit64::mcrf(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  if (inst.CRFS == inst.CRFD)
    return;
  MOV(64, R(RSCRATCH), CRField(inst.CRFS));
  MOV(64, CRField(inst.CRFD), R(RSCRATCH));
}

void Jit64::mcrxr(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);

  // Builds the nibble SO:OV:CA:0 and looks it up. xer_so_ov holds SO in bit 1
  // and OV in bit 0, so ca + 2 * so_ov is SO:OV:CA. The nibble is that times
  // 2, and its 8-byte table entry is at nibble * 8, so the shift is 4.
  MOVZX(32, 8, RSCRATCH, PPCSTATE(xer_ca));
  MOVZX(32, 8, RSCRATCH2, PPCSTATE(xer_so_ov));
  LEA(32, RSCRATCH, MComplex(RSCRATCH, RSCRATCH2, SCALE_2, 0));
  SHL(32, R(RSCRATCH), Imm8(4));

  MOV(64, R(RSCRATCH2), ImmPtr(PowerPC::ConditionRegister::s_crTable.data()));
  MOV(64, R(RSCRATCH), MRegSum(RSCRATCH, RSCRATCH2));
  MOV(64, CRField(inst.CRFD), R(RSCRATCH));

  // Clears CA, SO and OV with one store.
  static_assert(PPCSTATE_OFF(xer_ca) + 1 == PPCSTATE_OFF(xer_so_ov));
  MOV(16, PPCSTATE(xer_ca), Imm16(0));
}

void Jit64::crXXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITSystemRegistersOff);
  DEBUG_ASSERT_MSG(DYNA_REC, inst.OPCD == 19, "Invalid crXXX");

  enum : u32
  {
    CRNOR = 33,
    CRANDC = 129,
    CRXOR = 193,
    CRNAND = 225,
    CRAND = 257,
    CREQV = 289,
    CRORC = 417,
    CROR = 449,
  };

  const u32 op = inst.SUBOP10;
  const int d_field = CRFieldOf(inst.CRBD);
  const int d_bit = CRBitOf(inst.CRBD);

  if (inst.CRBA == inst.CRBB)
  {
    // With A == B every operation is a constant, A or ~A.
    switch (op)
    {
    case CRANDC:
    case CRXOR:
      ClearCRFieldBit(d_field, d_bit);
      return;
    case CREQV:
    case CRORC:
      SetCRFieldBit(d_field, d_bit);
      return;
    case CRAND:
    case CROR:
    case CRNOR:
    case CRNAND:
      GetCRFieldBit(CRFieldOf(inst.CRBA), CRBitOf(inst.CRBA), RSCRATCH,
                    op == CRNOR || op == CRNAND);
      SetCRFieldBit(d_field, d_bit, RSCRATCH);
      return;
    default:
      break;
    }
  }

  // Every operation is AND, OR or XOR of possibly negated inputs:
  //   nor = ~a & ~b, nand = ~a | ~b, eqv = ~a ^ b, andc = a & ~b, orc = a | ~b
  const bool not_a = op == CREQV || op == CRNAND || op == CRNOR;
  const bool not_b = op == CRANDC || op == CRORC || op == CRNAND || op == CRNOR;

  GetCRFieldBit(CRFieldOf(inst.CRBA), CRBitOf(inst.CRBA), RSCRATCH, not_a);
  GetCRFieldBit(CRFieldOf(inst.CRBB), CRBitOf(inst.CRBB), RSCRATCH2, not_b);

  if (op == CRNOR || op == CRANDC || op == CRAND)
    AND(8, R(RSCRATCH), R(RSCRATCH2));
  else if (op == CRXOR || op == CREQV)
    XOR(8, R(RSCRATCH), R(RSCRATCH2));
  else if (op == CRNAND || op == CRORC || op == CROR)
    OR(8, R(RSCRATCH), R(RSCRATCH2));

  SetCRFieldBit(d_field, d_bit, RSCRATCH);
}
