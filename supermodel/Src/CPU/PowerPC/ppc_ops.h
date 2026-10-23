// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// ppc_ops.h: instruction word fields and the shared condition helpers of
// the PowerPC core. Included by ppc.cpp only.

namespace {

// Instruction word fields (bit 0 is the least significant bit here; the
// PowerPC books number them the other way round).
inline unsigned FieldD(UINT32 op)   { return (op >> 21) & 31; }   // rD, rS, frD, BO, TO, crbD
inline unsigned FieldA(UINT32 op)   { return (op >> 16) & 31; }   // rA, frA, BI, crbA
inline unsigned FieldB(UINT32 op)   { return (op >> 11) & 31; }   // rB, frB, SH, NB, crbB
inline unsigned FieldC(UINT32 op)   { return (op >> 6) & 31; }    // frC, MB
inline unsigned FieldME(UINT32 op)  { return (op >> 1) & 31; }
inline unsigned FieldCrD(UINT32 op) { return (op >> 23) & 7; }
inline unsigned FieldCrS(UINT32 op) { return (op >> 18) & 7; }
inline unsigned FieldXO(UINT32 op)  { return (op >> 1) & 0x3FF; }
inline UINT32 SignedImm(UINT32 op)  { return (UINT32) (INT32) (INT16) (op & 0xFFFF); }
inline UINT32 UnsignedImm(UINT32 op) { return op & 0xFFFF; }
inline bool RecordBit(UINT32 op)    { return (op & 1) != 0; }
inline bool OverflowBit(UINT32 op)  { return (op & 0x400) != 0; }
inline bool LinkBit(UINT32 op)      { return (op & 1) != 0; }
inline bool AbsoluteBit(UINT32 op)  { return (op & 2) != 0; }

// The 10-bit SPR/TBR number: its two 5-bit halves are swapped in the word.
inline unsigned SprField(UINT32 op)
{
  return ((op >> 16) & 0x1F) | ((op >> 6) & 0x3E0);
}

// Effective address forms: (rA|0) + d, rA + d, (rA|0) + rB, rA + rB.
inline UINT32 EaDisp(UINT32 op)
{
  unsigned a = FieldA(op);
  return a ? cpu.gpr[a] + SignedImm(op) : SignedImm(op);
}

inline UINT32 EaDispUpdate(UINT32 op)
{
  return cpu.gpr[FieldA(op)] + SignedImm(op);
}

inline UINT32 EaIndexed(UINT32 op)
{
  unsigned a = FieldA(op);
  return a ? cpu.gpr[a] + cpu.gpr[FieldB(op)] : cpu.gpr[FieldB(op)];
}

inline UINT32 EaIndexedUpdate(UINT32 op)
{
  return cpu.gpr[FieldA(op)] + cpu.gpr[FieldB(op)];
}

// Condition register field for a signed or unsigned comparison: LT, GT or
// EQ, plus a copy of XER[SO].
inline UINT8 CompareSigned(INT32 a, INT32 b)
{
  UINT8 f = (a < b) ? 8 : ((a > b) ? 4 : 2);
  return f | ((cpu.xer & kXerSo) ? 1 : 0);
}

inline UINT8 CompareUnsigned(UINT32 a, UINT32 b)
{
  UINT8 f = (a < b) ? 8 : ((a > b) ? 4 : 2);
  return f | ((cpu.xer & kXerSo) ? 1 : 0);
}

// CR0 after a recording integer instruction: the result against zero.
inline void RecordCr0(UINT32 result)
{
  cpu.cr[0] = CompareSigned((INT32) result, 0);
}

// CR1 after a recording FPU instruction: FPSCR[FX, FEX, VX, OX].
inline void RecordCr1()
{
  cpu.cr[1] = (cpu.fpscr >> 28) & 0xF;
}

inline void SetCarry(bool carry)
{
  if (carry)
    cpu.xer |= kXerCa;
  else
    cpu.xer &= ~kXerCa;
}

inline UINT32 CarryIn()
{
  return (cpu.xer >> 29) & 1;
}

// XER[OV] from a sign bit computed by the caller (bit 31 of 'flag'); a set
// OV also sets the sticky SO.
inline void SetOverflow(UINT32 flag)
{
  if (flag & 0x80000000)
    cpu.xer |= kXerSo | kXerOv;
  else
    cpu.xer &= ~kXerOv;
}

// Signed overflow of r = a + b (sign of r differs from both operands) and of
// r = a - b (operands of different signs, r's sign differs from a's).
inline UINT32 AddOverflows(UINT32 r, UINT32 a, UINT32 b)
{
  return (a ^ r) & ~(a ^ b);
}

inline UINT32 SubOverflows(UINT32 r, UINT32 a, UINT32 b)
{
  return (a ^ r) & (a ^ b);
}

// One bit of the condition register (bit 0 = CR0[LT]).
inline unsigned CrBit(unsigned bit)
{
  return (cpu.cr[bit >> 2] >> (3 - (bit & 3))) & 1;
}

inline void SetCrBit(unsigned bit, unsigned value)
{
  UINT8 m = (UINT8) (8 >> (bit & 3));
  if (value)
    cpu.cr[bit >> 2] |= m;
  else
    cpu.cr[bit >> 2] &= (UINT8) ~m;
}

// The BO/BI test of the conditional branches. BO bit 2 (value 4) clear
// decrements CTR and tests it (bit 1 says against zero or non-zero); BO bit
// 4 (value 16) clear tests CR bit BI against BO bit 3 (value 8).
inline bool BranchTaken(UINT32 op)
{
  unsigned bo = FieldD(op);
  bool ctrOk = true;
  if (!(bo & 4))
  {
    cpu.ctr--;
    ctrOk = ((cpu.ctr != 0) != ((bo & 2) != 0));
  }
  bool condOk = (bo & 16) || (CrBit(FieldA(op)) == ((bo >> 3) & 1));
  return ctrOk && condOk;
}

// rlwinm-style mask with ones from bit mb to bit me (PowerPC numbering,
// bit 0 = MSB), wrapping round when mb > me.
inline UINT32 RotateMask(unsigned mb, unsigned me)
{
  UINT32 fromMb = 0xFFFFFFFFu >> mb;
  UINT32 pastMe = (me == 31) ? 0 : (0xFFFFFFFFu >> (me + 1));
  if (mb <= me)
    return fromMb & ~pastMe;
  return fromMb | ~pastMe;
}

inline UINT32 RotateLeft(UINT32 v, unsigned n)
{
  return n ? ((v << n) | (v >> (32 - n))) : v;
}

inline UINT16 Swap16(UINT16 v)
{
  return (UINT16) ((v >> 8) | (v << 8));
}

inline UINT32 Swap32(UINT32 v)
{
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

} // anonymous namespace
