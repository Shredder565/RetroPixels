// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Jumps, calls, returns, conditional execution, HALT and the hardware loops.
// The conditional forms carry their condition code in the low four bits.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

namespace DSP::Interpreter
{
namespace
{
constexpr u8 ConditionOf(UDSPInstruction opc)
{
  return static_cast<u8>(opc & 0xf);
}

// The register operand of JMPR/CALLR (bits 5-7).
constexpr int TargetRegister(UDSPInstruction opc)
{
  return (opc >> 5) & 7;
}
}  // namespace

// CALLcc addr (the address is the second word; it is consumed either way)
void Interpreter::call(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 target = dsp.FetchInstruction();
  if (!ConditionHolds(ConditionOf(opc)))
    return;
  dsp.StoreStack(StackRegister::Call, dsp.pc);
  dsp.pc = target;
}

// CALLRcc $R
void Interpreter::callr(const UDSPInstruction opc)
{
  if (!ConditionHolds(ConditionOf(opc)))
    return;
  SDSP& dsp = Regs();
  const u16 target = OpReadRegister(TargetRegister(opc));
  dsp.StoreStack(StackRegister::Call, dsp.pc);
  dsp.pc = target;
}

// IFcc: skips the next instruction unless the condition holds
void Interpreter::ifcc(const UDSPInstruction opc)
{
  if (!ConditionHolds(ConditionOf(opc)))
    Regs().SkipInstruction();
}

// Jcc addr
void Interpreter::jcc(const UDSPInstruction opc)
{
  SDSP& dsp = Regs();
  const u16 target = dsp.FetchInstruction();
  if (ConditionHolds(ConditionOf(opc)))
    dsp.pc = target;
}

// JMPRcc $R
void Interpreter::jmprcc(const UDSPInstruction opc)
{
  if (ConditionHolds(ConditionOf(opc)))
    Regs().pc = OpReadRegister(TargetRegister(opc));
}

// RETcc
void Interpreter::ret(const UDSPInstruction opc)
{
  if (ConditionHolds(ConditionOf(opc)))
    Regs().pc = Regs().PopStack(StackRegister::Call);
}

// RTIcc: restores $sr from the data stack, then returns
void Interpreter::rti(const UDSPInstruction opc)
{
  if (!ConditionHolds(ConditionOf(opc)))
    return;
  SDSP& dsp = Regs();
  dsp.r.sr = dsp.PopStack(StackRegister::Data);
  dsp.pc = dsp.PopStack(StackRegister::Call);
}

// HALT: stops the DSP on itself; the run loops notice the bit.
void Interpreter::halt(const UDSPInstruction)
{
  SDSP& dsp = Regs();
  dsp.control_reg |= CR_HALT;
  --dsp.pc;
}

// ---------------------------------------------------------------------------
// Hardware loops
//
// A loop pushes the address of its first body instruction (call stack), of its
// last body instruction (loop address stack) and the iteration count (loop
// counter stack). Step() sees the end through the analyzer's loop-end marks.
// A count of zero skips the body.
// ---------------------------------------------------------------------------

void Interpreter::EnterLoop(u16 last_address, u16 count)
{
  SDSP& dsp = Regs();
  dsp.StoreStack(StackRegister::Call, dsp.pc);
  dsp.StoreStack(StackRegister::LoopAddress, last_address);
  dsp.StoreStack(StackRegister::LoopCounter, count);
}

// LOOP $R / LOOPI #I: repeat the next instruction
void Interpreter::loop(const UDSPInstruction opc)
{
  const u16 count = OpReadRegister(opc & 0x1f);
  if (count != 0)
    EnterLoop(Regs().pc, count);
  else
    Regs().SkipInstruction();
}

void Interpreter::loopi(const UDSPInstruction opc)
{
  const u16 count = opc & 0xff;
  if (count != 0)
    EnterLoop(Regs().pc, count);
  else
    Regs().SkipInstruction();
}

// BLOOP $R, addr / BLOOPI #I, addr: repeat everything up to and including addr
void Interpreter::bloop(const UDSPInstruction opc)
{
  const u16 count = OpReadRegister(opc & 0x1f);
  const u16 last_address = Regs().FetchInstruction();
  if (count != 0)
  {
    EnterLoop(last_address, count);
    return;
  }
  Regs().pc = last_address;
  Regs().SkipInstruction();
}

void Interpreter::bloopi(const UDSPInstruction opc)
{
  const u16 count = opc & 0xff;
  const u16 last_address = Regs().FetchInstruction();
  if (count != 0)
  {
    EnterLoop(last_address, count);
    return;
  }
  Regs().pc = last_address;
  Regs().SkipInstruction();
}
}  // namespace DSP::Interpreter
