// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The product register and the multiplier.
//
// A product is the signed 16 x 16 bit product of its operands, doubled unless
// $sr has SR_MUL_MODIFY set. The multiply-and-move forms also move the product
// as it was before into an accumulator, in one of three ways selected by bits
// 9-10: plus the accumulator (AC), as it is (MV), or rounded to the middle
// word (MVZ).

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
enum class Move
{
  None,
  Rounded,      // MVZ
  WithAcc,      // AC
  AsIs,         // MV
};

Move MoveOf(UDSPInstruction opc)
{
  return static_cast<Move>((opc >> 9) & 0x3);
}
}  // namespace

void DSPEmitter::EmitMultiply(X64Reg lhs, X64Reg rhs)
{
  IMUL(64, lhs, R(rhs));
  TEST(16, m_regs.Word(DSP_REG_SR), Imm16(SR_MUL_MODIFY));
  FixupBranch single = J_CC(CC_NZ);
  ADD(64, R(lhs), R(lhs));
  SetJumpTarget(single);
}

// With SR_MUL_UNSIGNED set, the operands that are low halves of AX registers
// count as unsigned (both, one, or neither, by which halves the opcode names).
void DSPEmitter::EmitMultiplyAx(X64Reg lhs, X64Reg rhs, int lhs_high, int rhs_high)
{
  if (lhs_high == 0 || rhs_high == 0)
  {
    TEST(16, m_regs.Word(DSP_REG_SR), Imm16(SR_MUL_UNSIGNED));
    FixupBranch is_signed = J_CC(CC_Z);
    if (lhs_high == 0)
      MOVZX(32, 16, lhs, R(lhs));
    if (rhs_high == 0)
      MOVZX(32, 16, rhs, R(rhs));
    SetJumpTarget(is_signed);
  }
  EmitMultiply(lhs, rhs);
}

void DSPEmitter::LoadMulxOperands(UDSPInstruction opc, X64Reg lhs, X64Reg rhs)
{
  const int lhs_high = (opc >> 12) & 0x1;
  const int rhs_high = (opc >> 11) & 0x1;
  m_regs.LoadWord(DSP_REG_AXL0 + 2 * lhs_high, lhs, RegisterExtension::Sign);
  m_regs.LoadWord(DSP_REG_AXL1 + 2 * rhs_high, rhs, RegisterExtension::Sign);
}

// CLRP: the product register's cleared state.
void DSPEmitter::OpClrp(UDSPInstruction)
{
  MOV(64, R(RAX), Imm64(0x001000fffff00000ULL));
  MOV(64, m_regs.Product(), R(RAX));
}

// TSTPROD
void DSPEmitter::OpTstprod(UDSPInstruction)
{
  if (!WantFlags())
    return;
  LoadProduct(RAX);
  SetFlags40(RAX);
}

// MOVP $acD
void DSPEmitter::OpMovp(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  LoadProduct(RAX);
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// MOVNP $acD: the product negated
void DSPEmitter::OpMovnp(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  LoadProduct(RAX);
  NEG(64, R(RAX));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// MOVPZ $acD: the product rounded to the middle word
void DSPEmitter::OpMovpz(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x01;
  LoadProduct(RAX);
  RoundToMid(RAX);
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// ADDPAXZ $acD, $axS: rounded product plus $axS with its low word cleared. The
// flags are those of a plain move (no carry or overflow).
void DSPEmitter::OpAddpaxz(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  m_regs.LoadAx((opc >> 9) & 0x1, RDX);
  AND(64, R(RDX), Imm32(0xffff0000u));
  LoadProduct(RAX);
  RoundToMid(RAX);
  ADD(64, R(RAX), R(RDX));
  m_regs.StoreAcc(acc, RAX);
  if (WantFlags())
    SetFlags40(RAX);
}

// MULAXH: $ax0.h squared
void DSPEmitter::OpMulaxh(UDSPInstruction)
{
  m_regs.LoadWord(DSP_REG_AXH0, RAX, RegisterExtension::Sign);
  MOV(64, R(RCX), R(RAX));
  EmitMultiply(RAX, RCX);
  StoreProduct(RAX);
}

// MUL $axS.l, $axS.h and MULMVZ, MULAC, MULMV with $acR
void DSPEmitter::OpMul(UDSPInstruction opc)
{
  const int ax = (opc >> 11) & 0x1;
  const int acc = (opc >> 8) & 0x1;
  const Move move = MoveOf(opc);

  switch (move)
  {
  case Move::None:
    break;
  case Move::Rounded:
    LoadProduct(RSI);
    RoundToMid(RSI);
    m_regs.StoreAcc(acc, RSI);
    break;
  case Move::WithAcc:
    m_regs.LoadAcc(acc, RSI);
    LoadProduct(RDI);
    ADD(64, R(RSI), R(RDI));
    break;
  case Move::AsIs:
    LoadProduct(RSI);
    break;
  }

  m_regs.LoadWord(DSP_REG_AXH0 + ax, RAX, RegisterExtension::Sign);
  m_regs.LoadWord(DSP_REG_AXL0 + ax, RCX, RegisterExtension::Sign);
  EmitMultiply(RAX, RCX);
  StoreProduct(RAX);

  switch (move)
  {
  case Move::None:
    break;
  case Move::Rounded:
    // These flags come from the sign of the new product (all ones or all
    // zeros), not from the value moved: kept as the JIT has always done it.
    if (WantFlags())
    {
      SAR(64, R(RAX), Imm8(63));
      SetFlags40(RAX);
    }
    break;
  default:
    m_regs.StoreAcc(acc, RSI);
    if (WantFlags())
      SetFlags40(RSI);
    break;
  }
}

// MULX $ax0.S, $ax1.T and MULXMVZ, MULXAC, MULXMV with $acR
void DSPEmitter::OpMulx(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  const Move move = MoveOf(opc);

  switch (move)
  {
  case Move::None:
    break;
  case Move::Rounded:
    LoadProduct(RSI);
    RoundToMid(RSI);
    break;
  case Move::WithAcc:
    m_regs.LoadAcc(acc, RSI);
    LoadProduct(RDI);
    ADD(64, R(RSI), R(RDI));
    break;
  case Move::AsIs:
    LoadProduct(RSI);
    break;
  }

  LoadMulxOperands(opc, RCX, RAX);
  EmitMultiplyAx(RCX, RAX, (opc >> 12) & 0x1, (opc >> 11) & 0x1);
  StoreProduct(RCX);

  if (move != Move::None)
  {
    m_regs.StoreAcc(acc, RSI);
    if (WantFlags())
      SetFlags40(RSI);
  }
}

// MULC $acS.m, $axT.h and MULCMVZ, MULCAC, MULCMV with $acR
void DSPEmitter::OpMulc(UDSPInstruction opc)
{
  const int acc = (opc >> 8) & 0x1;
  const int ax = (opc >> 11) & 0x1;
  const int mid = (opc >> 12) & 0x1;
  const Move move = MoveOf(opc);

  switch (move)
  {
  case Move::None:
    break;
  case Move::Rounded:
    LoadProduct(RSI);
    RoundToMid(RSI);
    break;
  case Move::WithAcc:
    m_regs.LoadAcc(acc, RSI);
    LoadProduct(RDI);
    ADD(64, R(RSI), R(RDI));
    break;
  case Move::AsIs:
    LoadProduct(RSI);
    break;
  }

  m_regs.LoadWord(DSP_REG_ACM0 + mid, RCX, RegisterExtension::Sign);
  m_regs.LoadWord(DSP_REG_AXH0 + ax, RAX, RegisterExtension::Sign);
  EmitMultiply(RAX, RCX);
  StoreProduct(RAX);

  if (move != Move::None)
  {
    m_regs.StoreAcc(acc, RSI);
    if (WantFlags())
      SetFlags40(RSI);
  }
}

// MADDX, MSUBX $ax0.S, $ax1.T and MADDC, MSUBC $acS.m, $axT.h: the product
// register plus (bit 10 clear) or minus the new product.
void DSPEmitter::OpMaddxc(UDSPInstruction opc)
{
  const int t = (opc >> 8) & 0x1;
  const int s = (opc >> 9) & 0x1;
  if ((opc & 0x0800) != 0)
  {
    m_regs.LoadWord(DSP_REG_ACM0 + s, RCX, RegisterExtension::Sign);
    m_regs.LoadWord(DSP_REG_AXH0 + t, RAX, RegisterExtension::Sign);
  }
  else
  {
    m_regs.LoadWord(DSP_REG_AXL0 + 2 * s, RCX, RegisterExtension::Sign);
    m_regs.LoadWord(DSP_REG_AXL1 + 2 * t, RAX, RegisterExtension::Sign);
  }
  EmitMultiply(RAX, RCX);
  LoadProduct(RDX);
  if ((opc & 0x0400) != 0)
    SUB(64, R(RDX), R(RAX));
  else
    ADD(64, R(RDX), R(RAX));
  StoreProduct(RDX);
}

// MADD, MSUB $axS.l, $axS.h (bit 10 set: subtract)
void DSPEmitter::OpMadd(UDSPInstruction opc)
{
  const int ax = (opc >> 8) & 0x1;
  m_regs.LoadWord(DSP_REG_AXL0 + ax, RCX, RegisterExtension::Sign);
  m_regs.LoadWord(DSP_REG_AXH0 + ax, RAX, RegisterExtension::Sign);
  EmitMultiply(RAX, RCX);
  LoadProduct(RDX);
  if ((opc & 0x0400) != 0)
    SUB(64, R(RDX), R(RAX));
  else
    ADD(64, R(RDX), R(RAX));
  StoreProduct(RDX);
}

}  // namespace DSP::JIT::x64
