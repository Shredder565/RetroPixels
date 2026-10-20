// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Accumulator arithmetic, logic, comparisons and shifts.
//
// The accumulators are 40-bit values held sign extended in 64 bits. A result
// is stored sign extended from bit 39, but several instructions take their
// flags from the full 64-bit value before that (so a result that overflowed
// 40 bits sets the flags of the unwrapped value); this is kept as it is.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
u64 Bits(s64 value)
{
  return static_cast<u64>(value);
}
}  // namespace

void DSPEmitter::EmitSext40(X64Reg value)
{
  SHL(64, R(value), Imm8(24));
  SAR(64, R(value), Imm8(24));
}

void DSPEmitter::EmitAccAddSub(int acc, bool subtract)
{
  m_regs.LoadAcc(acc, RAX);
  MOV(64, R(RCX), R(RAX));
  if (subtract)
    SUB(64, R(RCX), R(RDX));
  else
    ADD(64, R(RCX), R(RDX));
  EmitSext40(RCX);
  m_regs.StoreAcc(acc, RCX);
  if (WantFlags())
    SetFlagsAddSub(RAX, RDX, RCX, subtract);
}

void DSPEmitter::EmitCompare()
{
  MOV(64, R(RCX), R(RAX));
  SUB(64, R(RCX), R(RDX));
  EmitSext40(RCX);
  SetFlagsAddSub(RAX, RDX, RCX, true);
}

// ---------------------------------------------------------------------------
// Clearing and testing

// CLR $acR
void DSPEmitter::OpClr(UDSPInstruction opc)
{
  const int acc = (opc >> 11) & 0x1;
  XOR(32, R(EAX), R(EAX));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// CLRL $acR.l: round the accumulator to its middle word.
void DSPEmitter::OpClrl(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  RoundToMid(RAX);
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// ANDF / ANDCF $acR.m, #imm: set logic-zero when no bit of imm is set (ANDF)
// or when all of them are (ANDCF).
void DSPEmitter::OpTestBits(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  const int acc = (opc >> 8) & 0x1;
  const bool all_bits = (opc & 0x00e0) == 0x00c0;
  const u16 imm = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));

  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + acc));
  AND(32, R(EAX), Imm32(imm));
  if (all_bits)
    CMP(32, R(EAX), Imm32(imm));
  else
    TEST(32, R(EAX), R(EAX));
  SETcc(CC_E, R(EDX));
  MOVZX(32, 8, EDX, R(EDX));
  SHL(32, R(EDX), Imm8(6));  // SR_LOGIC_ZERO
  MOVZX(32, 16, ECX, m_regs.Word(DSP_REG_SR));
  AND(32, R(ECX), Imm32(static_cast<u16>(~(SR_LOGIC_ZERO | SR_100))));
  OR(32, R(ECX), R(EDX));
  MOV(16, m_regs.Word(DSP_REG_SR), R(ECX));
}

// TST $acR
void DSPEmitter::OpTst(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  m_regs.LoadAcc((opc >> 11) & 0x1, RAX);
  SetFlags40(RAX);
}

// TSTAXH $axR.h
void DSPEmitter::OpTstaxh(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  m_regs.LoadWord(DSP_REG_AXH0 + ((opc >> 8) & 0x1), RAX, RegisterExtension::Zero);
  SetFlags16(RAX, INVALID_REG);
}

// CMP: $ac0 - $ac1
void DSPEmitter::OpCmp(UDSPInstruction)
{
  if (!WantFlags())
    return;
  m_regs.LoadAcc(0, RAX);
  m_regs.LoadAcc(1, RDX);
  EmitCompare();
}

// CMPAXH $acS, $axR.h
void DSPEmitter::OpCmpaxh(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  m_regs.LoadAcc((opc >> 11) & 0x1, RAX);
  m_regs.LoadWord(DSP_REG_AXH0 + ((opc >> 12) & 0x1), RDX, RegisterExtension::Sign);
  SHL(64, R(RDX), Imm8(16));
  EmitCompare();
}

// CMPI $acD, #imm16 (compared with the middle word)
void DSPEmitter::OpCmpi(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  const s16 imm = static_cast<s16>(m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1)));
  m_regs.LoadAcc((opc >> 8) & 0x1, RAX);
  MOV(64, R(RDX), Imm64(Bits(static_cast<s64>(imm) * 0x10000)));
  EmitCompare();
}

// CMPIS $acD, #imm8 (compared with the middle word)
void DSPEmitter::OpCmpis(UDSPInstruction opc)
{
  if (!WantFlags())
    return;
  const s8 imm = static_cast<s8>(opc & 0xff);
  m_regs.LoadAcc((opc >> 8) & 0x1, RAX);
  MOV(64, R(RDX), Imm64(Bits(static_cast<s64>(imm) * 0x10000)));
  EmitCompare();
}

// ---------------------------------------------------------------------------
// Logic on the middle word. The flags come from the 16-bit result, except
// over-s32, which comes from the whole accumulator.

namespace
{
enum class Logic
{
  Xor,
  And,
  Or,
};

void EmitLogic(XEmitter& code, Logic op, X64Reg dst, const OpArg& src)
{
  switch (op)
  {
  case Logic::Xor:
    code.XOR(32, R(dst), src);
    break;
  case Logic::And:
    code.AND(32, R(dst), src);
    break;
  case Logic::Or:
    code.OR(32, R(dst), src);
    break;
  }
}
}  // namespace

// XORR, ANDR, ORR $acD.m, $axS.h (bits 10-11 = 0, 1, 2)
void DSPEmitter::OpLogicAx(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  const int ax = (opc >> 9) & 0x1;
  const Logic op = static_cast<Logic>((opc >> 10) & 0x3);
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + acc));
  MOVZX(32, 16, EDX, m_regs.Word(DSP_REG_AXH0 + ax));
  EmitLogic(*this, op, EAX, R(EDX));
  m_regs.StoreWord(DSP_REG_ACM0 + acc, RAX);
  if (WantFlags())
  {
    m_regs.LoadAcc(acc, RCX);
    SetFlags16(RAX, RCX);
  }
}

// XORC, ANDC, ORC $acD.m, $ac(1-D).m
void DSPEmitter::OpLogicAcc(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  Logic op = Logic::Xor;
  if ((opc & 0x0c00) != 0)
    op = (opc & 0x0200) != 0 ? Logic::Or : Logic::And;
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + acc));
  MOVZX(32, 16, EDX, m_regs.Word(DSP_REG_ACM0 + (1 - acc)));
  EmitLogic(*this, op, EAX, R(EDX));
  m_regs.StoreWord(DSP_REG_ACM0 + acc, RAX);
  if (WantFlags())
  {
    m_regs.LoadAcc(acc, RCX);
    SetFlags16(RAX, RCX);
  }
}

// NOT $acD.m
void DSPEmitter::OpNotc(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + acc));
  NOT(32, R(EAX));
  m_regs.StoreWord(DSP_REG_ACM0 + acc, RAX);
  if (WantFlags())
  {
    m_regs.LoadAcc(acc, RCX);
    SetFlags16(RAX, RCX);
  }
}

// XORI, ANDI, ORI $acD.m, #imm16 (bits 5-6 = 1, 2, 3)
void DSPEmitter::OpLogicImm(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  const Logic op = static_cast<Logic>(((opc >> 5) & 0x3) - 1);
  const u16 imm = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + acc));
  EmitLogic(*this, op, EAX, Imm32(imm));
  m_regs.StoreWord(DSP_REG_ACM0 + acc, RAX);
  if (WantFlags())
  {
    m_regs.LoadAcc(acc, RCX);
    SetFlags16(RAX, RCX);
  }
}

// ---------------------------------------------------------------------------
// Additions and subtractions (bit 12 set: subtract)

// ADDR, SUBR $acD, $(0x18+S): the register goes into the middle word.
void DSPEmitter::OpAddSubAxPart(UDSPInstruction opc)
{
  const int src = DSP_REG_AXL0 + ((opc >> 9) & 0x3);
  m_regs.LoadWord(src, RDX, RegisterExtension::Sign);
  SHL(64, R(RDX), Imm8(16));
  EmitAccAddSub((opc >> 8) & 0x1, (opc & 0x1000) != 0);
}

// ADDAX, SUBAX $acD, $axS
void DSPEmitter::OpAddSubAx(UDSPInstruction opc)
{
  m_regs.LoadAx((opc >> 9) & 0x1, RDX);
  EmitAccAddSub((opc >> 8) & 0x1, (opc & 0x1000) != 0);
}

// ADD, SUB $acD, $ac(1-D)
void DSPEmitter::OpAddSubAcc(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(1 - acc, RDX);
  EmitAccAddSub(acc, (opc & 0x1000) != 0);
}

// ADDP, SUBP $acD: the product
void DSPEmitter::OpAddSubProd(UDSPInstruction opc)
{
  LoadProduct(RDX);
  EmitAccAddSub((opc >> 8) & 0x1, (opc & 0x1000) != 0);
}

// ADDAXL $acD, $axS.l (unsigned)
void DSPEmitter::OpAddaxl(UDSPInstruction opc)
{
  m_regs.LoadWord(DSP_REG_AXL0 + ((opc >> 9) & 0x1), RDX, RegisterExtension::Zero);
  EmitAccAddSub((opc >> 8) & 0x1, false);
}

// ADDI $acD, #imm16 (added to the middle word)
void DSPEmitter::OpAddi(UDSPInstruction opc)
{
  const s16 imm = static_cast<s16>(m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1)));
  MOV(64, R(RDX), Imm64(Bits(static_cast<s64>(imm) * 0x10000)));
  EmitAccAddSub((opc >> 8) & 0x1, false);
}

// ADDIS $acD, #imm8 (added to the middle word)
void DSPEmitter::OpAddis(UDSPInstruction opc)
{
  const s8 imm = static_cast<s8>(opc & 0xff);
  MOV(64, R(RDX), Imm64(Bits(static_cast<s64>(imm) * 0x10000)));
  EmitAccAddSub((opc >> 8) & 0x1, false);
}

// INCM, INC, DECM, DEC $acD: by 0x10000 (bit 9 clear) or 1; bit 11 subtracts.
void DSPEmitter::OpIncDec(UDSPInstruction opc)
{
  const u32 amount = (opc & 0x0200) != 0 ? 1u : 0x10000u;
  MOV(32, R(EDX), Imm32(amount));
  EmitAccAddSub((opc >> 8) & 0x1, (opc & 0x0800) != 0);
}

// NEG $acD
void DSPEmitter::OpNeg(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  MOV(64, R(RCX), R(RAX));
  NEG(64, R(RCX));
  EmitSext40(RCX);
  m_regs.StoreAcc(acc, RCX);
  if (WantFlags())
  {
    // 0 - acc
    XOR(32, R(EDX), R(EDX));
    SetFlagsAddSub(RDX, RAX, RCX, true);
  }
}

// ABS $acD
void DSPEmitter::OpAbs(UDSPInstruction opc)
{
  const int acc = (opc >> 11) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  TEST(64, R(RAX), R(RAX));
  FixupBranch non_negative = J_CC(CC_NS, Jump::Near);
  NEG(64, R(RAX));
  m_regs.StoreAcc(acc, RAX);
  SetJumpTarget(non_negative);
  if (WantFlags())
    SetFlags40(RAX);
}

// ---------------------------------------------------------------------------
// Moves into an accumulator

// MOVR $acD, $(0x18+S): into the middle word, sign extended.
void DSPEmitter::OpMovr(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadWord(DSP_REG_AXL0 + ((opc >> 9) & 0x3), RAX, RegisterExtension::Sign);
  SHL(64, R(RAX), Imm8(16));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// MOVAX $acD, $axS
void DSPEmitter::OpMovax(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAx((opc >> 9) & 0x1, RAX);
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// MOV $acD, $ac(1-D)
void DSPEmitter::OpMov(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(1 - acc, RAX);
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// ---------------------------------------------------------------------------
// Shifts

// LSL16 $acR
void DSPEmitter::OpLsl16(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  SHL(64, R(RAX), Imm8(16));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// LSR16 $acR: the 40 bits shifted right as an unsigned value.
void DSPEmitter::OpLsr16(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  SHR(64, R(RAX), Imm8(16));
  AND(32, R(EAX), Imm32(0x00ffffff));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// ASR16 $acR
void DSPEmitter::OpAsr16(UDSPInstruction opc)
{
  const int acc = (opc >> 11) & 0x1;
  m_regs.LoadAcc(acc, RAX);
  SAR(64, R(RAX), Imm8(16));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// LSL, LSR, ASL, ASR $acR, #count (bits 6-7 = 0, 1, 2, 3). The right shifts
// encode their count as 64 - count in six bits.
void DSPEmitter::OpShiftImm(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  const int kind = (opc >> 6) & 0x3;
  const u8 field = opc & 0x3f;
  m_regs.LoadAcc(acc, RAX);
  switch (kind)
  {
  case 0:  // LSL
  case 2:  // ASL
    if (field != 0)
      SHL(64, R(RAX), Imm8(field));
    break;
  case 1:  // LSR
    if (field != 0)
    {
      // Moving bit 39 up to bit 63 makes the shift unsigned. The x86 shift
      // count is taken modulo 64, which counts of 40 and more run into.
      const u8 count = static_cast<u8>((64 - field + 24) & 0x3f);
      SHL(64, R(RAX), Imm8(24));
      if (count != 0)
        SHR(64, R(RAX), Imm8(count));
    }
    break;
  default:  // ASR
    if (field != 0)
      SAR(64, R(RAX), Imm8(static_cast<u8>(64 - field)));
    break;
  }
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// The shifts by a register: the count is in the low seven bits of EAX. A
// count of 0 (in its low six bits) leaves the value as it is; otherwise bit 6
// picks the direction, and with it set the distance is 64 minus the low six
// bits. The logical forms work on the 40 bits as an unsigned value (and their
// flags see it that way too). A zero accumulator is not written back.
void DSPEmitter::EmitVariableShift(int acc, bool arithmetic, bool bit6_shifts_left)
{
  m_regs.LoadAcc(acc, RDX);
  if (!arithmetic)
  {
    SHL(64, R(RDX), Imm8(24));
    SHR(64, R(RDX), Imm8(24));
  }
  TEST(64, R(RDX), R(RDX));
  FixupBranch zero = J_CC(CC_Z, Jump::Near);

  MOV(32, R(ECX), R(EAX));
  AND(32, R(ECX), Imm32(0x3f));
  FixupBranch unshifted = J_CC(CC_Z, Jump::Near);

  const auto shift_right = [this, arithmetic] {
    if (arithmetic)
      SAR(64, R(RDX), R(ECX));
    else
      SHR(64, R(RDX), R(ECX));
  };

  TEST(32, R(EAX), Imm32(0x40));
  FixupBranch bit6_clear = J_CC(CC_Z, Jump::Near);
  NEG(32, R(ECX));
  ADD(32, R(ECX), Imm32(64));
  if (bit6_shifts_left)
    SHL(64, R(RDX), R(ECX));
  else
    shift_right();
  FixupBranch shifted = J(Jump::Near);

  SetJumpTarget(bit6_clear);
  if (bit6_shifts_left)
    shift_right();
  else
    SHL(64, R(RDX), R(ECX));

  SetJumpTarget(shifted);
  SetJumpTarget(unshifted);
  m_regs.StoreAcc(acc, RDX);
  SetJumpTarget(zero);
  if (WantFlags())
    SetFlags40(RDX);
}

// LSRN, ASRN: $ac0 by $ac1.m (bit 0 set: arithmetic)
void DSPEmitter::OpShiftByAcm1(UDSPInstruction opc)
{
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM1));
  EmitVariableShift(0, (opc & 0x1) != 0, true);
}

// LSRNRX, ASRNRX $acD, $axS.h (bit 11 set: arithmetic)
void DSPEmitter::OpShiftByAx(UDSPInstruction opc)
{
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_AXH0 + ((opc >> 9) & 0x1)));
  EmitVariableShift((opc >> 8) & 0x1, (opc & 0x0800) != 0, false);
}

// LSRNR, ASRNR $acD, $ac(1-D).m (bit 9 set: arithmetic)
void DSPEmitter::OpShiftByAcm(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_ACM0 + (1 - acc)));
  EmitVariableShift(acc, (opc & 0x0200) != 0, false);
}

}  // namespace DSP::JIT::x64
