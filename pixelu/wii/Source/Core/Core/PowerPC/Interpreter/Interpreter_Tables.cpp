// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Instruction decoding for the interpreter.
//
// Each primary opcode group has a decode function that maps the extended opcode bits to a
// handler. The functions are evaluated at compile time for every possible encoding to build flat
// dispatch tables, so decoding at run time is one or two array lookups.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <array>
#include <cstddef>

#include "Core/PowerPC/Gekko.h"

namespace
{
using I = Interpreter;
using Handler = Interpreter::Instruction;

// Primary opcode (bits 0..5).
constexpr Handler DecodePrimary(u32 opcd)
{
  switch (opcd)
  {
  case 3: return I::twi;
  case 4: return I::RunTable4;
  case 7: return I::mulli;
  case 8: return I::subfic;
  case 10: return I::cmpli;
  case 11: return I::cmpi;
  case 12: return I::addic;
  case 13: return I::addic_rc;
  case 14: return I::addi;
  case 15: return I::addis;
  case 16: return I::bcx;
  case 17: return I::sc;
  case 18: return I::bx;
  case 19: return I::RunTable19;
  case 20: return I::rlwimix;
  case 21: return I::rlwinmx;
  case 23: return I::rlwnmx;
  case 24: return I::ori;
  case 25: return I::oris;
  case 26: return I::xori;
  case 27: return I::xoris;
  case 28: return I::andi_rc;
  case 29: return I::andis_rc;
  case 31: return I::RunTable31;
  case 32: return I::lwz;
  case 33: return I::lwzu;
  case 34: return I::lbz;
  case 35: return I::lbzu;
  case 36: return I::stw;
  case 37: return I::stwu;
  case 38: return I::stb;
  case 39: return I::stbu;
  case 40: return I::lhz;
  case 41: return I::lhzu;
  case 42: return I::lha;
  case 43: return I::lhau;
  case 44: return I::sth;
  case 45: return I::sthu;
  case 46: return I::lmw;
  case 47: return I::stmw;
  case 48: return I::lfs;
  case 49: return I::lfsu;
  case 50: return I::lfd;
  case 51: return I::lfdu;
  case 52: return I::stfs;
  case 53: return I::stfsu;
  case 54: return I::stfd;
  case 55: return I::stfdu;
  case 56: return I::psq_l;
  case 57: return I::psq_lu;
  case 59: return I::RunTable59;
  case 60: return I::psq_st;
  case 61: return I::psq_stu;
  case 63: return I::RunTable63;
  default: return I::unknown_instruction;
  }
}

// Opcode 4: paired singles. xo is the 10-bit extended opcode (bits 21..30). Arithmetic forms
// only decode the low 5 bits (the rest is the FC register), and the indexed quantised
// loads/stores decode the low 6 bits (the rest holds W and I).
constexpr Handler Decode4(u32 xo)
{
  switch (xo)
  {
  case 0: return I::ps_cmpu0;
  case 32: return I::ps_cmpo0;
  case 40: return I::ps_neg;
  case 64: return I::ps_cmpu1;
  case 72: return I::ps_mr;
  case 96: return I::ps_cmpo1;
  case 136: return I::ps_nabs;
  case 264: return I::ps_abs;
  case 528: return I::ps_merge00;
  case 560: return I::ps_merge01;
  case 592: return I::ps_merge10;
  case 624: return I::ps_merge11;
  case 1014: return I::dcbz_l;
  default: break;
  }

  switch (xo & 0x3F)
  {
  case 6: return I::psq_lx;
  case 7: return I::psq_stx;
  case 38: return I::psq_lux;
  case 39: return I::psq_stux;
  default: break;
  }

  switch (xo & 0x1F)
  {
  case 10: return I::ps_sum0;
  case 11: return I::ps_sum1;
  case 12: return I::ps_muls0;
  case 13: return I::ps_muls1;
  case 14: return I::ps_madds0;
  case 15: return I::ps_madds1;
  case 18: return I::ps_div;
  case 20: return I::ps_sub;
  case 21: return I::ps_add;
  case 23: return I::ps_sel;
  case 24: return I::ps_res;
  case 25: return I::ps_mul;
  case 26: return I::ps_rsqrte;
  case 28: return I::ps_msub;
  case 29: return I::ps_madd;
  case 30: return I::ps_nmsub;
  case 31: return I::ps_nmadd;
  default: return I::unknown_instruction;
  }
}

// Opcode 19: branches to LR/CTR, condition register logic, rfi, isync.
constexpr Handler Decode19(u32 xo)
{
  switch (xo)
  {
  case 0: return I::mcrf;
  case 16: return I::bclrx;
  case 33: return I::crnor;
  case 50: return I::rfi;
  case 129: return I::crandc;
  case 150: return I::isync;
  case 193: return I::crxor;
  case 225: return I::crnand;
  case 257: return I::crand;
  case 289: return I::creqv;
  case 417: return I::crorc;
  case 449: return I::cror;
  case 528: return I::bcctrx;
  default: return I::unknown_instruction;
  }
}

// Opcode 31: integer, load/store indexed, cache and system instructions. The OE forms of the
// arithmetic instructions (xo + 512) share the handler, which reads OE itself.
constexpr Handler Decode31(u32 xo)
{
  switch (xo)
  {
  // arithmetic (with and without OE)
  case 8: case 520: return I::subfcx;
  case 10: case 522: return I::addcx;
  case 40: case 552: return I::subfx;
  case 104: case 616: return I::negx;
  case 136: case 648: return I::subfex;
  case 138: case 650: return I::addex;
  case 200: case 712: return I::subfzex;
  case 202: case 714: return I::addzex;
  case 232: case 744: return I::subfmex;
  case 234: case 746: return I::addmex;
  case 235: case 747: return I::mullwx;
  case 266: case 778: return I::addx;
  case 459: case 971: return I::divwux;
  case 491: case 1003: return I::divwx;
  case 11: return I::mulhwux;
  case 75: return I::mulhwx;

  // logic, compare, shifts
  case 0: return I::cmp;
  case 24: return I::slwx;
  case 26: return I::cntlzwx;
  case 28: return I::andx;
  case 32: return I::cmpl;
  case 60: return I::andcx;
  case 124: return I::norx;
  case 284: return I::eqvx;
  case 316: return I::xorx;
  case 412: return I::orcx;
  case 444: return I::orx;
  case 476: return I::nandx;
  case 536: return I::srwx;
  case 792: return I::srawx;
  case 824: return I::srawix;
  case 922: return I::extshx;
  case 954: return I::extsbx;

  // integer loads
  case 20: return I::lwarx;
  case 23: return I::lwzx;
  case 55: return I::lwzux;
  case 87: return I::lbzx;
  case 119: return I::lbzux;
  case 279: return I::lhzx;
  case 311: return I::lhzux;
  case 343: return I::lhax;
  case 375: return I::lhaux;
  case 533: return I::lswx;
  case 534: return I::lwbrx;
  case 597: return I::lswi;
  case 790: return I::lhbrx;

  // integer stores
  case 150: return I::stwcxd;
  case 151: return I::stwx;
  case 183: return I::stwux;
  case 215: return I::stbx;
  case 247: return I::stbux;
  case 407: return I::sthx;
  case 439: return I::sthux;
  case 661: return I::stswx;
  case 662: return I::stwbrx;
  case 725: return I::stswi;
  case 918: return I::sthbrx;

  // floating point loads/stores
  case 535: return I::lfsx;
  case 567: return I::lfsux;
  case 599: return I::lfdx;
  case 631: return I::lfdux;
  case 663: return I::stfsx;
  case 695: return I::stfsux;
  case 727: return I::stfdx;
  case 759: return I::stfdux;
  case 983: return I::stfiwx;

  // caches
  case 54: return I::dcbst;
  case 86: return I::dcbf;
  case 246: return I::dcbtst;
  case 278: return I::dcbt;
  case 470: return I::dcbi;
  case 758: return I::dcba;
  case 982: return I::icbi;
  case 1014: return I::dcbz;

  // system registers and control
  case 4: return I::tw;
  case 19: return I::mfcr;
  case 83: return I::mfmsr;
  case 144: return I::mtcrf;
  case 146: return I::mtmsr;
  case 210: return I::mtsr;
  case 242: return I::mtsrin;
  case 306: return I::tlbie;
  case 310: return I::eciwx;
  case 339: return I::mfspr;
  case 371: return I::mftb;
  case 438: return I::ecowx;
  case 467: return I::mtspr;
  case 512: return I::mcrxr;
  case 566: return I::tlbsync;
  case 595: return I::mfsr;
  case 598: return I::sync;
  case 659: return I::mfsrin;
  case 854: return I::eieio;
  default: return I::unknown_instruction;
  }
}

// Opcode 59: single precision arithmetic, decoded on the low 5 bits.
constexpr Handler Decode59(u32 xo5)
{
  switch (xo5)
  {
  case 18: return I::fdivsx;
  case 20: return I::fsubsx;
  case 21: return I::faddsx;
  case 24: return I::fresx;
  case 25: return I::fmulsx;
  case 28: return I::fmsubsx;
  case 29: return I::fmaddsx;
  case 30: return I::fnmsubsx;
  case 31: return I::fnmaddsx;
  default: return I::unknown_instruction;
  }
}

// Opcode 63: double precision. Moves, compares, conversions and FPSCR access use the full
// 10-bit extended opcode; the arithmetic forms only the low 5 bits.
constexpr Handler Decode63(u32 xo)
{
  switch (xo)
  {
  case 0: return I::fcmpu;
  case 12: return I::frspx;
  case 14: return I::fctiwx;
  case 15: return I::fctiwzx;
  case 32: return I::fcmpo;
  case 38: return I::mtfsb1x;
  case 40: return I::fnegx;
  case 64: return I::mcrfs;
  case 70: return I::mtfsb0x;
  case 72: return I::fmrx;
  case 134: return I::mtfsfix;
  case 136: return I::fnabsx;
  case 264: return I::fabsx;
  case 583: return I::mffsx;
  case 711: return I::mtfsfx;
  default: break;
  }

  switch (xo & 0x1F)
  {
  case 18: return I::fdivx;
  case 20: return I::fsubx;
  case 21: return I::faddx;
  case 23: return I::fselx;
  case 25: return I::fmulx;
  case 26: return I::frsqrtex;
  case 28: return I::fmsubx;
  case 29: return I::fmaddx;
  case 30: return I::fnmsubx;
  case 31: return I::fnmaddx;
  default: return I::unknown_instruction;
  }
}

template <std::size_t N, typename Decoder>
consteval std::array<Handler, N> BuildTable(Decoder decode)
{
  std::array<Handler, N> table{};
  for (u32 i = 0; i < N; ++i)
    table[i] = decode(i);
  return table;
}

constexpr auto s_primary = BuildTable<64>(DecodePrimary);
constexpr auto s_group4 = BuildTable<1024>(Decode4);
constexpr auto s_group19 = BuildTable<1024>(Decode19);
constexpr auto s_group31 = BuildTable<1024>(Decode31);
constexpr auto s_group59 = BuildTable<32>(Decode59);
constexpr auto s_group63 = BuildTable<1024>(Decode63);
}  // namespace

// Resolves an instruction all the way to its leaf handler (used by the cached interpreter and
// the JIT fallbacks, which call the handler directly).
Interpreter::Instruction Interpreter::GetInterpreterOp(UGeckoInstruction inst)
{
  switch (inst.OPCD)
  {
  case 4: return s_group4[inst.SUBOP10];
  case 19: return s_group19[inst.SUBOP10];
  case 31: return s_group31[inst.SUBOP10];
  case 59: return s_group59[inst.SUBOP5];
  case 63: return s_group63[inst.SUBOP10];
  default: return s_primary[inst.OPCD];
  }
}

void Interpreter::RunInterpreterOp(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_primary[inst.OPCD](interpreter, inst);
}

void Interpreter::RunTable4(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_group4[inst.SUBOP10](interpreter, inst);
}
void Interpreter::RunTable19(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_group19[inst.SUBOP10](interpreter, inst);
}
void Interpreter::RunTable31(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_group31[inst.SUBOP10](interpreter, inst);
}
void Interpreter::RunTable59(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_group59[inst.SUBOP5](interpreter, inst);
}
void Interpreter::RunTable63(Interpreter& interpreter, UGeckoInstruction inst)
{
  s_group63[inst.SUBOP10](interpreter, inst);
}
