// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Extension opcodes: the parallel loads, stores, moves and address steps in the
// low bits of many instructions.
//
// An extension sees the registers as they were before its main instruction, and
// its register results land after the main instruction. So values it loads wait
// in EXT_FIRST and EXT_SECOND (callee-saved, they survive calls in the main
// instruction) until CommitExtension. Its stores and address register updates
// happen right away: no extended main instruction touches memory or $arN.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
DSPEmitter::ArStep StepFor(bool by_index)
{
  return by_index ? DSPEmitter::ArStep::AddIndex : DSPEmitter::ArStep::Increment;
}
}  // namespace

void DSPEmitter::EmitExtLoadFirst(int dst_reg, int ar, bool keep_sr)
{
  m_regs.LoadWord(DSP_REG_AR0 + ar, RSI, RegisterExtension::Zero);
  EmitDataRead(RSI);
  MOV(32, R(EXT_FIRST), R(EAX));
  if (keep_sr)
  {
    // Whether 40-bit mode was on when the extension ran decides the sign
    // extension at commit time.
    MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_SR));
    SHL(32, R(EAX), Imm8(16));
    OR(32, R(EXT_FIRST), R(EAX));
  }
  m_ext_first_reg = dst_reg;
  m_ext_first_has_sr = keep_sr;
}

void DSPEmitter::EmitExtLoadSecond(int dst_reg, int ar)
{
  m_regs.LoadWord(DSP_REG_AR0 + ar, RSI, RegisterExtension::Zero);
  m_regs.LoadWord(DSP_REG_AR3, RAX, RegisterExtension::Zero);
  XOR(32, R(EAX), R(ESI));
  TEST(32, R(EAX), Imm32(0xfc00));
  FixupBranch same_page = J_CC(CC_Z, Jump::Near);
  m_regs.LoadWord(DSP_REG_AR3, RSI, RegisterExtension::Zero);
  SetJumpTarget(same_page);
  EmitDataRead(RSI);
  MOV(32, R(EXT_SECOND), R(EAX));
  m_ext_second_reg = dst_reg;
}

void DSPEmitter::CommitExtension()
{
  if (m_ext_first_reg != -1)
  {
    WriteOperand(m_ext_first_reg, EXT_FIRST);
    if (m_ext_first_has_sr && m_ext_second_reg == -1)
    {
      const int acc = m_ext_first_reg - DSP_REG_ACM0;
      TEST(32, R(EXT_FIRST), Imm32(static_cast<u32>(SR_40_MODE_BIT) << 16));
      FixupBranch not_40bit = J_CC(CC_Z, Jump::Near);
      MOVSX(32, 16, EAX, R(EXT_FIRST));
      SAR(32, R(EAX), Imm8(31));
      MOV(32, m_regs.AccHigh(acc), R(EAX));
      MOV(16, m_regs.Word(DSP_REG_ACL0 + acc), Imm16(0));
      SetJumpTarget(not_40bit);
    }
  }
  if (m_ext_second_reg != -1)
    WriteOperand(m_ext_second_reg, EXT_SECOND);

  m_ext_first_reg = -1;
  m_ext_second_reg = -1;
  m_ext_first_has_sr = false;
}

// (NOP), DR, IR, NR $arR: step an address register (bits 2-3 = 0, 1, 2, 3).
void DSPEmitter::ExtModifyAr(UDSPInstruction opc)
{
  EmitArStep(opc & 0x3, static_cast<ArStep>((opc >> 2) & 0x3));
}

// MV $(0x18+D), $(0x1c+S)
void DSPEmitter::ExtMove(UDSPInstruction opc)
{
  const int src = DSP_REG_ACL0 + (opc & 0x3);
  ReadOperand(src, RAX, RegisterExtension::Zero);
  MOV(32, R(EXT_FIRST), R(EAX));
  m_ext_first_reg = DSP_REG_AXL0 + ((opc >> 2) & 0x3);
  m_ext_first_has_sr = false;
}

// S, SN @$arD, $(0x1c+S); then $arD steps by one (S) or by $ixD (SN).
void DSPEmitter::ExtStore(UDSPInstruction opc)
{
  const int ar = opc & 0x3;
  const int src = DSP_REG_ACL0 + ((opc >> 3) & 0x3);
  ReadOperand(src, RSI, RegisterExtension::Zero);
  m_regs.LoadWord(DSP_REG_AR0 + ar, RAX, RegisterExtension::Zero);
  EmitDataWrite(RAX, RSI);
  EmitArStep(ar, StepFor((opc & 0x4) != 0));
}

// L, LN $(0x18+D), @$arS; then $arS steps by one (L) or by $ixS (LN).
void DSPEmitter::ExtLoad(UDSPInstruction opc)
{
  const int ar = opc & 0x3;
  const int dst = DSP_REG_AXL0 + ((opc >> 3) & 0x7);
  EmitExtLoadFirst(dst, ar, dst >= DSP_REG_ACM0);
  EmitArStep(ar, StepFor((opc & 0x4) != 0));
}

// LS (store through $ar3, load through $ar0) and SL (the other way round),
// with N (bit 2: $ar0 steps by $ix0) and M (bit 3: $ar3 steps by $ix3).
void DSPEmitter::ExtLoadStore(UDSPInstruction opc)
{
  const int src = DSP_REG_ACM0 + (opc & 0x1);
  const int dst = DSP_REG_AXL0 + ((opc >> 4) & 0x3);
  const bool store_first_through_ar0 = (opc & 0x2) != 0;
  const int store_ar = store_first_through_ar0 ? 0 : 3;
  const int load_ar = store_first_through_ar0 ? 3 : 0;

  ReadOperand(src, RSI, RegisterExtension::Zero);
  m_regs.LoadWord(DSP_REG_AR0 + store_ar, RAX, RegisterExtension::Zero);
  EmitDataWrite(RAX, RSI);
  EmitExtLoadFirst(dst, load_ar, false);

  EmitArStep(3, StepFor((opc & 0x8) != 0));
  EmitArStep(0, StepFor((opc & 0x4) != 0));
}

// LD $ax0.D, $ax1.R, @$arS: two loads, the second from @$ar3 unless both
// pointers are in the same page. N steps $arS by $ixS, M steps $ar3 by $ix3.
void DSPEmitter::ExtLoadDual(UDSPInstruction opc)
{
  const int ar = opc & 0x3;
  const int first = DSP_REG_AXL0 + (((opc >> 5) & 0x1) << 1);
  const int second = DSP_REG_AXL1 + (((opc >> 4) & 0x1) << 1);
  EmitExtLoadFirst(first, ar, false);
  EmitExtLoadSecond(second, ar);
  EmitArStep(ar, StepFor((opc & 0x4) != 0));
  EmitArStep(3, StepFor((opc & 0x8) != 0));
}

// LDAX $axR, @$arS: as LD, filling both halves of one AX register (high first).
void DSPEmitter::ExtLoadDualAx(UDSPInstruction opc)
{
  const int ar = (opc >> 5) & 0x1;
  const int half = (opc >> 4) & 0x1;
  EmitExtLoadFirst(DSP_REG_AXH0 + half, ar, false);
  EmitExtLoadSecond(DSP_REG_AXL0 + half, ar);
  EmitArStep(ar, StepFor((opc & 0x4) != 0));
  EmitArStep(3, StepFor((opc & 0x8) != 0));
}

}  // namespace DSP::JIT::x64
