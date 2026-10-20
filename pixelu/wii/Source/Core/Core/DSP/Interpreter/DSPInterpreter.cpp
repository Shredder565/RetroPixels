// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The interpreter's main loop and the machinery its handlers share: operand
// access, address generation, status flags and the extension write queue. The
// handlers themselves live in the DSPInt*.cpp files, grouped by kind.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"

#include "Core/CoreTiming.h"
#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPHost.h"
#include "Core/DSP/DSPTables.h"
#include "Core/DSP/Interpreter/DSPIntCCUtil.h"
#include "Core/DSP/Interpreter/DSPIntTables.h"
#include "Core/DSP/Interpreter/DSPIntUtil.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace DSP::Interpreter
{
namespace
{
// An extension and its main instruction work on the same register snapshot. If
// both write one register, the hardware ORs the two results. No ucode is known
// to depend on that and the JIT does not model it, so normally the extension's
// write simply lands last. Turning this on models the OR: main instructions clear
// the queued registers before writing, and the queue is ORed in afterwards.
constexpr bool OR_OVERLAPPING_EXTENSION_WRITES = false;

// Where the boot code is copied from when the CPU releases CR_INIT, and how much.
constexpr u32 INIT_CODE_SOURCE = 0x81000000;
constexpr u32 INIT_CODE_BYTES = 0x1000;

// Time base ticks until CR_INIT_CODE reads back as clear again (measured on a Wii,
// with some spread).
constexpr u64 INIT_CODE_BUSY_TICKS = 130;

[[maybe_unused]] bool IsAccumulatorPart(int reg, int acc)
{
  return reg == DSP_REG_ACL0 + acc || reg == DSP_REG_ACM0 + acc || reg == DSP_REG_ACH0 + acc;
}
}  // namespace

Interpreter::Interpreter(DSPCore& dsp) : m_dsp_core{dsp}
{
  InitInstructionTables();
}

Interpreter::~Interpreter() = default;

// ---------------------------------------------------------------------------
// Running code
// ---------------------------------------------------------------------------

void Interpreter::Execute(UDSPInstruction inst)
{
  // The extension sees the registers as they were before the instruction; its
  // writes wait in the queue until the main part is done.
  const bool has_extension = GetOpTemplate(inst)->extended;
  if (has_extension)
    (this->*GetExtOp(inst))(inst);

  (this->*GetOp(inst))(inst);

  if (has_extension)
    ApplyWriteBackLog();
}

void Interpreter::Step()
{
  SDSP& dsp = Regs();

  m_dsp_core.CheckExceptions();
  dsp.AdvanceStepCounter();

  Execute(dsp.FetchInstruction());

  // pc - 1 is the last word of what just ran (or the word before a jump target,
  // which is never marked as a loop end unless it really is one).
  if (dsp.GetAnalyzer().IsLoopEnd(static_cast<u16>(dsp.pc - 1)))
    ServiceLoopEnd();
}

void Interpreter::ServiceLoopEnd()
{
  SDSP& dsp = Regs();
  const u16 last_address = dsp.r.st[2];
  u16& iterations_left = dsp.r.st[3];

  if (last_address == 0 || iterations_left == 0)
    return;
  if (static_cast<int>(dsp.pc) - 1 != last_address)
    return;

  --iterations_left;
  if (iterations_left != 0)
  {
    // Back to the first instruction of the body.
    dsp.pc = dsp.r.st[0];
    return;
  }

  // Done: drop the loop's three stack entries.
  dsp.PopStack(StackRegister::Call);
  dsp.PopStack(StackRegister::LoopAddress);
  dsp.PopStack(StackRegister::LoopCounter);
}

Interpreter::BurstEnd Interpreter::RunBurst(int& cycles, int count, bool check_halt,
                                            bool check_idle, bool check_breaks)
{
  SDSP& dsp = Regs();
  for (int done = 0; done < count; ++done)
  {
    if (check_halt && (dsp.control_reg & CR_HALT) != 0)
      return BurstEnd::Finished;

    if (check_breaks && m_dsp_core.BreakPoints().IsAddressBreakPoint(dsp.pc))
    {
      m_dsp_core.SetState(State::Stepping);
      return BurstEnd::Breakpoint;
    }

    if (check_idle && dsp.GetAnalyzer().IsIdleSkip(dsp.pc))
      return BurstEnd::Finished;

    Step();
    --cycles;
    if (cycles <= 0)
      return BurstEnd::Finished;
  }
  return BurstEnd::Continue;
}

int Interpreter::RunCyclesThread(int cycles)
{
  SDSP& dsp = Regs();
  for (;;)
  {
    if ((dsp.control_reg & CR_HALT) != 0)
      return 0;

    // The CPU thread raises external interrupts through this flag.
    if (dsp.external_interrupt_waiting.exchange(false, std::memory_order_acquire))
      m_dsp_core.CheckExternalInterrupt();

    Step();
    --cycles;
    if (cycles <= 0)
      return 0;
  }
}

// The time slice is spent in rounds: a few instructions that may stop at an idle
// loop, then a longer stretch that does not look. Before the first round a few
// instructions run unconditionally, so code that was just woken up gets past the
// polling loop it was sitting in. (There is no pause support; when emulation
// pauses, this simply is not called.)
int Interpreter::RunCycles(int cycles)
{
  if (RunBurst(cycles, 8, true, false, false) != BurstEnd::Continue)
    return 0;

  for (;;)
  {
    if (RunBurst(cycles, 8, true, true, false) != BurstEnd::Continue)
      return 0;
    if (RunBurst(cycles, 200, false, false, false) != BurstEnd::Continue)
      return 0;
  }
}

// Like RunCycles, but stops in front of breakpoints, leaving the rest of the
// slice unused.
int Interpreter::RunCyclesDebug(int cycles)
{
  const auto finish = [&cycles](BurstEnd end) { return end == BurstEnd::Breakpoint ? cycles : 0; };

  BurstEnd end = RunBurst(cycles, 8, true, false, true);
  if (end != BurstEnd::Continue)
    return finish(end);

  for (;;)
  {
    end = RunBurst(cycles, 8, true, true, true);
    if (end != BurstEnd::Continue)
      return finish(end);
    end = RunBurst(cycles, 200, false, false, true);
    if (end != BurstEnd::Continue)
      return finish(end);
  }
}

// ---------------------------------------------------------------------------
// The CPU-side control register (DSP_CONTROL, not $cr)
// ---------------------------------------------------------------------------

void Interpreter::WriteControlRegister(u16 val)
{
  SDSP& dsp = Regs();

  // Halting itself is done by the run loops (and the JIT dispatcher), which look
  // at the bit; CR_EXTERNAL_INT is dealt with by DSPLLE.
  if (((dsp.control_reg ^ val) & CR_HALT) != 0)
  {
    INFO_LOG_FMT(DSPLLE, "DSP_CONTROL halt bit changed: {:04x} -> {:04x}, PC {:04x}",
                 dsp.control_reg, val, dsp.pc);
  }

  if ((val & CR_RESET) != 0)
  {
    INFO_LOG_FMT(DSPLLE, "DSP_CONTROL RESET");
    m_dsp_core.Reset();
    val &= static_cast<u16>(~CR_RESET);
  }

  // Releasing CR_INIT boots the DSP: a block of code is copied from main memory
  // into IRAM and run from address 0. CR_INIT_CODE stays set for a short while
  // afterwards. (What exactly the hardware copies is not fully known.)
  const bool init_released = (dsp.control_reg & CR_INIT) != 0 && (val & CR_INIT) == 0;
  if (init_released)
  {
    INFO_LOG_FMT(DSPLLE, "DSP_CONTROL INIT");
    dsp.pc = 0;

    Common::UnWriteProtectMemory(dsp.iram, DSP_IRAM_BYTE_SIZE, false);
    Host::DMAToDSP(dsp.iram, INIT_CODE_SOURCE, INIT_CODE_BYTES);
    Common::WriteProtectMemory(dsp.iram, DSP_IRAM_BYTE_SIZE, false);
    Host::CodeLoaded(m_dsp_core, INIT_CODE_SOURCE, INIT_CODE_BYTES);

    val = static_cast<u16>((val & ~CR_INIT) | CR_INIT_CODE);
    dsp.control_reg_init_code_clear_time =
        Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase() + INIT_CODE_BUSY_TICKS;
  }

  dsp.control_reg = val;
}

u16 Interpreter::ReadControlRegister()
{
  SDSP& dsp = Regs();
  if ((dsp.control_reg & CR_INIT_CODE) == 0)
    return dsp.control_reg;

  auto& system = Core::System::GetInstance();
  if (system.GetSystemTimers().GetFakeTimeBase() >= dsp.control_reg_init_code_clear_time)
  {
    dsp.control_reg &= static_cast<u16>(~CR_INIT_CODE);
  }
  else
  {
    // Make sure the CPU gets to poll again soon.
    system.GetCoreTiming().ForceExceptionCheck(50);
  }
  return dsp.control_reg;
}

void Interpreter::SetSRFlag(u16 flag)
{
  Regs().SetSRFlag(flag);
}

bool Interpreter::IsSRFlagSet(u16 flag) const
{
  return Regs().IsSRFlagSet(flag);
}

// ---------------------------------------------------------------------------
// Condition codes
// ---------------------------------------------------------------------------

bool Interpreter::ConditionHolds(u8 condition) const
{
  const u16 sr = Regs().r.sr;
  const auto set = [sr](u16 flag) { return (sr & flag) != 0; };

  const u8 code = condition & 0xf;
  if (code == 0xf)
    return true;
  if (code == 0xe)
    return set(SR_OVERFLOW);

  // The other codes come in pairs: the odd code tests a property, the even code
  // before it tests its negation.
  const bool less = set(SR_OVERFLOW) != set(SR_SIGN);
  bool property = false;
  switch (code >> 1)
  {
  case 0:  // GE / L
    property = less;
    break;
  case 1:  // G / LE
    property = less || set(SR_ARITH_ZERO);
    break;
  case 2:  // NZ / Z
    property = set(SR_ARITH_ZERO);
    break;
  case 3:  // NC / C
    property = set(SR_CARRY);
    break;
  case 4:  // X8 / X9: whether the last result needed more than 32 bits
    property = set(SR_OVER_S32);
    break;
  case 5:  // XA / XB
    property = set(SR_ARITH_ZERO) || !(set(SR_OVER_S32) || set(SR_TOP2BITS));
    break;
  default:  // LNZ / LZ
    property = set(SR_LOGIC_ZERO);
    break;
  }
  return (code & 1) != 0 ? property : !property;
}

// ---------------------------------------------------------------------------
// Address registers
//
// $wrN turns $arN into a ring: the low bits covered by the wrap mask cycle
// while the high bits stay put. These follow the published descriptions of the
// address generator, including its odd behaviour for masks that are not of the
// form 2^n - 1.
// ---------------------------------------------------------------------------

u16 Interpreter::AddressPlusOne(int ar) const
{
  const u32 address = Regs().r.ar[ar];
  const u32 wrap = Regs().r.wr[ar];

  u32 next = address + 1;
  if ((next ^ address) > ((wrap | 1) << 1))
    next -= wrap + 1;
  return static_cast<u16>(next);
}

u16 Interpreter::AddressMinusOne(int ar) const
{
  const u32 address = Regs().r.ar[ar];
  const u32 wrap = Regs().r.wr[ar];

  // Adding the wrap mask steps down by one inside the ring.
  u32 next = address + wrap;
  if (((next ^ address) & ((wrap | 1) << 1)) > wrap)
    next -= wrap + 1;
  return static_cast<u16>(next);
}

u16 Interpreter::AddressPlus(int ar, s16 step) const
{
  const u32 address = Regs().r.ar[ar];
  const u32 wrap = Regs().r.wr[ar];
  const u32 delta = static_cast<u32>(static_cast<s32>(step));
  const u32 carry_bits = (wrap | 1) << 1;

  u32 next = address + delta;
  const u32 carries = (next ^ address ^ delta) & carry_bits;

  if (step >= 0)
  {
    if (carries > wrap)
      next -= wrap + 1;
  }
  else if ((((next + wrap + 1) ^ next) & carries) <= wrap)
  {
    next += wrap + 1;
  }
  return static_cast<u16>(next);
}

u16 Interpreter::AddressMinus(int ar, s16 step) const
{
  const u32 address = Regs().r.ar[ar];
  const u32 wrap = Regs().r.wr[ar];
  const u32 delta = static_cast<u32>(static_cast<s32>(step));
  const u32 carry_bits = (wrap | 1) << 1;

  u32 next = address - delta;
  const u32 carries = (next ^ address ^ ~delta) & carry_bits;

  // A negative step (other than -0x8000, which cannot be negated in 16 bits)
  // moves the address up.
  const bool moves_up = step < 0 && step != -0x8000;
  if (moves_up)
  {
    if (carries > wrap)
      next -= wrap + 1;
  }
  else if ((((next + wrap + 1) ^ next) & carries) <= wrap)
  {
    next += wrap + 1;
  }
  return static_cast<u16>(next);
}

u16 Interpreter::AddressAfter(int ar, AddrStep step) const
{
  if (step == AddrStep::Increment)
    return AddressPlusOne(ar);
  return AddressPlus(ar, static_cast<s16>(Regs().r.ix[ar]));
}

// ---------------------------------------------------------------------------
// Register views
// ---------------------------------------------------------------------------

s64 Interpreter::Acc(int which) const
{
  return static_cast<s64>(Regs().r.ac[which].val);
}

void Interpreter::SetAcc(int which, s64 value)
{
  Regs().r.ac[which].val = static_cast<u64>(WrapTo40Bits(value));
}

s32 Interpreter::AxWhole(int which) const
{
  const auto& ax = Regs().r.ax[which];
  return static_cast<s32>((static_cast<u32>(ax.h) << 16) | ax.l);
}

s16 Interpreter::AxLow(int which) const
{
  return static_cast<s16>(Regs().r.ax[which].l);
}

s16 Interpreter::AxHigh(int which) const
{
  return static_cast<s16>(Regs().r.ax[which].h);
}

u16 Interpreter::AxHalf(int which, bool high) const
{
  const auto& ax = Regs().r.ax[which];
  return high ? ax.h : ax.l;
}

s64 Interpreter::Product() const
{
  // The product is kept as a sum of two partial products (m and m2), which the
  // hardware only adds up when the value is read.
  const auto& prod = Regs().r.prod;
  const s64 top = static_cast<s64>(static_cast<s8>(static_cast<u8>(prod.h)));
  const s64 middle = static_cast<s64>(prod.m) + static_cast<s64>(prod.m2);
  return top * (s64{1} << 32) + ((middle << 16) | prod.l);
}

s64 Interpreter::ProductRounded() const
{
  return RoundToMiddleWord(Product());
}

void Interpreter::SetProduct(s64 value)
{
  // Collapses the product into one value with m2 = 0. Real hardware keeps more
  // state in there, but no ucode depends on it.
  Regs().r.prod.val = static_cast<u64>(value) & ACC40_BITS;
}

// ---------------------------------------------------------------------------
// Status flags
// ---------------------------------------------------------------------------

void Interpreter::SetFlags16(s16 value, bool carry, bool overflow, bool over_s32)
{
  u16 flags = 0;
  if (carry)
    flags |= SR_CARRY;
  if (overflow)
    flags |= SR_OVERFLOW | SR_OVERFLOW_STICKY;
  if (value == 0)
    flags |= SR_ARITH_ZERO;
  if (value < 0)
    flags |= SR_SIGN;
  if (over_s32)
    flags |= SR_OVER_S32;

  // The top two bits of the 16-bit value agree.
  const u16 top_two = static_cast<u16>(static_cast<u16>(value) >> 14);
  if (top_two == 0 || top_two == 3)
    flags |= SR_TOP2BITS;

  u16& sr = Regs().r.sr;
  sr = static_cast<u16>((sr & ~SR_CMP_MASK) | flags);
}

void Interpreter::SetFlags40(s64 value, bool carry, bool overflow)
{
  u16 flags = 0;
  if (carry)
    flags |= SR_CARRY;
  if (overflow)
    flags |= SR_OVERFLOW | SR_OVERFLOW_STICKY;
  if (value == 0)
    flags |= SR_ARITH_ZERO;
  if (value < 0)
    flags |= SR_SIGN;
  if (ExceedsS32(value))
    flags |= SR_OVER_S32;

  // Bits 31 and 30 agree.
  const s64 top_two = value & 0xc0000000;
  if (top_two == 0 || top_two == 0xc0000000)
    flags |= SR_TOP2BITS;

  u16& sr = Regs().r.sr;
  sr = static_cast<u16>((sr & ~SR_CMP_MASK) | flags);
}

// result is a + b already reduced to 40 bits and sign extended.
void Interpreter::SetFlagsAdd(s64 a, s64 b, s64 result)
{
  SetFlags40(result, CarriedOnAdd(static_cast<u64>(a), static_cast<u64>(result)),
             SignedOverflow(a, b, result));
}

// result is a - b already reduced to 40 bits and sign extended.
void Interpreter::SetFlagsSub(s64 a, s64 b, s64 result)
{
  SetFlags40(result, CarriedOnSubtract(static_cast<u64>(a), static_cast<u64>(result)),
             SignedOverflow(a, -b, result));
}

void Interpreter::SetLogicZero(bool zero)
{
  u16& sr = Regs().r.sr;
  if (zero)
    sr |= SR_LOGIC_ZERO;
  else
    sr &= static_cast<u16>(~SR_LOGIC_ZERO);
}

// ---------------------------------------------------------------------------
// Register operands
// ---------------------------------------------------------------------------

u16 Interpreter::OpReadRegister(int reg)
{
  SDSP& dsp = Regs();
  const int index = reg & 0x1f;

  // 0x00-0x0f: four groups of four.
  if (index < 0x10)
  {
    const int n = index & 3;
    switch (index >> 2)
    {
    case 0:
      return dsp.r.ar[n];
    case 1:
      return dsp.r.ix[n];
    case 2:
      return dsp.r.wr[n];
    default:
      // Reading a stack register pops it.
      return dsp.PopStack(static_cast<StackRegister>(n));
    }
  }

  switch (index)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    return static_cast<u16>(dsp.r.ac[index - DSP_REG_ACH0].h);
  case DSP_REG_CR:
    return dsp.r.cr;
  case DSP_REG_SR:
    return dsp.r.sr;
  case DSP_REG_PRODL:
    return dsp.r.prod.l;
  case DSP_REG_PRODM:
    return dsp.r.prod.m;
  case DSP_REG_PRODH:
    return dsp.r.prod.h;
  case DSP_REG_PRODM2:
    return dsp.r.prod.m2;
  case DSP_REG_AXL0:
  case DSP_REG_AXL1:
    return dsp.r.ax[index - DSP_REG_AXL0].l;
  case DSP_REG_AXH0:
  case DSP_REG_AXH1:
    return dsp.r.ax[index - DSP_REG_AXH0].h;
  case DSP_REG_ACL0:
  case DSP_REG_ACL1:
    return dsp.r.ac[index - DSP_REG_ACL0].l;
  default:
  {
    // $ac0.m / $ac1.m. In 40-bit mode a value that does not fit in 32 bits
    // reads as the largest 16-bit value of its sign.
    const int acc = index - DSP_REG_ACM0;
    if (IsSRFlagSet(SR_40_MODE_BIT))
    {
      const s64 full = Acc(acc);
      if (ExceedsS32(full))
        return full > 0 ? u16{0x7fff} : u16{0x8000};
    }
    return dsp.r.ac[acc].m;
  }
  }
}

void Interpreter::OpWriteRegister(int reg, u16 val)
{
  SDSP& dsp = Regs();
  const int index = reg & 0x1f;

  if (index < 0x10)
  {
    const int n = index & 3;
    switch (index >> 2)
    {
    case 0:
      dsp.r.ar[n] = val;
      break;
    case 1:
      dsp.r.ix[n] = val;
      break;
    case 2:
      dsp.r.wr[n] = val;
      break;
    default:
      // Writing a stack register pushes onto it.
      dsp.StoreStack(static_cast<StackRegister>(n), val);
      break;
    }
    return;
  }

  switch (index)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    // Only 8 bits exist; they are sign extended over the upper half of val.
    dsp.r.ac[index - DSP_REG_ACH0].h = static_cast<u32>(static_cast<s32>(static_cast<s8>(val)));
    break;
  case DSP_REG_CR:
    dsp.r.cr = val & 0x00ff;
    break;
  case DSP_REG_SR:
    dsp.r.sr = val & static_cast<u16>(~SR_100);
    break;
  case DSP_REG_PRODL:
    dsp.r.prod.l = val;
    break;
  case DSP_REG_PRODM:
    dsp.r.prod.m = val;
    break;
  case DSP_REG_PRODH:
    // 8 bits as well, but not sign extended.
    dsp.r.prod.h = val & 0x00ff;
    break;
  case DSP_REG_PRODM2:
    dsp.r.prod.m2 = val;
    break;
  case DSP_REG_AXL0:
  case DSP_REG_AXL1:
    dsp.r.ax[index - DSP_REG_AXL0].l = val;
    break;
  case DSP_REG_AXH0:
  case DSP_REG_AXH1:
    dsp.r.ax[index - DSP_REG_AXH0].h = val;
    break;
  case DSP_REG_ACL0:
  case DSP_REG_ACL1:
    dsp.r.ac[index - DSP_REG_ACL0].l = val;
    break;
  default:
    dsp.r.ac[index - DSP_REG_ACM0].m = val;
    break;
  }
}

void Interpreter::ExtendIfMidAccumulator(int reg)
{
  if (reg != DSP_REG_ACM0 && reg != DSP_REG_ACM1)
    return;
  if (!IsSRFlagSet(SR_40_MODE_BIT))
    return;

  // In 40-bit mode a load into $acX.m loads the whole accumulator: the value
  // becomes the middle word, sign extended upwards, with a zero low word.
  auto& acc = Regs().r.ac[reg - DSP_REG_ACM0];
  acc.h = (acc.m & 0x8000) != 0 ? 0xffffffffU : 0U;
  acc.l = 0;
}

void Interpreter::LoadRegister(int reg, u16 value)
{
  OpWriteRegister(reg, value);
  ExtendIfMidAccumulator(reg);
}

// ---------------------------------------------------------------------------
// Extension write queue
// ---------------------------------------------------------------------------

void Interpreter::WriteToBackLog(int reg, u16 value)
{
  if (m_deferred_count < m_deferred.size())
    m_deferred[m_deferred_count++] = {reg, value};
}

void Interpreter::ApplyWriteBackLog()
{
  for (size_t i = 0; i < m_deferred_count; ++i)
  {
    const DeferredWrite& write = m_deferred[i];
    u16 value = write.value;
    if constexpr (OR_OVERLAPPING_EXTENSION_WRITES)
      value |= OpReadRegister(write.reg);
    OpWriteRegister(write.reg, value);
  }
  m_deferred_count = 0;
}

// Main handlers call these once they have read all their inputs and before they
// write anything. They only matter when overlapping writes are ORed.
void Interpreter::ZeroWriteBackLog()
{
  if constexpr (OR_OVERLAPPING_EXTENSION_WRITES)
  {
    for (size_t i = 0; i < m_deferred_count; ++i)
      OpWriteRegister(m_deferred[i].reg, 0);
  }
}

// The same, but leaves the parts of one accumulator alone (for instructions that
// change only $acX.m and read the rest of it afterwards).
void Interpreter::ZeroWriteBackLogPreserveAcc([[maybe_unused]] int acc)
{
  if constexpr (OR_OVERLAPPING_EXTENSION_WRITES)
  {
    for (size_t i = 0; i < m_deferred_count; ++i)
    {
      if (!IsAccumulatorPart(m_deferred[i].reg, acc))
        OpWriteRegister(m_deferred[i].reg, 0);
    }
  }
}

// ---------------------------------------------------------------------------

void Interpreter::nop(const UDSPInstruction opc)
{
  // Only 0x0000 is the real NOP; other words land here when nothing decodes them.
  if (opc != 0)
    ERROR_LOG_FMT(DSPLLE, "LLE: Unrecognized opcode {:#06x}", opc);
}
}  // namespace DSP::Interpreter
