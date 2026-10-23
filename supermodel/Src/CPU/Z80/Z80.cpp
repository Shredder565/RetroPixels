// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Z80.cpp: Zilog Z80 interpreter.
//
// Instructions are decoded from their bit fields (x = bits 7-6, y = 5-3,
// z = 2-0, p = bits 5-4, q = bit 3). Behaviour that the sound and drive
// board programs (and their save states) rely on, kept on purpose:
//
//  - Flags follow the documented results plus the undocumented X and Y
//    bits (bits 3 and 5), which come from the result in most cases, from
//    the operand for CP, and from the arithmetic the block instructions do.
//  - The R register is not advanced by instruction fetches; only LD R,A
//    changes it.
//  - Interrupts are checked after every instruction, EI included (no
//    one-instruction delay). HALT ends Run() with the PC after it.
//  - Prefixes that do not change the following instruction (DD/FD before
//    an instruction that does not use HL, an unknown ED in 40-7F) cost no
//    cycles; the following byte then runs as an ordinary instruction.
//  - Block repeat instructions (LDIR, CPIR, INIR, OTIR and the D forms)
//    complete in one step and are not charged extra cycles.
//  - With a count of zero, the repeats run the full 65536 (BC) or 256 (B)
//    iterations.

#include "Supermodel.h"
#include "CPU/Z80/Z80.h"

namespace
{

constexpr unsigned kFlagC = 0x01;
constexpr unsigned kFlagN = 0x02;
constexpr unsigned kFlagPV = 0x04;
constexpr unsigned kFlagX = 0x08;
constexpr unsigned kFlagH = 0x10;
constexpr unsigned kFlagY = 0x20;
constexpr unsigned kFlagZ = 0x40;
constexpr unsigned kFlagS = 0x80;
constexpr unsigned kFlagsSXY = kFlagS | kFlagY | kFlagX;
constexpr unsigned kFlagsXY = kFlagY | kFlagX;

inline unsigned ZeroFlag(unsigned v) { return (v & 0xFF) == 0 ? kFlagZ : 0; }

// Parity flag: set when the number of one bits is even.
inline unsigned ParityFlag(unsigned v)
{
  v &= 0xFF;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return (v & 1) ? 0 : kFlagPV;
}

// Overflow flag from the per-bit carry vector (operand ^ operand ^ result):
// the carry into bit 7 differs from the carry out of it.
inline unsigned OverflowFromCarries(unsigned carries)
{
  return (((carries >> 8) ^ (carries >> 7)) & 1) ? kFlagPV : 0;
}

// Cycles of an unprefixed instruction.
int BaseCost(unsigned op)
{
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
  switch (x)
  {
  case 0:
    switch (z)
    {
    case 0: return (y == 0 || y == 1) ? 4 : y == 2 ? 8 : y == 3 ? 12 : 7;
    case 1: return q ? 11 : 10;
    case 2: return p < 2 ? 7 : p == 2 ? 16 : 13;
    case 3: return 6;
    case 4: case 5: return y == 6 ? 11 : 4;
    case 6: return y == 6 ? 10 : 7;
    default: return 4;
    }
  case 1:
    return (op == 0x76) ? 4 : (y == 6 || z == 6) ? 7 : 4;
  case 2:
    return z == 6 ? 7 : 4;
  default:
    switch (z)
    {
    case 0: return 5;
    case 1:
      if (!q) return 10;
      return p == 0 ? 10 : p == 3 ? 6 : 4;
    case 2: return 10;
    case 3:
    {
      static const int kCost[8] = { 10, 0, 11, 11, 19, 4, 4, 4 };
      return kCost[y];
    }
    case 4: return 10;
    case 5:
      if (!q) return 11;
      return p == 0 ? 17 : 0;
    case 6: return 7;
    default: return 11;
    }
  }
}

// Cycles of a DD/FD-prefixed instruction that uses the index register.
int IndexedCost(unsigned op)
{
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, q = y & 1;
  if (x == 1 || x == 2)
    return (y == 6 && x == 1) || z == 6 ? 19 : 9;
  if (x == 3)
  {
    switch (op)
    {
    case 0xE1: return 14;
    case 0xE3: return 23;
    case 0xE5: return 15;
    case 0xE9: return 8;
    default:   return 10;   // 0xF9
    }
  }
  switch (z)
  {
  case 1: return q ? 15 : 14;
  case 2: return 20;
  case 3: return 10;
  case 4: case 5: return y == 6 ? 23 : 9;
  default: return y == 6 ? 19 : 9;   // z == 6
  }
}

// Whether a DD/FD prefix changes this instruction (it refers to H, L, HL
// or (HL), other than HALT and EX DE,HL).
bool UsesIndex(unsigned op)
{
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
  switch (x)
  {
  case 0:
    if (z == 1)
      return (op & 0x08) || y == 4;       // ADD HL,rr / LD HL,nn
    if (z == 2)
      return y == 4 || y == 5;            // LD (nn),HL / LD HL,(nn)
    if (z == 3)
      return y == 4 || y == 5;            // INC HL / DEC HL
    if (z >= 4 && z <= 6)
      return y >= 4 && y <= 6;            // H, L, (HL)
    return false;
  case 1:
    return op != 0x76 && ((y >= 4 && y <= 6) || (z >= 4 && z <= 6));
  case 2:
    return z >= 4 && z <= 6;
  default:
    return op == 0xE1 || op == 0xE3 || op == 0xE5 || op == 0xE9 || op == 0xF9;
  }
}

// Cycles of a CB-prefixed instruction (with or without an index prefix).
int BitOpCost(unsigned op, bool indexed)
{
  const unsigned x = op >> 6;
  const bool memory = (op & 7) == 6;
  if (indexed)
    return x == 1 ? 20 : memory ? 23 : 0;
  if (!memory)
    return 8;
  return x == 1 ? 12 : 15;
}

} // anonymous namespace

/******************************************************************************
 Register and bus access
******************************************************************************/

UINT16 &CZ80::IndexReg(Index idx)
{
  return idx == Index::IX ? m_ixReg : idx == Index::IY ? m_iyReg : HL();
}

// 8-bit register by its 3-bit code (B C D E H L - A); with an index prefix,
// H and L are the halves of IX or IY. Code 6 is never passed here.
UINT8 CZ80::GetReg8(unsigned r, Index idx)
{
  switch (r)
  {
  case 0: return (UINT8) (BC() >> 8);
  case 1: return (UINT8) BC();
  case 2: return (UINT8) (DE() >> 8);
  case 3: return (UINT8) DE();
  case 4: return (UINT8) (IndexReg(idx) >> 8);
  case 5: return (UINT8) IndexReg(idx);
  default: return A();
  }
}

void CZ80::SetReg8(unsigned r, Index idx, unsigned v)
{
  v &= 0xFF;
  switch (r)
  {
  case 0: BC() = (UINT16) ((BC() & 0x00FF) | (v << 8)); break;
  case 1: BC() = (UINT16) ((BC() & 0xFF00) | v); break;
  case 2: DE() = (UINT16) ((DE() & 0x00FF) | (v << 8)); break;
  case 3: DE() = (UINT16) ((DE() & 0xFF00) | v); break;
  case 4: { UINT16 &w = IndexReg(idx); w = (UINT16) ((w & 0x00FF) | (v << 8)); break; }
  case 5: { UINT16 &w = IndexReg(idx); w = (UINT16) ((w & 0xFF00) | v); break; }
  default: SetA(v); break;
  }
}

// Register pair by its 2-bit code (BC DE HL SP).
UINT16 CZ80::GetPair(unsigned p, Index idx)
{
  switch (p)
  {
  case 0: return BC();
  case 1: return DE();
  case 2: return IndexReg(idx);
  default: return m_spReg;
  }
}

void CZ80::SetPair(unsigned p, Index idx, unsigned v)
{
  switch (p)
  {
  case 0: BC() = (UINT16) v; break;
  case 1: DE() = (UINT16) v; break;
  case 2: IndexReg(idx) = (UINT16) v; break;
  default: m_spReg = (UINT16) v; break;
  }
}

UINT8 CZ80::Fetch8()
{
  return m_bus->Read8(m_pcReg++);
}

UINT16 CZ80::Fetch16()
{
  UINT16 v = Read16(m_pcReg);
  m_pcReg += 2;
  return v;
}

UINT16 CZ80::Read16(unsigned addr)
{
  unsigned lo = m_bus->Read8(addr & 0xFFFF);
  unsigned hi = m_bus->Read8((addr + 1) & 0xFFFF);
  return (UINT16) (lo | (hi << 8));
}

void CZ80::Write16(unsigned addr, unsigned v)
{
  m_bus->Write8(addr & 0xFFFF, (UINT8) v);
  m_bus->Write8((addr + 1) & 0xFFFF, (UINT8) (v >> 8));
}

void CZ80::Push(unsigned v)
{
  m_bus->Write8(--m_spReg, (UINT8) (v >> 8));
  m_bus->Write8(--m_spReg, (UINT8) v);
}

UINT16 CZ80::Pop()
{
  unsigned lo = m_bus->Read8(m_spReg++);
  unsigned hi = m_bus->Read8(m_spReg++);
  return (UINT16) (lo | (hi << 8));
}

bool CZ80::Condition(unsigned cc)
{
  static const UINT8 kFlag[4] = { kFlagZ, kFlagC, kFlagPV, kFlagS };
  bool set = (F() & kFlag[cc >> 1]) != 0;
  return (cc & 1) ? set : !set;
}

/******************************************************************************
 Arithmetic and flags
******************************************************************************/

// The eight accumulator operations: ADD ADC SUB SBC AND XOR OR CP.
void CZ80::Alu(unsigned op, unsigned operand)
{
  const unsigned a = A();
  const unsigned carryIn = F() & kFlagC;
  unsigned result;
  switch (op)
  {
  case 0: case 1: case 2: case 3: case 7:
  {
    const bool subtract = (op >= 2);
    const unsigned extra = (op == 1 || op == 3) ? carryIn : 0;
    result = subtract ? a - operand - extra : a + operand + extra;   // wraps above bit 8
    const unsigned carries = a ^ operand ^ result;
    unsigned f = ZeroFlag(result) | (carries & kFlagH) | OverflowFromCarries(carries) |
                 ((carries >> 8) & kFlagC) | (subtract ? kFlagN : 0);
    if (op == 7)          // CP: X and Y come from the operand, A is unchanged
    {
      SetF(f | (result & kFlagS) | (operand & kFlagsXY));
      return;
    }
    SetF(f | (result & kFlagsSXY));
    SetA(result);
    return;
  }
  case 4: result = a & operand; break;
  case 5: result = a ^ operand; break;
  default: result = a | operand; break;
  }
  result &= 0xFF;
  SetA(result);
  SetF((result & kFlagsSXY) | ZeroFlag(result) | ParityFlag(result) | (op == 4 ? kFlagH : 0));
}

unsigned CZ80::Increment(unsigned v)
{
  unsigned r = (v + 1) & 0xFF;
  SetF((F() & kFlagC) | (r & kFlagsSXY) | ZeroFlag(r) | ((r & 0x0F) == 0 ? kFlagH : 0) |
       (r == 0x80 ? kFlagPV : 0));
  return r;
}

unsigned CZ80::Decrement(unsigned v)
{
  unsigned r = (v - 1) & 0xFF;
  SetF((F() & kFlagC) | (r & kFlagsSXY) | ZeroFlag(r) | ((r & 0x0F) == 0x0F ? kFlagH : 0) |
       (r == 0x7F ? kFlagPV : 0) | kFlagN);
  return r;
}

// ADD HL/IX/IY,rr: S, Z and P/V are kept.
void CZ80::AddToPair(UINT16 &dst, unsigned src)
{
  const unsigned a = dst, sum = a + src;
  const unsigned carries = (a ^ src ^ sum) >> 8;
  dst = (UINT16) sum;
  SetF((F() & (kFlagS | kFlagZ | kFlagPV)) | ((sum >> 8) & kFlagsXY) | (carries & kFlagH) |
       ((carries >> 8) & kFlagC));
}

void CZ80::AddWithCarryHL(unsigned src)
{
  const unsigned a = HL(), sum = a + src + (F() & kFlagC);
  const unsigned carries = (a ^ src ^ sum) >> 8;
  HL() = (UINT16) sum;
  SetF(((sum >> 8) & kFlagsSXY) | ((sum & 0xFFFF) == 0 ? kFlagZ : 0) | OverflowFromCarries(carries) |
       (carries & kFlagH) | ((carries >> 8) & kFlagC));
}

void CZ80::SubWithCarryHL(unsigned src)
{
  const unsigned a = HL(), diff = a - src - (F() & kFlagC);
  const unsigned carries = (a ^ src ^ diff) >> 8;
  HL() = (UINT16) diff;
  SetF(((diff >> 8) & kFlagsSXY) | ((diff & 0xFFFF) == 0 ? kFlagZ : 0) | OverflowFromCarries(carries) |
       (carries & kFlagH) | kFlagN | ((carries >> 8) & kFlagC));
}

// CB-prefix rotates and shifts: RLC RRC RL RR SLA SRA SLL SRL.
unsigned CZ80::ShiftOrRotate(unsigned kind, unsigned v)
{
  unsigned r, carry;
  switch (kind)
  {
  case 0: r = (v << 1) | (v >> 7); carry = v >> 7; break;
  case 1: r = (v >> 1) | (v << 7); carry = v & 1; break;
  case 2: r = (v << 1) | (F() & kFlagC); carry = v >> 7; break;
  case 3: r = (v >> 1) | ((F() & kFlagC) << 7); carry = v & 1; break;
  case 4: r = v << 1; carry = v >> 7; break;
  case 5: r = (v >> 1) | (v & 0x80); carry = v & 1; break;
  case 6: r = (v << 1) | 1; carry = v >> 7; break;
  default: r = v >> 1; carry = v & 1; break;
  }
  r &= 0xFF;
  SetF((r & kFlagsSXY) | ZeroFlag(r) | ParityFlag(r) | (carry & 1));
  return r;
}

// BIT: X and Y come from the value only for the register forms (and are
// cleared for the memory forms).
void CZ80::BitTest(unsigned bit, unsigned v, bool fromRegister)
{
  unsigned f = F() & kFlagC;
  if (v & (1u << bit))
    f |= kFlagH | (bit == 7 ? kFlagS : 0);
  else
    f |= kFlagZ | kFlagH | kFlagPV;
  if (fromRegister)
    f |= v & kFlagsXY;
  SetF(f);
}

void CZ80::Daa()
{
  unsigned a = A();
  const unsigned low = a & 0x0F;
  unsigned carry = F() & kFlagC;
  unsigned f = F();
  if (f & kFlagN)
  {
    const bool adjustHigh = carry || a > 0x99;
    if ((f & kFlagH) || low > 9)
    {
      if (low > 5)
        f &= ~kFlagH;
      a = (a - 6) & 0xFF;
    }
    if (adjustHigh)
      a -= 0x160;
  }
  else
  {
    if ((f & kFlagH) || low > 9)
    {
      f = (low > 9) ? (f | kFlagH) : (f & ~kFlagH);
      a += 6;
    }
    if (carry || (a & 0x1F0) > 0x90)
      a += 0x60;
  }
  carry |= (a >> 8) & 1;
  a &= 0xFF;
  SetA(a);
  SetF((a & kFlagsSXY) | ZeroFlag(a) | (f & (kFlagH | kFlagN)) | ParityFlag(a) | carry);
}

// Flags of CPI/CPD/CPIR/CPDR. X and Y come from A - value - H.
void CZ80::BlockCompareFlags(unsigned a, unsigned value, bool more)
{
  const unsigned diff = a - value;
  const unsigned carries = a ^ value ^ diff;
  const unsigned halfBorrow = (carries >> 4) & 1;
  unsigned f = (F() & kFlagC) | (diff & kFlagS) | ZeroFlag(diff) | (carries & kFlagH) | kFlagN |
               (((diff - halfBorrow) & 0x02) << 4) | ((diff - halfBorrow) & kFlagX) |
               (more ? kFlagPV : 0);
  if ((diff & 0x0F) == 8 && halfBorrow)
    f &= ~kFlagX;
  SetF(f);
}

/******************************************************************************
 Execution
******************************************************************************/

void CZ80::ExecMain(unsigned op, Index idx)
{
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
  const bool indexed = (idx != Index::HL);
  m_budget -= indexed ? IndexedCost(op) : BaseCost(op);

  // Address of the (HL) / (IX+d) operand. The displacement is read when
  // the instruction first needs the address.
  auto memoryOperand = [&]() -> unsigned {
    if (!indexed)
      return HL();
    int disp = (INT8) Fetch8();
    return (unsigned) (IndexReg(idx) + disp) & 0xFFFF;
  };

  switch (x)
  {
  case 0:
    switch (z)
    {
    case 0:
      switch (y)
      {
      case 0:                                   // NOP
        break;
      case 1:                                   // EX AF,AF'
        m_afInUse ^= 1;
        break;
      case 2:                                   // DJNZ d
        SetReg8(0, idx, GetReg8(0, idx) - 1);
        if (BC() >> 8)
          m_pcReg += (INT8) m_bus->Read8(m_pcReg) + 1;
        else
          m_pcReg += 1;
        break;
      default:                                  // JR d / JR cc,d
        if (y == 3 || Condition(y - 4))
          m_pcReg += (INT8) m_bus->Read8(m_pcReg) + 1;
        else
          m_pcReg += 1;
        break;
      }
      break;
    case 1:
      if (q)                                    // ADD HL,rr
        AddToPair(IndexReg(idx), GetPair(p, idx));
      else                                      // LD rr,nn
        SetPair(p, idx, Fetch16());
      break;
    case 2:
    {
      if (p < 2)                                // LD (BC/DE),A / LD A,(BC/DE)
      {
        unsigned addr = p ? DE() : BC();
        if (q)
          SetA(m_bus->Read8(addr));
        else
          m_bus->Write8(addr, A());
        break;
      }
      unsigned addr = Fetch16();
      if (p == 2)                               // LD (nn),HL / LD HL,(nn)
      {
        if (q)
          IndexReg(idx) = Read16(addr);
        else
          Write16(addr, IndexReg(idx));
      }
      else if (q)                               // LD A,(nn)
        SetA(m_bus->Read8(addr));
      else                                      // LD (nn),A
        m_bus->Write8(addr, A());
      break;
    }
    case 3:                                     // INC rr / DEC rr
      SetPair(p, idx, GetPair(p, idx) + (q ? 0xFFFF : 1));
      break;
    case 4:
    case 5:                                     // INC r / DEC r
      if (y == 6)
      {
        unsigned addr = memoryOperand();
        unsigned v = m_bus->Read8(addr);
        m_bus->Write8(addr, (UINT8) (z == 4 ? Increment(v) : Decrement(v)));
      }
      else
      {
        unsigned v = GetReg8(y, idx);
        SetReg8(y, idx, z == 4 ? Increment(v) : Decrement(v));
      }
      break;
    case 6:                                     // LD r,n
      if (y == 6)
      {
        unsigned addr = memoryOperand();
        m_bus->Write8(addr, Fetch8());
      }
      else
        SetReg8(y, idx, Fetch8());
      break;
    default:
    {
      const unsigned a = A(), f = F();
      switch (y)
      {
      case 0:                                   // RLCA
        SetA((a << 1) | (a >> 7));
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (A() & kFlagsXY) | (a >> 7));
        break;
      case 1:                                   // RRCA
        SetA((a >> 1) | (a << 7));
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (A() & kFlagsXY) | (a & 1));
        break;
      case 2:                                   // RLA
        SetA((a << 1) | (f & kFlagC));
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (A() & kFlagsXY) | (a >> 7));
        break;
      case 3:                                   // RRA
        SetA((a >> 1) | ((f & kFlagC) << 7));
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (A() & kFlagsXY) | (a & 1));
        break;
      case 4:                                   // DAA
        Daa();
        break;
      case 5:                                   // CPL
        SetA(~a);
        SetF((f & (kFlagS | kFlagZ | kFlagPV | kFlagC)) | (A() & kFlagsXY) | kFlagH | kFlagN);
        break;
      case 6:                                   // SCF
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (a & kFlagsXY) | kFlagC);
        break;
      default:                                  // CCF
        SetF((f & (kFlagS | kFlagZ | kFlagPV)) | (a & kFlagsXY) | ((f & kFlagC) << 4) | (~f & kFlagC));
        break;
      }
      break;
    }
    }
    break;

  case 1:
    if (op == 0x76)                             // HALT
    {
      m_halted = true;
      break;
    }
    if (z == 6)                                 // LD r,(HL): r is never an index half
      SetReg8(y, Index::HL, m_bus->Read8(memoryOperand()));
    else if (y == 6)                            // LD (HL),r
    {
      unsigned addr = memoryOperand();
      m_bus->Write8(addr, GetReg8(z, Index::HL));
    }
    else                                        // LD r,r'
      SetReg8(y, idx, GetReg8(z, idx));
    break;

  case 2:                                       // ALU A,r
    Alu(y, z == 6 ? m_bus->Read8(memoryOperand()) : GetReg8(z, idx));
    break;

  default:
    switch (z)
    {
    case 0:                                     // RET cc
      if (Condition(y))
        m_pcReg = Pop();
      break;
    case 1:
      if (!q)                                   // POP rr
      {
        UINT16 v = Pop();
        if (p == 3)
          AF() = v;
        else
          SetPair(p, idx, v);
        break;
      }
      switch (p)
      {
      case 0: m_pcReg = Pop(); break;          // RET
      case 1: m_bankInUse ^= 1; break;          // EXX
      case 2: m_pcReg = IndexReg(idx); break;   // JP (HL)
      default: m_spReg = IndexReg(idx); break;  // LD SP,HL
      }
      break;
    case 2:                                     // JP cc,nn
      if (Condition(y))
        m_pcReg = Read16(m_pcReg);
      else
        m_pcReg += 2;
      break;
    case 3:
      switch (y)
      {
      case 0:                                   // JP nn
        m_pcReg = Read16(m_pcReg);
        break;
      case 2:                                   // OUT (n),A
      {
        unsigned port = Fetch8();
        m_bus->IOWrite8(port, A());
        break;
      }
      case 3:                                   // IN A,(n)
        SetA(m_bus->IORead8(Fetch8()));
        break;
      case 4:                                   // EX (SP),HL
      {
        UINT16 &reg = IndexReg(idx);
        unsigned old = reg;
        unsigned lo = m_bus->Read8(m_spReg);
        unsigned hi = m_bus->Read8((UINT16) (m_spReg + 1));
        reg = (UINT16) (lo | (hi << 8));
        m_bus->Write8((UINT16) (m_spReg + 1), (UINT8) (old >> 8));
        m_bus->Write8(m_spReg, (UINT8) old);
        break;
      }
      case 5:                                   // EX DE,HL
      {
        UINT16 t = DE();
        DE() = HL();
        HL() = t;
        break;
      }
      case 6:                                   // DI
        m_iffBits = 0;
        break;
      case 7:                                   // EI
        m_iffBits = 3;
        break;
      default:                                  // (CB is dispatched elsewhere)
        break;
      }
      break;
    case 4:                                     // CALL cc,nn
    case 5:
      if (z == 5 && !q)                         // PUSH rr
      {
        Push(p == 3 ? AF() : GetPair(p, idx));
        break;
      }
      if (z == 4 && !Condition(y))
      {
        m_pcReg += 2;
        break;
      }
      {                                         // CALL nn
        UINT16 target = Read16(m_pcReg);
        Push((UINT16) (m_pcReg + 2));
        m_pcReg = target;
      }
      break;
    case 6:                                     // ALU A,n
      Alu(y, Fetch8());
      break;
    default:                                    // RST
      Push(m_pcReg);
      m_pcReg = (UINT16) (y * 8);
      break;
    }
    break;
  }
}

// CB prefix, plain or after DD/FD (DD CB d op). The indexed register forms
// operate on the plain register (no memory write); the memory forms write
// the result back, BIT included.
void CZ80::ExecBitOps(Index idx)
{
  const bool indexed = (idx != Index::HL);
  unsigned addr = HL();
  if (indexed)
    addr = (unsigned) (IndexReg(idx) + (INT8) Fetch8()) & 0xFFFF;
  const unsigned op = Fetch8();
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
  m_budget -= BitOpCost(op, indexed);

  const unsigned v = (z == 6) ? m_bus->Read8(addr) : GetReg8(z, Index::HL);
  unsigned r;
  switch (x)
  {
  case 0: r = ShiftOrRotate(y, v); break;
  case 1: BitTest(y, v, z != 6); r = v; break;
  case 2: r = v & ~(1u << y); break;
  default: r = v | (1u << y); break;
  }
  if (z == 6)
    m_bus->Write8(addr, (UINT8) r);
  else
    SetReg8(z, Index::HL, r);
}

// LDI/LDD/CPI/CPD/INI/IND/OUTI/OUTD and their repeating forms.
void CZ80::ExecBlock(unsigned op)
{
  const unsigned y = (op >> 3) & 7, z = op & 7;
  const bool down = (y & 1) != 0;
  const bool repeat = (y >= 6);
  const UINT16 step = down ? 0xFFFF : 1;
  m_budget -= repeat ? 0 : 16;

  switch (z)
  {
  case 0:                                       // LDI/LDD(R)
  {
    unsigned count = repeat ? (BC() ? BC() : 0x10000u) : 1;
    unsigned v = 0;
    while (count--)
    {
      v = m_bus->Read8(HL());
      HL() += step;
      m_bus->Write8(DE(), (UINT8) v);
      DE() += step;
      BC()--;
    }
    unsigned n = v + A();
    SetF((F() & (kFlagS | kFlagZ | kFlagC)) | (n & kFlagX) | ((n & 0x02) << 4) |
         (BC() ? kFlagPV : 0));
    break;
  }
  case 1:                                       // CPI/CPD(R)
  {
    const unsigned a = A();
    unsigned v;
    bool more;
    do
    {
      v = m_bus->Read8(HL());
      HL() += step;
      BC()--;
      more = BC() != 0;
    } while (repeat && more && a != v);
    BlockCompareFlags(a, v, more);
    break;
  }
  case 2:                                       // INI/IND(R)
    if (repeat)
    {
      unsigned count = (BC() >> 8) ? (BC() >> 8) : 0x100u;
      while (count--)
      {
        m_bus->Write8(HL(), m_bus->IORead8((UINT8) BC()));
        HL() += step;
      }
      BC() &= 0x00FF;
      SetF(F() | kFlagN | kFlagZ);
    }
    else
    {
      m_bus->Write8(HL(), m_bus->IORead8((UINT8) BC()));
      HL() += step;
      unsigned f = F() | kFlagN;
      if (!down)                                // INI: P/V from BC - 1
      {
        BC()--;
        f = BC() ? (f | kFlagPV) : (f & ~kFlagPV);
      }
      else                                      // IND: B = C - 1, Z from C
      {
        unsigned c = BC() & 0xFF;
        BC() = (UINT16) ((((c - 1) & 0xFF) << 8) | c);
        f = c == 0 ? (f | kFlagZ) : (f & ~kFlagZ);
      }
      SetF(f);
    }
    break;
  default:                                      // OUTI/OUTD, OTIR/OTDR
    if (repeat)
    {
      unsigned count = (BC() >> 8) ? (BC() >> 8) : 0x100u;
      while (count--)
      {
        m_bus->IOWrite8((UINT8) BC(), m_bus->Read8(HL()));
        HL() += step;
      }
      BC() &= 0x00FF;
      SetF(F() | kFlagN | kFlagZ);
    }
    else
    {
      m_bus->IOWrite8((UINT8) BC(), m_bus->Read8(HL()));
      HL() += step;
      BC() -= 0x100;
      SetF((F() | kFlagN) & ~kFlagZ);
      if ((BC() >> 8) == 0)
        SetF(F() | kFlagZ);
    }
    break;
  }
}

// ED prefix.
void CZ80::ExecExtended()
{
  const unsigned op = Fetch8();
  const unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

  if (x == 2)
  {
    if (z <= 3 && y >= 4)
      ExecBlock(op);
    return;                                     // other ED 80-BF: two-byte no-op
  }
  if (x != 1)
    return;                                     // ED 00-3F, C0-FF: two-byte no-op

  switch (z)
  {
  case 0:                                       // IN r,(C) (y = 6: flags only)
  {
    m_budget -= 12;
    unsigned v = m_bus->IORead8((UINT8) BC());
    if (y != 6)
      SetReg8(y, Index::HL, v);
    SetF((F() & kFlagC) | (v & kFlagsSXY) | ZeroFlag(v) | ParityFlag(v));
    return;
  }
  case 1:                                       // OUT (C),r (y = 6: OUT (C),0)
    m_budget -= (y == 6) ? 0 : 12;
    m_bus->IOWrite8((UINT8) BC(), (UINT8) (y == 6 ? 0 : GetReg8(y, Index::HL)));
    return;
  case 2:                                       // SBC/ADC HL,rr
    m_budget -= 15;
    if (q)
      AddWithCarryHL(GetPair(p, Index::HL));
    else
      SubWithCarryHL(GetPair(p, Index::HL));
    return;
  case 3:                                       // LD (nn),rr / LD rr,(nn)
  {
    m_budget -= 20;
    unsigned addr = Fetch16();
    if (q)
      SetPair(p, Index::HL, Read16(addr));
    else
      Write16(addr, GetPair(p, Index::HL));
    return;
  }
  case 4:
    if (y == 0)                                 // NEG
    {
      m_budget -= 8;
      unsigned a = A(), r = (0u - a) & 0xFF;
      SetA(r);
      SetF((r & kFlagsSXY) | ZeroFlag(r) | ((a & 0x0F) ? kFlagH : 0) | (a == 0x80 ? kFlagPV : 0) |
           kFlagN | (a ? kFlagC : 0));
      return;
    }
    break;
  case 5:
    if (y <= 1)                                 // RETN / RETI
    {
      m_budget -= 14;
      m_iffBits |= m_iffBits >> 1;
      m_pcReg = Pop();
      return;
    }
    break;
  case 6:
    if (y == 0 || y == 2 || y == 3)             // IM 0/1/2
    {
      m_budget -= 8;
      m_intMode = (UINT8) (y == 0 ? 0 : y - 1);
      return;
    }
    break;
  default:
    switch (y)
    {
    case 0:                                     // LD I,A
      m_budget -= 9;
      m_iAndR = (UINT16) ((m_iAndR & 0x00FF) | (A() << 8));
      return;
    case 1:                                     // LD R,A
      m_budget -= 9;
      m_iAndR = (UINT16) ((m_iAndR & 0xFF00) | A());
      return;
    case 2:                                     // LD A,I
    case 3:                                     // LD A,R
    {
      m_budget -= 9;
      unsigned v = (y == 2) ? (m_iAndR >> 8) : (m_iAndR & 0xFF);
      SetA(v);
      // C, X and Y are kept from before; P/V is IFF2.
      SetF((F() & (kFlagC | kFlagsXY)) | (v & kFlagS) | ZeroFlag(v) | ((m_iffBits & 2) << 1));
      return;
    }
    case 4:                                     // RRD
    case 5:                                     // RLD
    {
      m_budget -= 18;
      unsigned m = m_bus->Read8(HL()), a = A();
      if (y == 4)
      {
        m_bus->Write8(HL(), (UINT8) ((m >> 4) | ((a & 0x0F) << 4)));
        a = (a & 0xF0) | (m & 0x0F);
      }
      else
      {
        m_bus->Write8(HL(), (UINT8) (((m & 0x0F) << 4) | (a & 0x0F)));
        a = (a & 0xF0) | (m >> 4);
      }
      SetA(a);
      SetF((a & kFlagsSXY) | ZeroFlag(a) | ParityFlag(a) | (F() & kFlagC));
      return;
    }
    default:
      break;
    }
    break;
  }

  // Not an instruction: the ED prefix is dropped and the second byte runs
  // as an ordinary instruction.
  --m_pcReg;
}

void CZ80::Step()
{
  const unsigned op = Fetch8();
  switch (op)
  {
  case 0xCB:
    ExecBitOps(Index::HL);
    break;
  case 0xED:
    ExecExtended();
    break;
  case 0xDD:
  case 0xFD:
  {
    const Index idx = (op == 0xDD) ? Index::IX : Index::IY;
    const unsigned next = Fetch8();
    if (next == 0xCB)
      ExecBitOps(idx);
    else if (UsesIndex(next))
      ExecMain(next, idx);
    else
      --m_pcReg;                                // prefix has no effect: run the next byte alone
    break;
  }
  default:
    ExecMain(op, Index::HL);
    break;
  }
}

// Takes a pending NMI, or an enabled maskable interrupt.
void CZ80::ServiceInterrupts()
{
  if (m_nmiPending)
  {
    Push(m_pcReg);
    m_pcReg = 0x0066;
    m_iffBits = (UINT8) ((m_iffBits & ~3) | ((m_iffBits & 1) << 1));   // IFF2 = IFF1, IFF1 = 0
    m_nmiPending = false;
    return;
  }
  if (!m_intAsserted || !(m_iffBits & 1))
    return;

  switch (m_intMode)
  {
  case 0:
    // The device supplies an instruction; only RST is accepted.
    if (!m_intAck)
    {
      m_intAsserted = false;
      return;
    }
    {
      int opcode = m_intAck(this);
      if ((opcode & ~0x38) == 0xC7 && opcode >= 0 && opcode <= 0xFF)
      {
        Push(m_pcReg);
        m_pcReg = (UINT16) (opcode & 0x38);
        m_iffBits = 0;
      }
    }
    return;
  case 1:
    Push(m_pcReg);
    m_pcReg = 0x0038;
    m_iffBits = 0;
    if (m_intAck)
      m_intAck(this);
    else
      m_intAsserted = false;
    return;
  case 2:
    if (!m_intAck)
    {
      m_intAsserted = false;
      return;
    }
    {
      int low = m_intAck(this);
      unsigned table = (m_iAndR & 0xFF00) | ((unsigned) low & 0xFE);
      Push(m_pcReg);
      m_pcReg = Read16(table);
      m_iffBits = 0;
    }
    return;
  default:
    m_intAsserted = false;
    return;
  }
}

int CZ80::Run(int numCycles)
{
  m_budget = numCycles;
  m_halted = false;
  while (m_budget > 0)
  {
    Step();
    if (m_halted)
      break;                                    // HALT: return with PC after it
    ServiceInterrupts();
  }
  return numCycles - m_budget;
}

void CZ80::TriggerNMI(void)
{
  m_nmiPending = true;
}

void CZ80::SetINT(bool state)
{
  m_intAsserted = state;
}

UINT16 CZ80::GetPC(void) const
{
  return m_pcReg;
}

void CZ80::Reset(void)
{
  for (RegisterBank &b : m_bank)
    b = RegisterBank{ 0, 0, 0 };
  m_afPair[0] = m_afPair[1] = 0;
  m_iAndR = 0;
  m_ixReg = m_iyReg = 0;
  m_spReg = 0xF000;
  m_pcReg = 0;
  m_iffBits = 0;
  m_intMode = 0;
  m_bankInUse = 0;
  m_afInUse = 0;
  m_nmiPending = false;
  m_intAsserted = false;
}

// Save state block: both register banks (BC DE HL each), both AF, IR, IX,
// IY, SP, PC, the interrupt flip-flops and mode, which bank and AF are in
// use (as ints), then the NMI and INT states (one byte each).
void CZ80::SaveState(CBlockFile *StateFile, const char *name)
{
  StateFile->NewBlock(name, __FILE__);
  for (RegisterBank &b : m_bank)
  {
    StateFile->Write(&b.bc, sizeof(b.bc));
    StateFile->Write(&b.de, sizeof(b.de));
    StateFile->Write(&b.hl, sizeof(b.hl));
  }
  StateFile->Write(m_afPair, sizeof(m_afPair));
  StateFile->Write(&m_iAndR, sizeof(m_iAndR));
  StateFile->Write(&m_ixReg, sizeof(m_ixReg));
  StateFile->Write(&m_iyReg, sizeof(m_iyReg));
  StateFile->Write(&m_spReg, sizeof(m_spReg));
  StateFile->Write(&m_pcReg, sizeof(m_pcReg));
  StateFile->Write(&m_iffBits, sizeof(m_iffBits));
  StateFile->Write(&m_intMode, sizeof(m_intMode));
  StateFile->Write(&m_bankInUse, sizeof(m_bankInUse));
  StateFile->Write(&m_afInUse, sizeof(m_afInUse));
  StateFile->Write(&m_nmiPending, sizeof(m_nmiPending));
  StateFile->Write(&m_intAsserted, sizeof(m_intAsserted));
}

void CZ80::LoadState(CBlockFile *StateFile, const char *name)
{
  if (StateFile->FindBlock(name) != Result::OKAY)
  {
    ErrorLog("Z80 state block '%s' is missing from the save state.", name);
    return;
  }
  for (RegisterBank &b : m_bank)
  {
    StateFile->Read(&b.bc, sizeof(b.bc));
    StateFile->Read(&b.de, sizeof(b.de));
    StateFile->Read(&b.hl, sizeof(b.hl));
  }
  StateFile->Read(m_afPair, sizeof(m_afPair));
  StateFile->Read(&m_iAndR, sizeof(m_iAndR));
  StateFile->Read(&m_ixReg, sizeof(m_ixReg));
  StateFile->Read(&m_iyReg, sizeof(m_iyReg));
  StateFile->Read(&m_spReg, sizeof(m_spReg));
  StateFile->Read(&m_pcReg, sizeof(m_pcReg));
  StateFile->Read(&m_iffBits, sizeof(m_iffBits));
  StateFile->Read(&m_intMode, sizeof(m_intMode));
  StateFile->Read(&m_bankInUse, sizeof(m_bankInUse));
  StateFile->Read(&m_afInUse, sizeof(m_afInUse));
  StateFile->Read(&m_nmiPending, sizeof(m_nmiPending));
  StateFile->Read(&m_intAsserted, sizeof(m_intAsserted));
}

void CZ80::Init(IBus *BusPtr, int (*INTF)(CZ80 *Z80))
{
  m_bus = BusPtr;
  m_intAck = INTF;
}

CZ80::CZ80(void)
  : m_bus(nullptr), m_intAck(nullptr), m_budget(0), m_halted(false)
{
  Reset();
}

CZ80::~CZ80(void)
{
}
