// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See M68000.h. Each of the 65,536 opcodes is classified once (Kind below:
// which instruction it is, or illegal); executing an instruction is a
// switch on its kind, with the operand fields taken from the opcode.

#include "M68000.h"

namespace
{
  // ---- small helpers ----

  constexpr uint32_t kMask[5] = { 0, 0xFF, 0xFFFF, 0, 0xFFFFFFFF };
  constexpr uint32_t kBits[5] = { 0, 8, 16, 0, 32 };
  constexpr int kSizeField[3] = { 1, 2, 4 };   // size bits 00, 01, 10

  inline uint32_t SignExtend8(uint32_t v)  { return uint32_t(int32_t(int8_t(v))); }
  inline uint32_t SignExtend16(uint32_t v) { return uint32_t(int32_t(int16_t(v))); }
  inline uint32_t Clip(uint32_t v, int sz) { return v & kMask[sz]; }
  inline uint32_t Msb(uint32_t v, int sz)  { return (v >> (kBits[sz] - 1)) & 1; }
  inline uint32_t Bit(bool b)              { return b ? 1 : 0; }

  inline uint32_t Ones16(uint32_t v)
  {
    uint32_t n = 0;
    for (v &= 0xFFFF; v != 0; v &= v - 1)
      n++;
    return n;
  }

  // (An)+ and -(An) step a byte access on A7 by 2, to keep it even.
  inline uint32_t StepOf(int sz, int reg)
  {
    return (sz == 1 && reg == 7) ? 2 : uint32_t(sz);
  }

  // ---- addressing-mode classes ----
  //   mode 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 d16(An), 6 d8(An,Xn),
  //   7/0 abs.W, 7/1 abs.L, 7/2 d16(PC), 7/3 d8(PC,Xn), 7/4 #imm

  enum class EA { Any, Data, Memory, Alterable, DataAlterable, MemoryAlterable, Control, ControlAlterable, ControlOrPostinc };

  bool Accepts(int mode, int reg, EA cls)
  {
    if (mode == 7 && reg > 4)
      return false;
    const bool pcRel = mode == 7 && (reg == 2 || reg == 3);
    const bool imm = mode == 7 && reg == 4;
    switch (cls)
    {
    case EA::Any:              return true;
    case EA::Data:             return mode != 1;
    case EA::Memory:           return mode >= 2;
    case EA::Alterable:        return !pcRel && !imm;
    case EA::DataAlterable:    return mode != 1 && !pcRel && !imm;
    case EA::MemoryAlterable:  return mode >= 2 && !pcRel && !imm;
    case EA::Control:          return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
    case EA::ControlAlterable: return mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1);
    case EA::ControlOrPostinc: return mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3);
    }
    return false;
  }

  // Internal clocks a long operation adds after its source is read.
  inline int LongExtra(int mode, int reg)
  {
    return (mode == 0 || mode == 1 || (mode == 7 && reg == 4)) ? 4 : 2;
  }

  // Extra internal clocks JMP and JSR spend per addressing mode.
  int JumpExtra(int mode, int reg)
  {
    switch (mode)
    {
    case 2: return 4;
    case 5: return 2;
    case 6: return 4;
    case 7: return reg == 0 ? 2 : reg == 1 ? 0 : reg == 2 ? 2 : 4;
    default: return 0;
    }
  }

  // ---- the opcode map ----

  enum Kind : uint8_t
  {
    ILLEGAL = 0,
    MOVEP, BIT_OP, LOGIC_TO_CCR_SR, IMMEDIATE,
    MOVE, MOVEA,
    LEA, CHK, MOVE_FROM_SR, NEGX, CLR, MOVE_TO_CCR, NEG, MOVE_TO_SR, NOT,
    NBCD, SWAP, PEA, EXT_W, EXT_L, MOVEM_TO_MEM, TAS, TST, MOVEM_TO_REG,
    TRAP, LINK, UNLK, MOVE_TO_USP, MOVE_FROM_USP, RESET, NOP, STOP, RTE,
    RTS, TRAPV, RTR, JSR, JMP,
    DBCC, SCC, QUICK_TO_AREG, QUICK,
    BRANCH, MOVEQ,
    DIVIDE, SBCD, OR, ADDSUB_A, ADDSUB_X, ADDSUB_TO_EA, ADDSUB_TO_DREG,
    CMPA, CMP, CMPM, EOR,
    MULTIPLY, ABCD, EXG_DD, EXG_AA, EXG_DA, AND,
    SHIFT_MEMORY, SHIFT_REGISTER,
  };

  Kind ClassifyGroup0(uint32_t op, int mode, int reg, int szf)
  {
    if ((op & 0x0138) == 0x0108)
      return MOVEP;
    if ((op & 0x0100) || (op & 0x0F00) == 0x0800)
    {
      const bool dynamic = (op & 0x0100) != 0;
      if (mode == 1)
        return ILLEGAL;
      if (((op >> 6) & 3) == 0)   // BTST
      {
        if (!Accepts(mode, reg, EA::Data) || (!dynamic && mode == 7 && reg == 4))
          return ILLEGAL;
      }
      else if (!Accepts(mode, reg, EA::DataAlterable))
        return ILLEGAL;
      return BIT_OP;
    }
    const uint32_t which = (op >> 9) & 7;   // 0 OR, 1 AND, 2 SUB, 3 ADD, 5 EOR, 6 CMP
    if ((op & 0xFF) == 0x3C || (op & 0xFF) == 0x7C)
      return (which == 0 || which == 1 || which == 5) ? LOGIC_TO_CCR_SR : ILLEGAL;
    if (szf == 3 || which == 4 || which == 7 || !Accepts(mode, reg, EA::DataAlterable))
      return ILLEGAL;
    return IMMEDIATE;
  }

  Kind ClassifyMove(uint32_t op, int mode, int reg)
  {
    const uint32_t hi = op >> 12;
    const int sz = hi == 1 ? 1 : hi == 3 ? 2 : 4;
    const int dstReg = (op >> 9) & 7, dstMode = (op >> 6) & 7;
    if (!Accepts(mode, reg, EA::Any) || (sz == 1 && mode == 1))
      return ILLEGAL;
    if (dstMode == 1)
      return sz == 1 ? ILLEGAL : MOVEA;
    return Accepts(dstMode, dstReg, EA::DataAlterable) ? MOVE : ILLEGAL;
  }

  Kind ClassifyGroup4(uint32_t op, int opmode, int mode, int reg, int szf)
  {
    if (opmode == 7)
      return Accepts(mode, reg, EA::Control) ? LEA : ILLEGAL;
    if (opmode == 6)
      return Accepts(mode, reg, EA::Data) ? CHK : ILLEGAL;
    switch (op & 0x0F00)
    {
    case 0x0000:
      if (!Accepts(mode, reg, EA::DataAlterable))
        return ILLEGAL;
      return szf == 3 ? MOVE_FROM_SR : NEGX;
    case 0x0200:
      return (szf != 3 && Accepts(mode, reg, EA::DataAlterable)) ? CLR : ILLEGAL;
    case 0x0400:
      if (szf == 3)
        return Accepts(mode, reg, EA::Data) ? MOVE_TO_CCR : ILLEGAL;
      return Accepts(mode, reg, EA::DataAlterable) ? NEG : ILLEGAL;
    case 0x0600:
      if (szf == 3)
        return Accepts(mode, reg, EA::Data) ? MOVE_TO_SR : ILLEGAL;
      return Accepts(mode, reg, EA::DataAlterable) ? NOT : ILLEGAL;
    case 0x0800:
      if (szf == 0)
        return Accepts(mode, reg, EA::DataAlterable) ? NBCD : ILLEGAL;
      if (szf == 1)
      {
        if (mode == 0)
          return SWAP;
        return Accepts(mode, reg, EA::Control) ? PEA : ILLEGAL;
      }
      if (mode == 0)
        return szf == 2 ? EXT_W : EXT_L;
      return (Accepts(mode, reg, EA::ControlAlterable) || mode == 4) ? MOVEM_TO_MEM : ILLEGAL;
    case 0x0A00:
      if (op == 0x4AFC)
        return ILLEGAL;   // the official ILLEGAL instruction
      if (szf == 3)
        return Accepts(mode, reg, EA::DataAlterable) ? TAS : ILLEGAL;
      return (Accepts(mode, reg, EA::Any) && !(szf == 0 && mode == 1)) ? TST : ILLEGAL;
    case 0x0C00:
      return (szf >= 2 && Accepts(mode, reg, EA::ControlOrPostinc)) ? MOVEM_TO_REG : ILLEGAL;
    case 0x0E00:
      if ((op & 0xFFF0) == 0x4E40) return TRAP;
      if ((op & 0xFFF8) == 0x4E50) return LINK;
      if ((op & 0xFFF8) == 0x4E58) return UNLK;
      if ((op & 0xFFF8) == 0x4E60) return MOVE_TO_USP;
      if ((op & 0xFFF8) == 0x4E68) return MOVE_FROM_USP;
      switch (op)
      {
      case 0x4E70: return RESET;
      case 0x4E71: return NOP;
      case 0x4E72: return STOP;
      case 0x4E73: return RTE;
      case 0x4E75: return RTS;
      case 0x4E76: return TRAPV;
      case 0x4E77: return RTR;
      default: break;
      }
      if ((op & 0xFFC0) == 0x4E80)
        return Accepts(mode, reg, EA::Control) ? JSR : ILLEGAL;
      if ((op & 0xFFC0) == 0x4EC0)
        return Accepts(mode, reg, EA::Control) ? JMP : ILLEGAL;
      return ILLEGAL;
    default:
      return ILLEGAL;
    }
  }

  Kind ClassifyGroup5(int mode, int reg, int szf)
  {
    if (szf == 3)
    {
      if (mode == 1)
        return DBCC;
      return Accepts(mode, reg, EA::DataAlterable) ? SCC : ILLEGAL;
    }
    if (!Accepts(mode, reg, EA::Alterable) || (mode == 1 && szf == 0))
      return ILLEGAL;
    return mode == 1 ? QUICK_TO_AREG : QUICK;
  }

  // OR (isOr) or AND between a data register and <ea>.
  Kind ClassifyLogic(int opmode, int mode, int reg, bool isOr)
  {
    if (opmode >= 4)
      return Accepts(mode, reg, EA::MemoryAlterable) ? (isOr ? OR : AND) : ILLEGAL;
    return Accepts(mode, reg, EA::Data) ? (isOr ? OR : AND) : ILLEGAL;
  }

  Kind ClassifyAddSub(int opmode, int mode, int reg)
  {
    if (opmode == 3 || opmode == 7)
      return Accepts(mode, reg, EA::Any) ? ADDSUB_A : ILLEGAL;
    if (opmode >= 4 && mode <= 1)
      return ADDSUB_X;
    if (opmode >= 4)
      return Accepts(mode, reg, EA::MemoryAlterable) ? ADDSUB_TO_EA : ILLEGAL;
    return (!Accepts(mode, reg, EA::Any) || (opmode == 0 && mode == 1)) ? ILLEGAL : ADDSUB_TO_DREG;
  }

  Kind ClassifyCmpEor(int opmode, int mode, int reg)
  {
    if (opmode == 3 || opmode == 7)
      return Accepts(mode, reg, EA::Any) ? CMPA : ILLEGAL;
    if (opmode < 3)
      return (!Accepts(mode, reg, EA::Any) || (opmode == 0 && mode == 1)) ? ILLEGAL : CMP;
    if (mode == 1)
      return CMPM;
    return Accepts(mode, reg, EA::DataAlterable) ? EOR : ILLEGAL;
  }

  Kind ClassifyAndMul(int opmode, int mode, int reg)
  {
    if (opmode == 3 || opmode == 7)
      return Accepts(mode, reg, EA::Data) ? MULTIPLY : ILLEGAL;
    if (opmode == 4 && mode <= 1)
      return ABCD;
    if (opmode == 5 && mode == 0) return EXG_DD;
    if (opmode == 5 && mode == 1) return EXG_AA;
    if (opmode == 6 && mode == 1) return EXG_DA;
    if (opmode == 6 && mode == 0) return ILLEGAL;
    return ClassifyLogic(opmode, mode, reg, false);
  }

  Kind Classify(uint32_t op)
  {
    const int opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
    const int szf = (op >> 6) & 3;
    switch (op >> 12)
    {
    case 0x0: return ClassifyGroup0(op, mode, reg, szf);
    case 0x1: case 0x2: case 0x3: return ClassifyMove(op, mode, reg);
    case 0x4: return ClassifyGroup4(op, opmode, mode, reg, szf);
    case 0x5: return ClassifyGroup5(mode, reg, szf);
    case 0x6: return BRANCH;
    case 0x7: return (op & 0x100) ? ILLEGAL : MOVEQ;
    case 0x8:
      if (opmode == 3 || opmode == 7)
        return Accepts(mode, reg, EA::Data) ? DIVIDE : ILLEGAL;
      if (opmode == 4 && mode <= 1)
        return SBCD;
      return ClassifyLogic(opmode, mode, reg, true);
    case 0x9: case 0xD: return ClassifyAddSub(opmode, mode, reg);
    case 0xB: return ClassifyCmpEor(opmode, mode, reg);
    case 0xC: return ClassifyAndMul(opmode, mode, reg);
    case 0xE:
      if (szf == 3)
        return ((op & 0x0800) || !Accepts(mode, reg, EA::MemoryAlterable)) ? ILLEGAL : SHIFT_MEMORY;
      return SHIFT_REGISTER;
    default: return ILLEGAL;   // A-line and F-line
    }
  }

  struct OpcodeMap
  {
    uint8_t kind[65536];
    OpcodeMap()
    {
      for (uint32_t op = 0; op < 65536; op++)
        kind[op] = Classify(op);
    }
  };

  const OpcodeMap &Map()
  {
    static const OpcodeMap map;
    return map;
  }

  // ---- division timing ----
  // The divider works out one quotient bit at a time, and how long a bit
  // takes depends on it and on the partial remainder. Both return the time
  // with a register operand, less the 4-clock opcode fetch.

  // DIVU: 72 clocks, plus for each quotient bit 15..1 whose partial
  // remainder going in was below 0x8000: 2 for a 1 bit, 4 for a 0 bit.
  // Overflow is found after 6.
  int DivuClocks(uint32_t dividend, uint32_t divisor)
  {
    if ((dividend >> 16) >= divisor)
      return 6;
    const uint32_t quotient = dividend / divisor;
    int t = 72;
    for (int bit = 15; bit >= 1; bit--)
    {
      const uint32_t partial = (dividend >> (bit + 1)) % divisor;
      if (partial < 0x8000)
        t += ((quotient >> bit) & 1) ? 2 : 4;
    }
    return t;
  }

  // DIVS: a base set by the operands' signs plus 2 for each 0 among bits
  // 15..1 of the unsigned quotient. Overflow: 12 (14 if the dividend is
  // negative).
  int DivsClocks(int64_t dividend, int64_t divisor)
  {
    const int negDividend = dividend < 0, negDivisor = divisor < 0;
    const int64_t n = negDividend ? -dividend : dividend;
    const int64_t d = negDivisor ? -divisor : divisor;
    if ((n >> 16) >= d)
      return 12 + 2 * negDividend;
    int64_t q = ((n / d) >> 1) & 0x7FFF;
    int ones = 0;
    for (; q != 0; q &= q - 1)
      ones++;
    static const int base[2][2] = { { 116, 118 }, { 122, 120 } };
    return base[negDividend][negDivisor] + 2 * (15 - ones);
  }
}

M68000::M68000()
{
  (void) Map();   // build the opcode map before the first instruction
}

// ---- status register ----

uint32_t M68000::CCR() const
{
  return (st.x << 4) | (st.n << 3) | (st.z << 2) | (st.v << 1) | st.c;
}

void M68000::SetCCR(uint32_t v)
{
  st.x = (v >> 4) & 1;
  st.n = (v >> 3) & 1;
  st.z = (v >> 2) & 1;
  st.v = (v >> 1) & 1;
  st.c = v & 1;
}

uint32_t M68000::SR() const
{
  return (st.t << 15) | (st.s << 13) | (st.ipl << 8) | CCR();
}

void M68000::SetSR(uint32_t v)
{
  const uint32_t s = (v >> 13) & 1;
  if (s != st.s)
  {
    if (st.s)
    {
      st.ssp = st.a[7];
      st.a[7] = st.usp;
    }
    else
    {
      st.usp = st.a[7];
      st.a[7] = st.ssp;
    }
    st.s = s;
  }
  st.t = (v >> 15) & 1;
  st.ipl = (v >> 8) & 7;
  SetCCR(v & 0x1F);
}

// ---- bus ----

uint32_t M68000::R8(uint32_t a)
{
  st.cycles += 4;
  return bus->Read8(a & 0xFFFFFF);
}

uint32_t M68000::R16(uint32_t a)
{
  st.cycles += 4;
  return bus->Read16(a & 0xFFFFFE);
}

uint32_t M68000::R32(uint32_t a)
{
  st.cycles += 8;
  return bus->Read32(a & 0xFFFFFE);
}

void M68000::W8(uint32_t a, uint32_t v)
{
  st.cycles += 4;
  bus->Write8(a & 0xFFFFFF, uint8_t(v));
}

void M68000::W16(uint32_t a, uint32_t v)
{
  st.cycles += 4;
  bus->Write16(a & 0xFFFFFE, uint16_t(v));
}

void M68000::W32(uint32_t a, uint32_t v)
{
  st.cycles += 8;
  bus->Write32(a & 0xFFFFFE, v);
}

uint32_t M68000::ReadSized(uint32_t a, int sz)
{
  return sz == 1 ? R8(a) : sz == 2 ? R16(a) : R32(a);
}

void M68000::WriteSized(uint32_t a, int sz, uint32_t v)
{
  if (sz == 1)
    W8(a, v);
  else if (sz == 2)
    W16(a, v);
  else
    W32(a, v);
}

uint32_t M68000::Fetch16()
{
  uint32_t v = R16(st.pc);
  st.pc += 2;
  return v;
}

uint32_t M68000::Fetch32()
{
  uint32_t v = R32(st.pc);
  st.pc += 4;
  return v;
}

void M68000::Push16(uint32_t v) { st.a[7] -= 2; W16(st.a[7], v); }
void M68000::Push32(uint32_t v) { st.a[7] -= 4; W32(st.a[7], v); }
uint32_t M68000::Pop16() { uint32_t v = R16(st.a[7]); st.a[7] += 2; return v; }
uint32_t M68000::Pop32() { uint32_t v = R32(st.a[7]); st.a[7] += 4; return v; }

// ---- effective addresses ----

uint32_t M68000::Indexed(uint32_t base)
{
  const uint32_t ext = Fetch16();
  st.cycles += 2;
  const int r = (ext >> 12) & 7;
  uint32_t index = (ext & 0x8000) ? st.a[r] : st.d[r];
  if (!(ext & 0x0800))
    index = SignExtend16(index & 0xFFFF);
  return base + index + SignExtend8(ext & 0xFF);
}

uint32_t M68000::Address(int mode, int reg, int sz)
{
  switch (mode)
  {
  case 2:
    return st.a[reg];
  case 3:
  {
    uint32_t a = st.a[reg];
    st.a[reg] += StepOf(sz, reg);
    return a;
  }
  case 4:
    st.cycles += 2;
    st.a[reg] -= StepOf(sz, reg);
    return st.a[reg];
  case 5:
  {
    uint32_t base = st.a[reg];
    return base + SignExtend16(Fetch16());
  }
  case 6:
    return Indexed(st.a[reg]);
  case 7:
    switch (reg)
    {
    case 0: return SignExtend16(Fetch16());
    case 1: return Fetch32();
    case 2:
    {
      uint32_t base = st.pc;
      return base + SignExtend16(Fetch16());
    }
    case 3: return Indexed(st.pc);
    default: break;
    }
    break;
  default:
    break;
  }
  return 0;
}

uint32_t M68000::ReadEA(int mode, int reg, int sz)
{
  if (mode == 0)
    return Clip(st.d[reg], sz);
  if (mode == 1)
    return Clip(st.a[reg], sz);
  if (mode == 7 && reg == 4)
  {
    if (sz == 1)
      return Fetch16() & 0xFF;
    if (sz == 2)
      return Fetch16();
    return Fetch32();
  }
  return ReadSized(Address(mode, reg, sz), sz);
}

void M68000::SetDn(int r, uint32_t v, int sz)
{
  if (sz == 4)
    st.d[r] = v;
  else if (sz == 2)
    st.d[r] = (st.d[r] & 0xFFFF0000) | (v & 0xFFFF);
  else
    st.d[r] = (st.d[r] & 0xFFFFFF00) | (v & 0xFF);
}

uint32_t M68000::RmwRead(int mode, int reg, int sz, uint32_t *addr)
{
  if (mode == 0)
  {
    *addr = 0;
    return Clip(st.d[reg], sz);
  }
  *addr = Address(mode, reg, sz);
  return ReadSized(*addr, sz);
}

void M68000::RmwWrite(int mode, int reg, int sz, uint32_t addr, uint32_t v)
{
  if (mode == 0)
    SetDn(reg, v, sz);
  else
    WriteSized(addr, sz, v);
}

// ---- flags ----

void M68000::SetNZ(uint32_t v, int sz)
{
  st.n = Msb(v, sz);
  st.z = Bit(Clip(v, sz) == 0);
}

void M68000::SetLogic(uint32_t v, int sz)
{
  SetNZ(v, sz);
  st.v = 0;
  st.c = 0;
}

// a + b + carry; sets C, V, N (callers set X and Z as each instruction does).
uint32_t M68000::Add(uint32_t a, uint32_t b, int sz, uint32_t carry)
{
  const uint64_t wide = uint64_t(a) + uint64_t(b) + uint64_t(carry);
  const uint32_t r = Clip(uint32_t(wide), sz);
  const uint32_t sa = Msb(a, sz), sb = Msb(b, sz), sr = Msb(r, sz);
  st.c = Bit(wide > kMask[sz]);
  st.v = Bit(sa == sb && sr != sa);
  st.n = sr;
  return r;
}

// a - b - borrow; sets C, V, N.
uint32_t M68000::Sub(uint32_t a, uint32_t b, int sz, uint32_t borrow)
{
  const int64_t wide = int64_t(a) - int64_t(b) - int64_t(borrow);
  const uint32_t r = Clip(uint32_t(wide), sz);
  const uint32_t sa = Msb(a, sz), sb = Msb(b, sz), sr = Msb(r, sz);
  st.c = Bit(wide < 0);
  st.v = Bit(sa != sb && sr != sa);
  st.n = sr;
  return r;
}

bool M68000::Cond(uint32_t cc) const
{
  switch (cc)
  {
  case 0:  return true;                          // T
  case 1:  return false;                         // F
  case 2:  return !st.c && !st.z;                // HI
  case 3:  return st.c || st.z;                  // LS
  case 4:  return !st.c;                         // CC
  case 5:  return st.c;                          // CS
  case 6:  return !st.z;                         // NE
  case 7:  return st.z;                          // EQ
  case 8:  return !st.v;                         // VC
  case 9:  return st.v;                          // VS
  case 10: return !st.n;                         // PL
  case 11: return st.n;                          // MI
  case 12: return st.n == st.v;                  // GE
  case 13: return st.n != st.v;                  // LT
  case 14: return !st.z && st.n == st.v;         // GT
  default: return st.z || st.n != st.v;          // LE
  }
}

// Decimal add and subtract, with what the chip does for digits above 9 and
// for the officially undefined N and V: the binary result is corrected by 6
// in each digit that carried (binary or decimal).
uint32_t M68000::DecimalAdd(uint32_t d32, uint32_t s32)
{
  const int d = int(d32), s = int(s32);
  const int sum = d + s + int(st.x);
  const int carried = ((s & d) | (~sum & s) | (~sum & d)) & 0x88;
  const int overNine = (((sum + 0x66) ^ sum) & 0x110) >> 1;
  const int fix = carried | overNine;
  const int r = sum + fix - (fix >> 2);
  st.c = uint32_t(((carried | (sum & ~r)) >> 7) & 1);
  st.x = st.c;
  st.v = uint32_t(((~sum & r) >> 7) & 1);
  st.n = uint32_t((r >> 7) & 1);
  if (r & 0xFF)
    st.z = 0;
  return uint32_t(r & 0xFF);
}

uint32_t M68000::DecimalSub(uint32_t d32, uint32_t s32)
{
  const int d = int(d32), s = int(s32);
  const int diff = d - s - int(st.x);
  const int borrowed = ((~d & s) | (diff & ~d) | (diff & s)) & 0x88;
  const int r = diff - (borrowed - (borrowed >> 2));
  st.c = uint32_t(((borrowed | (~diff & r)) >> 7) & 1);
  st.x = st.c;
  st.v = uint32_t(((diff & ~r) >> 7) & 1);
  st.n = uint32_t((r >> 7) & 1);
  if (r & 0xFF)
    st.z = 0;
  return uint32_t(r & 0xFF);
}

// kind: 0 AS, 1 LS, 2 ROX, 3 RO.
uint32_t M68000::Shift(int kind, bool left, uint32_t value, uint32_t count, int sz)
{
  const uint32_t bits = kBits[sz];
  const uint64_t top = uint64_t(1) << (bits - 1);
  const uint64_t mask = kMask[sz];
  auto msb = [bits](uint64_t x) { return uint32_t(x >> (bits - 1)) & 1; };
  uint64_t v = value;
  uint32_t carry = 0, overflow = 0;

  if (count == 0)
  {
    st.c = (kind == 2) ? st.x : 0;
    st.v = 0;
    SetNZ(value, sz);
    return value;
  }

  switch (kind)
  {
  case 0:
    if (left)
    {
      for (uint32_t i = 0; i < count; i++)
      {
        carry = msb(v);
        const uint64_t next = (v << 1) & mask;
        if (msb(next) != msb(v))
          overflow = 1;
        v = next;
      }
    }
    else
    {
      const uint32_t sign = msb(v);
      for (uint32_t i = 0; i < count; i++)
      {
        carry = uint32_t(v & 1);
        v >>= 1;
        if (sign)
          v += top;
        v &= mask;
      }
    }
    st.x = carry;
    break;
  case 1:
    for (uint32_t i = 0; i < count; i++)
    {
      if (left)
      {
        carry = msb(v);
        v = (v << 1) & mask;
      }
      else
      {
        carry = uint32_t(v & 1);
        v >>= 1;
      }
    }
    st.x = carry;
    break;
  case 2:
  {
    uint32_t x = st.x;
    for (uint32_t i = 0; i < count; i++)
    {
      if (left)
      {
        carry = msb(v);
        v = ((v << 1) + x) & mask;
      }
      else
      {
        carry = uint32_t(v & 1);
        v >>= 1;
        if (x)
          v += top;
      }
      x = carry;
    }
    st.x = x;
    carry = x;
    break;
  }
  default:
    for (uint32_t i = 0; i < count; i++)
    {
      if (left)
      {
        carry = msb(v);
        v = ((v << 1) + carry) & mask;
      }
      else
      {
        carry = uint32_t(v & 1);
        v >>= 1;
        if (carry)
          v += top;
      }
    }
    break;
  }
  const uint32_t r = uint32_t(v) & kMask[sz];
  st.c = carry;
  st.v = overflow;
  SetNZ(r, sz);
  return r;
}

void M68000::Divide(int rx, uint32_t src, bool isSigned)
{
  const uint32_t dividend = st.d[rx];
  if (src == 0)
  {
    st.cycles += 4;
    st.v = st.c = st.n = st.z = 0;
    Trap(5, 10);
    return;
  }
  if (!isSigned)
  {
    const uint32_t q = dividend / src, r = dividend % src;
    st.cycles += DivuClocks(dividend, src);
    if (q > 0xFFFF)
    {
      st.v = 1; st.n = 1; st.z = 0; st.c = 0;
      return;
    }
    st.d[rx] = (r << 16) | q;
    st.n = (q >> 15) & 1;
    st.z = Bit(q == 0);
    st.v = st.c = 0;
    return;
  }
  const int64_t num = int64_t(int32_t(dividend)), den = int64_t(int16_t(src));
  const int64_t q = num / den;
  const int64_t r = num - q * den;
  st.cycles += DivsClocks(num, den);
  if (q > 32767 || q < -32768)
  {
    st.v = 1; st.n = 1; st.z = 0; st.c = 0;
    return;
  }
  st.d[rx] = ((uint32_t(r) & 0xFFFF) << 16) | (uint32_t(q) & 0xFFFF);
  st.n = uint32_t(q >> 15) & 1;
  st.z = Bit(q == 0);
  st.v = st.c = 0;
}

// ---- exceptions ----

void M68000::Exception(uint32_t vector, uint32_t pc)
{
  const uint32_t sr = SR();
  if (!st.s)
  {
    st.usp = st.a[7];
    st.a[7] = st.ssp;
    st.s = 1;
  }
  st.t = 0;
  Push32(pc);
  Push16(sr);
  st.pc = R32(vector * 4);
}

void M68000::Trap(uint32_t vector, int extra)
{
  st.cycles += extra;
  Exception(vector, st.pc);
}

void M68000::PrivilegeViolation()
{
  st.cycles += 10;
  Exception(8, st.opPC);
}

void M68000::Illegal(uint32_t vector)
{
  st.cycles += 10;
  Exception(vector, st.opPC);
}

void M68000::SetIRQ(int level)
{
  st.irq = level;
  if (level != 7)
    st.nmiSeen = false;
}

bool M68000::TakeInterrupt()
{
  const int level = st.irq;
  if (level == 0)
    return false;
  if (level == 7)
  {
    if (st.nmiSeen)
      return false;
    st.nmiSeen = true;
  }
  else if (uint32_t(level) <= st.ipl)
    return false;

  st.stopped = false;
  const uint32_t sr = SR();
  if (!st.s)
  {
    st.usp = st.a[7];
    st.a[7] = st.ssp;
    st.s = 1;
  }
  st.t = 0;
  st.ipl = uint32_t(level);
  st.cycles += 24;
  int vector = kM68KAutoVector;
  if (ack != nullptr)
    vector = ack(user, level);
  if (vector == kM68KSpurious)
    vector = 24;
  else if (vector < 0 || vector > 255)
    vector = 24 + level;
  Push32(st.pc);
  Push16(sr);
  st.pc = R32(uint32_t(vector) * 4);
  return true;
}

// ---- running ----

void M68000::Reset()
{
  st.s = 1;
  st.t = 0;
  st.ipl = 7;
  st.stopped = false;
  st.a[7] = R32(0);
  st.ssp = st.a[7];
  st.pc = R32(4);
}

int M68000::Step()
{
  const int64_t start = st.cycles;
  if (TakeInterrupt())
    return int(st.cycles - start);
  if (st.stopped)
  {
    st.cycles += 4;
    return 4;
  }
  st.opPC = st.pc;
  const uint32_t op = Fetch16();
  st.op = op;
  Execute(op);
  return int(st.cycles - start);
}

int64_t M68000::Run(int64_t clocks)
{
  const int64_t start = st.cycles, until = start + clocks;
  while (st.cycles < until)
  {
    if (st.stopped && !(st.irq != 0 && (uint32_t(st.irq) > st.ipl || (st.irq == 7 && !st.nmiSeen))))
    {
      st.cycles = until;
      break;
    }
    Step();
  }
  return st.cycles - start;
}

// ---- instructions ----

void M68000::Execute(uint32_t op)
{
  switch (Map().kind[op])
  {
  case ILLEGAL:
    Illegal((op >> 12) == 0xA ? 10 : (op >> 12) == 0xF ? 11 : 4);
    break;
  case MOVEP: case BIT_OP: case LOGIC_TO_CCR_SR: case IMMEDIATE:
    Group0(op);
    break;
  case MOVE: case MOVEA:
    Move(op);
    break;
  case DBCC: case SCC: case QUICK_TO_AREG: case QUICK:
    Group5(op);
    break;
  case BRANCH:
    Branch(op);
    break;
  case MOVEQ:
  {
    const uint32_t v = SignExtend8(op & 0xFF);
    st.d[(op >> 9) & 7] = v;
    SetLogic(v, 4);
    break;
  }
  case DIVIDE: case SBCD: case OR:
    OrDivSbcd(op);
    break;
  case ADDSUB_A: case ADDSUB_X: case ADDSUB_TO_EA: case ADDSUB_TO_DREG:
    AddSub(op, (op >> 12) == 0xD);
    break;
  case CMPA: case CMP: case CMPM: case EOR:
    CmpEor(op);
    break;
  case MULTIPLY: case ABCD: case EXG_DD: case EXG_AA: case EXG_DA: case AND:
    AndMulAbcdExg(op);
    break;
  case SHIFT_MEMORY: case SHIFT_REGISTER:
    ShiftRotate(op);
    break;
  default:
    Group4(op);
    break;
  }
}

void M68000::Group0(uint32_t op)
{
  const int rx = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
  switch (Map().kind[op])
  {
  case MOVEP:
  {
    const bool toMemory = (op >> 7) & 1, isLong = (op >> 6) & 1;
    uint32_t a = st.a[reg] + SignExtend16(Fetch16());
    if (toMemory)
    {
      const uint32_t v = st.d[rx];
      if (isLong)
      {
        W8(a, v >> 24);
        W8(a + 2, v >> 16);
        a += 4;
      }
      W8(a, v >> 8);
      W8(a + 2, v);
    }
    else if (isLong)
    {
      const uint32_t b3 = R8(a), b2 = R8(a + 2), b1 = R8(a + 4), b0 = R8(a + 6);
      st.d[rx] = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
    }
    else
    {
      const uint32_t b1 = R8(a), b0 = R8(a + 2);
      SetDn(rx, (b1 << 8) | b0, 2);
    }
    return;
  }
  case BIT_OP:
  {
    const int which = (op >> 6) & 3;   // 0 BTST, 1 BCHG, 2 BCLR, 3 BSET
    uint32_t bit = (op & 0x100) ? st.d[rx] : (Fetch16() & 0xFF);
    if (mode == 0)
    {
      bit &= 31;
      const uint32_t m = uint32_t(1) << bit;
      const uint32_t v = st.d[reg];
      st.z = Bit((v & m) == 0);
      if (which == 0)
      {
        st.cycles += 2;
        return;
      }
      st.d[reg] = which == 1 ? (v ^ m) : which == 2 ? (v & ~m) : (v | m);
      st.cycles += (which == 2) ? 4 : 2;
      if (bit >= 16)
        st.cycles += 2;
      return;
    }
    bit &= 7;
    const uint32_t m = uint32_t(1) << bit;
    if (which == 0)
    {
      const uint32_t v = ReadEA(mode, reg, 1);
      st.z = Bit((v & m) == 0);
      if (mode == 7 && reg == 4)
        st.cycles += 2;
      return;
    }
    uint32_t a;
    uint32_t v = RmwRead(mode, reg, 1, &a);
    st.z = Bit((v & m) == 0);
    v = which == 1 ? (v ^ m) : which == 2 ? (v & ~m) : (v | m);
    RmwWrite(mode, reg, 1, a, v);
    return;
  }
  case LOGIC_TO_CCR_SR:
  {
    const bool toSR = (op & 0xFF) == 0x7C;
    if (toSR && !st.s)
    {
      PrivilegeViolation();
      return;
    }
    const uint32_t imm = Fetch16();
    const uint32_t cur = toSR ? SR() : CCR();
    const uint32_t which = (op >> 9) & 7;
    const uint32_t v = which == 0 ? (cur | imm) : which == 1 ? (cur & imm) : (cur ^ imm);
    if (toSR)
      SetSR(v & 0xA71F);
    else
      SetCCR(v & 0x1F);
    st.cycles += 12;
    return;
  }
  default:   // IMMEDIATE
  {
    const int sz = kSizeField[(op >> 6) & 3];
    const uint32_t which = (op >> 9) & 7;
    const uint32_t imm = sz == 4 ? Fetch32() : sz == 2 ? Fetch16() : (Fetch16() & 0xFF);
    if (which == 6)   // CMPI
    {
      const uint32_t v = ReadEA(mode, reg, sz);
      const uint32_t r = Sub(v, imm, sz, 0);
      st.z = Bit(r == 0);
      if (sz == 4 && mode == 0)
        st.cycles += 2;
      return;
    }
    if (mode == 0 && sz == 4)
      st.cycles += 4;
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, sz, &a);
    uint32_t r;
    switch (which)
    {
    case 0: r = Clip(v | imm, sz); SetLogic(r, sz); break;
    case 1: r = Clip(v & imm, sz); SetLogic(r, sz); break;
    case 5: r = Clip(v ^ imm, sz); SetLogic(r, sz); break;
    case 2: r = Sub(v, imm, sz, 0); st.x = st.c; st.z = Bit(r == 0); break;
    default: r = Add(v, imm, sz, 0); st.x = st.c; st.z = Bit(r == 0); break;
    }
    RmwWrite(mode, reg, sz, a, r);
    return;
  }
  }
}

void M68000::Move(uint32_t op)
{
  const uint32_t hi = op >> 12;
  const int sz = hi == 1 ? 1 : hi == 3 ? 2 : 4;
  const int dstReg = (op >> 9) & 7, dstMode = (op >> 6) & 7;
  const int srcMode = (op >> 3) & 7, srcReg = op & 7;
  uint32_t v = ReadEA(srcMode, srcReg, sz);
  if (dstMode == 1)   // MOVEA
  {
    st.a[dstReg] = (sz == 2) ? SignExtend16(v) : v;
    return;
  }
  if (dstMode == 0)
  {
    SetDn(dstReg, v, sz);
    SetLogic(v, sz);
    return;
  }
  uint32_t a;
  if (dstMode == 4)   // no extra time as a MOVE destination
  {
    st.a[dstReg] -= StepOf(sz, dstReg);
    a = st.a[dstReg];
  }
  else
    a = Address(dstMode, dstReg, sz);
  SetLogic(v, sz);
  WriteSized(a, sz, v);
}

// CLR, NEG, NEGX, NOT (kind 0-3): a long on a data register takes 2 more.
void M68000::Unary(int mode, int reg, int sz, int kind)
{
  uint32_t a = 0, v;
  if (mode == 0)
  {
    if (sz == 4)
      st.cycles += 2;
    v = Clip(st.d[reg], sz);
  }
  else
    v = RmwRead(mode, reg, sz, &a);

  uint32_t r;
  switch (kind)
  {
  case 0:   // CLR
    st.n = 0; st.z = 1; st.v = 0; st.c = 0;
    r = 0;
    break;
  case 1:   // NEG
    r = Sub(0, v, sz, 0);
    st.x = st.c;
    st.z = Bit(r == 0);
    break;
  case 2:   // NEGX: Z only ever cleared
  {
    const uint32_t z = st.z;
    r = Sub(0, v, sz, st.x);
    st.x = st.c;
    st.z = r ? 0 : z;
    break;
  }
  default:  // NOT
    r = Clip(~v, sz);
    SetLogic(r, sz);
    break;
  }
  RmwWrite(mode, reg, sz, a, r);
}

void M68000::MovemToMemory(int mode, int reg, int sz)
{
  const uint32_t mask = Fetch16();
  if (mode == 4)
  {
    // Registers are stored from A7 down to D0; the address register being
    // decremented is stored with its value from before the instruction.
    uint32_t a = st.a[reg];
    const uint32_t start = a;
    for (int i = 0; i < 16; i++)
    {
      if (!(mask & (1u << i)))
        continue;
      const int r = 15 - i;
      a -= uint32_t(sz);
      const uint32_t v = (r >= 8 && r - 8 == reg) ? start : (r >= 8) ? st.a[r - 8] : st.d[r];
      if (sz == 4)
      {
        W16(a + 2, v & 0xFFFF);   // the low word goes out first
        W16(a, v >> 16);
      }
      else
        W16(a, v);
    }
    st.a[reg] = a;
    return;
  }
  uint32_t a = Address(mode, reg, sz);
  for (int i = 0; i < 16; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    WriteSized(a, sz, i >= 8 ? st.a[i - 8] : st.d[i]);
    a += uint32_t(sz);
  }
}

void M68000::MovemToRegisters(int mode, int reg, int sz)
{
  const uint32_t mask = Fetch16();
  uint32_t a = (mode == 3) ? st.a[reg] : Address(mode, reg, sz);
  for (int i = 0; i < 16; i++)
  {
    if (!(mask & (1u << i)))
      continue;
    const uint32_t v = (sz == 4) ? R32(a) : SignExtend16(R16(a));
    if (i >= 8)
      st.a[i - 8] = v;
    else
      st.d[i] = v;
    a += uint32_t(sz);
  }
  R16(a);   // the chip reads one word more
  if (mode == 3)
    st.a[reg] = a;
}

void M68000::Group4(uint32_t op)
{
  const int rx = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
  const int szf = (op >> 6) & 3;
  switch (Map().kind[op])
  {
  case LEA:
  {
    const uint32_t a = Address(mode, reg, 4);
    if (mode == 6 || (mode == 7 && reg == 3))
      st.cycles += 2;
    st.a[rx] = a;
    return;
  }
  case CHK:
  {
    const int32_t bound = int16_t(ReadEA(mode, reg, 2));
    const int32_t value = int16_t(st.d[rx]);
    st.cycles += 6;
    st.z = Bit(value == 0);
    st.v = st.c = st.n = 0;
    if (value < 0)
    {
      st.n = 1;
      Trap(6, 10);
    }
    else if (value > bound)
      Trap(6, 8);
    return;
  }
  case MOVE_FROM_SR:
  {
    const uint32_t sr = SR();
    if (mode == 0)
    {
      SetDn(reg, sr, 2);
      st.cycles += 2;
      return;
    }
    uint32_t a;
    RmwRead(mode, reg, 2, &a);   // the chip reads the operand first
    RmwWrite(mode, reg, 2, a, sr);
    return;
  }
  case CLR:  Unary(mode, reg, kSizeField[szf], 0); return;
  case NEG:  Unary(mode, reg, kSizeField[szf], 1); return;
  case NEGX: Unary(mode, reg, kSizeField[szf], 2); return;
  case NOT:  Unary(mode, reg, kSizeField[szf], 3); return;
  case MOVE_TO_CCR:
    SetCCR(ReadEA(mode, reg, 2) & 0x1F);
    st.cycles += 8;
    return;
  case MOVE_TO_SR:
    if (!st.s)
    {
      PrivilegeViolation();
      return;
    }
    SetSR(ReadEA(mode, reg, 2) & 0xA71F);
    st.cycles += 8;
    return;
  case NBCD:
  {
    if (mode == 0)
      st.cycles += 2;
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, 1, &a);
    RmwWrite(mode, reg, 1, a, DecimalSub(0, v));
    return;
  }
  case SWAP:
  {
    const uint32_t v = st.d[reg];
    const uint32_t r = (v >> 16) | (v << 16);
    st.d[reg] = r;
    SetLogic(r, 4);
    return;
  }
  case PEA:
  {
    const uint32_t a = Address(mode, reg, 4);
    if (mode == 6 || (mode == 7 && reg == 3))
      st.cycles += 2;
    Push32(a);
    return;
  }
  case EXT_W:
  {
    const uint32_t r = SignExtend8(st.d[reg] & 0xFF) & 0xFFFF;
    SetDn(reg, r, 2);
    SetLogic(r, 2);
    return;
  }
  case EXT_L:
  {
    const uint32_t r = SignExtend16(st.d[reg] & 0xFFFF);
    st.d[reg] = r;
    SetLogic(r, 4);
    return;
  }
  case MOVEM_TO_MEM:
    MovemToMemory(mode, reg, szf == 3 ? 4 : 2);
    return;
  case MOVEM_TO_REG:
    MovemToRegisters(mode, reg, szf == 3 ? 4 : 2);
    return;
  case TAS:
  {
    if (mode == 0)
    {
      SetLogic(st.d[reg] & 0xFF, 1);
      st.d[reg] |= 0x80;
      return;
    }
    const uint32_t a = Address(mode, reg, 1);
    const uint32_t v = R8(a);
    SetLogic(v, 1);
    st.cycles += 2;
    W8(a, v | 0x80);
    return;
  }
  case TST:
  {
    const int sz = kSizeField[szf];
    SetLogic(ReadEA(mode, reg, sz), sz);
    return;
  }
  case TRAP:
    Trap(32 + (op & 15), 10);
    return;
  case LINK:
  {
    const uint32_t disp = SignExtend16(Fetch16());
    Push32(st.a[reg]);   // LINK A7 pushes A7's value from before the push
    st.a[reg] = st.a[7];
    st.a[7] += disp;
    return;
  }
  case UNLK:
  {
    st.a[7] = st.a[reg];
    const uint32_t v = Pop32();
    st.a[reg] = v;
    return;
  }
  case MOVE_TO_USP:
    if (!st.s)
      PrivilegeViolation();
    else
      st.usp = st.a[reg];
    return;
  case MOVE_FROM_USP:
    if (!st.s)
      PrivilegeViolation();
    else
      st.a[reg] = st.usp;
    return;
  case RESET:
    if (!st.s)
    {
      PrivilegeViolation();
      return;
    }
    if (onReset != nullptr)
      onReset(user);
    st.cycles += 128;
    return;
  case NOP:
    return;
  case STOP:
    if (!st.s)
    {
      PrivilegeViolation();
      return;
    }
    SetSR(Fetch16() & 0xA71F);
    st.stopped = true;
    return;
  case RTE:
  {
    if (!st.s)
    {
      PrivilegeViolation();
      return;
    }
    const uint32_t sr = Pop16();
    const uint32_t pc = Pop32();
    SetSR(sr & 0xA71F);
    st.pc = pc;
    st.cycles += 4;
    return;
  }
  case RTS:
    st.pc = Pop32();
    st.cycles += 4;
    return;
  case TRAPV:
    if (st.v)
      Trap(7, 10);
    return;
  case RTR:
  {
    const uint32_t ccr = Pop16();
    SetCCR(ccr & 0x1F);
    st.pc = Pop32();
    st.cycles += 4;
    return;
  }
  case JSR:
  {
    const uint32_t a = Address(mode, reg, 4);
    st.cycles += JumpExtra(mode, reg);
    Push32(st.pc);
    st.pc = a;
    return;
  }
  case JMP:
  {
    const uint32_t a = Address(mode, reg, 4);
    st.cycles += JumpExtra(mode, reg);
    st.pc = a;
    return;
  }
  default:
    return;
  }
}

void M68000::Group5(uint32_t op)
{
  const int mode = (op >> 3) & 7, reg = op & 7;
  const uint32_t cc = (op >> 8) & 15;
  switch (Map().kind[op])
  {
  case DBCC:
  {
    const uint32_t base = st.pc;
    const uint32_t disp = SignExtend16(Fetch16());
    if (Cond(cc))
    {
      st.cycles += 4;
      return;
    }
    const uint32_t count = (st.d[reg] - 1) & 0xFFFF;
    SetDn(reg, count, 2);
    if (count != 0xFFFF)
    {
      st.pc = base + disp;
      st.cycles += 2;
    }
    else
      st.cycles += 6;
    return;
  }
  case SCC:
  {
    const bool t = Cond(cc);
    const uint32_t v = t ? 0xFF : 0;
    if (mode == 0)
    {
      SetDn(reg, v, 1);
      if (t)
        st.cycles += 2;
      return;
    }
    uint32_t a;
    RmwRead(mode, reg, 1, &a);
    RmwWrite(mode, reg, 1, a, v);
    return;
  }
  default:
    break;
  }

  uint32_t q = (op >> 9) & 7;
  if (q == 0)
    q = 8;
  const bool isSub = (op & 0x100) != 0;
  if (Map().kind[op] == QUICK_TO_AREG)
  {
    if (isSub)
      st.a[reg] -= q;
    else
      st.a[reg] += q;
    st.cycles += 4;
    return;
  }
  const int sz = kSizeField[(op >> 6) & 3];
  if (mode == 0 && sz == 4)
    st.cycles += 4;
  uint32_t a;
  const uint32_t v = RmwRead(mode, reg, sz, &a);
  const uint32_t r = isSub ? Sub(v, q, sz, 0) : Add(v, q, sz, 0);
  st.x = st.c;
  st.z = Bit(r == 0);
  RmwWrite(mode, reg, sz, a, r);
}

void M68000::Branch(uint32_t op)
{
  const uint32_t cc = (op >> 8) & 15, d8 = op & 0xFF;
  const uint32_t base = st.pc;
  const uint32_t disp = d8 ? SignExtend8(d8) : SignExtend16(Fetch16());
  if (cc == 1)   // BSR
  {
    st.cycles += d8 ? 6 : 2;
    Push32(st.pc);
    st.pc = base + disp;
    return;
  }
  if (Cond(cc))
  {
    st.pc = base + disp;
    st.cycles += d8 ? 6 : 2;
  }
  else
    st.cycles += 4;
}

void M68000::LogicOp(uint32_t op, bool isOr)
{
  const int rx = (op >> 9) & 7, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  const int sz = kSizeField[opmode & 3];
  if (opmode >= 4)
  {
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, sz, &a);
    const uint32_t r = Clip(isOr ? (v | st.d[rx]) : (v & st.d[rx]), sz);
    SetLogic(r, sz);
    RmwWrite(mode, reg, sz, a, r);
    return;
  }
  const uint32_t v = ReadEA(mode, reg, sz);
  const uint32_t r = Clip(isOr ? (st.d[rx] | v) : (st.d[rx] & v), sz);
  SetDn(rx, r, sz);
  SetLogic(r, sz);
  if (sz == 4)
    st.cycles += LongExtra(mode, reg);
}

void M68000::Bcd(uint32_t op, bool isAdd)
{
  const int rx = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
  uint32_t s, d, a = 0;
  if (mode == 0)
  {
    s = st.d[reg] & 0xFF;
    d = st.d[rx] & 0xFF;
  }
  else
  {
    st.a[reg] -= StepOf(1, reg);
    s = R8(st.a[reg]);
    st.a[rx] -= StepOf(1, rx);
    a = st.a[rx];
    d = R8(a);
  }
  st.cycles += 2;
  const uint32_t r = isAdd ? DecimalAdd(d, s) : DecimalSub(d, s);
  if (mode == 0)
    SetDn(rx, r, 1);
  else
    W8(a, r);
}

void M68000::OrDivSbcd(uint32_t op)
{
  switch (Map().kind[op])
  {
  case DIVIDE:
  {
    const uint32_t src = ReadEA((op >> 3) & 7, op & 7, 2);
    Divide((op >> 9) & 7, src, ((op >> 6) & 7) == 7);
    return;
  }
  case SBCD:
    Bcd(op, false);
    return;
  default:
    LogicOp(op, true);
    return;
  }
}

void M68000::AddSub(uint32_t op, bool isAdd)
{
  const int rx = (op >> 9) & 7, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  switch (Map().kind[op])
  {
  case ADDSUB_A:
  {
    const int sz = (opmode == 7) ? 4 : 2;
    uint32_t v = ReadEA(mode, reg, sz);
    if (sz == 2)
      v = SignExtend16(v);
    if (isAdd)
      st.a[rx] += v;
    else
      st.a[rx] -= v;
    st.cycles += (sz == 2) ? 4 : LongExtra(mode, reg);
    return;
  }
  case ADDSUB_X:
  {
    const int sz = kSizeField[opmode & 3];
    uint32_t s, d, a = 0;
    if (mode == 0)
    {
      s = Clip(st.d[reg], sz);
      d = Clip(st.d[rx], sz);
      if (sz == 4)
        st.cycles += 4;
    }
    else
    {
      st.a[reg] -= StepOf(sz, reg);
      s = ReadSized(st.a[reg], sz);
      st.a[rx] -= StepOf(sz, rx);
      a = st.a[rx];
      d = ReadSized(a, sz);
      st.cycles += 2;
    }
    const uint32_t z = st.z;
    const uint32_t r = isAdd ? Add(d, s, sz, st.x) : Sub(d, s, sz, st.x);
    st.x = st.c;
    st.z = r ? 0 : z;
    if (mode == 0)
      SetDn(rx, r, sz);
    else
      WriteSized(a, sz, r);
    return;
  }
  case ADDSUB_TO_EA:
  {
    const int sz = kSizeField[opmode & 3];
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, sz, &a);
    const uint32_t src = Clip(st.d[rx], sz);
    const uint32_t r = isAdd ? Add(v, src, sz, 0) : Sub(v, src, sz, 0);
    st.x = st.c;
    st.z = Bit(r == 0);
    RmwWrite(mode, reg, sz, a, r);
    return;
  }
  default:   // ADDSUB_TO_DREG
  {
    const int sz = kSizeField[opmode & 3];
    const uint32_t v = ReadEA(mode, reg, sz), d = Clip(st.d[rx], sz);
    const uint32_t r = isAdd ? Add(d, v, sz, 0) : Sub(d, v, sz, 0);
    st.x = st.c;
    st.z = Bit(r == 0);
    SetDn(rx, r, sz);
    if (sz == 4)
      st.cycles += LongExtra(mode, reg);
    return;
  }
  }
}

void M68000::CmpEor(uint32_t op)
{
  const int rx = (op >> 9) & 7, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  const int sz = kSizeField[opmode & 3];
  switch (Map().kind[op])
  {
  case CMPA:
  {
    const int asz = (opmode == 7) ? 4 : 2;
    uint32_t v = ReadEA(mode, reg, asz);
    if (asz == 2)
      v = SignExtend16(v);
    st.z = Bit(Sub(st.a[rx], v, 4, 0) == 0);
    st.cycles += 2;
    return;
  }
  case CMP:
  {
    const uint32_t v = ReadEA(mode, reg, sz);
    st.z = Bit(Sub(Clip(st.d[rx], sz), v, sz, 0) == 0);
    if (sz == 4)
      st.cycles += 2;
    return;
  }
  case CMPM:
  {
    const uint32_t s = ReadSized(Address(3, reg, sz), sz);
    const uint32_t d = ReadSized(Address(3, rx, sz), sz);
    st.z = Bit(Sub(d, s, sz, 0) == 0);
    return;
  }
  default:   // EOR Dn,<ea>
  {
    if (mode == 0)
    {
      const uint32_t r = Clip(st.d[reg] ^ st.d[rx], sz);
      SetDn(reg, r, sz);
      SetLogic(r, sz);
      if (sz == 4)
        st.cycles += 4;
      return;
    }
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, sz, &a);
    const uint32_t r = Clip(v ^ st.d[rx], sz);
    SetLogic(r, sz);
    RmwWrite(mode, reg, sz, a, r);
    return;
  }
  }
}

void M68000::AndMulAbcdExg(uint32_t op)
{
  const int rx = (op >> 9) & 7, opmode = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
  switch (Map().kind[op])
  {
  case MULTIPLY:
  {
    const uint32_t s = ReadEA(mode, reg, 2);
    uint32_t r, ones;
    if (opmode == 7)   // MULS: time grows with each 01/10 pair in the source
    {
      r = uint32_t(int32_t(int16_t(s)) * int32_t(int16_t(st.d[rx])));
      ones = Ones16((s ^ (s << 1)) & 0xFFFF);
    }
    else               // MULU: time grows with each 1 bit in the source
    {
      r = s * (st.d[rx] & 0xFFFF);
      ones = Ones16(s);
    }
    st.d[rx] = r;
    SetLogic(r, 4);
    st.cycles += 34 + 2 * int64_t(ones);
    return;
  }
  case ABCD:
    Bcd(op, true);
    return;
  case EXG_DD:
  {
    const uint32_t t = st.d[rx];
    st.d[rx] = st.d[reg];
    st.d[reg] = t;
    st.cycles += 2;
    return;
  }
  case EXG_AA:
  {
    const uint32_t t = st.a[rx];
    st.a[rx] = st.a[reg];
    st.a[reg] = t;
    st.cycles += 2;
    return;
  }
  case EXG_DA:
  {
    const uint32_t t = st.d[rx];
    st.d[rx] = st.a[reg];
    st.a[reg] = t;
    st.cycles += 2;
    return;
  }
  default:
    LogicOp(op, false);
    return;
  }
}

void M68000::ShiftRotate(uint32_t op)
{
  const int rx = (op >> 9) & 7, mode = (op >> 3) & 7, reg = op & 7;
  const bool left = (op & 0x100) != 0;
  if (Map().kind[op] == SHIFT_MEMORY)   // one bit of a word in memory
  {
    uint32_t a;
    const uint32_t v = RmwRead(mode, reg, 2, &a);
    RmwWrite(mode, reg, 2, a, Shift((op >> 9) & 3, left, v, 1, 2));
    return;
  }
  const int sz = kSizeField[(op >> 6) & 3];
  uint32_t count;
  if (op & 0x20)
    count = st.d[rx] & 63;
  else
    count = rx ? uint32_t(rx) : 8;
  st.cycles += (sz == 4 ? 4 : 2) + 2 * int64_t(count);
  SetDn(reg, Shift((op >> 3) & 3, left, Clip(st.d[reg], sz), count, sz), sz);
}
