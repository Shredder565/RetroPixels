// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Register moves, immediate loads, address register steps and the $sr
// control bits.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
// NOP, NX: nothing (NX exists for its extension).
void DSPEmitter::OpNop(UDSPInstruction)
{
}

// DAR, IAR, SUBARN $arD (bits 2-3 = 1, 2, 3).
void DSPEmitter::OpModifyAr(UDSPInstruction opc)
{
  const int ar = opc & 0x3;
  switch ((opc >> 2) & 0x3)
  {
  case 1:
    EmitArDecrement(ar);
    break;
  case 2:
    EmitArIncrement(ar);
    break;
  default:
    EmitArSubIndex(ar);
    break;
  }
}

// ADDARN $arD, $ixS
void DSPEmitter::OpAddarn(UDSPInstruction opc)
{
  EmitArAddIndex(opc & 0x3, (opc >> 2) & 0x3);
}

// MRR $D, $S
void DSPEmitter::OpMrr(UDSPInstruction opc)
{
  const int src = opc & 0x1f;
  const int dst = (opc >> 5) & 0x1f;
  ReadOperand(src, RDX, RegisterExtension::None);
  WriteOperand(dst, RDX);
  ExtendMid(dst);
}

// LRI $D, #imm16
void DSPEmitter::OpLri(UDSPInstruction opc)
{
  const int dst = opc & 0x1f;
  const u16 value = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  WriteOperandImm(dst, value);
  ExtendMidImm(dst, value);
}

// LRIS $(0x18+D), #imm8 (sign extended)
void DSPEmitter::OpLris(UDSPInstruction opc)
{
  const int dst = DSP_REG_AXL0 + ((opc >> 8) & 0x7);
  const u16 value = static_cast<u16>(static_cast<s8>(opc & 0xff));
  WriteOperandImm(dst, value);
  ExtendMidImm(dst, value);
}

// SBCLR / SBSET #bit: clear or set bit (6 + bit) of $sr.
void DSPEmitter::OpSrBit(UDSPInstruction opc)
{
  const u16 bit = static_cast<u16>(1u << ((opc & 0x7) + 6));
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_SR));
  if ((opc & 0x0100) != 0)
    OR(32, R(EAX), Imm32(bit));
  else
    AND(32, R(EAX), Imm32(static_cast<u16>(~bit)));
  m_regs.StoreWord(DSP_REG_SR, RAX);
}

// M2/M0, CLR15/SET15, SET16/SET40 (bits 8-11 = a..f): even ones clear an $sr
// mode bit, odd ones set it.
void DSPEmitter::OpModeBit(UDSPInstruction opc)
{
  u16 bit;
  switch ((opc >> 8) & 0xf)
  {
  case 0xa:
  case 0xb:
    bit = SR_MUL_MODIFY;
    break;
  case 0xc:
  case 0xd:
    bit = SR_MUL_UNSIGNED;
    break;
  default:
    bit = SR_40_MODE_BIT;
    break;
  }
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_SR));
  if ((opc & 0x0100) != 0)
    OR(32, R(EAX), Imm32(bit));
  else
    AND(32, R(EAX), Imm32(static_cast<u16>(~bit)));
  m_regs.StoreWord(DSP_REG_SR, RAX);
}

}  // namespace DSP::JIT::x64
