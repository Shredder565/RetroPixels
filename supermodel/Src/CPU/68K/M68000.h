// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// RetroPixels' Motorola 68000 interpreter in C++. It behaves like
// RetroPixels' Go 68000 (cores/m68k): the same instructions, flags,
// exceptions and cycle counts. Time is counted the way the chip spends it:
// every bus access is 4 clocks and each instruction adds its internal
// clocks on top.
//
// Not emulated: address errors (word and long accesses are forced to even
// addresses), the trace exception, bus errors and the 68010+ extensions.
//
// The processor's whole state is the plain struct M68000State, so a board
// that owns a 68000 can park it and load another one (see 68K.h).

#ifndef INCLUDED_M68000_H
#define INCLUDED_M68000_H

#include <cstdint>
#include "CPU/Bus.h"

// Interrupt acknowledge: the vector number to use (0-255), or one of these.
constexpr int kM68KAutoVector = -1;   // 24 + level
constexpr int kM68KSpurious = -2;     // vector 24

struct M68000State
{
  uint32_t d[8] = {};
  uint32_t a[8] = {};          // a[7] is the active stack pointer
  uint32_t pc = 0;
  uint32_t usp = 0, ssp = 0;   // the inactive stack pointer is kept here

  // Status register, one field each (0 or 1, ipl 0-7).
  uint32_t s = 1, t = 0, ipl = 7;
  uint32_t x = 0, n = 0, z = 0, v = 0, c = 0;

  int64_t cycles = 0;          // clocks run so far
  bool stopped = false;        // STOP is waiting for an interrupt
  int irq = 0;                 // interrupt level requested (0 = none)
  bool nmiSeen = false;        // level 7 is taken once per rising edge

  uint32_t op = 0;             // the instruction being (or last) executed
  uint32_t opPC = 0;           // where it started
};

class M68000
{
public:
  M68000State st;

  // Where memory accesses go, and who answers interrupt acknowledges (null:
  // autovectored). The RESET instruction calls onReset if set.
  IBus *bus = nullptr;
  int (*ack)(void *user, int level) = nullptr;
  void (*onReset)(void *user) = nullptr;
  void *user = nullptr;

  M68000();

  uint32_t SR() const;
  void SetSR(uint32_t v);          // switches stacks as S says
  void SetIRQ(int level);

  // Reset: supervisor mode, interrupts masked, A7 and PC from vectors 0, 1.
  void Reset();

  // One instruction (or interrupt, or 4 idle clocks while stopped); returns
  // the clocks it took.
  int Step();

  // Runs for at least `clocks` clocks (finishing the last instruction) and
  // returns how many were run. A stopped CPU with nothing to wake it uses
  // up the time at once.
  int64_t Run(int64_t clocks);

private:
  // Bus, 4 clocks per access.
  uint32_t R8(uint32_t a);
  uint32_t R16(uint32_t a);
  uint32_t R32(uint32_t a);
  void W8(uint32_t a, uint32_t v);
  void W16(uint32_t a, uint32_t v);
  void W32(uint32_t a, uint32_t v);
  uint32_t ReadSized(uint32_t a, int sz);
  void WriteSized(uint32_t a, int sz, uint32_t v);
  uint32_t Fetch16();
  uint32_t Fetch32();
  void Push16(uint32_t v);
  void Push32(uint32_t v);
  uint32_t Pop16();
  uint32_t Pop32();

  // Effective addresses (mode/reg as in the opcode; sz 1, 2 or 4).
  uint32_t Indexed(uint32_t base);
  uint32_t Address(int mode, int reg, int sz);
  uint32_t ReadEA(int mode, int reg, int sz);
  void SetDn(int r, uint32_t v, int sz);
  uint32_t RmwRead(int mode, int reg, int sz, uint32_t *addr);
  void RmwWrite(int mode, int reg, int sz, uint32_t addr, uint32_t v);

  // Flags.
  uint32_t CCR() const;
  void SetCCR(uint32_t v);
  void SetNZ(uint32_t v, int sz);
  void SetLogic(uint32_t v, int sz);
  uint32_t Add(uint32_t a, uint32_t b, int sz, uint32_t carry);
  uint32_t Sub(uint32_t a, uint32_t b, int sz, uint32_t borrow);
  bool Cond(uint32_t cc) const;
  uint32_t DecimalAdd(uint32_t d, uint32_t s);
  uint32_t DecimalSub(uint32_t d, uint32_t s);
  uint32_t Shift(int kind, bool left, uint32_t v, uint32_t count, int sz);
  void Divide(int rx, uint32_t src, bool isSigned);

  // Exceptions.
  void Exception(uint32_t vector, uint32_t pc);
  void Trap(uint32_t vector, int extra);
  void PrivilegeViolation();
  void Illegal(uint32_t vector);
  bool TakeInterrupt();

  void Execute(uint32_t op);

  // Instruction groups.
  void Group0(uint32_t op);
  void Move(uint32_t op);
  void Group4(uint32_t op);
  void Group5(uint32_t op);
  void Branch(uint32_t op);
  void OrDivSbcd(uint32_t op);
  void AddSub(uint32_t op, bool isAdd);
  void CmpEor(uint32_t op);
  void AndMulAbcdExg(uint32_t op);
  void ShiftRotate(uint32_t op);
  void LogicOp(uint32_t op, bool isOr);
  void Bcd(uint32_t op, bool isAdd);
  void MovemToMemory(int mode, int reg, int sz);
  void MovemToRegisters(int mode, int reg, int sz);
  void Unary(int mode, int reg, int sz, int kind);
};

#endif  // INCLUDED_M68000_H
