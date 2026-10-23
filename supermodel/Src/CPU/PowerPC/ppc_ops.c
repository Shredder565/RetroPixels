// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// ppc_ops.c: the 603e instruction set. Included by ppc.cpp only (it is C++
// despite the name, which the build lists keep).
//
// ExecuteOne() decodes one instruction word on its primary opcode (bits
// 0-5) and, for the four extended groups, on the extended opcode, and runs
// it. Instructions the Model 3 has no use for (cache control, TLB
// management, eciwx/ecowx) are accepted as no-operations, except the
// external control ones, which stop the CPU.
//
// Some results follow the core's long-standing behaviour rather than the
// architecture books where the two differ (games were tuned against it):
//   - sraw reads rS again after writing rA when working out CA, so the
//     carry differs when rA and rS are the same register;
//   - subfze computes the overflow from rA after the result is written;
//   - adde/addme overflow ignores the carry in the addend for adde;
//   - stwcx. always succeeds while a reservation is held;
//   - mtfsfi to field 0 also ORs the immediate into FPSCR bits 28-31
//     (the rounding mode and non-IEEE bits);
//   - the FPSCR only tracks FPRF, the comparison result and the signaling
//     NaN bits: a signaling NaN operand sets FX (bit 0) rather than VXSNAN,
//     except in the compares, which also set VXSNAN.

namespace {

/******************************************************************************
 Memory
******************************************************************************/

inline UINT32 Load8(UINT32 ea)  { return bus->Read8(ea); }
inline UINT32 Load16(UINT32 ea) { return bus->Read16(ea); }
inline UINT32 Load32(UINT32 ea) { return bus->Read32(ea); }
inline UINT64 Load64(UINT32 ea) { return bus->Read64(ea); }
inline void Store8(UINT32 ea, UINT32 v)  { bus->Write8(ea, (UINT8) v); }
inline void Store16(UINT32 ea, UINT32 v) { bus->Write16(ea, (UINT16) v); }
inline void Store32(UINT32 ea, UINT32 v) { bus->Write32(ea, v); }
inline void Store64(UINT32 ea, UINT64 v) { bus->Write64(ea, v); }

// lswi/lswx: bytes fill registers from the most significant end, wrapping
// from r31 to r0; each register is cleared when the first byte goes in.
void LoadString(UINT32 ea, unsigned count, unsigned firstReg)
{
  unsigned reg = (firstReg + 31) & 31;
  unsigned shift = 24;
  for (; count > 0; count--, ea++)
  {
    if (shift == 24)
    {
      reg = (reg + 1) & 31;
      cpu.gpr[reg] = 0;
    }
    cpu.gpr[reg] |= Load8(ea) << shift;
    shift = (shift == 0) ? 24 : shift - 8;
  }
}

void StoreString(UINT32 ea, unsigned count, unsigned firstReg)
{
  unsigned reg = (firstReg + 31) & 31;
  unsigned shift = 24;
  for (; count > 0; count--, ea++)
  {
    if (shift == 24)
      reg = (reg + 1) & 31;
    Store8(ea, (cpu.gpr[reg] >> shift) & 0xFF);
    shift = (shift == 0) ? 24 : shift - 8;
  }
}

// Single precision values go through the host's float type both ways
// (loads widen, stores round with the current host mode).
inline double WidenSingle(UINT32 bits)
{
  float f;
  memcpy(&f, &bits, sizeof(f));
  return (double) f;
}

inline UINT32 NarrowToSingle(double d)
{
  float f = (float) d;
  UINT32 bits;
  memcpy(&bits, &f, sizeof(bits));
  return bits;
}

/******************************************************************************
 FPU helpers
******************************************************************************/

constexpr UINT64 kDblSign  = 0x8000000000000000ULL;
constexpr UINT64 kDblExp   = 0x7FF0000000000000ULL;
constexpr UINT64 kDblFrac  = 0x000FFFFFFFFFFFFFULL;
constexpr UINT64 kDblQuiet = 0x0008000000000000ULL;

inline UINT64 Magnitude(UINT64 b)    { return b & ~kDblSign; }
inline bool IsNan(UINT64 b)          { return Magnitude(b) > kDblExp; }
inline bool IsQuietNan(UINT64 b)     { return Magnitude(b) >= (kDblExp | kDblQuiet); }
inline bool IsSignalingNan(UINT64 b) { return IsNan(b) && !IsQuietNan(b); }

constexpr UINT32 kFpscrFx     = 0x80000000;
constexpr UINT32 kFpscrFex    = 0x40000000;
constexpr UINT32 kFpscrVx     = 0x20000000;
constexpr UINT32 kFpscrVxsnan = 0x01000000;
constexpr UINT32 kFpscrVxvc   = 0x00080000;
constexpr UINT32 kFpscrFprf   = 0x0001F000;

inline void NoteSignaling(unsigned a)
{
  if (IsSignalingNan(cpu.fpr[a]))
    cpu.fpscr |= kFpscrFx;
}

inline void NoteSignaling(unsigned a, unsigned b)
{
  if (IsSignalingNan(cpu.fpr[a]) || IsSignalingNan(cpu.fpr[b]))
    cpu.fpscr |= kFpscrFx;
}

// FPSCR[FPRF] class code of a result.
UINT32 ResultClass(UINT64 b)
{
  bool negative = (b & kDblSign) != 0;
  UINT64 mag = Magnitude(b);
  UINT64 exponent = b & kDblExp;
  if (mag >= (kDblExp | kDblQuiet))
    return 0x11;                                    // quiet NaN
  if (mag == kDblExp)
    return negative ? 0x09 : 0x05;                  // infinity
  if (exponent != 0 && exponent < kDblExp)
    return negative ? 0x08 : 0x04;                  // normalized
  if (mag != 0 && mag <= kDblFrac)
    return negative ? 0x18 : 0x14;                  // denormalized
  return negative ? 0x12 : 0x02;                    // zero (and signaling NaN)
}

inline void SetResultClass(unsigned t)
{
  cpu.fpscr = (cpu.fpscr & ~kFpscrFprf) | (ResultClass(cpu.fpr[t]) << 12);
}

// Runs an arithmetic operation in the guest's rounding mode (FPSCR[RN]).
template <class Op>
inline double Rounded(Op op)
{
  static const int kHostMode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
  unsigned rn = cpu.fpscr & 3;
  if (rn != 0)
    fesetround(kHostMode[rn]);
  double result = op();
  if (rn != 0)
    fesetround(FE_TONEAREST);
  return result;
}

// Finishes an arithmetic instruction: stores the result, sets FPRF and
// optionally CR1.
inline void FinishArith(UINT32 op, unsigned t, double result)
{
  SetFprValue(t, result);
  SetResultClass(t);
  if (RecordBit(op))
    RecordCr1();
}

// Conversion to a 64-bit integer the way x86-64 does it: values out of range
// (and NaN) give the "integer indefinite" value.
inline INT64 ToInt64(double d)
{
  if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
    return (INT64) 0x8000000000000000ULL;
  return (INT64) d;
}

// fctiw/fctiwz: saturate to a signed word. The word goes in the low half of
// frD; the high half is zero, or all ones for the negative saturation value.
void ConvertToWord(UINT32 op, INT64 r)
{
  unsigned t = FieldD(op);
  if (r > 0x7FFFFFFF)
    cpu.fpr[t] = 0x7FFFFFFF;
  else if (r < -0x80000000LL)
    cpu.fpr[t] = 0xFFFFFFFF80000000ULL;
  else
    cpu.fpr[t] = (UINT32) r;
  if (RecordBit(op))
    RecordCr1();
}

void FloatCompare(UINT32 op, bool ordered)
{
  unsigned a = FieldA(op), b = FieldB(op);
  UINT64 x = cpu.fpr[a], y = cpu.fpr[b];
  NoteSignaling(a, b);
  UINT32 c;
  if (IsNan(x) || IsNan(y))
  {
    c = 1;                                          // unordered
    if (IsSignalingNan(x) || IsSignalingNan(y))
    {
      cpu.fpscr |= kFpscrVxsnan;
      if (ordered && (!(cpu.fpscr & kFpscrFex) || IsQuietNan(x) || IsQuietNan(y)))
        cpu.fpscr |= kFpscrVxvc;
    }
  }
  else
  {
    double dx = FprValue(a), dy = FprValue(b);
    c = (dx < dy) ? 8 : ((dx > dy) ? 4 : 2);
  }
  cpu.cr[FieldCrD(op)] = (UINT8) c;
  cpu.fpscr = (cpu.fpscr & ~kFpscrFprf) | (c << 12);
}

/******************************************************************************
 Primary opcode 31: integer, logical, indexed load/store, system
******************************************************************************/

void ExecuteGroup31(UINT32 op)
{
  const unsigned d = FieldD(op), a = FieldA(op), b = FieldB(op);
  UINT32 &rd = cpu.gpr[d];
  UINT32 &ra = cpu.gpr[a];
  const UINT32 vb = cpu.gpr[b];

  switch (FieldXO(op))
  {
  // Arithmetic (XO form: rD, rA, rB; the OE bit is part of the opcode)
  case 266: case 778:   // add
  {
    UINT32 x = ra, r = x + vb;
    rd = r;
    if (OverflowBit(op)) SetOverflow(AddOverflows(r, x, vb));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 10: case 522:    // addc
  {
    UINT32 x = ra, r = x + vb;
    rd = r;
    SetCarry(r < x);
    if (OverflowBit(op)) SetOverflow(AddOverflows(r, x, vb));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 138: case 650:   // adde
  {
    UINT32 x = ra, c = CarryIn();
    UINT32 partial = vb + c, r = x + partial;
    rd = r;
    SetCarry(partial < vb || r < x);
    if (OverflowBit(op)) SetOverflow(AddOverflows(r, x, vb));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 234: case 746:   // addme
  {
    UINT32 x = ra, c = CarryIn();
    UINT32 partial = x + c, r = partial - 1;
    rd = r;
    SetCarry(partial < x || r < partial);
    if (OverflowBit(op)) SetOverflow(AddOverflows(r, x, c - 1));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 202: case 714:   // addze
  {
    UINT32 x = ra, c = CarryIn(), r = x + c;
    rd = r;
    SetCarry(r < x);
    if (OverflowBit(op)) SetOverflow(AddOverflows(r, x, c));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 40: case 552:    // subf
  {
    UINT32 x = ra, r = vb - x;
    rd = r;
    if (OverflowBit(op)) SetOverflow(SubOverflows(r, vb, x));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 8: case 520:     // subfc
  {
    UINT32 x = ra, r = vb - x;
    rd = r;
    SetCarry(vb >= x);
    if (OverflowBit(op)) SetOverflow(SubOverflows(r, vb, x));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 136: case 648:   // subfe
  {
    UINT32 x = ra, nx = ~x, partial = nx + CarryIn(), r = vb + partial;
    rd = r;
    SetCarry(partial < nx || r < partial);
    if (OverflowBit(op)) SetOverflow(SubOverflows(r, vb, x));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 232: case 744:   // subfme
  {
    UINT32 x = ra, nx = ~x, c = CarryIn(), partial = nx + c, r = partial - 1;
    rd = r;
    SetCarry(nx >= c || r < partial);
    if (OverflowBit(op)) SetOverflow(SubOverflows(r, 0xFFFFFFFF, x));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 200: case 712:   // subfze
  {
    UINT32 nx = ~ra, r = nx + CarryIn();
    rd = r;
    SetCarry(r < nx);
    if (OverflowBit(op)) SetOverflow(SubOverflows(r, 0, ra));
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 104: case 616:   // neg
  {
    UINT32 r = 0u - ra;
    rd = r;
    // Only 0x80000000 overflows (the old build never flagged it).
    if (OverflowBit(op)) SetOverflow(r == 0x80000000 ? 0x80000000 : 0);
    if (RecordBit(op)) RecordCr0(r);
    return;
  }
  case 235: case 747:   // mullw
  {
    INT64 p = (INT64) (INT32) ra * (INT64) (INT32) vb;
    rd = (UINT32) p;
    if (OverflowBit(op)) SetOverflow(p != (INT64) (INT32) p ? 0x80000000 : 0);
    if (RecordBit(op)) RecordCr0(rd);
    return;
  }
  case 75:              // mulhw
  {
    INT64 p = (INT64) (INT32) ra * (INT64) (INT32) vb;
    rd = (UINT32) ((UINT64) p >> 32);
    if (RecordBit(op)) RecordCr0(rd);
    return;
  }
  case 11:              // mulhwu
    rd = (UINT32) (((UINT64) ra * vb) >> 32);
    if (RecordBit(op)) RecordCr0(rd);
    return;
  case 491: case 1003:  // divw (undefined cases give 0 or -1 and set OV)
  {
    UINT32 x = ra;
    bool overflow = (vb == 0) || (vb == 0xFFFFFFFF && x == 0x80000000);
    if (overflow)
    {
      rd = (vb == 0 && x < 0x80000000) ? 0 : 0xFFFFFFFF;
      if (OverflowBit(op)) cpu.xer |= kXerSo | kXerOv;
    }
    else
    {
      rd = (UINT32) ((INT32) x / (INT32) vb);
      if (OverflowBit(op)) cpu.xer &= ~kXerOv;
    }
    if (RecordBit(op)) RecordCr0(rd);
    return;
  }
  case 459: case 971:   // divwu
    if (vb == 0)
    {
      rd = 0;
      if (OverflowBit(op)) cpu.xer |= kXerSo | kXerOv;
    }
    else
    {
      rd = ra / vb;
      if (OverflowBit(op)) cpu.xer &= ~kXerOv;
    }
    if (RecordBit(op)) RecordCr0(rd);
    return;

  // Compares and traps
  case 0:               // cmp
    cpu.cr[FieldCrD(op)] = CompareSigned((INT32) ra, (INT32) vb);
    return;
  case 32:              // cmpl
    cpu.cr[FieldCrD(op)] = CompareUnsigned(ra, vb);
    return;
  case 4:               // tw
  {
    INT32 x = (INT32) ra, y = (INT32) vb;
    if (((d & 16) && x < y) || ((d & 8) && x > y) || ((d & 4) && x == y) ||
        ((d & 2) && (UINT32) x < (UINT32) y) || ((d & 1) && (UINT32) x > (UINT32) y))
      TakeTrap();
    return;
  }

  // Logical (X form: rA, rS, rB)
  case 28:  ra = rd & vb;    break;   // and
  case 60:  ra = rd & ~vb;   break;   // andc
  case 124: ra = ~(rd | vb); break;   // nor
  case 284: ra = ~(rd ^ vb); break;   // eqv
  case 316: ra = rd ^ vb;    break;   // xor
  case 412: ra = rd | ~vb;   break;   // orc
  case 444: ra = rd | vb;    break;   // or
  case 476: ra = ~(rd & vb); break;   // nand
  case 922: ra = (UINT32) (INT32) (INT16) rd; break;   // extsh
  case 954: ra = (UINT32) (INT32) (INT8) rd;  break;   // extsb
  case 26:                            // cntlzw
  {
    UINT32 v = rd, n = 0;
    while (n < 32 && !(v & (0x80000000u >> n)))
      n++;
    ra = n;
    break;
  }

  // Shifts
  case 24:                            // slw
  {
    unsigned sh = vb & 0x3F;
    ra = (sh > 31) ? 0 : (rd << sh);
    break;
  }
  case 536:                           // srw
  {
    unsigned sh = vb & 0x3F;
    ra = (sh > 31) ? 0 : (rd >> sh);
    break;
  }
  case 792:                           // sraw
  {
    unsigned sh = vb & 0x3F;
    cpu.xer &= ~kXerCa;
    if (sh > 31)
    {
      ra = (rd & 0x80000000) ? 0xFFFFFFFF : 0;
      if (ra)
        cpu.xer |= kXerCa;
    }
    else
    {
      ra = (UINT32) ((INT32) rd >> sh);
      if ((INT32) rd < 0 && (rd & (UINT32) ((1ULL << sh) - 1)))
        cpu.xer |= kXerCa;
    }
    break;
  }
  case 824:                           // srawi
  {
    unsigned sh = b;
    UINT32 v = rd;
    cpu.xer &= ~kXerCa;
    if ((INT32) v < 0 && (v & (UINT32) ((1ULL << sh) - 1)))
      cpu.xer |= kXerCa;
    ra = (UINT32) ((INT32) v >> sh);
    break;
  }

  // Indexed loads
  case 23:  rd = Load32(EaIndexed(op)); return;               // lwzx
  case 87:  rd = Load8(EaIndexed(op)); return;                // lbzx
  case 279: rd = Load16(EaIndexed(op)); return;               // lhzx
  case 343: rd = (UINT32) (INT32) (INT16) Load16(EaIndexed(op)); return;   // lhax
  case 534: rd = Swap32(Load32(EaIndexed(op))); return;       // lwbrx
  case 790: rd = Swap16((UINT16) Load16(EaIndexed(op))); return;   // lhbrx
  case 55:  { UINT32 ea = EaIndexedUpdate(op); rd = Load32(ea); ra = ea; return; }   // lwzux
  case 119: { UINT32 ea = EaIndexedUpdate(op); rd = Load8(ea); ra = ea; return; }    // lbzux
  case 311: { UINT32 ea = EaIndexedUpdate(op); rd = Load16(ea); ra = ea; return; }   // lhzux
  case 375: { UINT32 ea = EaIndexedUpdate(op); rd = (UINT32) (INT32) (INT16) Load16(ea); ra = ea; return; }   // lhaux
  case 20:                            // lwarx
  {
    UINT32 ea = EaIndexed(op);
    cpu.reservedAddr = ea;
    cpu.reserved = 1;
    rd = Load32(ea);
    return;
  }
  case 533:                           // lswx
    LoadString(EaIndexed(op), cpu.xer & 0x7F, d);
    return;
  case 597:                           // lswi
    LoadString(a ? ra : 0, b ? b : 32, d);
    return;

  // Indexed stores
  case 151: Store32(EaIndexed(op), rd); return;               // stwx
  case 215: Store8(EaIndexed(op), rd); return;                // stbx
  case 407: Store16(EaIndexed(op), rd); return;               // sthx
  case 662: Store32(EaIndexed(op), Swap32(rd)); return;       // stwbrx
  case 918: Store16(EaIndexed(op), Swap16((UINT16) rd)); return;   // sthbrx
  case 183: { UINT32 ea = EaIndexedUpdate(op); Store32(ea, rd); ra = ea; return; }  // stwux
  case 247: { UINT32 ea = EaIndexedUpdate(op); Store8(ea, rd); ra = ea; return; }   // stbux
  case 439: { UINT32 ea = EaIndexedUpdate(op); Store16(ea, rd); ra = ea; return; }  // sthux
  case 150:                           // stwcx.
  {
    UINT32 ea = EaIndexed(op);
    if (cpu.reserved)
    {
      Store32(ea, rd);
      cpu.reserved = 0;
      cpu.reservedAddr = 0;
      cpu.cr[0] = 2;
    }
    else
      cpu.cr[0] = 0;
    if (cpu.xer & kXerSo)
      cpu.cr[0] |= 1;
    return;
  }
  case 661:                           // stswx
    StoreString(EaIndexed(op), cpu.xer & 0x7F, d);
    return;
  case 725:                           // stswi
    StoreString(a ? ra : 0, b ? b : 32, d);
    return;

  // Indexed FPU loads and stores
  case 535: SetFprValue(d, WidenSingle(Load32(EaIndexed(op)))); return;   // lfsx
  case 599: cpu.fpr[d] = Load64(EaIndexed(op)); return;                   // lfdx
  case 567: { UINT32 ea = EaIndexedUpdate(op); SetFprValue(d, WidenSingle(Load32(ea))); ra = ea; return; }  // lfsux
  case 631: { UINT32 ea = EaIndexedUpdate(op); cpu.fpr[d] = Load64(ea); ra = ea; return; }                 // lfdux
  case 663: Store32(EaIndexed(op), NarrowToSingle(FprValue(d))); return;  // stfsx
  case 727: Store64(EaIndexed(op), cpu.fpr[d]); return;                   // stfdx
  case 983: Store32(EaIndexed(op), (UINT32) cpu.fpr[d]); return;          // stfiwx
  case 695: { UINT32 ea = EaIndexedUpdate(op); Store32(ea, NarrowToSingle(FprValue(d))); ra = ea; return; } // stfsux
  case 759: { UINT32 ea = EaIndexedUpdate(op); Store64(ea, cpu.fpr[d]); ra = ea; return; }                 // stfdux

  // Condition register and special registers
  case 19:  rd = PackedCr(); return;                          // mfcr
  case 144:                                                   // mtcrf
  {
    unsigned fxm = (op >> 12) & 0xFF;
    UINT32 v = rd;
    for (int i = 0; i < 8; i++)
      if (fxm & (0x80 >> i))
        cpu.cr[i] = (v >> (28 - 4 * i)) & 0xF;
    return;
  }
  case 512:                                                   // mcrxr
    cpu.cr[FieldCrD(op)] = (cpu.xer >> 28) & 0xF;
    cpu.xer &= 0x0FFFFFFF;
    return;
  case 83:  rd = cpu.msr; return;                             // mfmsr
  case 146: SetMsr(rd); return;                               // mtmsr
  case 339: rd = ReadSpr(SprField(op)); return;               // mfspr
  case 467: WriteSpr(SprField(op), rd); return;               // mtspr
  case 371:                                                   // mftb
    switch (SprField(op))
    {
    case 268: rd = (UINT32) ReadTimeBase(); return;
    case 269: rd = (UINT32) (ReadTimeBase() >> 32); return;
    default:
      DebugLog("PowerPC: mftb %u at %08X\n", SprField(op), cpu.pc);
      Stop("read from an unknown time base register");
      return;
    }
  case 595: rd = cpu.sr[(op >> 16) & 15]; return;             // mfsr
  case 659: rd = cpu.sr[vb >> 28]; return;                    // mfsrin
  case 210: cpu.sr[(op >> 16) & 15] = rd; return;             // mtsr
  case 242: cpu.sr[vb >> 28] = rd; return;                    // mtsrin

  // Cache, TLB and ordering: nothing to do without caches or an MMU.
  case 54:   // dcbst
  case 86:   // dcbf
  case 246:  // dcbtst
  case 278:  // dcbt
  case 470:  // dcbi
  case 758:  // dcba
  case 982:  // icbi
  case 1014: // dcbz (the data is left as it is)
  case 306:  // tlbie
  case 370:  // tlbia
  case 566:  // tlbsync
  case 598:  // sync
  case 854:  // eieio
    return;

  case 310:  // eciwx
  case 438:  // ecowx
    DebugLog("PowerPC: external control instruction %08X at %08X\n", op, cpu.pc);
    Stop("hit an unimplemented instruction");
    return;

  default:
    DebugLog("PowerPC: invalid instruction %08X at %08X\n", op, cpu.pc);
    Stop("hit an invalid instruction");
    return;
  }

  // Logical and shift instructions fall through to here for Rc.
  if (RecordBit(op))
    RecordCr0(ra);
}

/******************************************************************************
 Primary opcode 19: branches to LR/CTR, condition register logic, rfi
******************************************************************************/

void ExecuteGroup19(UINT32 op)
{
  const unsigned d = FieldD(op), a = FieldA(op), b = FieldB(op);
  switch (FieldXO(op))
  {
  case 16:                            // bclr
    if (BranchTaken(op))
    {
      cpu.nextPc = cpu.lr & ~3u;
      FetchFrom(cpu.nextPc);
    }
    if (LinkBit(op))
      cpu.lr = cpu.pc + 4;
    return;
  case 528:                           // bcctr
    if (BranchTaken(op))
    {
      cpu.nextPc = cpu.ctr & ~3u;
      FetchFrom(cpu.nextPc);
    }
    if (LinkBit(op))
      cpu.lr = cpu.pc + 4;
    return;
  case 0:                             // mcrf
    cpu.cr[d >> 2] = cpu.cr[a >> 2];
    return;
  case 33:  SetCrBit(d, !(CrBit(a) | CrBit(b))); return;   // crnor
  case 129: SetCrBit(d, CrBit(a) & !CrBit(b)); return;     // crandc
  case 193: SetCrBit(d, CrBit(a) ^ CrBit(b)); return;      // crxor
  case 225: SetCrBit(d, !(CrBit(a) & CrBit(b))); return;   // crnand
  case 257: SetCrBit(d, CrBit(a) & CrBit(b)); return;      // crand
  case 289: SetCrBit(d, !(CrBit(a) ^ CrBit(b))); return;   // creqv
  case 417: SetCrBit(d, CrBit(a) | !CrBit(b)); return;     // crorc
  case 449: SetCrBit(d, CrBit(a) | CrBit(b)); return;      // cror
  case 50:                            // rfi
    cpu.nextPc = cpu.srr0;
    SetMsr(cpu.srr1);
    FetchFrom(cpu.nextPc);
    return;
  case 150:                           // isync
    return;
  default:
    DebugLog("PowerPC: invalid instruction %08X at %08X\n", op, cpu.pc);
    Stop("hit an invalid instruction");
    return;
  }
}

/******************************************************************************
 Primary opcodes 59 and 63: floating point arithmetic
******************************************************************************/

// The arithmetic shared by both groups; 'single' rounds the result to single
// precision (inside the guest rounding mode). Returns false if the opcode is
// not one of them.
// When an input is a NaN, the result is the first NaN among the inputs in
// the order the architecture gives (frA, frB, frC), made quiet. Doing this
// explicitly keeps the result independent of how the host compiler orders
// the operands of a commutative operation. A single precision result keeps
// only the top 23 fraction bits.
bool PickNan(double &out, bool single, unsigned x, int y = -1, int z = -1)
{
  const int order[3] = { (int) x, y, z };
  for (int reg : order)
  {
    if (reg >= 0 && IsNan(cpu.fpr[reg]))
    {
      UINT64 bits = cpu.fpr[reg] | kDblQuiet;
      if (single)
        bits &= ~0x1FFFFFFFULL;
      memcpy(&out, &bits, sizeof(out));
      return true;
    }
  }
  return false;
}

inline double Negated(double d)
{
  UINT64 bits;
  memcpy(&bits, &d, sizeof(bits));
  bits ^= kDblSign;
  memcpy(&d, &bits, sizeof(d));
  return d;
}

bool FloatArith(UINT32 op, bool single)
{
  const unsigned t = FieldD(op), a = FieldA(op), b = FieldB(op), c = FieldC(op);
  const double fa = FprValue(a), fb = FprValue(b), fc = FprValue(c);
  double r;

  // A-form multiply-add family: any frC field.
  switch (FieldXO(op) & 31)
  {
  case 25:                            // fmul(s)
    NoteSignaling(a, c);
    if (!PickNan(r, single, a, c))
      r = single ? Rounded([&] { return (double) (float) (fa * fc); })
                 : Rounded([&] { return fa * fc; });
    FinishArith(op, t, r);
    return true;
  case 29:                            // fmadd(s)
  case 28:                            // fmsub(s)
  case 31:                            // fnmadd(s)
  case 30:                            // fnmsub(s)
  {
    const unsigned form = FieldXO(op) & 31;
    const bool subtract = (form == 28 || form == 30);
    NoteSignaling(a, b);
    NoteSignaling(c);
    const bool negate = (form == 31 || form == 30);
    if (PickNan(r, single, a, b, c))
    {
      if (negate)
        r = Negated(r);
    }
    else
    {
      // The negation comes before rounding, which matters in the directed
      // rounding modes.
      const double addend = subtract ? -fb : fb;
      auto sum = [&] { double v = (fa * fc) + addend; return negate ? -v : v; };
      r = single ? Rounded([&] { return (double) (float) sum(); })
                 : Rounded(sum);
    }
    FinishArith(op, t, r);
    return true;
  }
  }

  // The rest need frC (and for one-operand forms frA) to be zero.
  switch (FieldXO(op))
  {
  case 21:                            // fadd(s)
    NoteSignaling(a, b);
    if (!PickNan(r, single, a, b))
      r = single ? Rounded([&] { return (double) (float) (fa + fb); })
                 : Rounded([&] { return fa + fb; });
    break;
  case 20:                            // fsub(s)
    NoteSignaling(a, b);
    if (!PickNan(r, single, a, b))
      r = single ? Rounded([&] { return (double) (float) (fa - fb); })
                 : Rounded([&] { return fa - fb; });
    break;
  case 18:                            // fdiv(s)
    NoteSignaling(a, b);
    if (!PickNan(r, single, a, b))
      r = single ? Rounded([&] { return (double) (float) (fa / fb); })
                 : Rounded([&] { return fa / fb; });
    break;
  case 22:                            // fsqrt(s) (not on the 603e, accepted anyway)
    NoteSignaling(b);
    r = single ? Rounded([&] { return (double) (float) sqrt(fb); })
               : Rounded([&] { return sqrt(fb); });
    break;
  default:
    return false;
  }
  FinishArith(op, t, r);
  return true;
}

void ExecuteGroup59(UINT32 op)
{
  if (FloatArith(op, true))
    return;
  if (FieldXO(op) == 24)              // fres: 1/x rounded to single
  {
    const unsigned b = FieldB(op);
    double fb = FprValue(b);
    NoteSignaling(b);
    FinishArith(op, FieldD(op), Rounded([&] { return (double) (float) (1.0 / fb); }));
    return;
  }
  DebugLog("PowerPC: invalid instruction %08X at %08X\n", op, cpu.pc);
  Stop("hit an invalid instruction");
}

void ExecuteGroup63(UINT32 op)
{
  const unsigned t = FieldD(op), a = FieldA(op), b = FieldB(op), c = FieldC(op);

  if ((FieldXO(op) & 31) == 23)       // fsel: no exceptions, no FPRF
  {
    bool pickB = IsNan(cpu.fpr[a]) || FprValue(a) < 0.0;
    cpu.fpr[t] = pickB ? cpu.fpr[b] : cpu.fpr[c];
    if (RecordBit(op))
      RecordCr1();
    return;
  }
  if (FloatArith(op, false))
    return;

  switch (FieldXO(op))
  {
  case 0:   FloatCompare(op, false); return;   // fcmpu
  case 32:  FloatCompare(op, true); return;    // fcmpo
  case 72:  cpu.fpr[t] = cpu.fpr[b]; break;                  // fmr
  case 40:  cpu.fpr[t] = cpu.fpr[b] ^ kDblSign; break;       // fneg
  case 264: cpu.fpr[t] = cpu.fpr[b] & ~kDblSign; break;      // fabs
  case 136: cpu.fpr[t] = cpu.fpr[b] | kDblSign; break;       // fnabs
  case 12:                            // frsp
  {
    double fb = FprValue(b);
    NoteSignaling(b);
    FinishArith(op, t, (double) (float) fb);
    return;
  }
  case 26:                            // frsqrte (computed exactly, not estimated)
  {
    double fb = FprValue(b);
    NoteSignaling(b);
    FinishArith(op, t, Rounded([&] { return 1.0 / sqrt(fb); }));
    return;
  }
  case 14:                            // fctiw: rounding per FPSCR[RN]
  {
    double fb = FprValue(b);
    NoteSignaling(b);
    INT64 r = 0;
    switch (cpu.fpscr & 3)
    {
    case 0: r = ToInt64(nearbyint(fb)); break;
    case 1: r = ToInt64(trunc(fb)); break;
    case 2: r = ToInt64(ceil(fb)); break;
    case 3: r = ToInt64(floor(fb)); break;
    }
    ConvertToWord(op, r);
    return;
  }
  case 15:                            // fctiwz
  {
    double fb = FprValue(b);
    NoteSignaling(b);
    ConvertToWord(op, ToInt64(trunc(fb)));
    return;
  }

  // FPSCR access
  case 583:                           // mffs
    cpu.fpr[t] = cpu.fpscr;
    break;
  case 70:                            // mtfsb0 (FEX and VX are summary bits)
    if (t != 1 && t != 2)
      cpu.fpscr &= ~(0x80000000u >> t);
    break;
  case 38:                            // mtfsb1
    if (t != 1 && t != 2)
      cpu.fpscr |= 0x80000000u >> t;
    break;
  case 711:                           // mtfsf
  {
    unsigned fm = (op >> 17) & 0xFF;
    UINT32 fields = 0;
    for (int i = 0; i < 8; i++)
      if (fm & (0x80 >> i))
        fields |= 0xF0000000u >> (4 * i);
    // Only the summary bits are cleared by the mask; the other bits of frB
    // are ORed in whatever the mask says.
    cpu.fpscr &= ~(fields & (kFpscrFex | kFpscrVx));
    cpu.fpscr |= (UINT32) cpu.fpr[b] & ~(kFpscrFex | kFpscrVx);
    break;
  }
  case 134:                           // mtfsfi
  {
    unsigned shift = (7 - FieldCrD(op)) * 4;
    UINT32 imm = (op >> 12) & 0xF;
    if (shift == 28)
      cpu.fpscr = (cpu.fpscr & ~(kFpscrFex | kFpscrVx)) | imm;
    cpu.fpscr = (cpu.fpscr & ~(0xFu << shift)) | (imm << shift);
    break;
  }
  case 64:                            // mcrfs: copy a field, clear its exception bits
  {
    unsigned field = FieldCrS(op);
    UINT32 value = (cpu.fpscr >> ((7 - field) * 4)) & 0xF;
    static const UINT32 kCleared[8] = {
      0x90000000, 0x0F000000, 0x00F00000, 0x00080000, 0, 0x00000700, 0, 0
    };
    cpu.fpscr &= ~kCleared[field];
    cpu.cr[FieldCrD(op)] = (UINT8) value;
    return;
  }
  default:
    DebugLog("PowerPC: invalid instruction %08X at %08X\n", op, cpu.pc);
    Stop("hit an invalid instruction");
    return;
  }
  (void) c;

  // Moves and FPSCR instructions: optional CR1 update.
  if (RecordBit(op))
    RecordCr1();
}

/******************************************************************************
 Dispatch on the primary opcode
******************************************************************************/

void ExecuteOne(UINT32 op)
{
  const unsigned d = FieldD(op), a = FieldA(op);
  UINT32 &rd = cpu.gpr[d];
  UINT32 &ra = cpu.gpr[a];

  switch (op >> 26)
  {
  case 31: ExecuteGroup31(op); return;
  case 19: ExecuteGroup19(op); return;
  case 59: ExecuteGroup59(op); return;
  case 63: ExecuteGroup63(op); return;

  // Branches
  case 18:                            // b
  {
    UINT32 offset = op & 0x03FFFFFC;
    if (offset & 0x02000000)
      offset |= 0xFC000000;
    cpu.nextPc = AbsoluteBit(op) ? offset : cpu.pc + offset;
    if (LinkBit(op))
      cpu.lr = cpu.pc + 4;
    FetchFrom(cpu.nextPc);
    return;
  }
  case 16:                            // bc
    if (BranchTaken(op))
    {
      UINT32 offset = SignedImm(op) & ~3u;
      cpu.nextPc = AbsoluteBit(op) ? offset : cpu.pc + offset;
      FetchFrom(cpu.nextPc);
    }
    if (LinkBit(op))
      cpu.lr = cpu.pc + 4;
    return;
  case 17:                            // sc
    TakeSystemCall();
    return;

  // Immediate arithmetic and logic
  case 14: rd = EaDisp(op); return;                                   // addi
  case 15: rd = (a ? ra : 0) + (UnsignedImm(op) << 16); return;       // addis
  case 7:  rd = ra * SignedImm(op); return;                           // mulli
  case 8:                             // subfic
  {
    UINT32 imm = SignedImm(op), x = ra;
    rd = imm - x;
    SetCarry(imm >= x);
    return;
  }
  case 12:                            // addic
  case 13:                            // addic.
  {
    UINT32 x = ra, r = x + SignedImm(op);
    rd = r;
    SetCarry(r < x);
    if (op & (1u << 26))
      RecordCr0(r);
    return;
  }
  case 11: cpu.cr[FieldCrD(op)] = CompareSigned((INT32) ra, (INT32) SignedImm(op)); return;   // cmpi
  case 10: cpu.cr[FieldCrD(op)] = CompareUnsigned(ra, UnsignedImm(op)); return;              // cmpli
  case 24: ra = rd | UnsignedImm(op); return;                         // ori
  case 25: ra = rd | (UnsignedImm(op) << 16); return;                 // oris
  case 26: ra = rd ^ UnsignedImm(op); return;                         // xori
  case 27: ra = rd ^ (UnsignedImm(op) << 16); return;                 // xoris
  case 28: ra = rd & UnsignedImm(op); RecordCr0(ra); return;          // andi.
  case 29: ra = rd & (UnsignedImm(op) << 16); RecordCr0(ra); return;  // andis.
  case 3:                             // twi
  {
    INT32 x = (INT32) ra, y = (INT32) SignedImm(op);
    if (((d & 16) && x < y) || ((d & 8) && x > y) || ((d & 4) && x == y) ||
        ((d & 2) && (UINT32) x < (UINT32) y) || ((d & 1) && (UINT32) x > (UINT32) y))
      TakeTrap();
    return;
  }

  // Rotates
  case 20:                            // rlwimi
  {
    UINT32 mask = RotateMask(FieldC(op), FieldME(op));
    ra = (ra & ~mask) | (RotateLeft(rd, FieldB(op)) & mask);
    if (RecordBit(op)) RecordCr0(ra);
    return;
  }
  case 21:                            // rlwinm
    ra = RotateLeft(rd, FieldB(op)) & RotateMask(FieldC(op), FieldME(op));
    if (RecordBit(op)) RecordCr0(ra);
    return;
  case 23:                            // rlwnm
    ra = RotateLeft(rd, cpu.gpr[FieldB(op)] & 31) & RotateMask(FieldC(op), FieldME(op));
    if (RecordBit(op)) RecordCr0(ra);
    return;

  // Loads and stores with a displacement
  case 32: rd = Load32(EaDisp(op)); return;                           // lwz
  case 34: rd = Load8(EaDisp(op)); return;                            // lbz
  case 40: rd = Load16(EaDisp(op)); return;                           // lhz
  case 42: rd = (UINT32) (INT32) (INT16) Load16(EaDisp(op)); return;  // lha
  case 33: { UINT32 ea = EaDispUpdate(op); rd = Load32(ea); ra = ea; return; }   // lwzu
  case 35: { UINT32 ea = EaDispUpdate(op); rd = Load8(ea); ra = ea; return; }    // lbzu
  case 41: { UINT32 ea = EaDispUpdate(op); rd = Load16(ea); ra = ea; return; }   // lhzu
  case 43: { UINT32 ea = EaDispUpdate(op); rd = (UINT32) (INT32) (INT16) Load16(ea); ra = ea; return; }   // lhau
  case 36: Store32(EaDisp(op), rd); return;                           // stw
  case 38: Store8(EaDisp(op), rd); return;                            // stb
  case 44: Store16(EaDisp(op), rd); return;                           // sth
  case 37: { UINT32 ea = EaDispUpdate(op); Store32(ea, rd); ra = ea; return; }   // stwu
  case 39: { UINT32 ea = EaDispUpdate(op); Store8(ea, rd); ra = ea; return; }    // stbu
  case 45: { UINT32 ea = EaDispUpdate(op); Store16(ea, rd); ra = ea; return; }   // sthu
  case 46:                            // lmw
  {
    UINT32 ea = EaDisp(op);
    for (unsigned r = d; r < 32; r++, ea += 4)
      cpu.gpr[r] = Load32(ea);
    return;
  }
  case 47:                            // stmw
  {
    UINT32 ea = EaDisp(op);
    for (unsigned r = d; r < 32; r++, ea += 4)
      Store32(ea, cpu.gpr[r]);
    return;
  }
  case 48: SetFprValue(d, WidenSingle(Load32(EaDisp(op)))); return;   // lfs
  case 50: cpu.fpr[d] = Load64(EaDisp(op)); return;                   // lfd
  case 49: { UINT32 ea = EaDispUpdate(op); SetFprValue(d, WidenSingle(Load32(ea))); ra = ea; return; }   // lfsu
  case 51: { UINT32 ea = EaDispUpdate(op); cpu.fpr[d] = Load64(ea); ra = ea; return; }                   // lfdu
  case 52: Store32(EaDisp(op), NarrowToSingle(FprValue(d))); return;  // stfs
  case 54: Store64(EaDisp(op), cpu.fpr[d]); return;                   // stfd
  case 53: { UINT32 ea = EaDispUpdate(op); Store32(ea, NarrowToSingle(FprValue(d))); ra = ea; return; }  // stfsu
  case 55: { UINT32 ea = EaDispUpdate(op); Store64(ea, cpu.fpr[d]); ra = ea; return; }                   // stfdu

  default:
    DebugLog("PowerPC: invalid instruction %08X at %08X\n", op, cpu.pc);
    Stop("hit an invalid instruction");
    return;
  }
}

} // anonymous namespace
