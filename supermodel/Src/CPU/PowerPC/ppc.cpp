// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// ppc.cpp: PowerPC 603e interpreter for the Model 3's main CPU.
//
// The core is split over four files that are built as one unit (only this
// file is compiled; it includes the others):
//
//   ppc.cpp     CPU state, special purpose registers, setup, save states and
//               the register access functions declared in ppc.h
//   ppc_ops.h   instruction field decoding and the condition/flag helpers
//   ppc603.c    exceptions, time base and decrementer, the run loop
//   ppc_ops.c   the instruction set (integer, branch, load/store, FPU)
//
// Timing model: every instruction takes one CPU cycle. The time base and the
// decrementer count once every "timer ratio" CPU cycles (the 603e counts them
// every 4 bus clocks; the Model 3 code adjusts the ratio every frame). The
// count is not kept up to date instruction by instruction: each run slice
// remembers where it started and the registers are brought up to date when
// they are read or written and when the slice ends. A decrementer interrupt
// is scheduled for the cycle on which the counter passes through zero.
//
// No MMU, caches or little-endian mode are emulated; the Model 3 software
// does not need them. Instructions and conditions the core cannot handle
// (invalid instruction words, unknown SPRs, little-endian mode, a jump
// outside every fetch region) stop the CPU until the next reset.

#include "ppc.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include "Supermodel.h"
#include "CPU/Bus.h"

// The FPU instructions change the host rounding mode around each operation
// when the guest selects a mode other than round-to-nearest, so the compiler
// must not assume the default mode or fold operations across those calls.
#if defined(_MSC_VER)
#pragma float_control(precise, on)
#pragma fenv_access(on)
#elif defined(__clang__)
#pragma STDC FENV_ACCESS ON
#elif defined(__GNUC__)
#pragma GCC optimize ("trapping-math,rounding-math")
#else
#pragma STDC FENV_ACCESS ON
#endif

namespace {

// Machine state register bits used by the core.
constexpr UINT32 kMsrPow = 0x00040000;
constexpr UINT32 kMsrIle = 0x00010000;
constexpr UINT32 kMsrEe  = 0x00008000;
constexpr UINT32 kMsrPr  = 0x00004000;
constexpr UINT32 kMsrFp  = 0x00002000;
constexpr UINT32 kMsrFe0 = 0x00000800;
constexpr UINT32 kMsrSe  = 0x00000400;
constexpr UINT32 kMsrBe  = 0x00000200;
constexpr UINT32 kMsrFe1 = 0x00000100;
constexpr UINT32 kMsrIp  = 0x00000040;
constexpr UINT32 kMsrIr  = 0x00000020;
constexpr UINT32 kMsrDr  = 0x00000010;
constexpr UINT32 kMsrRi  = 0x00000002;
constexpr UINT32 kMsrLe  = 0x00000001;

// Fixed-point exception register bits.
constexpr UINT32 kXerSo = 0x80000000;
constexpr UINT32 kXerOv = 0x40000000;
constexpr UINT32 kXerCa = 0x20000000;

// Pending interrupt sources (bit mask).
constexpr int kPendExternal    = 1;
constexpr int kPendDecrementer = 2;
constexpr int kPendSysMgmt     = 4;

// One block address translation register pair (upper word first, as saved).
struct BatPair
{
  UINT32 upper;
  UINT32 lower;
};

struct CpuState
{
  bool    stopped;          // hit something it cannot run: wait for reset

  // User registers
  UINT32  gpr[32];
  UINT64  fpr[32];          // raw IEEE double bits
  UINT8   cr[8];            // condition register, one 4-bit field per entry
  UINT32  lr;
  UINT32  ctr;
  UINT32  xer;
  UINT32  fpscr;

  // Program counter: pc is the instruction being executed, nextPc the one
  // after it (what an interrupt returns to). fetch points at nextPc's word.
  UINT32  pc;
  UINT32  nextPc;
  UINT32  *fetch;

  // Supervisor registers
  UINT32  msr;
  UINT32  pvr;
  UINT32  srr0, srr1, srr2, srr3;
  UINT32  hid0, hid1, hid2;
  UINT32  sdr1;
  UINT32  sprg[4];
  UINT32  dsisr, dar, ear;
  UINT32  dmiss, dcmp, hash1, hash2, imiss, icmp, rpa;   // 603 TLB miss registers
  BatPair ibat[4];
  BatPair dbat[4];
  UINT32  iabr;
  UINT32  sr[16];

  // Words of the save-state layout that belong to registers of other
  // PowerPC families (4xx timers, bank and DMA registers). The 603e has none
  // of them; they are only carried through save states.
  UINT32  foreignA[14];
  UINT32  foreignB[10];
  int     foreignIrq;

  // lwarx/stwcx. reservation
  int     reserved;
  UINT32  reservedAddr;

  // Interrupts
  int     pending;

  // Time base and decrementer (see the timing note at the top)
  UINT64  tb;
  UINT32  dec;
  UINT32  timerCarry;       // CPU cycles towards the next tick, between slices
  int     timerRatio;       // CPU cycles per tick
  int     tbMark;           // value of 'left' the time base was last synced to
  int     decMark;          // same for the decrementer
  int     decFireAt;        // 'left' value at which the decrementer passes zero

  // Cycle accounting
  UINT64  cyclesDone;       // cycles of the slices that have ended
  int     left;             // cycles left in the running slice
  int     sliceLength;      // length of the running slice
  int     busMultiplier;    // core:bus ratio x2
  int     clockHz;

  // Instruction fetch
  PPC_FETCH_REGION  lastRegion;
  PPC_FETCH_REGION  *regions;
};

CpuState cpu;
IBus *bus = nullptr;

// PLL_CFG settings for each core:bus ratio (rows: ratio x2 = 1..12) and bus
// clock (columns: 16, 20, 25, 33, 40, 50, 60, 66, 75 MHz), from the 603e,
// 603ev and 603r hardware specifications. '.' marks a combination the part
// does not support, which reads back as all ones.
const char *const kPll603e[12] = {
  ".........", "222111.0.", ".....c.c.", "555444...",
  "...66....", "..88.....", ".ee......", "aaa......",
  ".........", ".........", ".........", "........."
};
const char *const kPll603ev[12] = {
  ".........", ".........", ".........", ".......44",
  ".....6666", "....88888", "....eeee.", "...aaaa..",
  "...777...", "..bbb....", "..999....", "..ddd...."
};
const char *const kPll603r[12] = {
  ".........", ".........", ".........", "....55555",
  "......666", ".....8888", ".....eeee", "....aaaaa",
  "...77777.", "...bbbb..", "...999...", "..dddd..."
};

int PllSetting(const char *const table[12], int ratioX2, unsigned busIndex)
{
  if (ratioX2 < 1 || ratioX2 > 12 || busIndex >= 9)
    return -1;
  char c = table[ratioX2 - 1][busIndex];
  if (c == '.')
    return -1;
  return (c <= '9') ? (c - '0') : (c - 'a' + 10);
}

const int kBusHz[9] = { 16000000, 20000000, 25000000, 33000000, 40000000,
                        50000000, 60000000, 66000000, 75000000 };

void Stop(const char *why)
{
  ErrorLog("PowerPC: %s. Halting emulation until reset.", why);
  cpu.stopped = true;
}

// Points the fetch pointer at an address. The last region used is tried
// first, then the region list in order.
void FetchFrom(UINT32 addr)
{
  UINT32 offset = addr - cpu.lastRegion.start;
  if (offset <= cpu.lastRegion.end - cpu.lastRegion.start)
  {
    cpu.fetch = cpu.lastRegion.ptr + offset / 4;
    return;
  }
  for (PPC_FETCH_REGION *r = cpu.regions; r->ptr != NULL; r++)
  {
    offset = addr - r->start;
    if (offset <= r->end - r->start)
    {
      cpu.lastRegion = *r;
      cpu.fetch = r->ptr + offset / 4;
      return;
    }
  }
  DebugLog("PowerPC: no code at %08X (jumping from %08X)\n", addr, cpu.pc);
  Stop("program counter left the mapped code");
}

inline double FprValue(unsigned n)
{
  double d;
  memcpy(&d, &cpu.fpr[n], sizeof(d));
  return d;
}

inline void SetFprValue(unsigned n, double d)
{
  memcpy(&cpu.fpr[n], &d, sizeof(d));
}

// Defined in ppc603.c
void PollInterrupts();
UINT64 ReadTimeBase();
void WriteTimeBaseLow(UINT32 value);
void WriteTimeBaseHigh(UINT32 value);
UINT32 ReadDecrementer();
void WriteDecrementer(UINT32 value);

void SetMsr(UINT32 value)
{
  if (value & (kMsrIle | kMsrLe))
  {
    DebugLog("PowerPC: MSR %08X selects little-endian mode\n", value);
    Stop("little-endian mode is not emulated");
  }
  cpu.msr = value;
  PollInterrupts();
}

UINT32 PackedCr()
{
  UINT32 v = 0;
  for (int i = 0; i < 8; i++)
    v = (v << 4) | cpu.cr[i];
  return v;
}

// Special purpose registers by number.
enum : unsigned
{
  kSprXer = 1, kSprLr = 8, kSprCtr = 9, kSprDsisr = 18, kSprDar = 19,
  kSprDec = 22, kSprSdr1 = 25, kSprSrr0 = 26, kSprSrr1 = 27,
  kSprTbl = 268, kSprTbu = 269, kSprSprg0 = 272, kSprEar = 282,
  kSprTblWrite = 284, kSprTbuWrite = 285, kSprPvr = 287,
  kSprBatFirst = 528, kSprBatLast = 543,
  kSprDmiss = 976, kSprDcmp, kSprHash1, kSprHash2, kSprImiss, kSprIcmp, kSprRpa,
  kSprHid0 = 1008, kSprHid1 = 1009, kSprIabr = 1010, kSprHid2 = 1011
};

// The register an SPR number names, for the plain storage registers (the
// ones with no side effect on read or write). Null if none.
UINT32 *PlainSpr(unsigned spr)
{
  if (spr >= kSprBatFirst && spr <= kSprBatLast)
  {
    unsigned i = spr - kSprBatFirst;
    BatPair &b = (i < 8) ? cpu.ibat[(i & 7) >> 1] : cpu.dbat[(i & 7) >> 1];
    return (i & 1) ? &b.lower : &b.upper;
  }
  if (spr >= kSprSprg0 && spr < kSprSprg0 + 4)
    return &cpu.sprg[spr - kSprSprg0];
  switch (spr)
  {
  case kSprXer:   return &cpu.xer;
  case kSprLr:    return &cpu.lr;
  case kSprCtr:   return &cpu.ctr;
  case kSprDsisr: return &cpu.dsisr;
  case kSprDar:   return &cpu.dar;
  case kSprSdr1:  return &cpu.sdr1;
  case kSprSrr0:  return &cpu.srr0;
  case kSprSrr1:  return &cpu.srr1;
  case kSprEar:   return &cpu.ear;
  case kSprDmiss: return &cpu.dmiss;
  case kSprDcmp:  return &cpu.dcmp;
  case kSprHash1: return &cpu.hash1;
  case kSprHash2: return &cpu.hash2;
  case kSprImiss: return &cpu.imiss;
  case kSprIcmp:  return &cpu.icmp;
  case kSprRpa:   return &cpu.rpa;
  case kSprHid0:  return &cpu.hid0;
  case kSprHid1:  return &cpu.hid1;
  case kSprHid2:  return &cpu.hid2;
  default:        return nullptr;
  }
}

UINT32 ReadSpr(unsigned spr)
{
  if (UINT32 *reg = PlainSpr(spr))
    return *reg;
  switch (spr)
  {
  case kSprPvr:      return cpu.pvr;
  case kSprDec:      return ReadDecrementer();
  case kSprTblWrite: return (UINT32) ReadTimeBase();
  case kSprTbuWrite: return (UINT32) (ReadTimeBase() >> 32);
  default:
    // Includes 268/269: the time base is read with mftb on the 603e.
    DebugLog("PowerPC: mfspr %u at %08X\n", spr, cpu.pc);
    Stop("read from an unknown special purpose register");
    return 0;
  }
}

void WriteSpr(unsigned spr, UINT32 value)
{
  if (UINT32 *reg = PlainSpr(spr))
  {
    *reg = value;
    return;
  }
  switch (spr)
  {
  case kSprPvr:                   // read only
    return;
  case kSprIabr:
    cpu.iabr = value;
    return;
  case kSprDec:
    WriteDecrementer(value);
    return;
  case kSprTbl:
  case kSprTblWrite:
    WriteTimeBaseLow(value);
    return;
  case kSprTbu:
  case kSprTbuWrite:
    WriteTimeBaseHigh(value);
    return;
  default:
    DebugLog("PowerPC: mtspr %u at %08X\n", spr, cpu.pc);
    Stop("wrote to an unknown special purpose register");
    return;
  }
}

} // anonymous namespace

#include "ppc_ops.h"
#include "ppc603.c"
#include "ppc_ops.c"

/******************************************************************************
 Setup
******************************************************************************/

void ppc_init(const PPC_CONFIG *config)
{
  cpu = CpuState();

  cpu.pvr = config->pvr;

  // Core:bus ratio from its two digits (0x25 = 2.5), kept as ratio x2.
  float ratio = (float) ((config->bus_frequency_multiplier >> 4) & 0xF) +
                (float) (config->bus_frequency_multiplier & 0xF) / 10.0f;
  cpu.busMultiplier = (int) (ratio * 2);

  // The time base and decrementer tick every 4 bus clocks.
  cpu.timerRatio = 2 * cpu.busMultiplier;

  unsigned busIndex = (unsigned) config->bus_frequency;
  if (busIndex < 9)
    cpu.clockHz = (int) (ratio * (float) kBusHz[busIndex]);

  // HID1 reports the PLL configuration the part was strapped with.
  int pll = 0;
  switch (config->pvr)
  {
  case PPC_MODEL_603E:  pll = PllSetting(kPll603e, cpu.busMultiplier, busIndex); break;
  case PPC_MODEL_603EV: pll = PllSetting(kPll603ev, cpu.busMultiplier, busIndex); break;
  case PPC_MODEL_603R:  pll = PllSetting(kPll603r, cpu.busMultiplier, busIndex); break;
  default:              break;
  }
  cpu.hid1 = (UINT32) pll << 28;
}

void ppc_attach_bus(IBus *BusPtr)
{
  bus = BusPtr;
}

void ppc_set_fetch(PPC_FETCH_REGION *fetch)
{
  cpu.regions = fetch;
}

void ppc_reset(void)
{
  cpu.stopped = false;
  cpu.pc = 0xFFF00100;            // system reset vector, MSR[IP] set
  cpu.nextPc = cpu.pc;
  SetMsr(kMsrIp);
  FetchFrom(cpu.pc);
  cpu.hid0 = 1;
  cpu.pending = 0;
  cpu.tb = 0;
  cpu.timerCarry = 0;
  cpu.dec = 0xFFFFFFFF;
  cpu.cyclesDone = 0;
  cpu.sliceLength = 0;
  cpu.left = 0;
}

void ppc_shutdown(void)
{
}

/******************************************************************************
 Running
******************************************************************************/

void ppc_set_irq_line(int irqline)
{
  if (irqline)
  {
    cpu.pending |= kPendExternal;
    PollInterrupts();
  }
  else
    cpu.pending &= ~kPendExternal;
}

UINT64 ppc_total_cycles(void)
{
  return cpu.cyclesDone + (UINT64) (cpu.sliceLength - cpu.left);
}

int ppc_get_cycles_per_sec(void)
{
  return cpu.clockHz;
}

int ppc_get_bus_freq_multipler(void)
{
  return cpu.busMultiplier;
}

int ppc_get_timer_ratio(void)
{
  return cpu.timerRatio;
}

void ppc_set_timer_ratio(int ratio)
{
  cpu.timerRatio = ratio;
}

/******************************************************************************
 Save states

 Block "PowerPC". The layout is fixed (older save states must still load):
 slice counters, the registers, then words for registers the 603e does not
 have, the reservation, the time base and decrementer, the FPU and the
 segment registers. All values are in host byte order.
******************************************************************************/

namespace {

// Calls io(pointer, size) for each field of the block in order. The fetch
// pointer is re-derived as soon as the next PC is known, before the rest of
// the block, as earlier versions of the core did on loading.
template <class Io, class AfterPc>
void TransferState(Io io, AfterPc afterPc)
{
  io(&cpu.left, sizeof(cpu.left));
  io(&cpu.sliceLength, sizeof(cpu.sliceLength));
  io(&cpu.cyclesDone, sizeof(cpu.cyclesDone));
  io(cpu.gpr, sizeof(cpu.gpr));
  io(&cpu.pc, sizeof(cpu.pc));
  io(&cpu.nextPc, sizeof(cpu.nextPc));
  afterPc();
  for (UINT32 *w : { &cpu.lr, &cpu.ctr, &cpu.xer, &cpu.msr })
    io(w, sizeof(*w));
  io(cpu.cr, sizeof(cpu.cr));
  for (UINT32 *w : { &cpu.pvr, &cpu.srr0, &cpu.srr1, &cpu.srr2, &cpu.srr3,
                     &cpu.hid0, &cpu.hid1, &cpu.hid2, &cpu.sdr1 })
    io(w, sizeof(*w));
  io(cpu.sprg, sizeof(cpu.sprg));
  for (UINT32 *w : { &cpu.dsisr, &cpu.dar, &cpu.ear, &cpu.dmiss, &cpu.dcmp,
                     &cpu.hash1, &cpu.hash2, &cpu.imiss, &cpu.icmp, &cpu.rpa })
    io(w, sizeof(*w));
  io(cpu.ibat, sizeof(cpu.ibat));
  io(cpu.dbat, sizeof(cpu.dbat));
  io(cpu.foreignA, sizeof(cpu.foreignA));
  io(&cpu.iabr, sizeof(cpu.iabr));
  io(cpu.foreignB, sizeof(cpu.foreignB));
  io(&cpu.reserved, sizeof(cpu.reserved));
  io(&cpu.reservedAddr, sizeof(cpu.reservedAddr));
  io(&cpu.foreignIrq, sizeof(cpu.foreignIrq));
  io(&cpu.tb, sizeof(cpu.tb));
  io(&cpu.dec, sizeof(cpu.dec));
  io(&cpu.timerCarry, sizeof(cpu.timerCarry));
  io(&cpu.fpscr, sizeof(cpu.fpscr));
  io(cpu.fpr, sizeof(cpu.fpr));
  io(cpu.sr, sizeof(cpu.sr));
}

} // anonymous namespace

void ppc_save_state(CBlockFile *SaveState)
{
  SaveState->NewBlock("PowerPC", __FILE__);
  TransferState([SaveState](void *p, size_t n) { SaveState->Write(p, (uint32_t) n); },
                [] {});
}

void ppc_load_state(CBlockFile *SaveState)
{
  if (Result::OKAY != SaveState->FindBlock("PowerPC"))
  {
    ErrorLog("Unable to load PowerPC state. Save state file is corrupt.");
    return;
  }
  TransferState([SaveState](void *p, size_t n) { SaveState->Read(p, (uint32_t) n); },
                [] { FetchFrom(cpu.nextPc); });
}

/******************************************************************************
 Register access
******************************************************************************/

UINT32 ppc_get_pc(void)
{
  return cpu.pc;
}

void ppc_set_pc(UINT32 pc)
{
  cpu.pc = pc;
  FetchFrom(pc);
  cpu.nextPc = pc + 4;
}

UINT32 ppc_get_gpr(unsigned num)
{
  return cpu.gpr[num & 31];
}

void ppc_set_gpr(unsigned num, UINT32 val)
{
  cpu.gpr[num & 31] = val;
}

double ppc_get_fpr(unsigned num)
{
  return FprValue(num & 31);
}

void ppc_set_fpr(unsigned num, double val)
{
  SetFprValue(num & 31, val);
}

UINT8 ppc_get_cr(unsigned num)
{
  return cpu.cr[num & 7];
}

void ppc_set_cr(unsigned num, UINT8 val)
{
  cpu.cr[num & 7] = val;
}

UINT32 ppc_get_lr(void)
{
  return cpu.lr;
}

UINT32 ppc_read_spr(unsigned spr)
{
  return ReadSpr(spr);
}

void ppc_write_spr(unsigned spr, UINT32 val)
{
  WriteSpr(spr, val);
}

UINT32 ppc_read_sr(unsigned num)
{
  return cpu.sr[num & 15];
}

void ppc_write_sr(unsigned num, UINT32 val)
{
  cpu.sr[num & 15] = val;
}

UINT32 ppc_read_msr(void)
{
  return cpu.msr;
}

void ppc_break(void)
{
}
