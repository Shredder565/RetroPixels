// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Extension opcodes: the parallel moves in the low byte (low seven bits in the
// 0x3xxx group) of extendable instructions.
//
// An extension runs before its main instruction and must not disturb the
// registers that instruction reads, so it only queues its register writes
// (ApplyWriteBackLog commits them in queue order). Memory stores happen right
// away; data memory is never an input of a main instruction that has an
// extension.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

namespace DSP::Interpreter
{
namespace
{
// The dual loads read one word through $arS and one through $ar3, unless both
// pointers lie in the same 1 KiW bank: then the second read also goes through
// $arS.
bool SameBank(u16 a, u16 b)
{
  return (a >> 10) == (b >> 10);
}
}  // namespace

// ---------------------------------------------------------------------------
// Shared bodies
// ---------------------------------------------------------------------------

// S / SN @$arD, $(0x1c+S)
void Interpreter::ExtStore(UDSPInstruction opc, AddrStep step)
{
  SDSP& dsp = Regs();
  const int pointer = opc & 3;
  const u16 value = OpReadRegister(DSP_REG_ACL0 + ((opc >> 3) & 3));
  dsp.WriteDMEM(dsp.r.ar[pointer], value);
  WriteToBackLog(pointer, AddressAfter(pointer, step));
}

// L / LN $(0x18+D), @$arS
void Interpreter::ExtLoad(UDSPInstruction opc, AddrStep step)
{
  SDSP& dsp = Regs();
  const int pointer = opc & 3;
  const int target = DSP_REG_AXL0 + ((opc >> 3) & 7);
  const u16 value = dsp.ReadDMEM(dsp.r.ar[pointer]);

  if (target >= DSP_REG_ACM0 && IsSRFlagSet(SR_40_MODE_BIT))
  {
    // A 40-bit mode load into $acD.m sets the whole accumulator.
    const int acc = target - DSP_REG_ACM0;
    WriteToBackLog(DSP_REG_ACH0 + acc, (value & 0x8000) != 0 ? u16{0xffff} : u16{0});
    WriteToBackLog(target, value);
    WriteToBackLog(DSP_REG_ACL0 + acc, 0);
  }
  else
  {
    WriteToBackLog(target, value);
  }
  WriteToBackLog(pointer, AddressAfter(pointer, step));
}

// LS/SL family: store $acS.m through one of $ar0/$ar3, load $(0x18+D) through
// the other, then move both pointers.
void Interpreter::ExtLoadStore(UDSPInstruction opc, bool store_through_ar0, AddrStep ar0_step,
                               AddrStep ar3_step)
{
  SDSP& dsp = Regs();
  const int store_pointer = store_through_ar0 ? 0 : 3;
  const int load_pointer = store_through_ar0 ? 3 : 0;

  const u16 value = OpReadRegister(DSP_REG_ACM0 + (opc & 1));
  dsp.WriteDMEM(dsp.r.ar[store_pointer], value);

  WriteToBackLog(DSP_REG_AXL0 + ((opc >> 4) & 3), dsp.ReadDMEM(dsp.r.ar[load_pointer]));
  WriteToBackLog(DSP_REG_AR3, AddressAfter(3, ar3_step));
  WriteToBackLog(DSP_REG_AR0, AddressAfter(0, ar0_step));
}

// LD/LDAX family: two loads, through $arS and (usually) $ar3.
void Interpreter::ExtLoadPair(int first_reg, int second_reg, int source_ar, AddrStep source_step,
                              AddrStep ar3_step)
{
  SDSP& dsp = Regs();
  const u16 source = dsp.r.ar[source_ar];

  WriteToBackLog(first_reg, dsp.ReadDMEM(source));
  const u16 second_address = SameBank(source, dsp.r.ar[3]) ? source : dsp.r.ar[3];
  WriteToBackLog(second_reg, dsp.ReadDMEM(second_address));

  WriteToBackLog(source_ar, AddressAfter(source_ar, source_step));
  WriteToBackLog(DSP_REG_AR3, AddressAfter(3, ar3_step));
}

// ---------------------------------------------------------------------------
// Address register steps
// ---------------------------------------------------------------------------

// DR / IR / NR $arR
void Interpreter::dr(const UDSPInstruction opc)
{
  WriteToBackLog(opc & 3, AddressMinusOne(opc & 3));
}

void Interpreter::ir(const UDSPInstruction opc)
{
  WriteToBackLog(opc & 3, AddressPlusOne(opc & 3));
}

void Interpreter::nr(const UDSPInstruction opc)
{
  WriteToBackLog(opc & 3, AddressAfter(opc & 3, AddrStep::Index));
}

// MV $(0x18+D), $(0x1c+S)
void Interpreter::mv(const UDSPInstruction opc)
{
  WriteToBackLog(DSP_REG_AXL0 + ((opc >> 2) & 3), OpReadRegister(DSP_REG_ACL0 + (opc & 3)));
}

// ---------------------------------------------------------------------------
// Single loads and stores
// ---------------------------------------------------------------------------

void Interpreter::s(const UDSPInstruction opc)
{
  ExtStore(opc, AddrStep::Increment);
}

void Interpreter::sn(const UDSPInstruction opc)
{
  ExtStore(opc, AddrStep::Index);
}

void Interpreter::l(const UDSPInstruction opc)
{
  ExtLoad(opc, AddrStep::Increment);
}

void Interpreter::ln(const UDSPInstruction opc)
{
  ExtLoad(opc, AddrStep::Index);
}

// ---------------------------------------------------------------------------
// Load and store at once (N: $ar0 += $ix0, M: $ar3 += $ix3, else +1)
// ---------------------------------------------------------------------------

void Interpreter::ls(const UDSPInstruction opc)
{
  ExtLoadStore(opc, false, AddrStep::Increment, AddrStep::Increment);
}

void Interpreter::lsn(const UDSPInstruction opc)
{
  ExtLoadStore(opc, false, AddrStep::Index, AddrStep::Increment);
}

void Interpreter::lsm(const UDSPInstruction opc)
{
  ExtLoadStore(opc, false, AddrStep::Increment, AddrStep::Index);
}

void Interpreter::lsnm(const UDSPInstruction opc)
{
  ExtLoadStore(opc, false, AddrStep::Index, AddrStep::Index);
}

void Interpreter::sl(const UDSPInstruction opc)
{
  ExtLoadStore(opc, true, AddrStep::Increment, AddrStep::Increment);
}

void Interpreter::sln(const UDSPInstruction opc)
{
  ExtLoadStore(opc, true, AddrStep::Index, AddrStep::Increment);
}

void Interpreter::slm(const UDSPInstruction opc)
{
  ExtLoadStore(opc, true, AddrStep::Increment, AddrStep::Index);
}

void Interpreter::slnm(const UDSPInstruction opc)
{
  ExtLoadStore(opc, true, AddrStep::Index, AddrStep::Index);
}

// ---------------------------------------------------------------------------
// Dual loads (N: $arS += $ixS, M: $ar3 += $ix3, else +1)
// ---------------------------------------------------------------------------

// LD $ax0.D, $ax1.R, @$arS: bit 5 is D (l/h), bit 4 is R, bits 0-1 are S
void Interpreter::LoadAxPair(UDSPInstruction opc, AddrStep source_step, AddrStep ar3_step)
{
  const int first = DSP_REG_AXL0 + (((opc >> 5) & 1) << 1);
  const int second = DSP_REG_AXL1 + (((opc >> 4) & 1) << 1);
  ExtLoadPair(first, second, opc & 3, source_step, ar3_step);
}

// LDAX $axR, @$arS: bit 5 is S ($ar0/$ar1), bit 4 is R
void Interpreter::LoadAxWhole(UDSPInstruction opc, AddrStep source_step, AddrStep ar3_step)
{
  const int ax = (opc >> 4) & 1;
  ExtLoadPair(DSP_REG_AXH0 + ax, DSP_REG_AXL0 + ax, (opc >> 5) & 1, source_step, ar3_step);
}

void Interpreter::ld(const UDSPInstruction opc)
{
  LoadAxPair(opc, AddrStep::Increment, AddrStep::Increment);
}

void Interpreter::ldn(const UDSPInstruction opc)
{
  LoadAxPair(opc, AddrStep::Index, AddrStep::Increment);
}

void Interpreter::ldm(const UDSPInstruction opc)
{
  LoadAxPair(opc, AddrStep::Increment, AddrStep::Index);
}

void Interpreter::ldnm(const UDSPInstruction opc)
{
  LoadAxPair(opc, AddrStep::Index, AddrStep::Index);
}

void Interpreter::ldax(const UDSPInstruction opc)
{
  LoadAxWhole(opc, AddrStep::Increment, AddrStep::Increment);
}

void Interpreter::ldaxn(const UDSPInstruction opc)
{
  LoadAxWhole(opc, AddrStep::Index, AddrStep::Increment);
}

void Interpreter::ldaxm(const UDSPInstruction opc)
{
  LoadAxWhole(opc, AddrStep::Increment, AddrStep::Index);
}

void Interpreter::ldaxnm(const UDSPInstruction opc)
{
  LoadAxWhole(opc, AddrStep::Index, AddrStep::Index);
}

// The empty extension.
void Interpreter::nop_ext(const UDSPInstruction)
{
}
}  // namespace DSP::Interpreter
