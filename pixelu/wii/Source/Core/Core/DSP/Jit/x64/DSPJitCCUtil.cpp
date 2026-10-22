// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Condition codes and the result flags in $sr.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
// The flag bits of $sr an arithmetic result replaces; bit 8 always reads as 0
// and is dropped with them.
constexpr u32 RESULT_FLAGS_AND_BIT8 = SR_CMP_MASK | SR_100;
}  // namespace

bool DSPEmitter::WantFlags() const
{
  const auto& analyzer = m_dsp_core.DSPState().GetAnalyzer();
  return !analyzer.IsStartOfInstruction(m_pc) || analyzer.IsUpdateSR(m_pc);
}

// Conditions (S sign, O overflow, Z zero, C carry):
//   0 GE  S == O          1 L   S != O
//   2 G   S == O and !Z   3 LE  S != O or Z
//   4 NZ  !Z              5 Z   Z
//   6 NC  !C              7 C   C
//   8     !over-s32       9     over-s32
//   a     (over-s32 or top-two-bits) and !Z
//   b     !(over-s32 or top-two-bits) or Z
//   c LNZ !logic-zero     d LZ  logic-zero
//   e O   O               f     always
FixupBranch DSPEmitter::EmitSkipUnless(u8 cond)
{
  ASSERT(cond < 0xf);
  MOVZX(32, 16, HELPER_A, m_regs.Word(DSP_REG_SR));

  u32 mask;
  bool holds_when_set;  // the condition holds when the tested bit is set
  switch (cond)
  {
  case 0x0:
  case 0x1:
    // bit 1 of sr ^ (sr >> 2) is O ^ S
    MOV(32, R(HELPER_B), R(HELPER_A));
    SHR(32, R(HELPER_B), Imm8(2));
    XOR(32, R(HELPER_A), R(HELPER_B));
    mask = 0x2;
    holds_when_set = cond == 0x1;
    break;
  case 0x2:
  case 0x3:
    // bit 1 of (sr ^ (sr >> 2)) | (sr >> 1) is (O ^ S) | Z
    MOV(32, R(HELPER_B), R(HELPER_A));
    SHR(32, R(HELPER_B), Imm8(2));
    XOR(32, R(HELPER_B), R(HELPER_A));
    SHR(32, R(HELPER_A), Imm8(1));
    OR(32, R(HELPER_A), R(HELPER_B));
    mask = 0x2;
    holds_when_set = cond == 0x3;
    break;
  case 0x4:
  case 0x5:
    mask = SR_ARITH_ZERO;
    holds_when_set = cond == 0x5;
    break;
  case 0x6:
  case 0x7:
    mask = SR_CARRY;
    holds_when_set = cond == 0x7;
    break;
  case 0x8:
  case 0x9:
    mask = SR_OVER_S32;
    holds_when_set = cond == 0x9;
    break;
  case 0xa:
  case 0xb:
    // bit 0 of ((sr >> 4) | (sr >> 5)) & ~(sr >> 2) is (over-s32 | top2) & !Z
    MOV(32, R(HELPER_B), R(HELPER_A));
    SHR(32, R(HELPER_B), Imm8(2));
    NOT(32, R(HELPER_B));
    MOV(32, R(HELPER_C), R(HELPER_A));
    SHR(32, R(HELPER_C), Imm8(5));
    SHR(32, R(HELPER_A), Imm8(4));
    OR(32, R(HELPER_A), R(HELPER_C));
    AND(32, R(HELPER_A), R(HELPER_B));
    mask = 0x1;
    holds_when_set = cond == 0xa;
    break;
  case 0xc:
  case 0xd:
    mask = SR_LOGIC_ZERO;
    holds_when_set = cond == 0xd;
    break;
  default:  // 0xe
    mask = SR_OVERFLOW;
    holds_when_set = true;
    break;
  }

  TEST(32, R(HELPER_A), Imm32(mask));
  return J_CC(holds_when_set ? CC_Z : CC_NZ, Jump::Near);
}

void DSPEmitter::CollectFlags40(X64Reg value, X64Reg flags)
{
  ASSERT(value != HELPER_A && flags != HELPER_A);

  TEST(64, R(value), R(value));
  FixupBranch non_zero = J_CC(CC_NZ);
  OR(32, R(flags), Imm32(SR_ARITH_ZERO));
  SetJumpTarget(non_zero);

  TEST(64, R(value), R(value));
  FixupBranch non_negative = J_CC(CC_NS);
  OR(32, R(flags), Imm32(SR_SIGN));
  SetJumpTarget(non_negative);

  MOVSX(64, 32, HELPER_A, R(value));
  CMP(64, R(HELPER_A), R(value));
  FixupBranch fits_s32 = J_CC(CC_E);
  OR(32, R(flags), Imm32(SR_OVER_S32));
  SetJumpTarget(fits_s32);

  // Bits 31 and 30 are equal exactly when adding 0x40000000 leaves bit 31 clear.
  LEA(32, HELPER_A, MDisp(value, 0x40000000));
  TEST(32, R(HELPER_A), R(HELPER_A));
  FixupBranch differ = J_CC(CC_S);
  OR(32, R(flags), Imm32(SR_TOP2BITS));
  SetJumpTarget(differ);
}

void DSPEmitter::CommitFlags(X64Reg flags)
{
  AND(16, m_regs.Word(DSP_REG_SR), Imm16(static_cast<u16>(~RESULT_FLAGS_AND_BIT8)));
  OR(16, m_regs.Word(DSP_REG_SR), R(flags));
}

void DSPEmitter::SetFlags40(X64Reg value)
{
  ASSERT(value != HELPER_B);
  XOR(32, R(HELPER_B), R(HELPER_B));
  CollectFlags40(value, HELPER_B);
  CommitFlags(HELPER_B);
}

void DSPEmitter::SetFlagsAddSub(X64Reg a, X64Reg b, X64Reg result, bool subtract)
{
  ASSERT(a != HELPER_B && b != HELPER_B && result != HELPER_B);
  XOR(32, R(HELPER_B), R(HELPER_B));

  // Carry: the unsigned 64-bit result wrapped (for a subtraction: did not borrow).
  CMP(64, R(a), R(result));
  FixupBranch no_carry = J_CC(subtract ? CC_B : CC_BE);
  OR(32, R(HELPER_B), Imm32(SR_CARRY));
  SetJumpTarget(no_carry);

  // Overflow: both operands (b negated for a subtraction) differ in sign from the result.
  MOV(64, R(HELPER_C), R(a));
  XOR(64, R(HELPER_C), R(result));
  MOV(64, R(HELPER_D), R(b));
  if (subtract)
    NEG(64, R(HELPER_D));
  XOR(64, R(HELPER_D), R(result));
  TEST(64, R(HELPER_C), R(HELPER_D));
  FixupBranch no_overflow = J_CC(CC_NS);
  OR(32, R(HELPER_B), Imm32(SR_OVERFLOW | SR_OVERFLOW_STICKY));
  SetJumpTarget(no_overflow);

  CollectFlags40(result, HELPER_B);
  CommitFlags(HELPER_B);
}

void DSPEmitter::SetFlags16(X64Reg value, X64Reg full_acc)
{
  ASSERT(value != HELPER_B && value != HELPER_C);
  XOR(32, R(HELPER_B), R(HELPER_B));
  MOVZX(32, 16, HELPER_C, R(value));

  TEST(32, R(HELPER_C), R(HELPER_C));
  FixupBranch non_zero = J_CC(CC_NZ);
  OR(32, R(HELPER_B), Imm32(SR_ARITH_ZERO));
  SetJumpTarget(non_zero);

  TEST(32, R(HELPER_C), Imm32(0x8000));
  FixupBranch non_negative = J_CC(CC_Z);
  OR(32, R(HELPER_B), Imm32(SR_SIGN));
  SetJumpTarget(non_negative);

  // Bits 15 and 14 are equal exactly when adding 0x4000 leaves bit 15 clear.
  ADD(32, R(HELPER_C), Imm32(0x4000));
  TEST(32, R(HELPER_C), Imm32(0x8000));
  FixupBranch differ = J_CC(CC_NZ);
  OR(32, R(HELPER_B), Imm32(SR_TOP2BITS));
  SetJumpTarget(differ);

  if (full_acc != INVALID_REG)
  {
    ASSERT(full_acc != HELPER_A && full_acc != HELPER_B);
    MOVSX(64, 32, HELPER_A, R(full_acc));
    CMP(64, R(HELPER_A), R(full_acc));
    FixupBranch fits_s32 = J_CC(CC_E);
    OR(32, R(HELPER_B), Imm32(SR_OVER_S32));
    SetJumpTarget(fits_s32);
  }

  CommitFlags(HELPER_B);
}

}  // namespace DSP::JIT::x64
