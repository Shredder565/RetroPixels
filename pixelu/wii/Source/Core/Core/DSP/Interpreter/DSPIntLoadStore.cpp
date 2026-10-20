// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Loads and stores between registers and data memory (and the IMEM reads of
// ILRR). Data memory accesses may reach the hardware registers at 0xffxx, so the
// order of reads, writes and register updates below is significant.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

namespace DSP::Interpreter
{
namespace
{
// LRS/SRS/SRSH address a 256-word page selected by $cr.
u16 PagedAddress(const SDSP& dsp, UDSPInstruction opc)
{
  return static_cast<u16>((dsp.r.cr << 8) | (opc & 0xff));
}

// Register fields of the LRR/SRR family.
constexpr int PointerField(UDSPInstruction opc)
{
  return (opc >> 5) & 3;
}

constexpr int RegisterField(UDSPInstruction opc)
{
  return opc & 0x1f;
}
}  // namespace

// ---------------------------------------------------------------------------
// Short (paged) and direct addresses
// ---------------------------------------------------------------------------

// SRSH @M, $acS.h
void Interpreter::srsh(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 address = PagedAddress(dsp, opc);
  dsp.WriteDMEM(address, OpReadRegister(DSP_REG_ACH0 + ((opc >> 8) & 1)));
}

// SRS @M, $(0x1c+S)
void Interpreter::srs(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 address = PagedAddress(dsp, opc);
  dsp.WriteDMEM(address, OpReadRegister(DSP_REG_ACL0 + ((opc >> 8) & 3)));
}

// LRS $(0x18+D), @M
void Interpreter::lrs(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const int reg = DSP_REG_AXL0 + ((opc >> 8) & 7);
  LoadRegister(reg, dsp.ReadDMEM(PagedAddress(dsp, opc)));
}

// LR $D, @M (M is the second word)
void Interpreter::lr(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 address = dsp.FetchInstruction();
  LoadRegister(RegisterField(opc), dsp.ReadDMEM(address));
}

// SR @M, $S
void Interpreter::sr(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 address = dsp.FetchInstruction();
  dsp.WriteDMEM(address, OpReadRegister(RegisterField(opc)));
}

// SI @M, #I: M is a sign-extended byte (so 0xff00-0xffff, the hardware registers,
// or 0x0000-0x007f)
void Interpreter::si(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 address = static_cast<u16>(static_cast<s8>(opc & 0xff));
  const u16 value = dsp.FetchInstruction();
  dsp.WriteDMEM(address, value);
}

// ---------------------------------------------------------------------------
// Through an address register
// ---------------------------------------------------------------------------

// LRR / LRRD / LRRI / LRRN $D, @$arS (then $arS unchanged / -1 / +1 / +$ixS).
// The pointer moves after the load, so a load into $arS or $wrS is seen by it.
void Interpreter::LoadThroughPointer(UDSPInstruction opc, PointerStep step)
{
  const int pointer = PointerField(opc);
  const u16 value = Regs().ReadDMEM(OpReadRegister(pointer));
  LoadRegister(RegisterField(opc), value);
  UpdatePointer(pointer, step);
}

// SRR / SRRD / SRRI / SRRN @$arD, $S
void Interpreter::StoreThroughPointer(UDSPInstruction opc, PointerStep step)
{
  SDSP& dsp = Regs();
  const int pointer = PointerField(opc);
  dsp.WriteDMEM(dsp.r.ar[pointer], OpReadRegister(RegisterField(opc)));
  UpdatePointer(pointer, step);
}

void Interpreter::UpdatePointer(int ar, PointerStep step)
{
  u16& address = Regs().r.ar[ar];
  switch (step)
  {
  case PointerStep::Keep:
    break;
  case PointerStep::Decrement:
    address = AddressMinusOne(ar);
    break;
  case PointerStep::Increment:
    address = AddressPlusOne(ar);
    break;
  case PointerStep::AddIndex:
    address = AddressPlus(ar, static_cast<s16>(Regs().r.ix[ar]));
    break;
  }
}

void Interpreter::lrr(const UDSPInstruction opc)
{
  LoadThroughPointer(opc, PointerStep::Keep);
}

void Interpreter::lrrd(const UDSPInstruction opc)
{
  LoadThroughPointer(opc, PointerStep::Decrement);
}

void Interpreter::lrri(const UDSPInstruction opc)
{
  LoadThroughPointer(opc, PointerStep::Increment);
}

void Interpreter::lrrn(const UDSPInstruction opc)
{
  LoadThroughPointer(opc, PointerStep::AddIndex);
}

void Interpreter::srr(const UDSPInstruction opc)
{
  StoreThroughPointer(opc, PointerStep::Keep);
}

void Interpreter::srrd(const UDSPInstruction opc)
{
  StoreThroughPointer(opc, PointerStep::Decrement);
}

void Interpreter::srri(const UDSPInstruction opc)
{
  StoreThroughPointer(opc, PointerStep::Increment);
}

void Interpreter::srrn(const UDSPInstruction opc)
{
  StoreThroughPointer(opc, PointerStep::AddIndex);
}

// ---------------------------------------------------------------------------
// Instruction memory reads
// ---------------------------------------------------------------------------

// ILRR / ILRRD / ILRRI / ILRRN $acD.m, @$arS: bit 8 is D, bits 0-1 are S
void Interpreter::LoadFromIMEM(UDSPInstruction opc, PointerStep step)
{
  SDSP& dsp = Regs();
  const int pointer = opc & 3;
  const int acc = (opc >> 8) & 1;
  dsp.r.ac[acc].m = dsp.ReadIMEM(dsp.r.ar[pointer]);
  ExtendIfMidAccumulator(DSP_REG_ACM0 + acc);
  UpdatePointer(pointer, step);
}

void Interpreter::ilrr(const UDSPInstruction opc)
{
  LoadFromIMEM(opc, PointerStep::Keep);
}

void Interpreter::ilrrd(const UDSPInstruction opc)
{
  LoadFromIMEM(opc, PointerStep::Decrement);
}

void Interpreter::ilrri(const UDSPInstruction opc)
{
  LoadFromIMEM(opc, PointerStep::Increment);
}

void Interpreter::ilrrn(const UDSPInstruction opc)
{
  LoadFromIMEM(opc, PointerStep::AddIndex);
}
}  // namespace DSP::Interpreter
