// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// FPSCR moves, condition register logic, MSR / segment register access and the special purpose
// registers with their side effects (timers, caches, BATs, locked-cache DMA, thermal sensors).

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
using PowerPC::PowerPCState;

// Expands an 8-bit field mask (bit i selects 4-bit field 7-i) into a 32-bit mask.
u32 ExpandFieldMask(u32 fields)
{
  u32 mask = 0;
  for (u32 i = 0; i < 8; ++i)
  {
    if (fields & (1u << i))
      mask |= 0xFu << (4 * i);
  }
  return mask;
}

// After any FPSCR write: refresh VX/FEX (possibly raising a program exception) and hand the new
// rounding / non-IEEE mode to the host FPU.
void AfterFPSCRWrite(PowerPCState& s, UGeckoInstruction inst)
{
  InterpreterFP::RefreshFPSummary(s);
  PowerPC::RoundingModeUpdated(s);
  if (inst.Rc)
    s.UpdateCR1();
}

// Supervisor-only instructions raise a privileged-instruction program exception in user mode.
bool Supervisor(PowerPCState& s)
{
  if (!s.msr.PR)
    return true;
  GenerateProgramException(s, ProgramExceptionCause::PrivilegedInstruction);
  return false;
}

// The 10-bit SPR number is encoded with its two 5-bit halves swapped.
u32 DecodeSPR(u32 field)
{
  return ((field & 0x1F) << 5) | ((field >> 5) & 0x1F);
}

bool UserReadableSPR(u32 index)
{
  return index == SPR_XER || index == SPR_LR || index == SPR_CTR || index == SPR_TL ||
         index == SPR_TU;
}

bool UserWritableSPR(u32 index)
{
  return index == SPR_XER || index == SPR_LR || index == SPR_CTR;
}

bool IsDBAT(u32 index)
{
  return (index >= SPR_DBAT0U && index <= SPR_DBAT3L) ||
         (index >= SPR_DBAT4U && index <= SPR_DBAT7L);
}

bool IsIBAT(u32 index)
{
  return (index >= SPR_IBAT0U && index <= SPR_IBAT3L) ||
         (index >= SPR_IBAT4U && index <= SPR_IBAT7L);
}

// Thermal management: the sensor always reads a fixed temperature. A threshold register reports
// a valid comparison only while THRM3[E] and its own V bit are set.
void RefreshThermalRegister(const PowerPCState& s, UReg_THRM12& reg)
{
  constexpr u32 SENSOR_CELSIUS = 42;
  const bool active = THRM3(s).E && reg.V;
  reg.TIV = active;
  if (!active)
    return;
  const u32 threshold = reg.THRESHOLD;
  reg.TIN = reg.TID ? (SENSOR_CELSIUS < threshold) : (SENSOR_CELSIUS > threshold);
}

// Locked-cache DMA, performed instantly when DMAL[T] is written as 1.
void RunLockedCacheDMA(PowerPCState& s, PowerPC::MMU& mmu)
{
  if (DMAL(s).DMA_T)
  {
    const u32 memory = DMAU(s).MEM_ADDR << 5;
    const u32 cache = DMAL(s).LC_ADDR << 5;
    u32 blocks = (DMAU(s).DMA_LEN_U << 2) | DMAL(s).DMA_LEN_L;
    if (blocks == 0)
      blocks = 128;
    if (DMAL(s).DMA_LD)
      mmu.DMA_MemoryToLC(cache, memory, blocks);
    else
      mmu.DMA_LCToMemory(memory, cache, blocks);
  }
  DMAL(s).DMA_T = 0;
}

void WriteHID0(Core::System& system, PowerPCState& s, u32 previous)
{
  UReg_HID0 before;
  before.Hex = previous;
  if (HID0(s).ICE != before.ICE)
    INFO_LOG_FMT(POWERPC, "HID0: instruction cache enable is now {}", HID0(s).ICE);
  if (HID0(s).ILOCK != before.ILOCK)
    INFO_LOG_FMT(POWERPC, "HID0: instruction cache lock is now {}", HID0(s).ILOCK);
  if (HID0(s).ICFI)
  {
    // The flash-invalidate bit is self-clearing.
    HID0(s).ICFI = 0;
    INFO_LOG_FMT(POWERPC, "HID0: instruction cache invalidated (ICE={})", HID0(s).ICE);
    s.iCache.Reset(system.GetJitInterface());
  }
}

template <u32 truth_table>
void ConditionBitOp(PowerPCState& s, UGeckoInstruction inst)
{
  // Bit (2a + b) of the table is the result for inputs a and b.
  const u32 a = s.cr.GetBit(inst.CRBA);
  const u32 b = s.cr.GetBit(inst.CRBB);
  s.cr.SetBit(inst.CRBD, (truth_table >> (2 * a + b)) & 1);
}
}  // namespace

// ---- FPSCR

void Interpreter::mtfsb0x(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  s.fpscr.Hex &= ~(0x80000000u >> inst.CRBD);
  AfterFPSCRWrite(s, inst);
}

void Interpreter::mtfsb1x(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 bit = 0x80000000u >> inst.CRBD;
  // Setting an exception flag this way behaves like the exception occurring (it can set FX).
  if (bit & FPSCR_ANY_X)
    InterpreterFP::RaiseFP(s, bit);
  else
    s.fpscr |= bit;
  AfterFPSCRWrite(s, inst);
}

void Interpreter::mtfsfix(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 shift = 4 * inst.CRFD;
  const u32 field = 0xF0000000u >> shift;
  const u32 value = ((inst.hex >> 12) & 0xF) << 28;
  s.fpscr = (s.fpscr.Hex & ~field) | (value >> shift);
  AfterFPSCRWrite(s, inst);
}

void Interpreter::mtfsfx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 mask = ExpandFieldMask(inst.FM);
  const u32 source = static_cast<u32>(s.ps[inst.FB].PS0AsU64());
  s.fpscr = (s.fpscr.Hex & ~mask) | (source & mask);
  AfterFPSCRWrite(s, inst);
}

// mcrfs copies one FPSCR field to CR; any exception flag (or FX) it copies is cleared.
void Interpreter::mcrfs(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 shift = 28 - 4 * inst.CRFS;
  const u32 field = (s.fpscr.Hex >> shift) & 0xF;
  s.fpscr.Hex &= ~((0xFu << shift) & (FPSCR_FX | FPSCR_ANY_X));
  InterpreterFP::RefreshFPSummary(s);
  PowerPC::RoundingModeUpdated(s);
  s.cr.SetField(inst.CRFD, field);
}

// The upper word of the result reads as a quiet NaN pattern.
void Interpreter::mffsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  s.ps[inst.FD].SetPS0(u64{0xFFF8000000000000} | s.fpscr.Hex);
  if (inst.Rc)
    s.UpdateCR1();
}

// ---- condition register

void Interpreter::mcrxr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  s.cr.SetField(inst.CRFD, s.GetXER().Hex >> 28);
  s.xer_ca = 0;
  s.xer_so_ov = 0;
}

void Interpreter::mfcr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  s.gpr[inst.RD] = s.cr.Get();
}

void Interpreter::mtcrf(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 mask = ExpandFieldMask(inst.CRM);
  s.cr.Set((s.cr.Get() & ~mask) | (s.gpr[inst.RS] & mask));
}

void Interpreter::mcrf(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  s.cr.SetField(inst.CRFD, s.cr.GetField(inst.CRFS));
}

void Interpreter::crand(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b1000>(interpreter.m_ppc_state, inst);
}
void Interpreter::crandc(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b0100>(interpreter.m_ppc_state, inst);
}
void Interpreter::creqv(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b1001>(interpreter.m_ppc_state, inst);
}
void Interpreter::crnand(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b0111>(interpreter.m_ppc_state, inst);
}
void Interpreter::crnor(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b0001>(interpreter.m_ppc_state, inst);
}
void Interpreter::cror(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b1110>(interpreter.m_ppc_state, inst);
}
void Interpreter::crorc(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b1101>(interpreter.m_ppc_state, inst);
}
void Interpreter::crxor(Interpreter& interpreter, UGeckoInstruction inst)
{
  ConditionBitOp<0b0110>(interpreter.m_ppc_state, inst);
}

void Interpreter::isync(Interpreter&, UGeckoInstruction)
{
  // Nothing is prefetched, so there is nothing to discard.
}

// ---- MSR and segment registers

void Interpreter::mfmsr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (Supervisor(s))
    s.gpr[inst.RD] = s.msr.Hex;
}

void Interpreter::mtmsr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!Supervisor(s))
    return;

  s.msr.Hex = s.gpr[inst.RS];
  auto& power_pc = interpreter.m_system.GetPowerPC();
  power_pc.MSRUpdated();
  // Newly enabled FE0/FE1 make a pending enabled FP exception take effect now.
  if (s.fpscr.FEX && (s.msr.FE0 || s.msr.FE1))
    GenerateProgramException(s, ProgramExceptionCause::FloatingPoint);
  power_pc.CheckExceptions();
  interpreter.m_end_block = true;
}

void Interpreter::mfsr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (Supervisor(s))
    s.gpr[inst.RD] = s.sr[inst.SR];
}

void Interpreter::mfsrin(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (Supervisor(s))
    s.gpr[inst.RD] = s.sr[s.gpr[inst.RB] >> 28];
}

void Interpreter::mtsr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!Supervisor(s))
    return;
  s.sr[inst.SR] = s.gpr[inst.RS];
  interpreter.m_system.GetMMU().SRUpdated();
}

void Interpreter::mtsrin(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  if (!Supervisor(s))
    return;
  s.sr[s.gpr[inst.RB] >> 28] = s.gpr[inst.RS];
  interpreter.m_system.GetMMU().SRUpdated();
}

// ---- special purpose registers

void Interpreter::mftb(Interpreter& interpreter, UGeckoInstruction inst)
{
  [[maybe_unused]] const u32 index = DecodeSPR(inst.TBR);
  DEBUG_ASSERT_MSG(POWERPC, index == SPR_TL || index == SPR_TU, "mftb names a non-timebase SPR");
  mfspr(interpreter, inst);
}

void Interpreter::mfspr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  auto& system = interpreter.m_system;
  const u32 index = DecodeSPR(inst.SPR);
  if (s.msr.PR && !UserReadableSPR(index))
  {
    GenerateProgramException(s, ProgramExceptionCause::PrivilegedInstruction);
    return;
  }

  u32& reg = s.spr[index];
  switch (index)
  {
  case SPR_DEC:
    // Once the decrementer has gone negative it stays at the value it reached.
    if (!(reg & 0x80000000))
      reg = system.GetSystemTimers().GetFakeDecrementer();
    break;
  case SPR_TL:
  case SPR_TU:
    system.GetPowerPC().WriteFullTimeBaseValue(system.GetSystemTimers().GetFakeTimeBase());
    break;
  case SPR_WPAR:
    // Bit 0 (buffer not empty) mirrors the gather pipe; display list code polls it.
    reg = system.GetGPFifo().IsBNE() ? (reg | 1) : (reg & ~1u);
    break;
  case SPR_XER:
    reg = s.GetXER().Hex;
    break;
  case SPR_UPMC1:
    reg = s.spr[SPR_PMC1];
    break;
  case SPR_UPMC2:
    reg = s.spr[SPR_PMC2];
    break;
  case SPR_UPMC3:
    reg = s.spr[SPR_PMC3];
    break;
  case SPR_UPMC4:
    reg = s.spr[SPR_PMC4];
    break;
  case SPR_IABR:
    // The translate-enable bit of IABR always reads back as 0 (unlike DABR).
    s.gpr[inst.RD] = reg & ~1u;
    return;
  default:
    break;
  }
  s.gpr[inst.RD] = reg;
}

void Interpreter::mtspr(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  auto& system = interpreter.m_system;
  auto& mmu = interpreter.m_mmu;
  const u32 index = DecodeSPR(inst.SPR);
  if (s.msr.PR && !UserWritableSPR(index))
  {
    GenerateProgramException(s, ProgramExceptionCause::PrivilegedInstruction);
    return;
  }

  const u32 value = s.gpr[inst.RD];
  const u32 previous = s.spr[index];
  u32& reg = s.spr[index];
  reg = value;

  if (IsDBAT(index))
  {
    if (previous != value)
    {
      INFO_LOG_FMT(POWERPC, "DBAT {} changed {:x} -> {:x}", index, previous, value);
      mmu.DBATUpdated();
    }
    return;
  }
  if (IsIBAT(index))
  {
    if (previous != value)
    {
      INFO_LOG_FMT(POWERPC, "IBAT {} changed {:x} -> {:x}", index, previous, value);
      mmu.IBATUpdated();
    }
    return;
  }

  switch (index)
  {
  case SPR_TL:
  case SPR_TU:
    PanicAlertFmt("Illegal Write to TL/TU");
    break;
  case SPR_TL_W:
    TL(s) = value;
    system.GetSystemTimers().TimeBaseSet();
    break;
  case SPR_TU_W:
    TU(s) = value;
    system.GetSystemTimers().TimeBaseSet();
    break;
  case SPR_PVR:
    reg = previous;  // read-only
    break;
  case SPR_HID0:
    WriteHID0(system, s, previous);
    break;
  case SPR_HID1:
    // Only the top five bits (PLL configuration) are writable.
    reg &= 0xF8000000;
    break;
  case SPR_HID2:
    // The low half is reserved/status and DMAQL is read-only.
    reg = (reg & 0xF0FF0000) | (previous & 0x0F000000);
    break;
  case SPR_HID4:
    if (previous != value)
    {
      INFO_LOG_FMT(POWERPC, "HID4 changed {:x} -> {:x}", previous, value);
      mmu.IBATUpdated();
      mmu.DBATUpdated();
    }
    break;
  case SPR_WPAR:
    ASSERT_MSG(POWERPC, reg == GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS,
               "Gather pipe moved to unexpected address {:08x} @ PC {:08x}", reg, s.pc);
    system.GetGPFifo().ResetGatherPipe();
    break;
  case SPR_DMAL:
    RunLockedCacheDMA(s, mmu);
    break;
  case SPR_DEC:
    // Writing a value with the top bit newly set triggers the decrementer exception at once.
    if (!(previous & 0x80000000) && (value & 0x80000000))
    {
      INFO_LOG_FMT(POWERPC, "Decrementer exception triggered by software");
      s.Exceptions |= EXCEPTION_DECREMENTER;
    }
    system.GetSystemTimers().DecrementerSet();
    break;
  case SPR_SDR:
    mmu.SDRUpdated();
    break;
  case SPR_XER:
    s.SetXER(UReg_XER{reg});
    break;
  case SPR_MMCR0:
  case SPR_MMCR1:
    MMCRUpdated(s);
    break;
  case SPR_THRM1:
  case SPR_THRM2:
  case SPR_THRM3:
    // THRM3 affects both comparators, so both are refreshed on any write.
    RefreshThermalRegister(s, THRM1(s));
    RefreshThermalRegister(s, THRM2(s));
    break;
  default:
    // GQRs, L2CR and the rest are plain storage.
    break;
  }
}
