// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// ppc603.c: 603e exceptions, the time base and decrementer, and the run
// loop. Included by ppc.cpp only (it is C++ despite the name, which the
// build lists keep).

namespace {

// Exception vector offsets (added to 0xFFF00000 when MSR[IP] is set).
constexpr UINT32 kVecDataStorage   = 0x0300;
constexpr UINT32 kVecInstStorage   = 0x0400;
constexpr UINT32 kVecExternal      = 0x0500;
constexpr UINT32 kVecProgram       = 0x0700;
constexpr UINT32 kVecDecrementer   = 0x0900;
constexpr UINT32 kVecSystemCall    = 0x0C00;
constexpr UINT32 kVecSystemMgmt    = 0x1400;

constexpr UINT32 kSrr1Trap = 0x00020000;    // program exception cause: trap

// Enters an exception handler: SRR0 gets the return address, SRR1 the
// saved MSR bits, and the MSR switches to supervisor mode with interrupts,
// the FPU, tracing and translation off. MSR[LE] follows MSR[ILE].
void EnterHandler(UINT32 vector, UINT32 returnAddr, UINT32 srr1Cause)
{
  UINT32 msr = cpu.msr;
  cpu.srr0 = returnAddr;
  cpu.srr1 = (msr & 0xFF73) | srr1Cause;
  msr &= ~(kMsrPow | kMsrEe | kMsrPr | kMsrFp | kMsrFe0 | kMsrSe | kMsrBe |
           kMsrFe1 | kMsrIr | kMsrDr | kMsrRi);
  msr = (msr & kMsrIle) ? (msr | kMsrLe) : (msr & ~kMsrLe);
  SetMsr(msr);
  cpu.nextPc = ((msr & kMsrIp) ? 0xFFF00000 : 0) | vector;
}

// Asynchronous interrupts: taken only while MSR[EE] is set, returning to
// the next instruction. The external line stays pending until the board
// drops it; the decrementer and SMI requests are consumed.
void TakeAsync(UINT32 vector, int consumes)
{
  if (!(cpu.msr & kMsrEe))
    return;
  EnterHandler(vector, cpu.nextPc, 0);
  cpu.pending &= ~consumes;
  FetchFrom(cpu.nextPc);
}

void TakeTrap()
{
  EnterHandler(kVecProgram, cpu.pc, kSrr1Trap);
  FetchFrom(cpu.nextPc);
}

void TakeSystemCall()
{
  EnterHandler(kVecSystemCall, cpu.nextPc, 0);
  FetchFrom(cpu.nextPc);
}

// Storage exceptions are never raised (no MMU) but are kept for
// completeness.
[[maybe_unused]] void TakeStorageFault(bool instruction)
{
  EnterHandler(instruction ? kVecInstStorage : kVecDataStorage, cpu.nextPc, 0);
  cpu.pending &= ~kPendSysMgmt;
  FetchFrom(cpu.nextPc);
}

// Takes the highest priority pending interrupt if interrupts are enabled.
void PollInterrupts()
{
  if (!(cpu.msr & kMsrEe) || cpu.pending == 0)
    return;
  if (cpu.pending & kPendExternal)
    TakeAsync(kVecExternal, 0);
  else if (cpu.pending & kPendDecrementer)
    TakeAsync(kVecDecrementer, kPendDecrementer);
  else if (cpu.pending & kPendSysMgmt)
    TakeAsync(kVecSystemMgmt, kPendSysMgmt);
}

// Ticks since a mark, in whole timer periods.
inline int TicksSince(int mark)
{
  return (mark - cpu.left) / cpu.timerRatio;
}

// Moves a mark to the current cycle, keeping the part of a period that has
// already gone by.
inline int Resync(int mark)
{
  return cpu.left + (mark - cpu.left) % cpu.timerRatio;
}

UINT64 ReadTimeBase()
{
  return cpu.tb + TicksSince(cpu.tbMark);
}

// Writing TBL replaces the whole 64-bit time base with the 32-bit value
// (TBU reads back as zero afterwards); the core has always behaved like this
// and software running on it may depend on it.
void WriteTimeBaseLow(UINT32 value)
{
  cpu.tbMark = Resync(cpu.tbMark);
  cpu.tb = value;
}

void WriteTimeBaseHigh(UINT32 value)
{
  UINT64 now = ReadTimeBase();
  cpu.tbMark = Resync(cpu.tbMark);
  cpu.tb = (now & 0xFFFFFFFF) | ((UINT64) value << 32);
}

UINT32 ReadDecrementer()
{
  return cpu.dec - TicksSince(cpu.decMark);
}

// Works out on which cycle of the slice the decrementer passes through zero
// (or that it does not in this slice).
void ScheduleDecrementer()
{
  if ((UINT32) (cpu.decMark / cpu.timerRatio) > cpu.dec)
    cpu.decFireAt = (int) ((UINT32) cpu.decMark - (1 + cpu.dec) * (UINT32) cpu.timerRatio);
  else
    cpu.decFireAt = 0x7FFFFFFF;
}

void WriteDecrementer(UINT32 value)
{
  // Writing a negative value to a non-negative decrementer requests the
  // interrupt straight away, as the counter's sign bit going from 0 to 1
  // does on the 603e.
  if ((value & 0x80000000) && !(ReadDecrementer() & 0x80000000))
  {
    cpu.pending |= kPendDecrementer;
    PollInterrupts();
  }
  cpu.decMark = Resync(cpu.decMark);
  cpu.dec = value;
  ScheduleDecrementer();
}

void ExecuteOne(UINT32 op);    // ppc_ops.c

} // anonymous namespace

int ppc_execute(int cycles)
{
  cpu.sliceLength = cycles;
  cpu.left = cycles;
  cpu.tbMark = cycles + cpu.timerCarry;
  cpu.decMark = cycles + cpu.timerCarry;
  ScheduleDecrementer();

  FetchFrom(cpu.nextPc);
  PollInterrupts();

  while (cpu.left > 0 && !cpu.stopped)
  {
    cpu.pc = cpu.nextPc;
    UINT32 op = *cpu.fetch++;     // code words are stored in host order
    cpu.nextPc = cpu.pc + 4;
    ExecuteOne(op);
    cpu.left--;
    if (cpu.left == cpu.decFireAt)
    {
      cpu.pending |= kPendDecrementer;
      PollInterrupts();
    }
  }

  // Bring the timers up to date and close the slice.
  cpu.timerCarry = (cpu.tbMark - cpu.left) % cpu.timerRatio;
  cpu.tb += TicksSince(cpu.tbMark);
  cpu.dec -= TicksSince(cpu.decMark);

  int ran = cycles - cpu.left;
  cpu.cyclesDone += ran;
  cpu.sliceLength = 0;
  cpu.left = 0;
  cpu.tbMark = 0;
  cpu.decMark = 0;
  return ran;
}
