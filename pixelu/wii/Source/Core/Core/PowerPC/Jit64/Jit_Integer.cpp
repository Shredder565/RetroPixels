// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <utility>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/SmallVector.h"
#include "Common/x64Emitter.h"

#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/JitCommon/DivUtils.h"
#include "Core/PowerPC/PPCAnalyst.h"

using namespace Gen;
using namespace JitCommon;

namespace
{
// regimmop identifies the operation by these functions' addresses.
u32 OpAdd(u32 x, u32 y)
{
  return x + y;
}

u32 OpOr(u32 x, u32 y)
{
  return x | y;
}

u32 OpAnd(u32 x, u32 y)
{
  return x & y;
}

u32 OpXor(u32 x, u32 y)
{
  return x ^ y;
}

// XER[SO:OV] byte after clearing OV, indexed by the old byte: SO stays.
alignas(4) constexpr std::array<u8, 4> CLEAR_OV_TABLE{0, 0, XER_SO_MASK, XER_SO_MASK};

constexpr u8 SO_AND_OV = XER_OV_MASK | XER_SO_MASK;
constexpr u8 ALL_BUT_OV = static_cast<u8>(~XER_OV_MASK);

bool FitsInImm8(u32 value)
{
  const s32 signed_value = static_cast<s32>(value);
  return signed_value >= -128 && signed_value <= 127;
}

// The bcx/bclrx/bcctrx forms that a compare can be fused with: no CTR
// decrement, and a CR bit is tested.
bool IsFusableBranch(UGeckoInstruction next)
{
  const bool branch_form = next.OPCD == 16 ||
                           (next.OPCD == 19 && (next.SUBOP10 == 528 || next.SUBOP10 == 16));
  return branch_form && (next.BO & BO_DONT_DECREMENT_FLAG) != 0 &&
         (next.BO & BO_DONT_CHECK_CONDITION) == 0;
}
}  // namespace

// ---------------------------------------------------------------------------
// XER and CR0

void Jit64::GenerateConstantOverflow(bool overflow)
{
  if (!overflow)
    AND(8, PPCSTATE(xer_so_ov), Imm8(ALL_BUT_OV));
  else
    MOV(8, PPCSTATE(xer_so_ov), Imm8(SO_AND_OV));
}

void Jit64::GenerateConstantOverflow(s64 val)
{
  const bool fits = val >= std::numeric_limits<s32>::min() && val <= std::numeric_limits<s32>::max();
  GenerateConstantOverflow(!fits);
}

// Sets OV (and SO) when `cond` is false, clears OV otherwise. The clearing
// path must not change the host flags (the carry or a fused compare may
// still be in them), so it uses a table lookup instead of AND.
void Jit64::GenerateOverflow(CCFlags cond)
{
  const FixupBranch no_overflow = J_CC(cond);
  MOV(8, PPCSTATE(xer_so_ov), Imm8(SO_AND_OV));
  const FixupBranch done = J();

  SetJumpTarget(no_overflow);
  MOVZX(32, 8, RSCRATCH, PPCSTATE(xer_so_ov));
  LEA(64, RSCRATCH2, MConst(CLEAR_OV_TABLE));
  MOV(8, R(RSCRATCH), MRegSum(RSCRATCH, RSCRATCH2));
  MOV(8, PPCSTATE(xer_so_ov), R(RSCRATCH));
  SetJumpTarget(done);
}

void Jit64::FinalizeCarry(CCFlags cond)
{
  js.carryFlag = CarryFlag::InPPCState;
  if (!js.op->wantsCA)
    return;

  // When the next instruction consumes CA from the host flags, the carry
  // is left there (nothing can come in between, so this is safe).
  if (!CanMergeNextInstructions(1) || !js.op[1].wantsCAInFlags)
  {
    JitSetCAIf(cond);
    return;
  }

  if (cond == CC_NC)
  {
    js.carryFlag = CarryFlag::InHostCarryInverted;
  }
  else
  {
    if (cond != CC_C)
    {
      // Move the condition into CF: SETcc gives 0/1, and SHR by one shifts
      // that bit out into CF.
      SETcc(cond, R(RSCRATCH));
      SHR(8, R(RSCRATCH), Imm8(1));
    }
    js.carryFlag = CarryFlag::InHostCarry;
  }
  LockFlags();
}

void Jit64::FinalizeCarry(bool ca)
{
  js.carryFlag = CarryFlag::InPPCState;
  if (!js.op->wantsCA)
    return;

  if (CanMergeNextInstructions(1) && js.op[1].wantsCAInFlags)
  {
    if (ca)
      STC();
    else
      CLC();
    LockFlags();
    js.carryFlag = CarryFlag::InHostCarry;
    return;
  }

  if (ca)
    JitSetCA();
  else
    JitClearCA();
}

void Jit64::FinalizeCarryOverflow(bool oe, bool inv)
{
  if (oe)
    GenerateOverflow();
  FinalizeCarry(inv ? CC_NC : CC_C);
}

void Jit64::FlushCarry()
{
  const CarryFlag where = js.carryFlag;
  js.carryFlag = CarryFlag::InPPCState;
  if (where == CarryFlag::InPPCState)
    return;

  JitSetCAIf(where == CarryFlag::InHostCarry ? CC_C : CC_NC);
  UnlockFlags();
}

// Writes CR0 from the 32-bit result in `preg`. `needs_test` may be false
// only when the host flags already describe the result exactly (from an
// instruction that sets SF and ZF from the full result and clears OF);
// `needs_sext` false means the cached value is known to be non-negative.
void Jit64::ComputeRC(preg_t preg, bool needs_test, bool needs_sext)
{
  RCOpArg result = gpr.Use(preg, RCMode::Read);
  RegCache::Realize(result);

  if (result.IsImm())
  {
    const s32 value = result.SImm32();
    result.Unlock();
    FinalizeImmediateRC(value);
    return;
  }

  if (!needs_sext)
  {
    MOV(64, PPCSTATE_CR(0), result);
  }
  else
  {
    MOVSX(64, 32, RSCRATCH, result);
    MOV(64, PPCSTATE_CR(0), R(RSCRATCH));
  }

  if (!CheckMergedBranch(0))
    return;

  // Registers that are outputs here but dead afterwards are written back
  // now rather than on both sides of the branch. This comes before the
  // TEST so that TEST and the branch can macro-fuse.
  gpr.Flush(~js.op->gprWillBeWritten & js.op->regsOut, RegCache::FlushMode::Undirty);
  if (needs_test)
    TEST(32, result, result);
  result.Unlock();
  DoMergedBranchCondition();
}

void Jit64::FinalizeImmediateRC(s32 value)
{
  MOV(64, PPCSTATE_CR(0), Imm32(static_cast<u32>(value)));
  if (CheckMergedBranch(0))
    DoMergedBranchImmediate(value);
}

// Masks `reg` in place. MOVZX does not set flags the way AND does, which is
// why this lives here and not in the emitter.
void Jit64::AndWithMask(X64Reg reg, u32 mask)
{
  switch (mask)
  {
  case 0xffffffff:
    break;
  case 0:
    XOR(32, R(reg), R(reg));
    break;
  case 0xff:
    MOVZX(32, 8, reg, R(reg));
    break;
  case 0xffff:
    MOVZX(32, 16, reg, R(reg));
    break;
  default:
    AND(32, R(reg), Imm32(mask));
    break;
  }
}

void Jit64::RotateLeft(int bits, X64Reg regOp, const OpArg& arg, u8 rotate)
{
  const bool in_place = arg.IsSimpleReg(regOp);

  // RORX copies and rotates in one instruction (rotating right by the
  // complement), without touching the flags.
  if (rotate != 0 && !in_place && cpu_info.bBMI2)
  {
    RORX(bits, regOp, arg, static_cast<u8>(bits - rotate));
    return;
  }

  if (!in_place)
    MOV(bits, R(regOp), arg);
  if (rotate != 0)
    ROL(bits, R(regOp), Imm8(rotate));
}

// ---------------------------------------------------------------------------
// Immediate forms

void Jit64::regimmop(int d, int a, bool binary, u32 value, Operation doop,
                     void (XEmitter::*op)(int, const OpArg&, const OpArg&), bool Rc, bool carry)
{
  const preg_t dst = static_cast<preg_t>(d);
  const preg_t src = static_cast<preg_t>(a);
  const bool is_add = doop == &OpAdd;

  // addi/addis read r0 as zero; addic and the logical forms read r0.
  if (src == 0 && !binary && !carry)
  {
    if (is_add)
      gpr.SetImmediate32(dst, value);
    else
      ASSERT_MSG(DYNA_REC, 0, "WTF regimmop");
  }
  else
  {
    carry = carry && js.op->wantsCA;
    RCOpArg Ra = gpr.Use(src, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(dst, RCMode::Write);
    RegCache::Realize(Ra, Rd);
    if (is_add && !carry && dst != src && Ra.IsSimpleReg())
    {
      LEA(32, Rd, MDisp(Ra.GetSimpleReg(), static_cast<s32>(value)));
    }
    else
    {
      if (dst != src)
        MOV(32, Rd, Ra);
      (this->*op)(32, Rd, Imm32(value));
    }
    if (carry)
      FinalizeCarry(CC_C);
  }

  // Flags from ADD include OF, so an add needs a TEST. ANDing with a
  // positive mask cannot give a negative result.
  if (Rc)
    ComputeRC(dst, is_add, doop != &OpAnd || (value & 0x80000000) != 0);
}

void Jit64::reg_imm(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 d = inst.RD;
  const u32 a = inst.RA;
  const u32 s = inst.RS;
  const u32 simm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  const u32 uimm = inst.UIMM;
  const int rd = static_cast<int>(d);
  const int ra = static_cast<int>(a);
  const int rs = static_cast<int>(s);

  switch (inst.OPCD)
  {
  case 14:  // addi
    if (a != 0 && d != a && simm == 0)
    {
      // "addi rD, rA, 0" is a register move.
      RCOpArg Ra = gpr.Use(a, RCMode::Read);
      RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
      RegCache::Realize(Ra, Rd);
      MOV(32, Rd, Ra);
      return;
    }
    regimmop(rd, ra, false, simm, &OpAdd, &XEmitter::ADD);
    return;
  case 15:  // addis
    regimmop(rd, ra, false, simm << 16, &OpAdd, &XEmitter::ADD);
    return;
  case 12:  // addic
    regimmop(rd, ra, false, simm, &OpAdd, &XEmitter::ADD, false, true);
    return;
  case 13:  // addic.
    regimmop(rd, ra, true, simm, &OpAdd, &XEmitter::ADD, true, true);
    return;
  case 24:  // ori
    regimmop(ra, rs, true, uimm, &OpOr, &XEmitter::OR);
    return;
  case 25:  // oris
    regimmop(ra, rs, true, uimm << 16, &OpOr, &XEmitter::OR);
    return;
  case 26:  // xori
    regimmop(ra, rs, true, uimm, &OpXor, &XEmitter::XOR, false);
    return;
  case 27:  // xoris
    regimmop(ra, rs, true, uimm << 16, &OpXor, &XEmitter::XOR, false);
    return;
  case 28:  // andi.
    regimmop(ra, rs, true, uimm, &OpAnd, &XEmitter::AND, true);
    return;
  case 29:  // andis.
    regimmop(ra, rs, true, uimm << 16, &OpAnd, &XEmitter::AND, true);
    return;
  default:
    FALLBACK_IF(true);
  }
}

// ---------------------------------------------------------------------------
// Compare-and-branch fusion
//
// A CR-setting instruction immediately followed by a conditional branch on
// the field it wrote is compiled as one unit: the host flags (or the known
// result) decide the branch directly, and the branch instruction is
// skipped.

bool Jit64::CheckMergedBranch(u32 crf) const
{
  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_BRANCH_MERGE) ||
      !CanMergeNextInstructions(1))
  {
    return false;
  }
  const UGeckoInstruction next = js.op[1].inst;
  return IsFusableBranch(next) && (u32{next.BI} >> 2) == crf;
}

void Jit64::DoMergedBranch()
{
  // The taken path of the fused branch, with registers flushed.
  const PPCAnalyst::CodeOp& branch = js.op[1];
  const UGeckoInstruction next = branch.inst;
  const u32 branch_pc = branch.address;
  const u32 return_pc = branch_pc + 4;

  const bool is_bclr = next.OPCD == 19 && next.SUBOP10 == 16;
  const bool is_bcctr = next.OPCD == 19 && next.SUBOP10 == 528;

  if (branch.branchIsIdleLoop || next.OPCD == 16)
  {
    if (next.LK)
      MOV(32, PPCSTATE_SPR(SPR_LR), Imm32(return_pc));
    WriteBranchWatch<true>(branch_pc, branch.branchTo, next, {});
    if (branch.branchIsIdleLoop)
      WriteIdleExit(branch.branchTo);
    else
      WriteExit(branch.branchTo, next.LK, return_pc);
  }
  else if (is_bcctr)
  {
    if (next.LK)
      MOV(32, PPCSTATE_SPR(SPR_LR), Imm32(return_pc));
    MOV(32, R(RSCRATCH), PPCSTATE_SPR(SPR_CTR));
    AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    WriteBranchWatchDestInRSCRATCH(branch_pc, next, BitSet32{RSCRATCH});
    WriteExitDestInRSCRATCH(next.LK, return_pc);
  }
  else if (is_bclr)
  {
    // LR is read before a bclrl overwrites it.
    MOV(32, R(RSCRATCH), PPCSTATE_SPR(SPR_LR));
    if (!m_enable_blr_optimization)
      AND(32, R(RSCRATCH), Imm32(0xFFFFFFFC));
    if (next.LK)
      MOV(32, PPCSTATE_SPR(SPR_LR), Imm32(return_pc));
    WriteBranchWatchDestInRSCRATCH(branch_pc, next, BitSet32{RSCRATCH});
    WriteBLRExit();
  }
  else
  {
    PanicAlertFmt("WTF invalid branch");
  }
}

// The not-taken path shared by both fusion helpers.
void Jit64::EmitMergedBranchFallthrough()
{
  const PPCAnalyst::CodeOp& branch = js.op[1];
  const u32 next_pc = branch.address + 4;
  if (analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    WriteBranchWatch<false>(branch.address, next_pc, branch.inst, CallerSavedRegistersInUse());
    return;
  }
  gpr.Flush();
  fpr.Flush();
  WriteBranchWatch<false>(branch.address, next_pc, branch.inst, {});
  WriteExit(next_pc);
}

// Fused branch on the host flags of a signed comparison of the result with
// zero (as left by TEST or CMP).
void Jit64::DoMergedBranchCondition()
{
  js.downcountAmount++;
  js.skipInstructions = 1;
  const UGeckoInstruction next = js.op[1].inst;
  const int bit = 3 - static_cast<int>(next.BI & 3);
  const bool if_true = (next.BO & BO_BRANCH_IF_TRUE) != 0;

  ASSERT(gpr.IsAllUnlocked());

  // Jump past the taken path when the branch is not taken.
  FixupBranch not_taken;
  if (bit == PowerPC::CR_LT_BIT)
    not_taken = J_CC(if_true ? CC_GE : CC_L, Jump::Near);
  else if (bit == PowerPC::CR_GT_BIT)
    not_taken = J_CC(if_true ? CC_LE : CC_G, Jump::Near);
  else if (bit == PowerPC::CR_EQ_BIT)
    not_taken = J_CC(if_true ? CC_NE : CC_E, Jump::Near);
  else if (bit == PowerPC::CR_SO_BIT)
    not_taken = J(Jump::Near);  // compares never set SO here

  {
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();
    gpr.Flush();
    fpr.Flush();
    DoMergedBranch();
  }

  SetJumpTarget(not_taken);
  EmitMergedBranchFallthrough();
}

// Fused branch whose condition is known at compile time from the signed
// value `val` the CR field was computed from.
void Jit64::DoMergedBranchImmediate(s64 val)
{
  js.downcountAmount++;
  js.skipInstructions = 1;
  const UGeckoInstruction next = js.op[1].inst;
  const int bit = 3 - static_cast<int>(next.BI & 3);
  const bool if_true = (next.BO & BO_BRANCH_IF_TRUE) != 0;

  ASSERT(gpr.IsAllUnlocked());

  bool bit_set = false;
  bool known = true;
  if (bit == PowerPC::CR_LT_BIT)
    bit_set = val < 0;
  else if (bit == PowerPC::CR_GT_BIT)
    bit_set = val > 0;
  else if (bit == PowerPC::CR_EQ_BIT)
    bit_set = val == 0;
  else
    known = false;  // SO: never taken

  if (known && bit_set == if_true)
  {
    gpr.Flush();
    fpr.Flush();
    DoMergedBranch();
    return;
  }
  EmitMergedBranchFallthrough();
}

// ---------------------------------------------------------------------------
// Compares

void Jit64::cmpXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 crf = inst.CRFD;
  const bool fuse = CheckMergedBranch(crf);

  // The CR field is stored as the 64-bit difference of the operands,
  // sign- or zero-extended as the compare requires.
  bool is_signed = false;
  std::optional<u32> rhs_imm;
  if (inst.OPCD == 31)  // cmp, cmpl
  {
    is_signed = inst.SUBOP10 == 0;
    if (gpr.IsImm(b))
      rhs_imm = gpr.Imm32(b);
  }
  else if (inst.OPCD == 10)  // cmpli
  {
    rhs_imm = u32{inst.UIMM};
  }
  else if (inst.OPCD == 11)  // cmpi
  {
    is_signed = true;
    rhs_imm = static_cast<u32>(static_cast<s32>(static_cast<s16>(inst.UIMM)));
  }
  else
  {
    PanicAlertFmt("cmpXX");
  }

  if (rhs_imm && gpr.IsImm(a))
  {
    // Fully known.
    s64 difference;
    if (is_signed)
      difference = s64{gpr.SImm32(a)} - s64{static_cast<s32>(*rhs_imm)};
    else
      difference = static_cast<s64>(u64{gpr.Imm32(a)} - u64{*rhs_imm});

    if (difference != static_cast<s32>(difference))
    {
      MOV(64, R(RSCRATCH), Imm64(static_cast<u64>(difference)));
      MOV(64, PPCSTATE_CR(crf), R(RSCRATCH));
    }
    else
    {
      MOV(64, PPCSTATE_CR(crf), Imm32(static_cast<u32>(difference)));
    }
    if (fuse)
      DoMergedBranchImmediate(difference);
    return;
  }

  const bool rhs_is_zero = rhs_imm == u32{0};

  if (!is_signed && rhs_is_zero && !gpr.IsImm(a))
  {
    // Unsigned compare with 0: the zero-extended value itself.
    RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
    RegCache::Realize(Ra);
    MOV(64, PPCSTATE_CR(crf), Ra);
    if (fuse)
    {
      TEST(64, Ra, Ra);
      RegCache::Unlock(Ra);
      DoMergedBranchCondition();
    }
    return;
  }

  // An unsigned immediate with the top bit set would be sign-extended by a
  // 64-bit SUB with an imm32, so it must go through a register.
  const bool rhs_needs_reg = !is_signed && rhs_imm && (*rhs_imm & 0x80000000U) != 0;

  RCOpArg Rb;
  OpArg rhs;
  if (inst.OPCD != 31 || (is_signed && rhs_imm))
  {
    // An immediate (from the encoding, or preferred over a register).
    rhs = Imm32(*rhs_imm);
  }
  else if (!rhs_is_zero)
  {
    if (is_signed)
      Rb = gpr.Use(b, RCMode::Read);
    else if (rhs_needs_reg)
      Rb = gpr.Bind(b, RCMode::Read);
    else
      Rb = gpr.BindOrImm(b, RCMode::Read);
    RegCache::Realize(Rb);
    rhs = Rb;
  }

  const X64Reg lhs = RSCRATCH;
  if (!gpr.IsImm(a))
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RegCache::Realize(Ra);
    if (is_signed)
      MOVSX(64, 32, lhs, Ra);
    else
      MOVZX(64, 32, lhs, Ra);
  }
  else if (is_signed)
  {
    MOV(64, R(lhs), Imm32(static_cast<u32>(gpr.SImm32(a))));
  }
  else
  {
    MOV(32, R(lhs), Imm32(gpr.Imm32(a)));
  }

  if (rhs_is_zero)
  {
    MOV(64, PPCSTATE_CR(crf), R(lhs));
    // Next to the branch, for macro-op fusion.
    if (fuse)
      TEST(64, R(lhs), R(lhs));
  }
  else
  {
    if (rhs.IsImm() && rhs_needs_reg)
    {
      MOV(32, R(RSCRATCH2), rhs);
      rhs = R(RSCRATCH2);
    }
    else if (!rhs.IsImm() && is_signed)
    {
      MOVSX(64, 32, RSCRATCH2, rhs);
      rhs = R(RSCRATCH2);
    }
    SUB(64, R(lhs), rhs);
    MOV(64, PPCSTATE_CR(crf), R(lhs));
  }

  if (fuse)
  {
    RegCache::Unlock(Rb);
    DoMergedBranchCondition();
  }
}

// ---------------------------------------------------------------------------
// Logical operations

void Jit64::boolX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  DEBUG_ASSERT_MSG(DYNA_REC, inst.OPCD == 31, "Invalid boolX");

  enum : u32
  {
    NOR = 124,
    ANDC = 60,
    AND_ = 28,
    EQV = 284,
    XOR_ = 316,
    ORC = 412,
    OR_ = 444,
    NAND = 476,
  };
  const u32 op = inst.SUBOP10;
  const u32 a = inst.RA;
  const u32 s = inst.RS;
  const u32 b = inst.RB;
  // The result is already in the host flags (from AND/OR/XOR) unless the
  // last instruction was a NOT or a plain move.
  bool needs_test = false;

  using EmitOp = void (XEmitter::*)(int, const OpArg&, const OpArg&);

  if (gpr.IsImm(s) || gpr.IsImm(b))
  {
    // One operand is known: k is its value, rj the other register.
    const bool s_known = gpr.IsImm(s);
    const u32 rj = s_known ? b : s;
    u32 k = gpr.Imm32(s_known ? s : b);

    bool invert_rb = op == ANDC || op == ORC;
    const bool invert_result = op == NAND || op == NOR;

    // A known rB is inverted now; eqv is xor with the inverted constant.
    if ((invert_rb && gpr.IsImm(b)) || op == EQV)
    {
      k = ~k;
      invert_rb = false;
    }

    RCOpArg Rj;
    RCX64Reg Ra;
    const auto bind = [&] {
      Rj = gpr.Use(rj, RCMode::Read);
      Ra = gpr.Bind(a, RCMode::Write);
      RegCache::Realize(Rj, Ra);
    };
    const auto copy_rj = [&] {
      if (a != rj)
        MOV(32, Ra, Rj);
    };
    // Ra = Rj <op> k, choosing the shortest encoding.
    const auto combine = [&](EmitOp emit) {
      if (a == rj)
      {
        (this->*emit)(32, Ra, Imm32(k));
      }
      else if (FitsInImm8(k))
      {
        MOV(32, Ra, Rj);
        (this->*emit)(32, Ra, Imm32(k));
      }
      else
      {
        MOV(32, Ra, Imm32(k));
        (this->*emit)(32, Ra, Rj);
      }
    };
    // AND and OR with a possibly inverted rB and result. `identity` is the
    // constant for which the operation does nothing.
    const auto and_or = [&](EmitOp emit, u32 identity) {
      bind();
      if (k == identity)
      {
        copy_rj();
        if (invert_result || invert_rb)
          NOT(32, Ra);
        needs_test = true;
      }
      else if (invert_rb)
      {
        copy_rj();
        NOT(32, Ra);
        (this->*emit)(32, Ra, Imm32(k));
      }
      else
      {
        combine(emit);
        if (invert_result)
        {
          NOT(32, Ra);
          needs_test = true;
        }
      }
    };

    if (op == XOR_ || op == EQV)
    {
      bind();
      if (k == 0)
      {
        copy_rj();
        needs_test = true;
      }
      else if (k == 0xFFFFFFFF && !inst.Rc)
      {
        copy_rj();
        NOT(32, Ra);
      }
      else
      {
        combine(&XEmitter::XOR);
      }
    }
    else if (op == AND_ || op == ANDC || op == NAND)
    {
      and_or(&XEmitter::AND, 0xFFFFFFFF);
    }
    else if (op == OR_ || op == ORC || op == NOR)
    {
      and_or(&XEmitter::OR, 0);
    }
    else
    {
      PanicAlertFmt("WTF!");
    }
  }
  else if (s == b)
  {
    // x op x is x, ~x or a constant.
    switch (op)
    {
    case AND_:
    case OR_:
      if (a != s)
      {
        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
        RegCache::Realize(Rs, Ra);
        MOV(32, Ra, Rs);
      }
      else if (inst.Rc)
      {
        // Loads rA so the CR0 update below can test a register.
        gpr.Bind(a, RCMode::Read).Realize();
      }
      needs_test = true;
      break;
    case NAND:
    case NOR:
      if (a != s || inst.Rc)
      {
        RCOpArg Rs = gpr.Use(s, RCMode::Read);
        RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
        RegCache::Realize(Rs, Ra);
        MOV(32, Ra, Rs);
        NOT(32, Ra);
      }
      else
      {
        RCOpArg Ra = gpr.UseNoImm(a, RCMode::ReadWrite);
        RegCache::Realize(Ra);
        NOT(32, Ra);
      }
      needs_test = true;
      break;
    case ORC:
    case EQV:
      gpr.SetImmediate32(a, 0xFFFFFFFF);
      break;
    case ANDC:
    case XOR_:
      gpr.SetImmediate32(a, 0);
      break;
    default:
      PanicAlertFmt("WTF!");
      break;
    }
  }
  else if (a == s || a == b)
  {
    // In place: Ra already holds one operand, `other` is the second.
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCOpArg other = gpr.Use(a == s ? b : s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
    RegCache::Realize(Rb, Rs, other, Ra);

    // For andc and orc, rB is the inverted operand; it is either Ra itself
    // or `other`.
    const auto with_inverted_b = [&](EmitOp emit) {
      if (a == b)
      {
        NOT(32, Ra);
        (this->*emit)(32, Ra, other);
      }
      else
      {
        MOV(32, R(RSCRATCH), other);
        NOT(32, R(RSCRATCH));
        (this->*emit)(32, Ra, R(RSCRATCH));
      }
    };

    switch (op)
    {
    case AND_:
      AND(32, Ra, other);
      break;
    case OR_:
      OR(32, Ra, other);
      break;
    case XOR_:
      XOR(32, Ra, other);
      break;
    case NAND:
      AND(32, Ra, other);
      NOT(32, Ra);
      needs_test = true;
      break;
    case NOR:
      OR(32, Ra, other);
      NOT(32, Ra);
      needs_test = true;
      break;
    case EQV:
      NOT(32, Ra);
      XOR(32, Ra, other);
      break;
    case ANDC:
      if (cpu_info.bBMI1 && Rb.IsSimpleReg())
        ANDN(32, Ra, Rb.GetSimpleReg(), Rs);
      else
        with_inverted_b(&XEmitter::AND);
      break;
    case ORC:
      with_inverted_b(&XEmitter::OR);
      break;
    default:
      PanicAlertFmt("WTF");
      break;
    }
  }
  else
  {
    // Three distinct registers.
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rb, Rs, Ra);

    switch (op)
    {
    case AND_:
    case NAND:
      MOV(32, Ra, Rs);
      AND(32, Ra, Rb);
      break;
    case OR_:
    case NOR:
      MOV(32, Ra, Rs);
      OR(32, Ra, Rb);
      break;
    case XOR_:
      MOV(32, Ra, Rs);
      XOR(32, Ra, Rb);
      break;
    case EQV:
      MOV(32, Ra, Rs);
      NOT(32, Ra);
      XOR(32, Ra, Rb);
      break;
    case ANDC:
      if (cpu_info.bBMI1 && Rb.IsSimpleReg())
      {
        ANDN(32, Ra, Rb.GetSimpleReg(), Rs);
      }
      else
      {
        MOV(32, Ra, Rb);
        NOT(32, Ra);
        AND(32, Ra, Rs);
      }
      break;
    case ORC:
      MOV(32, Ra, Rb);
      NOT(32, Ra);
      OR(32, Ra, Rs);
      break;
    default:
      PanicAlertFmt("WTF!");
      break;
    }
    if (op == NAND || op == NOR)
    {
      NOT(32, Ra);
      needs_test = true;
    }
  }

  if (inst.Rc)
    ComputeRC(a, needs_test);
}

void Jit64::extsXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  // extsh is 922, extsb 954.
  const int bits = inst.SUBOP10 == 922 ? 16 : 8;

  {
    RCOpArg Rs = gpr.UseNoImm(inst.RS, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);
    MOVSX(32, bits, Ra, Rs);
  }
  if (inst.Rc)
    ComputeRC(a);
}

// ---------------------------------------------------------------------------
// Addition and subtraction

void Jit64::subfic(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 d = inst.RD;
  const s32 imm = inst.SIMM_16;

  RCOpArg Ra = gpr.Use(a, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rd);

  // rD = imm - rA; CA is set when there is no borrow.
  if (imm == 0 || imm == -1)
  {
    if (d != a)
      MOV(32, Rd, Ra);
    if (imm == 0)
    {
      // 0 - x: NEG sets CF exactly when there is a borrow.
      NEG(32, Rd);
      FinalizeCarry(CC_NC);
    }
    else
    {
      // -1 - x is ~x and never borrows.
      NOT(32, Rd);
      FinalizeCarry(true);
    }
  }
  else if (d == a)
  {
    // imm - x == ~x + (imm + 1), whose carry is the PowerPC carry.
    NOT(32, Rd);
    ADD(32, Rd, Imm32(static_cast<u32>(imm) + 1));
    FinalizeCarry(CC_C);
  }
  else
  {
    MOV(32, Rd, Imm32(static_cast<u32>(imm)));
    SUB(32, Rd, Ra);
    FinalizeCarry(CC_NC);
  }
}

void Jit64::subfx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;
  // subfc (8) sets CA; subf (40) does not.
  const bool carry = (inst.SUBOP10 & 32) == 0;

  // After a SUB the host carry is the borrow, the inverse of CA.
  const auto finish_sub = [&] {
    if (carry)
      FinalizeCarry(CC_NC);
    if (inst.OE)
      GenerateOverflow();
  };

  if (gpr.IsImm(a))
  {
    // rD = rB - k
    const s32 k = gpr.SImm32(a);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rb, Rd);

    if (k == 0)
    {
      if (d != b)
        MOV(32, Rd, Rb);
      if (carry)
        FinalizeCarry(true);
      if (inst.OE)
        GenerateConstantOverflow(false);
    }
    else if (d == b)
    {
      SUB(32, Rd, Imm32(static_cast<u32>(k)));
      finish_sub();
    }
    else if (!carry && !inst.OE && Rb.IsSimpleReg())
    {
      LEA(32, Rd, MDisp(Rb.GetSimpleReg(), static_cast<s32>(0u - static_cast<u32>(k))));
    }
    else
    {
      MOV(32, Rd, Rb);
      SUB(32, Rd, Imm32(static_cast<u32>(k)));
      finish_sub();
    }
  }
  else if (gpr.IsImm(b) && gpr.Imm32(b) == 0)
  {
    // rD = 0 - rA
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rd);
    if (d != a)
      MOV(32, Rd, Ra);
    NEG(32, Rd);
    finish_sub();
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (d == a && d != b)
    {
      // Subtraction does not commute, so rA is copied out first.
      MOV(32, R(RSCRATCH), Ra);
      MOV(32, Rd, Rb);
      SUB(32, Rd, R(RSCRATCH));
    }
    else
    {
      if (d != b)
        MOV(32, Rd, Rb);
      SUB(32, Rd, Ra);
    }
    finish_sub();
  }

  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::addx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;
  // addc (10) sets CA; add (266) does not.
  const bool carry = (inst.SUBOP10 & (1 << 8)) == 0;

  const auto finish_add = [&] {
    if (carry)
      FinalizeCarry(CC_C);
    if (inst.OE)
      GenerateOverflow();
  };

  if (!gpr.IsImm(a) && !gpr.IsImm(b))
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rb, Rd);

    if (d == a)
    {
      ADD(32, Rd, Rb);
    }
    else if (d == b)
    {
      ADD(32, Rd, Ra);
    }
    else if (!carry && !inst.OE && Ra.IsSimpleReg() && Rb.IsSimpleReg())
    {
      LEA(32, Rd, MRegSum(Ra.GetSimpleReg(), Rb.GetSimpleReg()));
    }
    else
    {
      MOV(32, Rd, Ra);
      ADD(32, Rd, Rb);
    }
    finish_add();
  }
  else
  {
    // rD = rj + k, with k the known operand.
    const bool a_known = gpr.IsImm(a);
    const u32 rj = a_known ? b : a;
    const s32 k = gpr.SImm32(a_known ? a : b);
    RCOpArg Rj = gpr.Use(rj, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rj, Rd);

    if (k == 0)
    {
      if (d != rj)
        MOV(32, Rd, Rj);
      if (carry)
        FinalizeCarry(false);
      if (inst.OE)
        GenerateConstantOverflow(false);
    }
    else if (d == rj)
    {
      ADD(32, Rd, Imm32(static_cast<u32>(k)));
      finish_add();
    }
    else if (!carry && !inst.OE && Rj.IsSimpleReg())
    {
      LEA(32, Rd, MDisp(Rj.GetSimpleReg(), k));
    }
    else
    {
      if (FitsInImm8(static_cast<u32>(k)))
      {
        MOV(32, Rd, Rj);
        ADD(32, Rd, Imm32(static_cast<u32>(k)));
      }
      else
      {
        MOV(32, Rd, Imm32(static_cast<u32>(k)));
        ADD(32, Rd, Rj);
      }
      finish_add();
    }
  }

  if (inst.Rc)
    ComputeRC(d);
}

// adde, addme, addze, subfe, subfme, subfze: an add with carry-in from CA.
// The subf forms compute ~rA + src + CA.
void Jit64::arithXex(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const bool from_reg = (inst.SUBOP10 & 64) == 0;   // adde, subfe
  const bool minus_one = (inst.SUBOP10 & 32) != 0;  // "me" forms add -1, "ze" forms 0
  const bool is_add = (inst.SUBOP10 & 2) != 0;
  const u32 a = inst.RA;
  const u32 b = from_reg ? u32{inst.RB} : a;
  const u32 d = inst.RD;

  // The carry-in goes to the host carry flag.
  if (js.carryFlag == CarryFlag::InPPCState)
    JitGetAndClearCAOV(inst.OE);
  else
    UnlockFlags();

  const bool carry_inverted = js.carryFlag == CarryFlag::InHostCarryInverted;
  // Whether the host carry after the operation is the inverse of CA.
  bool result_inverted = false;

  if (!is_add && from_reg && a == b)
  {
    // subfe rD, rA, rA: ~x + x + CA is CA - 1, which SBB of a register with
    // itself computes from the borrow.
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Rd);
    if (!carry_inverted)
      CMC();
    SBB(32, Rd, Rd);
    result_inverted = true;
  }
  else if (!is_add && from_reg && d == b)
  {
    // rD = rB - rA - borrow
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::ReadWrite);
    RegCache::Realize(Ra, Rd);
    if (!carry_inverted)
      CMC();
    SBB(32, Rd, Ra);
    result_inverted = true;
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCOpArg addend = from_reg ? gpr.Use(d == b ? a : b, RCMode::Read) :
                                RCOpArg::Imm32(minus_one ? 0xFFFFFFFF : 0);
    RegCache::Realize(Ra, Rb, Rd, addend);

    if (d != a && d != b)
      MOV(32, Rd, Ra);
    if (!is_add)
      NOT(32, Rd);

    if (carry_inverted && addend.IsImm())
    {
      // x + k + !borrow == x - (-1 - k) - borrow
      SBB(32, Rd, Imm32(static_cast<u32>(-1 - addend.SImm32())));
      result_inverted = true;
    }
    else
    {
      if (carry_inverted)
        CMC();
      ADC(32, Rd, addend);
    }
  }

  FinalizeCarryOverflow(inst.OE, result_inverted);
  if (inst.Rc)
    ComputeRC(d);
}

// ---------------------------------------------------------------------------
// Multiplication

void Jit64::MultiplyImmediate(u32 imm, int a, int d, bool overflow)
{
  const preg_t src = static_cast<preg_t>(a);
  const preg_t dst = static_cast<preg_t>(d);
  RCOpArg Ra = gpr.UseNoImm(src, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(dst, RCMode::Write);
  RegCache::Realize(Ra, Rd);

  if (imm == 0xFFFFFFFF)
  {
    // NEG sets OF exactly as IMUL by -1 would.
    if (dst != src)
      MOV(32, Rd, Ra);
    NEG(32, Rd);
    return;
  }

  // Shifts and LEA do not set OF, so they are only used without OE.
  if (!overflow && MathUtil::IsPow2(imm))
  {
    const u32 shift = static_cast<u32>(MathUtil::IntLog2(imm));
    if (shift >= 1 && shift <= 3 && dst != src && Ra.IsSimpleReg())
    {
      LEA(32, Rd, MScaled(Ra.GetSimpleReg(), SCALE_1 << shift, 0));
      return;
    }
    if (dst != src)
      MOV(32, Rd, Ra);
    if (shift != 0)
      SHL(32, Rd, Imm8(static_cast<u8>(shift)));
    return;
  }

  // x * 3, 5 and 9 as x + x * 2, 4 and 8. (Larger multiples of these via an
  // extra shift were measured to be slower.)
  if (!overflow && Ra.IsSimpleReg())
  {
    const X64Reg x = Ra.GetSimpleReg();
    if (imm == 3)
    {
      LEA(32, Rd, MComplex(x, x, SCALE_2, 0));
      return;
    }
    if (imm == 5)
    {
      LEA(32, Rd, MComplex(x, x, SCALE_4, 0));
      return;
    }
    if (imm == 9)
    {
      LEA(32, Rd, MComplex(x, x, SCALE_8, 0));
      return;
    }
  }

  IMUL(32, Rd, Ra, Imm32(imm));
}

void Jit64::mulli(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 factor = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  MultiplyImmediate(factor, static_cast<int>(inst.RA), static_cast<int>(inst.RD), false);
}

void Jit64::mullwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;

  if (!gpr.IsImm(a) && !gpr.IsImm(b))
  {
    RCOpArg lhs = gpr.Use(a, RCMode::Read);
    RCOpArg rhs = gpr.Use(b, RCMode::Read);
    RCX64Reg product = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(lhs, rhs, product);

    // Two-operand IMUL: multiply into whichever source already is rD.
    if (d != a)
    {
      if (d != b)
        MOV(32, product, rhs);
      IMUL(32, product, lhs);
    }
    else
    {
      IMUL(32, product, rhs);
    }
  }
  else
  {
    const bool a_known = gpr.IsImm(a);
    const u32 factor = a_known ? gpr.Imm32(a) : gpr.Imm32(b);
    const u32 other = a_known ? b : a;
    MultiplyImmediate(factor, static_cast<int>(other), static_cast<int>(d), inst.OE);
  }

  if (inst.OE)
    GenerateOverflow();
  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::mulhwXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;
  const bool is_signed = inst.SUBOP10 == 75;  // mulhw; mulhwu is 11

  if (!is_signed)
  {
    // A 64-bit IMUL of the zero-extended operands gives the unsigned
    // product (for signed operands this would need two MOVSX). Both must be
    // zero-extended registers.
    const u32 multiplier = d == b ? a : b;
    const u32 multiplicand = multiplier == b ? a : b;

    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCX64Reg Rm = gpr.Bind(multiplier, RCMode::Read);
    RCOpArg Rn = gpr.Use(multiplicand, RCMode::Read);
    RegCache::Realize(Rd, Rm, Rn);

    if (multiplicand != d)
      MOV(32, Rd, Rn);
    IMUL(64, Rd, Rm);
    SHR(64, Rd, Imm8(32));
  }
  else
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCOpArg Rb = gpr.UseNoImm(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RCX64Reg eax = gpr.Scratch(EAX);
    RCX64Reg edx = gpr.Scratch(EDX);
    RegCache::Realize(Ra, Rb, Rd, eax, edx);

    // One-operand IMUL leaves the high word in EDX.
    MOV(32, eax, Ra);
    IMUL(32, Rb);
    MOV(32, Rd, edx);
  }

  if (inst.Rc)
    ComputeRC(d);
}

// ---------------------------------------------------------------------------
// Division
//
// Division by zero (and 0x80000000 / -1) gives what the Gekko produces:
// divwu gives 0, divw gives 0 or -1 by the dividend's sign.

void Jit64::divwux(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;

  if (!gpr.IsImm(b))
  {
    RCOpArg Ra = gpr.Use(a, RCMode::Read);
    RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    // DIV takes the dividend in EDX:EAX.
    RCX64Reg eax = gpr.Scratch(EAX);
    RCX64Reg edx = gpr.Scratch(EDX);
    RegCache::Realize(Ra, Rb, Rd, eax, edx);

    MOV(32, eax, Ra);
    XOR(32, edx, edx);
    TEST(32, Rb, Rb);
    const FixupBranch nonzero = J_CC(CC_NZ);
    MOV(32, Rd, edx);  // zero
    if (inst.OE)
      GenerateConstantOverflow(true);
    const FixupBranch done = J();

    SetJumpTarget(nonzero);
    DIV(32, Rb);
    MOV(32, Rd, eax);
    if (inst.OE)
      GenerateConstantOverflow(false);
    SetJumpTarget(done);
  }
  else if (const u32 divisor = gpr.Imm32(b); divisor == 0)
  {
    gpr.SetImmediate32(d, 0);
    if (inst.OE)
      GenerateConstantOverflow(true);
  }
  else
  {
    if (MathUtil::IsPow2(divisor))
    {
      const u32 shift = static_cast<u32>(MathUtil::IntLog2(divisor));
      RCOpArg Ra = gpr.Use(a, RCMode::Read);
      RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
      RegCache::Realize(Ra, Rd);
      if (d != a)
        MOV(32, Rd, Ra);
      if (shift != 0)
        SHR(32, Rd, Imm8(static_cast<u8>(shift)));
    }
    else
    {
      // Multiply by a magic constant and keep the high bits.
      const UnsignedMagic magic = UnsignedDivisionConstants(divisor);
      const u8 total_shift = static_cast<u8>(magic.shift + 32);

      if (magic.fast)
      {
        // (x * m) >> (32 + shift), with x zero-extended.
        RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
        RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
        RegCache::Realize(Ra, Rd);

        // The imm32 form sign-extends, so a multiplier with the top bit set
        // goes through a register.
        if ((magic.multiplier & 0x80000000) == 0)
        {
          IMUL(64, Rd, Ra, Imm32(magic.multiplier));
        }
        else if (d != a)
        {
          MOV(32, Rd, Imm32(magic.multiplier));
          IMUL(64, Rd, Ra);
        }
        else
        {
          MOV(32, R(RSCRATCH), Imm32(magic.multiplier));
          IMUL(64, Rd, R(RSCRATCH));
        }
        SHR(64, Rd, Imm8(total_shift));
      }
      else
      {
        // ((x + 1) * m) >> (32 + shift), computed as x * m + m.
        RCOpArg Ra = gpr.Use(a, RCMode::Read);
        RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
        RegCache::Realize(Ra, Rd);

        MOV(32, R(RSCRATCH), Imm32(magic.multiplier));
        if (d != a)
          MOV(32, Rd, Ra);
        IMUL(64, Rd, R(RSCRATCH));
        ADD(64, Rd, R(RSCRATCH));
        SHR(64, Rd, Imm8(total_shift));
      }
    }
    if (inst.OE)
      GenerateConstantOverflow(false);
  }

  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::divwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;

  if (gpr.IsImm(a))
    DivideKnownDividend(inst, gpr.Imm32(a));
  else if (gpr.IsImm(b))
    DivideByConstant(inst, gpr.SImm32(b));
  else
    DivideRegisters(inst);

  if (inst.Rc)
    ComputeRC(d);
}

void Jit64::DivideKnownDividend(UGeckoInstruction inst, u32 dividend)
{
  const u32 b = inst.RB;
  const u32 d = inst.RD;
  const bool negative_dividend = (dividend & 0x80000000) != 0;

  if (dividend == 0)
  {
    // 0 / x is 0 for every x; only x == 0 overflows.
    if (inst.OE)
    {
      RCOpArg Rb = gpr.Use(b, RCMode::Read);
      RegCache::Realize(Rb);
      CMP_or_TEST(32, Rb, Imm32(0));
      GenerateOverflow(CC_NZ);
    }
    gpr.SetImmediate32(d, 0);
    return;
  }

  RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  // IDIV takes the dividend in EDX:EAX.
  RCX64Reg eax = gpr.Scratch(EAX);
  RCX64Reg edx = gpr.Scratch(EDX);
  RegCache::Realize(Rb, Rd, eax, edx);

  TEST(32, Rb, Rb);

  FixupBranch done;
  if (d == b && !negative_dividend && !inst.OE)
  {
    // Division by zero must give 0, which rD (== rB) already holds.
    done = J_CC(CC_Z);
  }
  else
  {
    // The invalid cases: x / 0, and 0x80000000 / -1.
    FixupBranch valid;
    if (dividend == 0x80000000)
    {
      const FixupBranch by_zero = J_CC(CC_Z);
      CMP(32, Rb, Imm32(0xFFFFFFFF));
      valid = J_CC(CC_NE);
      SetJumpTarget(by_zero);
    }
    else
    {
      valid = J_CC(CC_NZ);
    }

    // The result is the dividend's sign, replicated.
    if (negative_dividend)
      MOV(32, Rd, Imm32(0xFFFFFFFF));
    else if (d != b)
      XOR(32, Rd, Rd);
    if (inst.OE)
      GenerateConstantOverflow(true);
    done = J();

    SetJumpTarget(valid);
  }

  MOV(32, eax, Imm32(dividend));
  CDQ();
  IDIV(32, Rb);
  MOV(32, Rd, eax);
  if (inst.OE)
    GenerateConstantOverflow(false);

  SetJumpTarget(done);
}

void Jit64::DivideByConstant(UGeckoInstruction inst, s32 divisor)
{
  const u32 a = inst.RA;
  const u32 d = inst.RD;
  RCOpArg Ra = gpr.Use(a, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
  RegCache::Realize(Ra, Rd);

  // Every path except division by zero leaves OV clear.
  bool clear_ov = true;

  if (divisor == 0 || divisor == 1 || divisor == -1)
  {
    if (d != a)
      MOV(32, Rd, Ra);

    if (divisor == 0)
    {
      // 0 or -1 by the dividend's sign.
      SAR(32, Rd, Imm8(31));
      clear_ov = false;
      if (inst.OE)
        GenerateConstantOverflow(true);
    }
    else if (divisor == -1)
    {
      // NEG overflows only for 0x80000000, whose result is -1.
      NEG(32, Rd);
      const FixupBranch no_overflow = J_CC(CC_NO);
      MOV(32, Rd, Imm32(0xFFFFFFFF));
      if (inst.OE)
        GenerateConstantOverflow(true);
      const FixupBranch done = J();
      SetJumpTarget(no_overflow);
      if (inst.OE)
        GenerateConstantOverflow(false);
      SetJumpTarget(done);
      clear_ov = false;
    }
  }
  else if (divisor == 2 || divisor == -2)
  {
    // (x + (x >>> 31)) >> 1, rounding toward zero. `bias` receives the sign
    // bit; `value` is added to it.
    X64Reg value = RSCRATCH;
    X64Reg bias = RSCRATCH;
    if (!Ra.IsSimpleReg())
    {
      MOV(32, R(RSCRATCH), Ra);
      MOV(32, Rd, R(RSCRATCH));
    }
    else if (d == a)
    {
      MOV(32, R(RSCRATCH), Ra);
    }
    else
    {
      MOV(32, Rd, Ra);
      value = Ra.GetSimpleReg();
      bias = Rd;
    }

    SHR(32, R(bias), Imm8(31));
    ADD(32, Rd, R(value));
    SAR(32, Rd, Imm8(1));
    if (divisor < 0)
      NEG(32, Rd);
  }
  else if (MathUtil::IsPow2(divisor) || MathUtil::IsPow2(-static_cast<s64>(divisor)))
  {
    // Negative dividends get 2^k - 1 added first so the shift rounds
    // toward zero: rD = (x < 0 ? x + 2^k - 1 : x) >> k.
    const u32 magnitude = static_cast<u32>(std::abs(static_cast<s64>(divisor)));

    X64Reg x;
    X64Reg biased;
    X64Reg pick;
    CCFlags pick_when = CC_NS;
    if (!Ra.IsSimpleReg())
    {
      x = RSCRATCH;
      biased = Rd;
      pick = RSCRATCH;
      MOV(32, R(x), Ra);
    }
    else if (d != a)
    {
      x = Ra.GetSimpleReg();
      biased = Rd;
      pick = x;
    }
    else
    {
      // rD holds x, so the biased value goes to RSCRATCH and the condition
      // flips.
      x = Rd;
      biased = RSCRATCH;
      pick = RSCRATCH;
      pick_when = CC_S;
    }

    TEST(32, R(x), R(x));
    LEA(32, biased, MDisp(x, static_cast<s32>(magnitude - 1)));
    CMOVcc(32, Rd, R(pick), pick_when);
    SAR(32, Rd, Imm8(static_cast<u8>(MathUtil::IntLog2(magnitude))));
    if (divisor < 0)
      NEG(32, Rd);
  }
  else
  {
    // q = high32(x * m) (+ or - x when m's sign differs from the divisor's),
    // shifted right arithmetically, plus one when q is negative.
    const SignedMagic magic = SignedDivisionConstants(divisor);
    const u32 multiplier = static_cast<u32>(magic.multiplier);

    MOVSX(64, 32, RSCRATCH, Ra);

    if (divisor > 0 && magic.multiplier < 0)
    {
      IMUL(64, Rd, R(RSCRATCH), Imm32(multiplier));
      SHR(64, Rd, Imm8(32));
      ADD(32, Rd, R(RSCRATCH));
      SHR(32, R(RSCRATCH), Imm8(31));
      SAR(32, Rd, Imm8(magic.shift));
    }
    else if (divisor < 0 && magic.multiplier > 0)
    {
      IMUL(64, Rd, R(RSCRATCH), Imm32(multiplier));
      SHR(64, R(RSCRATCH), Imm8(32));
      SUB(32, R(RSCRATCH), Rd);
      MOV(32, Rd, R(RSCRATCH));
      SHR(32, Rd, Imm8(31));
      SAR(32, R(RSCRATCH), Imm8(magic.shift));
    }
    else if (magic.multiplier > 0)
    {
      IMUL(64, Rd, R(RSCRATCH), Imm32(multiplier));
      SHR(32, R(RSCRATCH), Imm8(31));
      SAR(64, R(Rd), Imm8(static_cast<u8>(32 + magic.shift)));
    }
    else
    {
      IMUL(64, RSCRATCH, R(RSCRATCH), Imm32(multiplier));
      MOV(64, Rd, R(RSCRATCH));
      SHR(64, R(RSCRATCH), Imm8(63));
      SAR(64, R(Rd), Imm8(static_cast<u8>(32 + magic.shift)));
    }

    ADD(32, Rd, R(RSCRATCH));
  }

  if (clear_ov && inst.OE)
    GenerateConstantOverflow(false);
}

void Jit64::DivideRegisters(UGeckoInstruction inst)
{
  RCOpArg Ra = gpr.Use(inst.RA, RCMode::Read);
  RCX64Reg Rb = gpr.Bind(inst.RB, RCMode::Read);
  RCX64Reg Rd = gpr.Bind(inst.RD, RCMode::Write);
  RCX64Reg eax = gpr.Scratch(EAX);
  RCX64Reg edx = gpr.Scratch(EDX);
  RegCache::Realize(Ra, Rb, Rd, eax, edx);

  MOV(32, eax, Ra);
  TEST(32, Rb, Rb);
  const FixupBranch by_zero = J_CC(CC_E);
  CMP(32, eax, Imm32(0x80000000));
  const FixupBranch valid_dividend = J_CC(CC_NE);
  CMP(32, Rb, Imm32(0xFFFFFFFF));
  const FixupBranch valid_divisor = J_CC(CC_NE);

  // Invalid: the result is the dividend's sign, replicated.
  SetJumpTarget(by_zero);
  SAR(32, eax, Imm8(31));
  if (inst.OE)
    GenerateConstantOverflow(true);
  const FixupBranch done = J();

  SetJumpTarget(valid_dividend);
  SetJumpTarget(valid_divisor);
  CDQ();
  IDIV(32, Rb);
  if (inst.OE)
    GenerateConstantOverflow(false);

  SetJumpTarget(done);
  MOV(32, Rd, eax);
}

void Jit64::negx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 d = inst.RD;

  {
    RCOpArg Ra = gpr.UseNoImm(a, RCMode::Read);
    RCX64Reg Rd = gpr.Bind(d, RCMode::Write);
    RegCache::Realize(Ra, Rd);
    if (d != a)
      MOV(32, Rd, Ra);
    NEG(32, Rd);
    if (inst.OE)
      GenerateOverflow();
  }
  // NEG's SF and ZF describe the result, and OF is only set for
  // 0x80000000, where SF is right as well.
  if (inst.Rc)
    ComputeRC(d, false);
}

// ---------------------------------------------------------------------------
// Rotates

void Jit64::rlwinmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 s = inst.RS;
  const u32 sh = inst.SH;
  const u32 mb = inst.MB;
  const u32 me = inst.ME;

  // Special forms: slwi, srwi, and extrwi of a field ending at bit 31.
  const bool is_slwi = sh != 0 && mb == 0 && me == 31 - sh;
  const bool is_srwi = sh != 0 && me == 31 && mb == 32 - sh;
  const bool is_extract = sh != 0 && me == 31 && mb > 32 - sh;
  const u32 mask = MakeRotationMask(mb, me);
  // The mask before the rotation, i.e. which source bits survive.
  const u32 source_mask = std::rotr(mask, static_cast<int>(sh));
  const bool byte_or_half_mask = mask == 0xff || mask == 0xffff;
  const bool byte_or_half_source = source_mask == 0xff || source_mask == 0xffff;
  const int width = static_cast<int>(me) - static_cast<int>(mb) + 1;

  // Whether the host flags still need setting for CR0, and whether the
  // result may have its top bit set.
  bool needs_test = true;
  bool needs_sext = true;

  if (byte_or_half_mask && (static_cast<int>(sh) & (width - 1)) == 0 && !gpr.IsBound(s) &&
      !gpr.IsImm(s))
  {
    // An aligned byte or halfword of rS, which is in memory: load it
    // directly. (The location is taken before binding rA, since binding
    // rA == rS would load rS into a register.)
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Rs);
    OpArg field = Rs.Location();
    if (sh != 0)
      field.AddMemOffset(static_cast<int>((32 - sh) >> 3));
    Rs.Unlock();

    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Ra);
    MOVZX(32, width, Ra, field);
    needs_sext = false;
  }
  else
  {
    RCOpArg Rs = gpr.UseNoImm(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);

    if (is_slwi && sh <= 3 && a != s && Rs.IsSimpleReg())
    {
      LEA(32, Ra, MScaled(Rs.GetSimpleReg(), SCALE_1 << sh, 0));
    }
    else if (byte_or_half_source && !is_slwi)
    {
      // Zero-extend the surviving byte or halfword, then rotate it in.
      MOVZX(32, source_mask == 0xff ? 8 : 16, Ra, Rs);
      if (sh != 0)
        ROL(32, Ra, Imm8(static_cast<u8>(sh)));
      needs_sext = (mask & 0x80000000) != 0;
    }
    else if (is_extract && cpu_info.bBMI1 && cpu_info.vendor == CPUVendor::AMD)
    {
      // BEXTR is a single uop only on AMD.
      MOV(32, R(RSCRATCH), Imm32((static_cast<u32>(width) << 8) | (32 - sh)));
      BEXTR(32, Ra, Rs, RSCRATCH);
      needs_sext = false;
    }
    else if (is_slwi || is_srwi)
    {
      if (a != s)
        MOV(32, Ra, Rs);
      if (is_slwi)
      {
        SHL(32, Ra, Imm8(static_cast<u8>(sh)));
      }
      else
      {
        SHR(32, Ra, Imm8(static_cast<u8>(mb)));
        needs_sext = false;
      }
    }
    else
    {
      RotateLeft(32, Ra, Rs, static_cast<u8>(sh));
      if (mb != 0 || me != 31)
      {
        // A fused branch needs the flags of a real AND.
        if (inst.Rc && CheckMergedBranch(0))
          AND(32, Ra, Imm32(mask));
        else
          AndWithMask(Ra, mask);
        needs_sext = mb == 0;
        needs_test = false;
      }
    }
  }

  if (inst.Rc)
    ComputeRC(a, needs_test, needs_sext);
}

void Jit64::rlwimix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 s = inst.RS;
  const u32 sh = inst.SH;
  const u8 sh8 = static_cast<u8>(sh);

  // rA = (rotl(rS, sh) & mask) | (rA & ~mask)
  const u32 mask = MakeRotationMask(inst.MB, inst.ME);
  // When the mask is exactly the bits a shift keeps, the shift does the
  // masking too.
  const bool shift_left_fits = mask == 0U - (1U << sh);
  const bool shift_right_fits = mask == (1U << sh) - 1;
  const bool byte_or_half = mask == 0xFF || mask == 0xFFFF;
  bool needs_test = false;

  // rotl(rS, sh) into `out` with only the masked bits possibly set.
  const auto shifted_source = [&](X64Reg out, const OpArg& src) {
    if (shift_left_fits)
    {
      MOV(32, R(out), src);
      SHL(32, R(out), Imm8(sh8));
    }
    else if (shift_right_fits)
    {
      MOV(32, R(out), src);
      SHR(32, R(out), Imm8(static_cast<u8>(32 - sh)));
    }
    else
    {
      RotateLeft(32, out, src, sh8);
    }
  };

  if (mask == 0 || (a == s && sh == 0))
  {
    // rA is unchanged.
    needs_test = true;
  }
  else if (mask == 0xFFFFFFFF)
  {
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);
    RotateLeft(32, Ra, Rs, sh8);
    needs_test = true;
  }
  else if (gpr.IsImm(s))
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
    RegCache::Realize(Ra);
    AndWithMask(Ra, ~mask);
    OR(32, Ra, Imm32(std::rotl(gpr.Imm32(s), static_cast<int>(sh)) & mask));
  }
  else if (gpr.IsImm(a))
  {
    const u32 kept = gpr.Imm32(a) & ~mask;

    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RegCache::Realize(Rs, Ra);

    if (sh == 0)
    {
      MOV(32, Ra, Rs);
      AndWithMask(Ra, mask);
    }
    else if (shift_left_fits || shift_right_fits)
    {
      shifted_source(Ra, Rs);
    }
    else
    {
      RotateLeft(32, Ra, Rs, sh8);
      AndWithMask(Ra, mask);
    }

    if (kept != 0)
      OR(32, Ra, Imm32(kept));
    else
      needs_test = true;
  }
  else if (sh != 0)
  {
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
    RegCache::Realize(Rs, Ra);

    shifted_source(RSCRATCH, Rs);
    if (byte_or_half)
    {
      // A partial-register move does the merge.
      MOV(mask == 0xFF ? 8 : 16, Ra, R(RSCRATCH));
      needs_test = true;
    }
    else
    {
      if (!shift_left_fits && !shift_right_fits)
        AndWithMask(RSCRATCH, mask);
      AndWithMask(Ra, ~mask);
      OR(32, Ra, R(RSCRATCH));
    }
  }
  else
  {
    RCX64Reg Rs = gpr.Bind(s, RCMode::Read);
    RCX64Reg Ra = gpr.Bind(a, RCMode::ReadWrite);
    RegCache::Realize(Rs, Ra);

    if (byte_or_half)
    {
      MOV(mask == 0xFF ? 8 : 16, Ra, Rs);
      needs_test = true;
    }
    else
    {
      // a ^ ((a ^ s) & mask), i.e. s where the mask is set.
      XOR(32, Ra, Rs);
      AndWithMask(Ra, ~mask);
      XOR(32, Ra, Rs);
    }
  }

  if (inst.Rc)
    ComputeRC(a, needs_test);
}

void Jit64::rlwnmx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 s = inst.RS;
  const u32 mask = MakeRotationMask(inst.MB, inst.ME);

  // A fused branch needs the flags of a real AND.
  const auto apply_mask = [&](RCX64Reg& Ra) {
    if (inst.Rc && CheckMergedBranch(0))
      AND(32, Ra, Imm32(mask));
    else
      AndWithMask(Ra, mask);
  };

  if (!gpr.IsImm(b))
  {
    // Variable rotates take the count in CL.
    RCX64Reg ecx = gpr.Scratch(ECX);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
      MOV(32, Ra, Rs);
    ROL(32, Ra, ecx);
    apply_mask(Ra);
  }
  else
  {
    const u8 count = static_cast<u8>(gpr.Imm32(b) & 0x1f);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);
    RotateLeft(32, Ra, Rs, count);
    apply_mask(Ra);
  }

  if (inst.Rc)
    ComputeRC(a, false);
}

// ---------------------------------------------------------------------------
// Shifts

// slw and srw. PowerPC uses six bits of rB: counts of 32 to 63 give 0. A
// 64-bit shift of the zero-extended value gets that for free.
void Jit64::EmitWordShift(UGeckoInstruction inst, bool left)
{
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 s = inst.RS;

  if (gpr.IsImm(b))
  {
    const u32 count = gpr.Imm32(b);
    if ((count & 0x20) != 0)
    {
      gpr.SetImmediate32(a, 0);
    }
    else
    {
      RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
      RCOpArg Rs = gpr.Use(s, RCMode::Read);
      RegCache::Realize(Ra, Rs);
      if (a != s)
        MOV(32, Ra, Rs);
      if (const u8 bits = static_cast<u8>(count & 0x1f); bits != 0)
      {
        if (left)
          SHL(32, Ra, Imm8(bits));
        else
          SHR(32, Ra, Imm8(bits));
      }
    }
    // A shift by 0 does not set the flags, so CR0 needs a TEST.
    if (inst.Rc)
      ComputeRC(a);
    return;
  }

  // A left shift leaves bits in the upper half, which must be cleared;
  // with Rc, the AND that does so also sets the flags. `held` lists the
  // operands to release before CR0 is computed.
  const auto finish_left = [&](RCX64Reg& Ra, auto&... held) {
    if (inst.Rc)
    {
      AND(32, Ra, Ra);
      RegCache::Unlock(held...);
      ComputeRC(a, false);
    }
    else
    {
      MOVZX(64, 32, Ra, Ra);
    }
  };

  if (cpu_info.bBMI2)
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
    if (left)
    {
      RCOpArg Rs = gpr.UseNoImm(s, RCMode::Read);
      RegCache::Realize(Ra, Rb, Rs);
      SHLX(64, Ra, Rs, Rb);
      finish_left(Ra, Ra, Rb, Rs);
      return;
    }
    // A 64-bit SHRX needs rS in a register: a memory operand would read the
    // neighbouring register's slot as the upper half.
    RCX64Reg Rs = gpr.Bind(s, RCMode::Read);
    RegCache::Realize(Ra, Rb, Rs);
    SHRX(64, Ra, Rs, Rb);
  }
  else
  {
    RCX64Reg ecx = gpr.Scratch(ECX);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(ecx, Ra, Rb, Rs);

    MOV(32, ecx, Rb);
    if (a != s)
      MOV(32, Ra, Rs);
    if (left)
    {
      SHL(64, Ra, ecx);
      finish_left(Ra, ecx, Ra, Rb, Rs);
      return;
    }
    SHR(64, Ra, ecx);
  }

  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::srwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  EmitWordShift(inst, false);
}

void Jit64::slwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  EmitWordShift(inst, true);
}

// sraw: CA is set when the result is negative and any 1 bits were shifted
// out.
void Jit64::srawx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 s = inst.RS;

  if (gpr.IsImm(b))
  {
    const u32 count = gpr.Imm32(b);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (a != s)
      MOV(32, Ra, Rs);

    const u8 bits = static_cast<u8>(count & 0x1f);
    if ((count & 0x20) != 0)
    {
      // Everything shifted out: the sign fills the word, and CA is the
      // sign.
      SAR(32, Ra, Imm8(31));
      FinalizeCarry(CC_NZ);
    }
    else if (bits == 0)
    {
      FinalizeCarry(false);
    }
    else if (js.op->wantsCA)
    {
      // The shifted-out bits are the low `bits` bits of the source; they
      // count only if the result is negative (all ones in Ra's top).
      MOV(32, R(RSCRATCH), Ra);
      SAR(32, Ra, Imm8(bits));
      SHL(32, R(RSCRATCH), Imm8(static_cast<u8>(32 - bits)));
      TEST(32, Ra, R(RSCRATCH));
      FinalizeCarry(CC_NZ);
    }
    else
    {
      SAR(32, Ra, Imm8(bits));
      FinalizeCarry(CC_NZ);
    }
  }
  else
  {
    // With the value in the upper half of a 64-bit register, a 64-bit SAR
    // by up to 63 gives the result in the upper half and the shifted-out
    // bits in the lower half.
    const auto finish = [&](RCX64Reg& Ra) {
      if (js.op->wantsCA)
      {
        MOV(32, R(RSCRATCH), Ra);
        SHR(64, Ra, Imm8(32));
        TEST(32, Ra, R(RSCRATCH));
      }
      else
      {
        SHR(64, Ra, Imm8(32));
      }
      FinalizeCarry(CC_NZ);
    };

    if (cpu_info.bBMI2)
    {
      RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
      RCX64Reg Rb = gpr.Bind(b, RCMode::Read);
      RCOpArg Rs = gpr.Use(s, RCMode::Read);
      RegCache::Realize(Ra, Rb, Rs);

      X64Reg wide = RSCRATCH;
      if (a == s && a != b)
        wide = Ra;
      else
        MOV(32, R(wide), Rs);
      SHL(64, R(wide), Imm8(32));
      SARX(64, Ra, R(wide), Rb);
      finish(Ra);
    }
    else
    {
      // Legacy shifts take their count in CL.
      RCX64Reg count = gpr.Scratch(ECX);
      RCX64Reg target = gpr.Bind(a, RCMode::Write);
      RCOpArg amount = gpr.Use(b, RCMode::Read);
      RCOpArg value = gpr.Use(s, RCMode::Read);
      RegCache::Realize(count, target, amount, value);

      MOV(32, count, amount);
      if (a != s)
        MOV(32, target, value);
      // Place rS in the high half so the carry-out bits land in the low half.
      SHL(64, target, Imm8(32));
      SAR(64, target, count);
      finish(target);
    }
  }

  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::srawix(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  const u32 s = inst.RS;
  const u8 bits = static_cast<u8>(inst.SH);

  if (bits == 0)
  {
    FinalizeCarry(false);
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);
    if (a != s)
      MOV(32, Ra, Rs);
  }
  else
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (!js.op->wantsCA)
    {
      if (a != s)
        MOV(32, Ra, Rs);
      SAR(32, Ra, Imm8(bits));
    }
    else
    {
      MOV(32, R(RSCRATCH), Rs);
      if (a != s)
        MOV(32, Ra, R(RSCRATCH));
      if (bits == 1)
      {
        // CA = sign & bit 0, i.e. (x & 0x80000001) == 0x80000001.
        SHR(32, R(RSCRATCH), Imm8(31));
        AND(32, R(RSCRATCH), Ra);
        SAR(32, Ra, Imm8(1));
        MOV(8, PPCSTATE(xer_ca), R(RSCRATCH));
      }
      else
      {
        SAR(32, Ra, Imm8(bits));
        SHL(32, R(RSCRATCH), Imm8(static_cast<u8>(32 - bits)));
        TEST(32, R(RSCRATCH), Ra);
        FinalizeCarry(CC_NZ);
      }
    }
  }

  if (inst.Rc)
    ComputeRC(a);
}

void Jit64::cntlzwx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;
  bool needs_test = false;

  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Write);
    RCOpArg Rs = gpr.UseNoImm(inst.RS, RCMode::Read);
    RegCache::Realize(Ra, Rs);

    if (!cpu_info.bLZCNT)
    {
      // BSR gives the index of the top set bit; 31 - index is 31 ^ index.
      // For zero, 63 ^ 31 = 32.
      BSR(32, Ra, Rs);
      const FixupBranch nonzero = J_CC(CC_NZ);
      MOV(32, Ra, Imm32(63));
      SetJumpTarget(nonzero);
      XOR(32, Ra, Imm8(0x1f));
    }
    else
    {
      LZCNT(32, Ra, Rs);
      needs_test = true;
    }
  }

  // The count is at most 32, so never negative.
  if (inst.Rc)
    ComputeRC(a, needs_test, false);
}

// ---------------------------------------------------------------------------
// Traps

void Jit64::twX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITIntegerOff);
  const u32 a = inst.RA;

  if (inst.OPCD == 3)  // twi
  {
    RCOpArg Ra = gpr.UseNoImm(a, RCMode::Read);
    RegCache::Realize(Ra);
    CMP(32, Ra, Imm32(static_cast<u32>(static_cast<s32>(static_cast<s16>(inst.SIMM_16)))));
  }
  else  // tw
  {
    RCX64Reg Ra = gpr.Bind(a, RCMode::Read);
    RCOpArg Rb = gpr.Use(inst.RB, RCMode::Read);
    RegCache::Realize(Ra, Rb);
    CMP(32, Ra, Rb);
  }

  // TO bits from the least significant: logically greater, logically less,
  // equal, greater, less.
  constexpr std::array<CCFlags, 5> TRAP_CONDITIONS{CC_A, CC_B, CC_E, CC_G, CC_L};
  Common::SmallVector<FixupBranch, TRAP_CONDITIONS.size()> traps;
  for (u32 bit = 0; bit < TRAP_CONDITIONS.size(); bit++)
  {
    if ((inst.TO >> bit) & 1)
      traps.push_back(J_CC(TRAP_CONDITIONS[bit], Jump::Near));
  }

  if (!traps.empty())
  {
    SwitchToFarCode();
    RCForkGuard gpr_guard = gpr.Fork();
    RCForkGuard fpr_guard = fpr.Fork();

    for (const FixupBranch& trap : traps)
      SetJumpTarget(trap);
    LOCK();
    OR(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_PROGRAM));
    MOV(32, PPCSTATE_SRR1, Imm32(static_cast<u32>(ProgramExceptionCause::Trap)));

    gpr.Flush();
    fpr.Flush();
    // The program exception reports the trap instruction itself.
    MOV(32, PPCSTATE(pc), Imm32(js.compilerPC));
    WriteExceptionExit();
    SwitchToNearCode();
  }

  if (!analyzer.HasOption(PPCAnalyst::PPCAnalyzer::OPTION_CONDITIONAL_CONTINUE))
  {
    gpr.Flush();
    fpr.Flush();
    WriteExit(js.compilerPC + 4);
  }
}
