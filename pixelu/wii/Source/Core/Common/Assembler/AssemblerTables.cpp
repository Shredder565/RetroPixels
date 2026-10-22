// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/AssemblerTables.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assembler/CaseInsensitiveDict.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
// ---------------------------------------------------------------------------------------------
// Operand fields

// One operand slot of a machine word. The user's value is shifted left by `shift` and must fit
// in `mask`. Signed fields accept negative values in two's complement; bits of the value that
// fall below the mask (branch displacements) must be zero.
struct Field
{
  u32 mask;
  u32 shift;
  bool is_signed;

  u32 Span() const { return mask >> shift; }
  u32 TopBit() const { return std::bit_floor(Span()); }
  u32 LowBit() const { return Span() & (0u - Span()); }

  u32 Max() const { return is_signed ? TopBit() - 1 : Span(); }
  u32 Min() const { return is_signed ? ~Max() : 0; }
  u32 AlignMask() const { return LowBit() - 1; }

  bool Accepts(u32 v) const
  {
    if (!is_signed)
      return (v & Span()) == v;
    return (v <= Max() || v >= Min()) && (v & AlignMask()) == 0;
  }
  u32 Place(u32 v) const { return (v << shift) & mask; }
};

struct NamedField
{
  std::string_view name;
  Field field;
};

// Field names used in the syntax strings below. Several architectural names share a position:
// "D" also stands for rS, BO, TO and crbD; "A" for BI and crbA; "B" for SH, NB and crbB; "C" for
// MB.
constexpr NamedField kFields[] = {
    {"D", {0x03E00000, 21, false}},    {"A", {0x001F0000, 16, false}},
    {"B", {0x0000F800, 11, false}},    {"C", {0x000007C0, 6, false}},
    {"ME", {0x0000003E, 1, false}},    {"crfD", {0x03800000, 23, false}},
    {"crfS", {0x001C0000, 18, false}}, {"L", {0x00200000, 21, false}},
    {"CRM", {0x000FF000, 12, false}},  {"FM", {0x01FE0000, 17, false}},
    {"IMM", {0x0000F000, 12, false}},  {"LI", {0x03FFFFFC, 0, true}},
    {"BD", {0x0000FFFC, 0, true}},     {"simm", {0x0000FFFF, 0, true}},
    {"uimm", {0x0000FFFF, 0, false}},  {"d12", {0x00000FFF, 0, true}},
    {"SPR", {0x001FF800, 11, false}},  {"SR", {0x000F0000, 16, false}},
    {"W", {0x00008000, 15, false}},    {"Wx", {0x00000400, 10, false}},
    {"I", {0x00007000, 12, false}},    {"Ix", {0x00000380, 7, false}},
};

// ---------------------------------------------------------------------------------------------
// Plain instructions

// Which spellings a mnemonic has. Bit 0 of the variant is Rc or LK; bit 1 is OE (for kOeRc)
// or AA (for kAaLk).
enum class Suffix : u8
{
  kNone,
  kRc,    // x, x.
  kOeRc,  // x, x., xo, xo.
  kLk,    // x, xl
  kAaLk,  // x, xl, xa, xla
};

struct InstrRow
{
  std::string_view name;
  u32 opcode;
  u32 xo;
  Suffix suffix;
  // Operands in assembly order, comma separated; "x(y)" marks a displacement form.
  std::string_view syntax;
  u32 fixed_bits = 0;
};

constexpr Suffix N = Suffix::kNone;
constexpr Suffix R = Suffix::kRc;
constexpr Suffix O = Suffix::kOeRc;

// clang-format off
constexpr InstrRow kInstrRows[] = {
    // Integer arithmetic
    {"add", 31, 266, O, "D,A,B"},
    {"addc", 31, 10, O, "D,A,B"},
    {"adde", 31, 138, O, "D,A,B"},
    {"addi", 14, 0, N, "D,A,simm"},
    {"addic", 12, 0, N, "D,A,simm"},
    {"addic.", 13, 0, N, "D,A,simm"},
    {"addis", 15, 0, N, "D,A,simm"},
    {"addme", 31, 234, O, "D,A"},
    {"addze", 31, 202, O, "D,A"},
    {"divw", 31, 491, O, "D,A,B"},
    {"divwu", 31, 459, O, "D,A,B"},
    {"mulhw", 31, 75, R, "D,A,B"},
    {"mulhwu", 31, 11, R, "D,A,B"},
    {"mulli", 7, 0, N, "D,A,simm"},
    {"mullw", 31, 235, O, "D,A,B"},
    {"neg", 31, 104, O, "D,A"},
    {"subf", 31, 40, O, "D,A,B"},
    {"subfc", 31, 8, O, "D,A,B"},
    {"subfe", 31, 136, O, "D,A,B"},
    {"subfic", 8, 0, N, "D,A,simm"},
    {"subfme", 31, 232, O, "D,A"},
    {"subfze", 31, 200, O, "D,A"},

    // Compares
    {"cmp", 31, 0, N, "crfD,L,A,B"},
    {"cmpi", 11, 0, N, "crfD,L,A,simm"},
    {"cmpl", 31, 32, N, "crfD,L,A,B"},
    {"cmpli", 10, 0, N, "crfD,L,A,uimm"},

    // Logical, rotate and shift (rS is printed second)
    {"and", 31, 28, R, "A,D,B"},
    {"andc", 31, 60, R, "A,D,B"},
    {"andi.", 28, 0, N, "A,D,uimm"},
    {"andis.", 29, 0, N, "A,D,uimm"},
    {"cntlzw", 31, 26, R, "A,D"},
    {"eqv", 31, 284, R, "A,D,B"},
    {"extsb", 31, 954, R, "A,D"},
    {"extsh", 31, 922, R, "A,D"},
    {"nand", 31, 476, R, "A,D,B"},
    {"nor", 31, 124, R, "A,D,B"},
    {"or", 31, 444, R, "A,D,B"},
    {"orc", 31, 412, R, "A,D,B"},
    {"ori", 24, 0, N, "A,D,uimm"},
    {"oris", 25, 0, N, "A,D,uimm"},
    {"xor", 31, 316, R, "A,D,B"},
    {"xori", 26, 0, N, "A,D,uimm"},
    {"xoris", 27, 0, N, "A,D,uimm"},
    {"rlwimi", 20, 0, R, "A,D,B,C,ME"},
    {"rlwinm", 21, 0, R, "A,D,B,C,ME"},
    {"rlwnm", 23, 0, R, "A,D,B,C,ME"},
    {"slw", 31, 24, R, "A,D,B"},
    {"sraw", 31, 792, R, "A,D,B"},
    {"srawi", 31, 824, R, "A,D,B"},
    {"srw", 31, 536, R, "A,D,B"},

    // Floating point arithmetic
    {"fadd", 63, 21, R, "D,A,B"},
    {"fadds", 59, 21, R, "D,A,B"},
    {"fdiv", 63, 18, R, "D,A,B"},
    {"fdivs", 59, 18, R, "D,A,B"},
    {"fmul", 63, 25, R, "D,A,C"},
    {"fmuls", 59, 25, R, "D,A,C"},
    {"fres", 59, 24, R, "D,B"},
    {"frsqrte", 63, 26, R, "D,B"},
    {"fsub", 63, 20, R, "D,A,B"},
    {"fsubs", 59, 20, R, "D,A,B"},
    {"fsel", 63, 23, R, "D,A,C,B"},
    {"fmadd", 63, 29, R, "D,A,C,B"},
    {"fmadds", 59, 29, R, "D,A,C,B"},
    {"fmsub", 63, 28, R, "D,A,C,B"},
    {"fmsubs", 59, 28, R, "D,A,C,B"},
    {"fnmadd", 63, 31, R, "D,A,C,B"},
    {"fnmadds", 59, 31, R, "D,A,C,B"},
    {"fnmsub", 63, 30, R, "D,A,C,B"},
    {"fnmsubs", 59, 30, R, "D,A,C,B"},
    {"fctiw", 63, 14, R, "D,B"},
    {"fctiwz", 63, 15, R, "D,B"},
    {"frsp", 63, 12, R, "D,B"},
    {"fcmpo", 63, 32, N, "crfD,A,B"},
    {"fcmpu", 63, 0, N, "crfD,A,B"},
    {"fabs", 63, 264, R, "D,B"},
    {"fmr", 63, 72, R, "D,B"},
    {"fnabs", 63, 136, R, "D,B"},
    {"fneg", 63, 40, R, "D,B"},

    // FPSCR
    {"mcrfs", 63, 64, N, "crfD,crfS"},
    {"mffs", 63, 583, R, "D"},
    {"mtfsb0", 63, 70, R, "D"},
    {"mtfsb1", 63, 38, R, "D"},
    {"mtfsf", 63, 711, R, "FM,B"},
    {"mtfsfi", 63, 134, R, "crfD,IMM"},

    // Integer loads and stores
    {"lbz", 34, 0, N, "D,simm(A)"},
    {"lbzu", 35, 0, N, "D,simm(A)"},
    {"lbzux", 31, 119, N, "D,A,B"},
    {"lbzx", 31, 87, N, "D,A,B"},
    {"lha", 42, 0, N, "D,simm(A)"},
    {"lhau", 43, 0, N, "D,simm(A)"},
    {"lhaux", 31, 375, N, "D,A,B"},
    {"lhax", 31, 343, N, "D,A,B"},
    {"lhz", 40, 0, N, "D,simm(A)"},
    {"lhzu", 41, 0, N, "D,simm(A)"},
    {"lhzux", 31, 311, N, "D,A,B"},
    {"lhzx", 31, 279, N, "D,A,B"},
    {"lwz", 32, 0, N, "D,simm(A)"},
    {"lwzu", 33, 0, N, "D,simm(A)"},
    {"lwzux", 31, 55, N, "D,A,B"},
    {"lwzx", 31, 23, N, "D,A,B"},
    {"stb", 38, 0, N, "D,simm(A)"},
    {"stbu", 39, 0, N, "D,simm(A)"},
    {"stbux", 31, 247, N, "D,A,B"},
    {"stbx", 31, 215, N, "D,A,B"},
    {"sth", 44, 0, N, "D,simm(A)"},
    {"sthu", 45, 0, N, "D,simm(A)"},
    {"sthux", 31, 439, N, "D,A,B"},
    {"sthx", 31, 407, N, "D,A,B"},
    {"stw", 36, 0, N, "D,simm(A)"},
    {"stwu", 37, 0, N, "D,simm(A)"},
    {"stwux", 31, 183, N, "D,A,B"},
    {"stwx", 31, 151, N, "D,A,B"},
    {"lhbrx", 31, 790, N, "D,A,B"},
    {"lwbrx", 31, 534, N, "D,A,B"},
    {"sthbrx", 31, 918, N, "D,A,B"},
    {"stwbrx", 31, 662, N, "D,A,B"},
    {"lmw", 46, 0, N, "D,simm(A)"},
    {"stmw", 47, 0, N, "D,simm(A)"},
    {"lswi", 31, 597, N, "D,A,B"},
    {"lswx", 31, 533, N, "D,A,B"},
    {"stswi", 31, 725, N, "D,A,B"},
    {"stswx", 31, 661, N, "D,A,B"},

    // Synchronisation
    {"eieio", 31, 854, N, ""},
    {"isync", 19, 150, N, ""},
    {"lwarx", 31, 20, N, "D,A,B"},
    {"stwcx.", 31, 150, N, "D,A,B", 1},
    {"sync", 31, 598, N, ""},

    // Floating point loads and stores
    {"lfd", 50, 0, N, "D,simm(A)"},
    {"lfdu", 51, 0, N, "D,simm(A)"},
    {"lfdux", 31, 631, N, "D,A,B"},
    {"lfdx", 31, 599, N, "D,A,B"},
    {"lfs", 48, 0, N, "D,simm(A)"},
    {"lfsu", 49, 0, N, "D,simm(A)"},
    {"lfsux", 31, 567, N, "D,A,B"},
    {"lfsx", 31, 535, N, "D,A,B"},
    {"stfd", 54, 0, N, "D,simm(A)"},
    {"stfdu", 55, 0, N, "D,simm(A)"},
    {"stfdux", 31, 759, N, "D,A,B"},
    {"stfdx", 31, 727, N, "D,A,B"},
    {"stfiwx", 31, 983, N, "D,A,B"},
    {"stfs", 52, 0, N, "D,simm(A)"},
    {"stfsu", 53, 0, N, "D,simm(A)"},
    {"stfsux", 31, 695, N, "D,A,B"},
    {"stfsx", 31, 663, N, "D,A,B"},

    // Branches and condition register
    {"b", 18, 0, Suffix::kAaLk, "LI"},
    {"bc", 16, 0, Suffix::kAaLk, "D,A,BD"},
    {"bcctr", 19, 528, Suffix::kLk, "D,A"},
    {"bclr", 19, 16, Suffix::kLk, "D,A"},
    {"crand", 19, 257, N, "D,A,B"},
    {"crandc", 19, 129, N, "D,A,B"},
    {"creqv", 19, 289, N, "D,A,B"},
    {"crnand", 19, 225, N, "D,A,B"},
    {"crnor", 19, 33, N, "D,A,B"},
    {"cror", 19, 449, N, "D,A,B"},
    {"crorc", 19, 417, N, "D,A,B"},
    {"crxor", 19, 193, N, "D,A,B"},
    {"mcrf", 19, 0, N, "crfD,crfS"},

    // System
    {"rfi", 19, 50, N, ""},
    {"sc", 17, 1, N, ""},
    {"tw", 31, 4, N, "D,A,B"},
    {"twi", 3, 0, N, "D,A,simm"},
    {"mcrxr", 31, 512, N, "crfD"},
    {"mfcr", 31, 19, N, "D"},
    {"mfmsr", 31, 83, N, "D"},
    {"mfspr_nobitswap", 31, 339, N, "D,SPR"},
    {"mftb_nobitswap", 31, 371, N, "D,SPR"},
    {"mtcrf", 31, 144, N, "CRM,D"},
    {"mtmsr", 31, 146, N, "D"},
    {"mtspr_nobitswap", 31, 467, N, "SPR,D"},

    // Cache, segment and TLB management
    {"dcbf", 31, 86, N, "A,B"},
    {"dcbi", 31, 470, N, "A,B"},
    {"dcbst", 31, 54, N, "A,B"},
    {"dcbt", 31, 278, N, "A,B"},
    {"dcbtst", 31, 246, N, "A,B"},
    {"dcbz", 31, 1014, N, "A,B"},
    {"icbi", 31, 982, N, "A,B"},
    {"mfsr", 31, 595, N, "D,SR"},
    {"mfsrin", 31, 659, N, "D,B"},
    {"mtsr", 31, 210, N, "SR,D"},
    {"mtsrin", 31, 242, N, "D,B"},
    {"tlbie", 31, 306, N, "B"},
    {"tlbsync", 31, 566, N, ""},
    {"eciwx", 31, 310, N, "D,A,B"},
    {"ecowx", 31, 438, N, "D,A,B"},

    // Paired singles
    {"psq_lx", 4, 6, N, "D,A,B,Wx,Ix"},
    {"psq_stx", 4, 7, N, "D,A,B,Wx,Ix"},
    {"psq_lux", 4, 38, N, "D,A,B,Wx,Ix"},
    {"psq_stux", 4, 39, N, "D,A,B,Wx,Ix"},
    {"psq_l", 56, 0, N, "D,d12(A),W,I"},
    {"psq_lu", 57, 0, N, "D,d12(A),W,I"},
    {"psq_st", 60, 0, N, "D,d12(A),W,I"},
    {"psq_stu", 61, 0, N, "D,d12(A),W,I"},
    {"ps_div", 4, 18, R, "D,A,B"},
    {"ps_sub", 4, 20, R, "D,A,B"},
    {"ps_add", 4, 21, R, "D,A,B"},
    {"ps_sel", 4, 23, R, "D,A,C,B"},
    {"ps_res", 4, 24, R, "D,B"},
    {"ps_mul", 4, 25, R, "D,A,C"},
    {"ps_rsqrte", 4, 26, R, "D,B"},
    {"ps_msub", 4, 28, R, "D,A,C,B"},
    {"ps_madd", 4, 29, R, "D,A,C,B"},
    {"ps_nmsub", 4, 30, R, "D,A,C,B"},
    {"ps_nmadd", 4, 31, R, "D,A,C,B"},
    {"ps_neg", 4, 40, R, "D,B"},
    {"ps_mr", 4, 72, R, "D,B"},
    {"ps_nabs", 4, 136, R, "D,B"},
    {"ps_abs", 4, 264, R, "D,B"},
    {"ps_sum0", 4, 10, R, "D,A,C,B"},
    {"ps_sum1", 4, 11, R, "D,A,C,B"},
    {"ps_muls0", 4, 12, R, "D,A,C"},
    {"ps_muls1", 4, 13, R, "D,A,C"},
    {"ps_madds0", 4, 14, R, "D,A,C,B"},
    {"ps_madds1", 4, 15, R, "D,A,C,B"},
    {"ps_cmpu0", 4, 0, N, "crfD,A,B"},
    {"ps_cmpo0", 4, 32, N, "crfD,A,B"},
    {"ps_cmpu1", 4, 64, N, "crfD,A,B"},
    {"ps_cmpo1", 4, 96, N, "crfD,A,B"},
    {"ps_merge00", 4, 528, R, "D,A,B"},
    {"ps_merge01", 4, 560, R, "D,A,B"},
    {"ps_merge10", 4, 592, R, "D,A,B"},
    {"ps_merge11", 4, 624, R, "D,A,B"},
    {"dcbz_l", 4, 1014, N, "A,B"},
};
// clang-format on

// A plain instruction after its syntax string has been decoded.
struct Instr
{
  const InstrRow* row;
  u32 base_bits;
  u32 count;
  ParseAlg alg;
  std::array<Field, MAX_OPERANDS> fields;
};

const Field& FieldByName(std::string_view name)
{
  for (const NamedField& f : kFields)
  {
    if (f.name == name)
      return f.field;
  }
  ASSERT_MSG(COMMON, false, "Unknown operand field {}", name);
  return kFields[0].field;
}

Instr DecodeRow(const InstrRow& row)
{
  Instr instr{&row, (row.opcode << 26) | (row.xo << 1) | row.fixed_bits, 0, ParseAlg::None, {}};
  bool displacement = false;
  std::string_view rest = row.syntax;
  while (!rest.empty())
  {
    const size_t cut = rest.find_first_of(",()");
    const std::string_view name = rest.substr(0, cut);
    if (!name.empty())
      instr.fields[instr.count++] = FieldByName(name);
    if (cut == std::string_view::npos)
      break;
    displacement |= rest[cut] == '(';
    rest.remove_prefix(cut + 1);
  }

  static constexpr ParseAlg by_count[] = {ParseAlg::None, ParseAlg::Op1, ParseAlg::Op2,
                                          ParseAlg::Op3,  ParseAlg::Op4, ParseAlg::Op5};
  if (displacement)
    instr.alg = instr.count == 3 ? ParseAlg::Op1Off1 : ParseAlg::Op1Off1Op2;
  else
    instr.alg = by_count[instr.count];
  return instr;
}

const std::vector<Instr>& Instrs()
{
  static const std::vector<Instr> instrs = [] {
    std::vector<Instr> v;
    for (const InstrRow& row : kInstrRows)
      v.push_back(DecodeRow(row));
    return v;
  }();
  return instrs;
}

size_t InstrIndex(std::string_view name)
{
  for (size_t i = 0; i < std::size(kInstrRows); i++)
  {
    if (kInstrRows[i].name == name)
      return i;
  }
  ASSERT_MSG(COMMON, false, "Unknown base mnemonic {}", name);
  return 0;
}

u32 VariantBits(const Instr& instr, u32 variant)
{
  u32 bits = 0;
  if (variant & 1)
    bits |= 1;  // Rc or LK
  if (variant & 2)
    bits |= instr.row->suffix == Suffix::kOeRc ? 0x400 : 0x2;  // OE or AA
  return bits;
}

// ---------------------------------------------------------------------------------------------
// Simplified mnemonics, which rewrite their operands into a plain instruction's.

enum class Rewrite : u8
{
  NegateImm,        // subi rD,rA,v -> addi rD,rA,-v
  SwapOperands12,   // swap the values of the 2nd and 3rd operands
  CompareWord,      // optional crf, then L=0
  ExtractLeft,      // extlwi
  ExtractRight,     // extrwi
  InsertLeft,       // inslwi
  InsertRight,      // insrwi
  RotateLeft,       // rotlwi, rotlw
  RotateRight,      // rotrwi
  ShiftLeft,        // slwi
  ShiftRight,       // srwi
  ClearLeft,        // clrlwi
  ClearRight,       // clrrwi
  ClearLeftShift,   // clrlslwi
  PrependBO,        // bt, bdnzt, btlr ...: BO from `param`
  PrependBOZeroBI,  // bdnz, blr ...: BO from `param`, BI = 0
  CondBranch,       // blt ...: BO from `param`, BI from an optional crf and `cond`
  CrSelf3,          // crset: d, d, d
  Copy2nd,          // crmove, mr, not: duplicate the second operand
  PrependTO,        // twlt ...
  TrapAlways,       // trap
  PrependSpr,       // mtlr ...: SPR first
  InsertSpr2nd,     // mflr ...: SPR second
  SprgFirst,        // mtsprg n, rS
  SprgSecond,       // mfsprg rD, n
  BatFirst,         // mtibatu n, rS
  BatSecond,        // mfibatu rD, n
  Nop,
  ZeroRA,           // li, lis
  MtcrAll,
  BitswapFirst,     // mtspr
  BitswapSecond,    // mfspr, mftb
};

struct AliasRow
{
  std::string name;
  u8 suffixes;  // see kAlias* below
  ParseAlg alg;
  size_t target;
  Rewrite rewrite;
  u32 param = 0;
  u32 cond = 0;
  u32 needed = 0;  // operand count when the optional leading one is present
};

// Spelling sets for simplified mnemonics.
enum : u8
{
  kAliasPlain,
  kAliasRc,
  kAliasOeRc,
  kAliasLk,       // x, xl
  kAliasLkPred,   // x, xl, and each with - or +
  kAliasLkAaPred  // x, xl, xa, xla, and each with - or +
};

constexpr u32 SprSwap(u32 spr)
{
  return ((spr & 0x1f) << 5) | ((spr >> 5) & 0x1f);
}

std::vector<AliasRow> BuildAliases()
{
  std::vector<AliasRow> out;
  const auto add = [&out](std::string name, u8 suffixes, ParseAlg alg, std::string_view target,
                          Rewrite rw, u32 param = 0, u32 cond = 0, u32 needed = 0) {
    out.push_back({std::move(name), suffixes, alg, InstrIndex(target), rw, param, cond, needed});
  };

  add("subi", kAliasPlain, ParseAlg::Op3, "addi", Rewrite::NegateImm);
  add("subis", kAliasPlain, ParseAlg::Op3, "addis", Rewrite::NegateImm);
  add("subic", kAliasPlain, ParseAlg::Op3, "addic", Rewrite::NegateImm);
  add("subic.", kAliasPlain, ParseAlg::Op3, "addic.", Rewrite::NegateImm);
  add("sub", kAliasOeRc, ParseAlg::Op3, "subf", Rewrite::SwapOperands12);
  add("subc", kAliasOeRc, ParseAlg::Op3, "subfc", Rewrite::SwapOperands12);

  add("cmpwi", kAliasPlain, ParseAlg::Op2Or3, "cmpi", Rewrite::CompareWord);
  add("cmpw", kAliasPlain, ParseAlg::Op2Or3, "cmp", Rewrite::CompareWord);
  add("cmplwi", kAliasPlain, ParseAlg::Op2Or3, "cmpli", Rewrite::CompareWord);
  add("cmplw", kAliasPlain, ParseAlg::Op2Or3, "cmpl", Rewrite::CompareWord);

  add("extlwi", kAliasRc, ParseAlg::Op4, "rlwinm", Rewrite::ExtractLeft);
  add("extrwi", kAliasRc, ParseAlg::Op4, "rlwinm", Rewrite::ExtractRight);
  add("inslwi", kAliasRc, ParseAlg::Op4, "rlwimi", Rewrite::InsertLeft);
  add("insrwi", kAliasRc, ParseAlg::Op4, "rlwimi", Rewrite::InsertRight);
  add("rotlwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::RotateLeft);
  add("rotrwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::RotateRight);
  add("rotlw", kAliasRc, ParseAlg::Op3, "rlwnm", Rewrite::RotateLeft);
  add("slwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::ShiftLeft);
  add("srwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::ShiftRight);
  add("clrlwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::ClearLeft);
  add("clrrwi", kAliasRc, ParseAlg::Op3, "rlwinm", Rewrite::ClearRight);
  add("clrlslwi", kAliasRc, ParseAlg::Op4, "rlwinm", Rewrite::ClearLeftShift);

  // Branches on a condition bit or the count register. BO values are for the unpredicted form;
  // a '+' suffix sets the prediction bit (BO + 1).
  struct CtrForm
  {
    const char* stem;
    u32 bo;
    bool needs_bi;
  };
  static constexpr CtrForm ctr_forms[] = {
      {"t", 12, true},    {"f", 4, true},     {"dnz", 16, false}, {"dnzt", 8, true},
      {"dnzf", 0, true},  {"dz", 18, false},  {"dzt", 10, true},  {"dzf", 2, true},
  };
  for (const CtrForm& f : ctr_forms)
  {
    const Rewrite rw = f.needs_bi ? Rewrite::PrependBO : Rewrite::PrependBOZeroBI;
    add(fmt::format("b{}", f.stem), kAliasLkAaPred, f.needs_bi ? ParseAlg::Op2 : ParseAlg::Op1,
        "bc", rw, f.bo);
    add(fmt::format("b{}lr", f.stem), kAliasLkPred, f.needs_bi ? ParseAlg::Op1 : ParseAlg::None,
        "bclr", rw, f.bo);
    if (f.bo == 12 || f.bo == 4)
      add(fmt::format("b{}ctr", f.stem), kAliasLkPred, ParseAlg::Op1, "bcctr", rw, f.bo);
  }
  add("blr", kAliasLk, ParseAlg::None, "bclr", Rewrite::PrependBOZeroBI, 20);
  add("bctr", kAliasLk, ParseAlg::None, "bcctr", Rewrite::PrependBOZeroBI, 20);

  // Branches on a named condition, with an optional CR field.
  struct CondForm
  {
    const char* name;
    u32 bo;
    u32 bit;
  };
  static constexpr CondForm cond_forms[] = {
      {"lt", 12, 0}, {"le", 4, 1}, {"eq", 12, 2}, {"ge", 4, 0}, {"gt", 12, 1}, {"nl", 4, 0},
      {"ne", 4, 2},  {"ng", 4, 1}, {"so", 12, 3}, {"ns", 4, 3}, {"un", 12, 3}, {"nu", 4, 3},
  };
  for (const CondForm& c : cond_forms)
  {
    add(fmt::format("b{}", c.name), kAliasLkAaPred, ParseAlg::Op1Or2, "bc", Rewrite::CondBranch,
        c.bo, c.bit, 2);
    add(fmt::format("b{}lr", c.name), kAliasLkPred, ParseAlg::NoneOrOp1, "bclr",
        Rewrite::CondBranch, c.bo, c.bit, 1);
    add(fmt::format("b{}ctr", c.name), kAliasLkPred, ParseAlg::NoneOrOp1, "bcctr",
        Rewrite::CondBranch, c.bo, c.bit, 1);
  }

  add("crset", kAliasPlain, ParseAlg::Op1, "creqv", Rewrite::CrSelf3);
  add("crclr", kAliasPlain, ParseAlg::Op1, "crxor", Rewrite::CrSelf3);
  add("crmove", kAliasPlain, ParseAlg::Op2, "cror", Rewrite::Copy2nd);
  add("crnot", kAliasPlain, ParseAlg::Op2, "crnor", Rewrite::Copy2nd);

  struct TrapForm
  {
    const char* name;
    u32 to;
  };
  static constexpr TrapForm trap_forms[] = {
      {"lt", 16}, {"le", 20}, {"eq", 4},  {"ge", 12}, {"gt", 8}, {"nl", 12}, {"ne", 24},
      {"ng", 20}, {"llt", 2}, {"lle", 6}, {"lge", 5}, {"lgt", 1}, {"lnl", 5}, {"lng", 6},
  };
  for (const TrapForm& t : trap_forms)
  {
    add(fmt::format("tw{}", t.name), kAliasPlain, ParseAlg::Op2, "tw", Rewrite::PrependTO, t.to);
    add(fmt::format("tw{}i", t.name), kAliasPlain, ParseAlg::Op2, "twi", Rewrite::PrependTO,
        t.to);
  }
  add("trap", kAliasPlain, ParseAlg::None, "tw", Rewrite::TrapAlways);

  struct SprForm
  {
    const char* name;
    u32 spr;
  };
  static constexpr SprForm spr_forms[] = {
      {"xer", 1},   {"lr", 8},     {"ctr", 9},    {"dsisr", 18}, {"dar", 19},  {"dec", 22},
      {"sdr1", 25}, {"srr0", 26},  {"srr1", 27},  {"asr", 280},  {"ear", 282},
  };
  for (const SprForm& s : spr_forms)
  {
    add(fmt::format("mt{}", s.name), kAliasPlain, ParseAlg::Op1, "mtspr_nobitswap",
        Rewrite::PrependSpr, SprSwap(s.spr));
    add(fmt::format("mf{}", s.name), kAliasPlain, ParseAlg::Op1, "mfspr_nobitswap",
        Rewrite::InsertSpr2nd, SprSwap(s.spr));
  }
  add("mttbl", kAliasPlain, ParseAlg::Op1, "mtspr_nobitswap", Rewrite::PrependSpr, SprSwap(284));
  add("mftbl", kAliasPlain, ParseAlg::Op1, "mftb_nobitswap", Rewrite::InsertSpr2nd,
      SprSwap(268));
  add("mttbu", kAliasPlain, ParseAlg::Op1, "mtspr_nobitswap", Rewrite::PrependSpr, SprSwap(285));
  add("mftbu", kAliasPlain, ParseAlg::Op1, "mftb_nobitswap", Rewrite::InsertSpr2nd,
      SprSwap(269));
  add("mtsprg", kAliasPlain, ParseAlg::Op2, "mtspr_nobitswap", Rewrite::SprgFirst, 272);
  add("mfsprg", kAliasPlain, ParseAlg::Op2, "mfspr_nobitswap", Rewrite::SprgSecond, 272);

  struct BatForm
  {
    const char* name;
    u32 first_spr;
  };
  static constexpr BatForm bat_forms[] = {
      {"ibatu", 528}, {"ibatl", 529}, {"dbatu", 536}, {"dbatl", 537}};
  for (const BatForm& b : bat_forms)
  {
    add(fmt::format("mt{}", b.name), kAliasPlain, ParseAlg::Op2, "mtspr_nobitswap",
        Rewrite::BatFirst, b.first_spr);
    add(fmt::format("mf{}", b.name), kAliasPlain, ParseAlg::Op2, "mfspr_nobitswap",
        Rewrite::BatSecond, b.first_spr);
  }

  add("nop", kAliasPlain, ParseAlg::None, "ori", Rewrite::Nop);
  add("li", kAliasPlain, ParseAlg::Op2, "addi", Rewrite::ZeroRA);
  add("lis", kAliasPlain, ParseAlg::Op2, "addis", Rewrite::ZeroRA);
  add("la", kAliasPlain, ParseAlg::Op1Off1, "addi", Rewrite::SwapOperands12);
  add("mr", kAliasRc, ParseAlg::Op2, "or", Rewrite::Copy2nd);
  add("not", kAliasRc, ParseAlg::Op2, "nor", Rewrite::Copy2nd);
  add("mtcr", kAliasPlain, ParseAlg::Op1, "mtcrf", Rewrite::MtcrAll);
  add("mfspr", kAliasPlain, ParseAlg::Op2, "mfspr_nobitswap", Rewrite::BitswapSecond);
  add("mftb", kAliasPlain, ParseAlg::Op2, "mftb_nobitswap", Rewrite::BitswapSecond);
  add("mtspr", kAliasPlain, ParseAlg::Op2, "mtspr_nobitswap", Rewrite::BitswapFirst);
  return out;
}

const std::vector<AliasRow>& Aliases()
{
  static const std::vector<AliasRow> aliases = BuildAliases();
  return aliases;
}

// Handle layout for simplified mnemonics: bits 0-1 variant, bit 2 '+' prediction, rest index.
constexpr size_t kPredictBit = 4;

void ApplyRewrite(const AliasRow& alias, bool predict, OperandList& ops)
{
  const u32 bo = alias.param + (predict ? 1 : 0);
  switch (alias.rewrite)
  {
  case Rewrite::NegateImm:
    ops[2] = 0u - ops[2];
    break;
  case Rewrite::SwapOperands12:
    std::swap(ops[1], ops[2]);
    break;
  case Rewrite::CompareWord:
    if (ops.count == 2)
      ops.Insert(0, 0);
    ops.Insert(1, 0);
    break;
  case Rewrite::ExtractLeft:
  {
    const u32 n = ops[2], b = ops[3];
    ops[2] = b;
    ops[3] = 0;
    ops.Insert(4, n - 1);
    break;
  }
  case Rewrite::ExtractRight:
  {
    const u32 n = ops[2], b = ops[3];
    ops[2] = b + n;
    ops[3] = 32 - n;
    ops.Insert(4, 31);
    break;
  }
  case Rewrite::InsertLeft:
  {
    const u32 n = ops[2], b = ops[3];
    ops[2] = 32 - b;
    ops[3] = b;
    ops.Insert(4, b + n - 1);
    break;
  }
  case Rewrite::InsertRight:
  {
    const u32 n = ops[2], b = ops[3];
    ops[2] = 32 - (b + n);
    ops[3] = b;
    ops.Insert(4, b + n - 1);
    break;
  }
  case Rewrite::RotateLeft:
    ops.Insert(3, 0);
    ops.Insert(4, 31);
    break;
  case Rewrite::RotateRight:
    ops[2] = 32 - ops[2];
    ops.Insert(3, 0);
    ops.Insert(4, 31);
    break;
  case Rewrite::ShiftLeft:
  {
    const u32 n = ops[2];
    ops.Insert(3, 0);
    ops.Insert(4, 31 - n);
    break;
  }
  case Rewrite::ShiftRight:
  {
    const u32 n = ops[2];
    ops[2] = 32 - n;
    ops.Insert(3, n);
    ops.Insert(4, 31);
    break;
  }
  case Rewrite::ClearLeft:
  {
    const u32 n = ops[2];
    ops[2] = 0;
    ops.Insert(3, n);
    ops.Insert(4, 31);
    break;
  }
  case Rewrite::ClearRight:
  {
    const u32 n = ops[2];
    ops[2] = 0;
    ops.Insert(3, 0);
    ops.Insert(4, 31 - n);
    break;
  }
  case Rewrite::ClearLeftShift:
  {
    const u32 b = ops[2], n = ops[3];
    ops[2] = n;
    ops[3] = b - n;
    ops.Insert(4, 31 - n);
    break;
  }
  case Rewrite::PrependBO:
    ops.Insert(0, bo);
    break;
  case Rewrite::PrependBOZeroBI:
    ops.Insert(0, bo);
    ops.Insert(1, 0);
    break;
  case Rewrite::CondBranch:
    if (ops.count < alias.needed)
      ops.Insert(0, 0);
    ops[0] = (ops[0] << 2) | alias.cond;
    ops.Insert(0, bo);
    break;
  case Rewrite::CrSelf3:
    ops.Insert(1, ops[0]);
    ops.Insert(2, ops[0]);
    break;
  case Rewrite::Copy2nd:
    ops.Insert(2, ops[1]);
    break;
  case Rewrite::PrependTO:
    ops.Insert(0, alias.param);
    break;
  case Rewrite::TrapAlways:
    ops.Insert(0, 31);
    ops.Insert(1, 0);
    ops.Insert(2, 0);
    break;
  case Rewrite::PrependSpr:
    ops.Insert(0, alias.param);
    break;
  case Rewrite::InsertSpr2nd:
    ops.Insert(1, alias.param);
    break;
  case Rewrite::SprgFirst:
    ops[0] = SprSwap(ops[0] + alias.param);
    break;
  case Rewrite::SprgSecond:
    ops[1] = SprSwap(ops[1] + alias.param);
    break;
  case Rewrite::BatFirst:
    ops[0] = SprSwap(2 * ops[0] + alias.param);
    break;
  case Rewrite::BatSecond:
    ops[1] = SprSwap(2 * ops[1] + alias.param);
    break;
  case Rewrite::Nop:
    ops.Insert(0, 0);
    ops.Insert(1, 0);
    ops.Insert(2, 0);
    break;
  case Rewrite::ZeroRA:
    ops.Insert(1, 0);
    break;
  case Rewrite::MtcrAll:
    ops.Insert(0, 0xff);
    break;
  case Rewrite::BitswapFirst:
    ops[0] = SprSwap(ops[0]);
    break;
  case Rewrite::BitswapSecond:
    ops[1] = SprSwap(ops[1]);
    break;
  }
}

// ---------------------------------------------------------------------------------------------
// Name lookup

struct NameTables
{
  std::unordered_map<std::string, ParseInfo> plain;
  std::unordered_map<std::string, ParseInfo> simplified;
};

const NameTables& Names()
{
  static const NameTables tables = [] {
    NameTables t;
    const std::vector<Instr>& instrs = Instrs();
    for (size_t i = 0; i < instrs.size(); i++)
    {
      const Instr& in = instrs[i];
      const std::string base(in.row->name);
      const auto put = [&](std::string_view suffix, size_t variant) {
        t.plain.emplace(base + std::string(suffix), ParseInfo{i * 4 + variant, in.alg});
      };
      put("", 0);
      switch (in.row->suffix)
      {
      case Suffix::kNone:
        break;
      case Suffix::kRc:
        put(".", 1);
        break;
      case Suffix::kOeRc:
        put(".", 1);
        put("o", 2);
        put("o.", 3);
        break;
      case Suffix::kLk:
        put("l", 1);
        break;
      case Suffix::kAaLk:
        put("l", 1);
        put("a", 2);
        put("la", 3);
        break;
      }
    }

    const std::vector<AliasRow>& aliases = Aliases();
    for (size_t i = 0; i < aliases.size(); i++)
    {
      const AliasRow& a = aliases[i];
      const auto put = [&](std::string_view suffix, size_t handle_bits) {
        t.simplified.emplace(a.name + std::string(suffix),
                             ParseInfo{i * 8 + handle_bits, a.alg});
      };
      put("", 0);
      switch (a.suffixes)
      {
      case kAliasPlain:
        break;
      case kAliasRc:
        put(".", 1);
        break;
      case kAliasOeRc:
        put(".", 1);
        put("o", 2);
        put("o.", 3);
        break;
      case kAliasLk:
        put("l", 1);
        break;
      case kAliasLkPred:
      case kAliasLkAaPred:
      {
        const bool aa = a.suffixes == kAliasLkAaPred;
        static constexpr std::pair<std::string_view, size_t> link_forms[] = {
            {"", 0}, {"l", 1}, {"a", 2}, {"la", 3}};
        for (const auto& [form, bits] : link_forms)
        {
          if (!aa && (bits & 2))
            continue;
          if (!form.empty())
            put(form, bits);
          put(std::string(form) + "-", bits);
          put(std::string(form) + "+", bits | kPredictBit);
        }
        break;
      }
      }
    }
    return t;
  }();
  return tables;
}

std::string LowerAscii(std::string_view s)
{
  std::string out(s);
  for (char& c : out)
  {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

const CaseInsensitiveDict<u32, '_'> kSprNames = {
    {"xer", 1},      {"lr", 8},       {"ctr", 9},      {"dsisr", 18},   {"dar", 19},
    {"dec", 22},     {"sdr1", 25},    {"srr0", 26},    {"srr1", 27},    {"sprg0", 272},
    {"sprg1", 273},  {"sprg2", 274},  {"sprg3", 275},  {"ear", 282},    {"tbl", 284},
    {"tbu", 285},    {"ibat0u", 528}, {"ibat0l", 529}, {"ibat1u", 530}, {"ibat1l", 531},
    {"ibat2u", 532}, {"ibat2l", 533}, {"ibat3u", 534}, {"ibat3l", 535}, {"dbat0u", 536},
    {"dbat0l", 537}, {"dbat1u", 538}, {"dbat1l", 539}, {"dbat2u", 540}, {"dbat2l", 541},
    {"dbat3u", 542}, {"dbat3l", 543}, {"gqr0", 912},   {"gqr1", 913},   {"gqr2", 914},
    {"gqr3", 915},   {"gqr4", 916},   {"gqr5", 917},   {"gqr6", 918},   {"gqr7", 919},
    {"hid2", 920},   {"wpar", 921},   {"dma_u", 922},  {"dma_l", 923},  {"ummcr0", 936},
    {"upmc1", 937},  {"upmc2", 938},  {"usia", 939},   {"ummcr1", 940}, {"upmc3", 941},
    {"upmc4", 942},  {"usda", 943},   {"mmcr0", 952},  {"pmc1", 953},   {"pmc2", 954},
    {"sia", 955},    {"mmcr1", 956},  {"pmc3", 957},   {"pmc4", 958},   {"sda", 959},
    {"hid0", 1008},  {"hid1", 1009},  {"iabr", 1010},  {"dabr", 1013},  {"l2cr", 1017},
    {"ictc", 1019},  {"thrm1", 1020}, {"thrm2", 1021}, {"thrm3", 1022},
};

const CaseInsensitiveDict<GekkoDirective> kDirectiveNames = {
    {"byte", GekkoDirective::Byte},       {"2byte", GekkoDirective::_2byte},
    {"4byte", GekkoDirective::_4byte},    {"8byte", GekkoDirective::_8byte},
    {"float", GekkoDirective::Float},     {"double", GekkoDirective::Double},
    {"locate", GekkoDirective::Locate},   {"padalign", GekkoDirective::PadAlign},
    {"align", GekkoDirective::Align},     {"zeros", GekkoDirective::Zeros},
    {"skip", GekkoDirective::Skip},       {"defvar", GekkoDirective::DefVar},
    {"ascii", GekkoDirective::Ascii},     {"asciz", GekkoDirective::Asciz},
};

std::string RangeMessage(const Field& f, u32 value)
{
  std::string text;
  if (f.is_signed)
  {
    text = fmt::format("{:#x} not between {:#x} and {:#x}", static_cast<s32>(value),
                       static_cast<s32>(f.Min()), static_cast<s32>(f.Max()));
  }
  else
  {
    text = fmt::format("{:#x} not between {:#x} and {:#x}", value, f.Min(), f.Max());
  }
  if (f.AlignMask() != 0)
    text += fmt::format(" or not aligned to {}", f.AlignMask() + 1);
  return text;
}
}  // namespace

const ParseInfo* FindMnemonic(std::string_view name, bool* extended)
{
  const NameTables& names = Names();
  const std::string key = LowerAscii(name);
  if (const auto it = names.plain.find(key); it != names.plain.end())
  {
    *extended = false;
    return &it->second;
  }
  if (const auto it = names.simplified.find(key); it != names.simplified.end())
  {
    *extended = true;
    return &it->second;
  }
  return nullptr;
}

std::optional<u32> FindSprName(std::string_view name)
{
  if (const u32* spr = kSprNames.Find(name))
    return *spr;
  return std::nullopt;
}

const GekkoDirective* FindDirective(std::string_view name)
{
  return kDirectiveNames.Find(name);
}

void OperandList::Insert(size_t before, u32 val)
{
  overfill = count == MAX_OPERANDS;
  const size_t last = std::min<size_t>(count, MAX_OPERANDS - 1);
  for (size_t i = last; i > before; i--)
    list[i] = list[i - 1];
  list[before] = Tagged<Interval, u32>(Interval{0, 0}, val);
  if (!overfill)
    count++;
}

FailureOr<u32> EncodeInstruction(size_t mnemonic_index, bool extended, OperandList operands)
{
  size_t instr_index = mnemonic_index / 4;
  const u32 variant = static_cast<u32>(mnemonic_index & 3);
  if (extended)
  {
    const AliasRow& alias = Aliases()[mnemonic_index / 8];
    ApplyRewrite(alias, (mnemonic_index & kPredictBit) != 0, operands);
    instr_index = alias.target;
  }
  const Instr& instr = Instrs()[instr_index];

  // GNU as lets addis/lis take an unsigned 16-bit immediate as well.
  if (instr.row->opcode == 15 && operands[2] >= 0x8000 && operands[2] <= 0xffff)
    operands[2] -= 0x10000;

  ASSERT_MSG(COMMON, operands.count == instr.count && !operands.overfill,
             "Operand count mismatch for {}: expected {}, got {}", instr.row->name, instr.count,
             operands.overfill ? 6 : operands.count);

  u32 word = instr.base_bits | VariantBits(instr, variant);
  const u32 n = std::min(operands.count, instr.count);
  for (u32 i = 0; i < n; i++)
  {
    const Field& f = instr.fields[i];
    if (!f.Accepts(operands[i]))
    {
      const Interval& where = TagOf(operands.list[i]);
      return AssemblerError{RangeMessage(f, operands[i]), "", 0, where.begin, where.len};
    }
    word |= f.Place(operands[i]);
  }
  return word;
}
}  // namespace Common::GekkoAssembler::detail
