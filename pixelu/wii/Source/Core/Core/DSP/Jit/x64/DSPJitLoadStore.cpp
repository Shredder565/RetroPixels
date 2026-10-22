// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Loads and stores of the main instruction set.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
// The post-modify field of the register-indirect forms:
// none, decrement, increment, add the matching $ix.
constexpr int StepIndex(UDSPInstruction opc, int shift)
{
  return (opc >> shift) & 0x3;
}
}  // namespace

// LR $D, @addr
void DSPEmitter::OpLr(UDSPInstruction opc)
{
  const int dst = opc & 0x1f;
  const u16 address = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  EmitDataReadImm(address);
  WriteOperand(dst, RAX);
  ExtendMid(dst);
}

// SR @addr, $S
void DSPEmitter::OpSr(UDSPInstruction opc)
{
  const int src = opc & 0x1f;
  const u16 address = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  ReadOperand(src, RSI, RegisterExtension::None);
  EmitDataWriteImm(address, RSI);
}

// SI @(sign-extended imm8), #imm16
void DSPEmitter::OpSi(UDSPInstruction opc)
{
  const u16 address = static_cast<u16>(static_cast<s8>(opc & 0xff));
  const u16 value = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  MOV(32, R(RSI), Imm32(value));
  EmitDataWriteImm(address, RSI);
}

// LRR, LRRD, LRRI, LRRN $D, @$arS
void DSPEmitter::OpLrr(UDSPInstruction opc)
{
  const int ar = (opc >> 5) & 0x3;
  const int dst = opc & 0x1f;
  m_regs.LoadWord(DSP_REG_AR0 + ar, RSI, RegisterExtension::Zero);
  EmitDataRead(RSI);
  WriteOperand(dst, RAX);
  ExtendMid(dst);
  EmitArStep(ar, static_cast<ArStep>(StepIndex(opc, 7)));
}

// SRR, SRRD, SRRI, SRRN @$arD, $S
void DSPEmitter::OpSrr(UDSPInstruction opc)
{
  const int ar = (opc >> 5) & 0x3;
  const int src = opc & 0x1f;
  ReadOperand(src, RSI, RegisterExtension::None);
  m_regs.LoadWord(DSP_REG_AR0 + ar, RAX, RegisterExtension::Zero);
  EmitDataWrite(RAX, RSI);
  EmitArStep(ar, static_cast<ArStep>(StepIndex(opc, 7)));
}

// ILRR, ILRRD, ILRRI, ILRRN $acD.m, @$arS (instruction memory)
void DSPEmitter::OpIlrr(UDSPInstruction opc)
{
  const int ar = opc & 0x3;
  const int mid = DSP_REG_ACM0 + ((opc >> 8) & 0x1);
  m_regs.LoadWord(DSP_REG_AR0 + ar, RSI, RegisterExtension::Zero);
  EmitCodeRead(RSI);
  m_regs.StoreWord(mid, RAX);
  ExtendMid(mid);
  EmitArStep(ar, static_cast<ArStep>(StepIndex(opc, 2)));
}

// LRS $(0x18+D), @(($cr << 8) | imm8)
void DSPEmitter::OpLrs(UDSPInstruction opc)
{
  const int dst = DSP_REG_AXL0 + ((opc >> 8) & 0x7);
  MOVZX(32, 16, ESI, m_regs.Word(DSP_REG_CR));
  SHL(32, R(ESI), Imm8(8));
  OR(32, R(ESI), Imm32(opc & 0xffu));
  EmitDataRead(RSI);
  WriteOperand(dst, RAX);
  ExtendMid(dst);
}

// SRS @(($cr << 8) | imm8), $(0x1c+S)
void DSPEmitter::OpSrs(UDSPInstruction opc)
{
  const int src = DSP_REG_ACL0 + ((opc >> 8) & 0x3);
  ReadOperand(src, RSI, RegisterExtension::Zero);
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_CR));
  SHL(32, R(EAX), Imm8(8));
  OR(32, R(EAX), Imm32(opc & 0xffu));
  EmitDataWrite(RAX, RSI);
}

// SRSH @(($cr << 8) | imm8), $acS.h
void DSPEmitter::OpSrsh(UDSPInstruction opc)
{
  const int src = DSP_REG_ACH0 + ((opc >> 8) & 0x1);
  m_regs.LoadWord(src, RSI, RegisterExtension::Zero);
  MOVZX(32, 16, EAX, m_regs.Word(DSP_REG_CR));
  SHL(32, R(EAX), Imm8(8));
  OR(32, R(EAX), Imm32(opc & 0xffu));
  EmitDataWrite(RAX, RSI);
}

}  // namespace DSP::JIT::x64
