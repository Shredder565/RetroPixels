// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// PPCDisasm.cpp: PowerPC 603e disassembler (used by the debugger), and with
// -DSTANDALONE a small command line tool that disassembles a binary file.
//
// Instructions are found through lookup tables keyed by primary opcode and,
// for the four extended groups (19, 31, 59, 63), by the 10-bit extended
// opcode. Every bit of a word that is neither opcode nor an operand field of
// the instruction must be zero, or the word is not recognised.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef STANDALONE
#include <cstdlib>
#endif
#include "Supermodel.h"
#include "CPU/PowerPC/PPCDisasm.h"

namespace
{

// Bit fields of an instruction word.
constexpr uint32_t kFieldD     = 0x03E00000;   // rD / rS / frD / BO / TO / crbD
constexpr uint32_t kFieldA     = 0x001F0000;   // rA / frA / BI / crbA
constexpr uint32_t kFieldB     = 0x0000F800;   // rB / frB / crbB / SH / NB
constexpr uint32_t kFieldC     = 0x000007C0;   // frC / MB
constexpr uint32_t kFieldMe    = 0x0000003E;
constexpr uint32_t kFieldImm   = 0x0000FFFF;
constexpr uint32_t kFieldCrfD  = 0x03800000;
constexpr uint32_t kFieldL     = 0x00200000;
constexpr uint32_t kFieldCrfS  = 0x001C0000;
constexpr uint32_t kFieldSr    = 0x000F0000;
constexpr uint32_t kFieldU     = 0x0000F000;   // mtfsfi immediate
constexpr uint32_t kFieldCrm   = 0x000FF000;
constexpr uint32_t kFieldFm    = 0x01FE0000;
constexpr uint32_t kFieldSpr   = 0x001FF800;
constexpr uint32_t kFieldLi    = 0x03FFFFFC;
constexpr uint32_t kFieldBd    = 0x0000FFFC;
constexpr uint32_t kBitOe      = 0x00000400;
constexpr uint32_t kBitRc      = 0x00000001;   // also LK
constexpr uint32_t kBitAa      = 0x00000002;
constexpr uint32_t kExtOpcode  = 0x000007FE;
constexpr uint32_t kBelowOpcode = 0x03FFFFFF;

inline unsigned Rd(uint32_t op)   { return (op >> 21) & 31; }
inline unsigned Ra(uint32_t op)   { return (op >> 16) & 31; }
inline unsigned Rb(uint32_t op)   { return (op >> 11) & 31; }
inline unsigned Rc5(uint32_t op)  { return (op >> 6) & 31; }
inline unsigned Me(uint32_t op)   { return (op >> 1) & 31; }
inline unsigned CrfD(uint32_t op) { return (op >> 23) & 7; }
inline unsigned CrfS(uint32_t op) { return (op >> 18) & 7; }
inline unsigned LBit(uint32_t op) { return (op >> 21) & 1; }
inline unsigned Imm(uint32_t op)  { return op & 0xFFFF; }
inline uint32_t SignedImm(uint32_t op) { return (uint32_t) (int32_t) (int16_t) (op & 0xFFFF); }

// How the operands are laid out (and, through LayoutFields, which bits of
// the word belong to them).
enum class Layout : uint8_t
{
  None,
  Branch,         // target
  CondBranch,     // BO, BI-or-crN[bit], target
  BranchReg,      // BO, BI-or-crN[bit]
  AddImm,         // rD, rA|0, simm
  AddImmHigh,     // rD, rA|0, uimm (shown unsigned)
  RegRegSimm,     // rD, rA, simm
  LogicImm,       // rA, rS, uimm
  CmpSimm,        // crfD, L, rA, simm
  CmpUimm,        // crfD, L, rA, uimm
  Trap,           // TO, rA, rB
  TrapImm,        // TO, rA, simm
  Cmp,            // crfD, L, rA, rB
  RegRegReg,      // rD, rA, rB
  RegReg0Reg,     // rD, rA|0, rB
  LogicReg,       // rA, rS, rB
  Unary,          // rD, rA
  LogicUnary,     // rA, rS
  Reg,            // rD
  RegRegB,        // rD, rB
  CacheOp,        // rA|0, rB
  OnlyB,          // rB
  Load,           // rD, d(rA|0)
  LoadUpdate,     // rD, d(rA)
  FLoad,          // frD, d(rA|0)
  FLoadUpdate,    // frD, d(rA)
  FIndexed,       // frD, rA|0, rB
  FIndexedUpdate, // frD, rA, rB
  StringImm,      // rD, rA|0, NB (0 means 32)
  ShiftImm,       // rA, rS, SH
  Rotate,         // rA, rS, SH, MB, ME
  RotateReg,      // rA, rS, rB, MB, ME
  CrLogic,        // crbD, crbA, crbB
  CrField,        // crfD, crfS
  CrFieldOnly,    // crfD
  MoveFromSpr,    // rD, spr
  MoveToSpr,      // spr, rS
  MoveFromSr,     // rD, srN
  MoveToSr,       // srN, rS
  MoveToCr,       // CRM, rS
  FUnary,         // frD, frB
  FBinary,        // frD, frA, frB
  FMul,           // frD, frA, frC
  FMulAdd,        // frD, frA, frC, frB
  FCompare,       // crfD, frA, frB
  FMoveFromFpscr, // frD
  FpscrBit,       // fpscr[bit]
  FpscrField,     // crfD, imm
  FpscrFields     // FM, frB
};

uint32_t LayoutFields(Layout layout)
{
  switch (layout)
  {
  case Layout::None:           return 0;
  case Layout::Branch:         return kFieldLi;
  case Layout::CondBranch:     return kFieldD | kFieldA | kFieldBd;
  case Layout::BranchReg:      return kFieldD | kFieldA;
  case Layout::AddImm:
  case Layout::AddImmHigh:
  case Layout::RegRegSimm:
  case Layout::LogicImm:
  case Layout::TrapImm:
  case Layout::Load:
  case Layout::LoadUpdate:
  case Layout::FLoad:
  case Layout::FLoadUpdate:    return kFieldD | kFieldA | kFieldImm;
  case Layout::CmpSimm:
  case Layout::CmpUimm:        return kFieldCrfD | kFieldL | kFieldA | kFieldImm;
  case Layout::Cmp:            return kFieldCrfD | kFieldL | kFieldA | kFieldB;
  case Layout::Trap:
  case Layout::RegRegReg:
  case Layout::RegReg0Reg:
  case Layout::LogicReg:
  case Layout::FIndexed:
  case Layout::FIndexedUpdate:
  case Layout::StringImm:
  case Layout::ShiftImm:
  case Layout::CrLogic:
  case Layout::FBinary:        return kFieldD | kFieldA | kFieldB;
  case Layout::Unary:
  case Layout::LogicUnary:     return kFieldD | kFieldA;
  case Layout::Reg:
  case Layout::FMoveFromFpscr:
  case Layout::FpscrBit:       return kFieldD;
  case Layout::RegRegB:
  case Layout::FUnary:         return kFieldD | kFieldB;
  case Layout::CacheOp:        return kFieldA | kFieldB;
  case Layout::OnlyB:          return kFieldB;
  case Layout::Rotate:
  case Layout::RotateReg:      return kFieldD | kFieldA | kFieldB | kFieldC | kFieldMe;
  case Layout::CrField:        return kFieldCrfD | kFieldCrfS;
  case Layout::CrFieldOnly:    return kFieldCrfD;
  case Layout::MoveFromSpr:
  case Layout::MoveToSpr:      return kFieldD | kFieldSpr;
  case Layout::MoveFromSr:
  case Layout::MoveToSr:       return kFieldD | kFieldSr;
  case Layout::MoveToCr:       return kFieldD | kFieldCrm;
  case Layout::FMul:           return kFieldD | kFieldA | kFieldC;
  case Layout::FMulAdd:        return kFieldD | kFieldA | kFieldB | kFieldC;
  case Layout::FCompare:       return kFieldCrfD | kFieldA | kFieldB;
  case Layout::FpscrField:     return kFieldCrfD | kFieldU;
  case Layout::FpscrFields:    return kFieldFm | kFieldB;
  }
  return 0;
}

// Options of an instruction.
enum : uint16_t
{
  kOptOe        = 1 << 0,   // has an OE bit ("o" suffix)
  kOptRc        = 1 << 1,   // has an Rc bit ("." suffix)
  kOptLk        = 1 << 2,   // has an LK bit ("l" suffix)
  kOptAa        = 1 << 3,   // has an AA bit ("a" suffix)
  kOptRcSet     = 1 << 4,   // the Rc bit must be 1 (stwcx.)
  kOptExact     = 1 << 5,   // the word must be exactly opcode | 2 (sc)
  kOptBaseNotRd = 1 << 6,   // invalid if rA is 0 or equals rD
  kOptBaseNot0  = 1 << 7,   // invalid if rA is 0
  kOptLswi      = 1 << 8,   // lswi register range check
  kOptLswx      = 1 << 9    // lswx register check
};

struct Insn
{
  const char *name;
  Layout layout;
  uint16_t opts;
};

struct Keyed
{
  unsigned key;
  Insn insn;
};

constexpr uint16_t kOeRc = kOptOe | kOptRc;

const Keyed kPrimary[] = {
  {  3, { "twi",    Layout::TrapImm,     0 } },
  {  7, { "mulli",  Layout::RegRegSimm,  0 } },
  {  8, { "subfic", Layout::RegRegSimm,  0 } },
  { 10, { "cmpli",  Layout::CmpUimm,     0 } },
  { 11, { "cmpi",   Layout::CmpSimm,     0 } },
  { 12, { "addic",  Layout::RegRegSimm,  0 } },
  { 13, { "addic.", Layout::RegRegSimm,  0 } },
  { 14, { "addi",   Layout::AddImm,      0 } },
  { 15, { "addis",  Layout::AddImmHigh,  0 } },
  { 16, { "bc",     Layout::CondBranch,  kOptAa | kOptLk } },
  { 17, { "sc",     Layout::None,        kOptExact } },
  { 18, { "b",      Layout::Branch,      kOptAa | kOptLk } },
  { 20, { "rlwimi", Layout::Rotate,      kOptRc } },
  { 21, { "rlwinm", Layout::Rotate,      kOptRc } },
  { 23, { "rlwnm",  Layout::RotateReg,   kOptRc } },
  { 24, { "ori",    Layout::LogicImm,    0 } },
  { 25, { "oris",   Layout::LogicImm,    0 } },
  { 26, { "xori",   Layout::LogicImm,    0 } },
  { 27, { "xoris",  Layout::LogicImm,    0 } },
  { 28, { "andi.",  Layout::LogicImm,    0 } },
  { 29, { "andis.", Layout::LogicImm,    0 } },
  { 32, { "lwz",    Layout::Load,        0 } },
  { 33, { "lwzu",   Layout::LoadUpdate,  kOptBaseNotRd } },
  { 34, { "lbz",    Layout::Load,        0 } },
  { 35, { "lbzu",   Layout::LoadUpdate,  kOptBaseNotRd } },
  { 36, { "stw",    Layout::Load,        0 } },
  { 37, { "stwu",   Layout::LoadUpdate,  kOptBaseNot0 } },
  { 38, { "stb",    Layout::Load,        0 } },
  { 39, { "stbu",   Layout::LoadUpdate,  kOptBaseNot0 } },
  { 40, { "lhz",    Layout::Load,        0 } },
  { 41, { "lhzu",   Layout::LoadUpdate,  kOptBaseNotRd } },
  { 42, { "lha",    Layout::Load,        0 } },
  { 43, { "lhau",   Layout::LoadUpdate,  kOptBaseNotRd } },
  { 44, { "sth",    Layout::Load,        0 } },
  { 45, { "sthu",   Layout::LoadUpdate,  kOptBaseNot0 } },
  { 46, { "lmw",    Layout::Load,        0 } },
  { 47, { "stmw",   Layout::Load,        0 } },
  { 48, { "lfs",    Layout::FLoad,       0 } },
  { 49, { "lfsu",   Layout::FLoadUpdate, kOptBaseNot0 } },
  { 50, { "lfd",    Layout::FLoad,       0 } },
  { 51, { "lfdu",   Layout::FLoadUpdate, kOptBaseNot0 } },
  { 52, { "stfs",   Layout::FLoad,       0 } },
  { 53, { "stfsu",  Layout::FLoadUpdate, kOptBaseNot0 } },
  { 54, { "stfd",   Layout::FLoad,       0 } },
  { 55, { "stfdu",  Layout::FLoadUpdate, kOptBaseNot0 } },
};

const Keyed kGroup19[] = {
  {   0, { "mcrf",   Layout::CrField,   0 } },
  {  16, { "bclr",   Layout::BranchReg, kOptLk } },
  {  33, { "crnor",  Layout::CrLogic,   0 } },
  {  50, { "rfi",    Layout::None,      0 } },
  { 129, { "crandc", Layout::CrLogic,   0 } },
  { 150, { "isync",  Layout::None,      0 } },
  { 193, { "crxor",  Layout::CrLogic,   0 } },
  { 225, { "crnand", Layout::CrLogic,   0 } },
  { 257, { "crand",  Layout::CrLogic,   0 } },
  { 289, { "creqv",  Layout::CrLogic,   0 } },
  { 417, { "crorc",  Layout::CrLogic,   0 } },
  { 449, { "cror",   Layout::CrLogic,   0 } },
  { 528, { "bcctr",  Layout::BranchReg, kOptLk } },
};

const Keyed kGroup31[] = {
  {    0, { "cmp",     Layout::Cmp,            0 } },
  {    4, { "tw",      Layout::Trap,           0 } },
  {    8, { "subfc",   Layout::RegRegReg,      kOeRc } },
  {   10, { "addc",    Layout::RegRegReg,      kOeRc } },
  {   11, { "mulhwu",  Layout::RegRegReg,      kOptRc } },
  {   19, { "mfcr",    Layout::Reg,            0 } },
  {   20, { "lwarx",   Layout::RegReg0Reg,     0 } },
  {   23, { "lwzx",    Layout::RegReg0Reg,     0 } },
  {   24, { "slw",     Layout::LogicReg,       kOptRc } },
  {   26, { "cntlzw",  Layout::LogicUnary,     kOptRc } },
  {   28, { "and",     Layout::LogicReg,       kOptRc } },
  {   32, { "cmpl",    Layout::Cmp,            0 } },
  {   40, { "subf",    Layout::RegRegReg,      kOeRc } },
  {   54, { "dcbst",   Layout::CacheOp,        0 } },
  {   55, { "lwzux",   Layout::RegRegReg,      kOptBaseNotRd } },
  {   60, { "andc",    Layout::LogicReg,       kOptRc } },
  {   75, { "mulhw",   Layout::RegRegReg,      kOptRc } },
  {   83, { "mfmsr",   Layout::Reg,            0 } },
  {   86, { "dcbf",    Layout::CacheOp,        0 } },
  {   87, { "lbzx",    Layout::RegReg0Reg,     0 } },
  {  104, { "neg",     Layout::Unary,          kOeRc } },
  {  119, { "lbzux",   Layout::RegRegReg,      kOptBaseNotRd } },
  {  124, { "nor",     Layout::LogicReg,       kOptRc } },
  {  136, { "subfe",   Layout::RegRegReg,      kOeRc } },
  {  138, { "adde",    Layout::RegRegReg,      kOeRc } },
  {  144, { "mtcrf",   Layout::MoveToCr,       0 } },
  {  146, { "mtmsr",   Layout::Reg,            0 } },
  {  150, { "stwcx.",  Layout::RegReg0Reg,     kOptRcSet } },
  {  151, { "stwx",    Layout::RegReg0Reg,     0 } },
  {  183, { "stwux",   Layout::RegRegReg,      kOptBaseNot0 } },
  {  200, { "subfze",  Layout::Unary,          kOeRc } },
  {  202, { "addze",   Layout::Unary,          kOeRc } },
  {  210, { "mtsr",    Layout::MoveToSr,       0 } },
  {  215, { "stbx",    Layout::RegReg0Reg,     0 } },
  {  232, { "subfme",  Layout::Unary,          kOeRc } },
  {  234, { "addme",   Layout::Unary,          kOeRc } },
  {  235, { "mullw",   Layout::RegRegReg,      kOeRc } },
  {  242, { "mtsrin",  Layout::RegRegB,        0 } },
  {  246, { "dcbtst",  Layout::CacheOp,        0 } },
  {  247, { "stbux",   Layout::RegRegReg,      kOptBaseNot0 } },
  {  266, { "add",     Layout::RegRegReg,      kOeRc } },
  {  278, { "dcbt",    Layout::CacheOp,        0 } },
  {  279, { "lhzx",    Layout::RegReg0Reg,     0 } },
  {  284, { "eqv",     Layout::LogicReg,       kOptRc } },
  {  306, { "tlbie",   Layout::OnlyB,          0 } },
  {  310, { "eciwx",   Layout::RegReg0Reg,     0 } },
  {  311, { "lhzux",   Layout::RegRegReg,      kOptBaseNotRd } },
  {  316, { "xor",     Layout::LogicReg,       kOptRc } },
  {  339, { "mfspr",   Layout::MoveFromSpr,    0 } },
  {  343, { "lhax",    Layout::RegReg0Reg,     0 } },
  {  370, { "tlbia",   Layout::None,           0 } },
  {  371, { "mftb",    Layout::MoveFromSpr,    0 } },
  {  375, { "lhaux",   Layout::RegRegReg,      kOptBaseNotRd } },
  {  407, { "sthx",    Layout::RegReg0Reg,     0 } },
  {  412, { "orc",     Layout::LogicReg,       kOptRc } },
  {  438, { "ecowx",   Layout::RegReg0Reg,     0 } },
  {  439, { "sthux",   Layout::RegRegReg,      kOptBaseNot0 } },
  {  444, { "or",      Layout::LogicReg,       kOptRc } },
  {  459, { "divwu",   Layout::RegRegReg,      kOeRc } },
  {  467, { "mtspr",   Layout::MoveToSpr,      0 } },
  {  470, { "dcbi",    Layout::CacheOp,        0 } },
  {  476, { "nand",    Layout::LogicReg,       kOptRc } },
  {  491, { "divw",    Layout::RegRegReg,      kOeRc } },
  {  512, { "mcrxr",   Layout::CrFieldOnly,    0 } },
  {  533, { "lswx",    Layout::RegReg0Reg,     kOptLswx } },
  {  534, { "lwbrx",   Layout::RegReg0Reg,     0 } },
  {  535, { "lfsx",    Layout::FIndexed,       0 } },
  {  536, { "srw",     Layout::LogicReg,       kOptRc } },
  {  566, { "tlbsync", Layout::None,           0 } },
  {  567, { "lfsux",   Layout::FIndexedUpdate, kOptBaseNot0 } },
  {  595, { "mfsr",    Layout::MoveFromSr,     0 } },
  {  597, { "lswi",    Layout::StringImm,      kOptLswi } },
  {  598, { "sync",    Layout::None,           0 } },
  {  599, { "lfdx",    Layout::FIndexed,       0 } },
  {  631, { "lfdux",   Layout::FIndexedUpdate, kOptBaseNot0 } },
  {  659, { "mfsrin",  Layout::RegRegB,        0 } },
  {  661, { "stswx",   Layout::RegReg0Reg,     0 } },
  {  662, { "stwbrx",  Layout::RegReg0Reg,     0 } },
  {  663, { "stfsx",   Layout::FIndexed,       0 } },
  {  695, { "stfsux",  Layout::FIndexedUpdate, kOptBaseNot0 } },
  {  725, { "stswi",   Layout::StringImm,      0 } },
  {  727, { "stfdx",   Layout::FIndexed,       0 } },
  {  758, { "dcba",    Layout::CacheOp,        0 } },
  {  759, { "stfdux",  Layout::FIndexedUpdate, kOptBaseNot0 } },
  {  790, { "lhbrx",   Layout::RegReg0Reg,     0 } },
  {  792, { "sraw",    Layout::LogicReg,       kOptRc } },
  {  824, { "srawi",   Layout::ShiftImm,       kOptRc } },
  {  854, { "eieio",   Layout::None,           0 } },
  {  918, { "sthbrx",  Layout::RegReg0Reg,     0 } },
  {  922, { "extsh",   Layout::LogicUnary,     kOptRc } },
  {  954, { "extsb",   Layout::LogicUnary,     kOptRc } },
  {  978, { "tlbld",   Layout::OnlyB,          0 } },   // 603e only
  {  982, { "icbi",    Layout::CacheOp,        0 } },
  {  983, { "stfiwx",  Layout::FIndexed,       0 } },
  { 1010, { "tlbli",   Layout::OnlyB,          0 } },   // 603e only
  { 1014, { "dcbz",    Layout::CacheOp,        0 } },
};

const Keyed kGroup59[] = {
  { 18, { "fdivs",   Layout::FBinary, kOptRc } },
  { 20, { "fsubs",   Layout::FBinary, kOptRc } },
  { 21, { "fadds",   Layout::FBinary, kOptRc } },
  { 22, { "fsqrts",  Layout::FUnary,  kOptRc } },
  { 24, { "fres",    Layout::FUnary,  kOptRc } },
  { 25, { "fmuls",   Layout::FMul,    kOptRc } },
  { 28, { "fmsubs",  Layout::FMulAdd, kOptRc } },
  { 29, { "fmadds",  Layout::FMulAdd, kOptRc } },
  { 30, { "fnmsubs", Layout::FMulAdd, kOptRc } },
  { 31, { "fnmadds", Layout::FMulAdd, kOptRc } },
};

const Keyed kGroup63[] = {
  {   0, { "fcmpu",   Layout::FCompare,       0 } },
  {  12, { "frsp",    Layout::FUnary,         kOptRc } },
  {  14, { "fctiw",   Layout::FUnary,         kOptRc } },
  {  15, { "fctiwz",  Layout::FUnary,         kOptRc } },
  {  18, { "fdiv",    Layout::FBinary,        kOptRc } },
  {  20, { "fsub",    Layout::FBinary,        kOptRc } },
  {  21, { "fadd",    Layout::FBinary,        kOptRc } },
  {  22, { "fsqrt",   Layout::FUnary,         kOptRc } },
  {  23, { "fsel",    Layout::FMulAdd,        kOptRc } },
  {  25, { "fmul",    Layout::FMul,           kOptRc } },
  {  26, { "frsqrte", Layout::FUnary,         kOptRc } },
  {  28, { "fmsub",   Layout::FMulAdd,        kOptRc } },
  {  29, { "fmadd",   Layout::FMulAdd,        kOptRc } },
  {  30, { "fnmsub",  Layout::FMulAdd,        kOptRc } },
  {  31, { "fnmadd",  Layout::FMulAdd,        kOptRc } },
  {  32, { "fcmpo",   Layout::FCompare,       0 } },
  {  38, { "mtfsb1",  Layout::FpscrBit,       kOptRc } },
  {  40, { "fneg",    Layout::FUnary,         kOptRc } },
  {  64, { "mcrfs",   Layout::CrField,        0 } },
  {  70, { "mtfsb0",  Layout::FpscrBit,       kOptRc } },
  {  72, { "fmr",     Layout::FUnary,         kOptRc } },
  { 134, { "mtfsfi",  Layout::FpscrField,     kOptRc } },
  { 136, { "fnabs",   Layout::FUnary,         kOptRc } },
  { 264, { "fabs",    Layout::FUnary,         kOptRc } },
  { 583, { "mffs",    Layout::FMoveFromFpscr, kOptRc } },
  { 711, { "mtfsf",   Layout::FpscrFields,    kOptRc } },
};

// Lookup tables, filled once. An instruction with an OE bit is entered under
// both values of it, and one whose frC field overlaps the extended opcode
// (A-form) under every frC value.
struct Tables
{
  const Insn *primary[64] = {};
  const Insn *group19[1024] = {};
  const Insn *group31[1024] = {};
  const Insn *group59[1024] = {};
  const Insn *group63[1024] = {};

  template <size_t N>
  static void Fill(const Insn **table, const Keyed (&list)[N])
  {
    for (const Keyed &k : list)
    {
      bool aForm = (LayoutFields(k.insn.layout) & kFieldC) != 0;
      unsigned spread = aForm ? 32 : 1;
      for (unsigned c = 0; c < spread; c++)
      {
        unsigned key = k.key | (c << 5);
        table[key] = &k.insn;
        if (k.insn.opts & kOptOe)
          table[key | 0x200] = &k.insn;
      }
    }
  }

  Tables()
  {
    for (const Keyed &k : kPrimary)
      primary[k.key] = &k.insn;
    Fill(group19, kGroup19);
    Fill(group31, kGroup31);
    Fill(group59, kGroup59);
    Fill(group63, kGroup63);
  }
};

const Insn *Find(uint32_t op)
{
  static const Tables tables;
  unsigned ext = (op >> 1) & 0x3FF;
  switch (op >> 26)
  {
  case 19: return tables.group19[ext];
  case 31: return tables.group31[ext];
  case 59: return tables.group59[ext];
  case 63: return tables.group63[ext];
  default: return tables.primary[op >> 26];
  }
}

// True when all bits of op outside the opcode, the instruction's fields and
// its option bits are zero (and the fixed bits sc and stwcx. need are set).
bool FitsForm(uint32_t op, const Insn &insn)
{
  if (insn.opts & kOptExact)
    return (op & kBelowOpcode) == kBitAa;
  uint32_t used = LayoutFields(insn.layout);
  unsigned primary = op >> 26;
  if (primary == 19 || primary == 31 || primary == 59 || primary == 63)
    used |= kExtOpcode;
  if (insn.opts & (kOptRc | kOptLk))
    used |= kBitRc;
  if (insn.opts & kOptAa)
    used |= kBitAa;
  if (insn.opts & kOptRcSet)
  {
    if (!(op & kBitRc))
      return false;
    used |= kBitRc;
  }
  return (op & kBelowOpcode & ~used) == 0;
}

// Small output buffer helper (truncates instead of overflowing).
class Out
{
public:
  Out(char *buf, size_t size) : buf_(buf), size_(size), len_(0)
  {
    if (size_)
      buf_[0] = '\0';
  }

  void Add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    if (len_ + 1 >= size_)
      return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf_ + len_, size_ - len_, fmt, ap);
    va_end(ap);
    if (n > 0)
      len_ = (len_ + (size_t) n < size_) ? len_ + (size_t) n : size_ - 1;
  }

private:
  char *buf_;
  size_t size_;
  size_t len_;
};

const char *const kCrBitName[4] = { "lt", "gt", "eq", "so" };

// Names of the special purpose registers the 603e has (plus a few others
// that may appear in code), by SPR number.
const char *SprName(unsigned spr)
{
  switch (spr)
  {
  case 1:    return "xer";
  case 8:    return "lr";
  case 9:    return "ctr";
  case 18:   return "dsisr";
  case 19:   return "dar";
  case 22:   return "dec";
  case 25:   return "sdr1";
  case 26:   return "srr0";
  case 27:   return "srr1";
  case 268:  return "tbl";
  case 269:  return "tbu";
  case 272:  return "sprg0";
  case 273:  return "sprg1";
  case 274:  return "sprg2";
  case 275:  return "sprg3";
  case 282:  return "ear";
  case 287:  return "pvr";
  case 976:  return "dmiss";
  case 977:  return "dcmp";
  case 978:  return "hash1";
  case 979:  return "hash2";
  case 980:  return "imiss";
  case 981:  return "icmp";
  case 982:  return "rpa";
  case 1008: return "hid0";
  case 1009: return "hid1";
  case 1010: return "iabr";
  case 1013: return "dabr";
  default:   break;
  }
  if (spr >= 528 && spr <= 543)
  {
    static const char *const bats[16] = {
      "ibat0u", "ibat0l", "ibat1u", "ibat1l", "ibat2u", "ibat2l", "ibat3u", "ibat3l",
      "dbat0u", "dbat0l", "dbat1u", "dbat1l", "dbat2u", "dbat2l", "dbat3u", "dbat3l" };
    return bats[spr - 528];
  }
  return nullptr;
}

void AddSpr(Out &out, uint32_t op)
{
  // The SPR number is stored with its two 5-bit halves swapped.
  unsigned field = (op >> 11) & 0x3FF;
  unsigned spr = ((field & 31) << 5) | (field >> 5);
  if (const char *name = SprName(spr))
    out.Add("%s", name);
  else
    out.Add("%u", spr);
}

// A 16-bit signed immediate as shown in operands: sign then hex magnitude.
void AddSigned16(Out &out, uint32_t op)
{
  int value = (int16_t) (op & 0xFFFF);
  if (value < 0)
    out.Add("-0x%02X", -value);
  else
    out.Add("0x%02X", value);
}

// A condition register bit given by a BI-style field.
void AddCrBit(Out &out, unsigned bit)
{
  out.Add("cr%u[%s]", bit / 4, kCrBitName[bit & 3]);
}

// The bits MB through ME (big-endian numbering, wrapping around).
uint32_t RotateMask(unsigned mb, unsigned me)
{
  uint32_t fromMb = 0xFFFFFFFFu >> mb;
  uint32_t toMe = 0xFFFFFFFFu << (31 - me);
  return (mb <= me) ? (fromMb & toMe) : (fromMb | toMe);
}

uint32_t BranchTarget(uint32_t op, uint32_t vpc, uint32_t disp)
{
  return disp + ((op & kBitAa) ? 0 : vpc);
}

uint32_t ShortDisp(uint32_t op)
{
  return (uint32_t) (int32_t) (int16_t) (op & 0xFFFC);
}

void AddOperands(Out &out, uint32_t op, uint32_t vpc, Layout layout)
{
  const unsigned d = Rd(op), a = Ra(op), b = Rb(op), c = Rc5(op);
  switch (layout)
  {
  case Layout::None:
    break;
  case Layout::Branch:
  {
    uint32_t disp = op & kFieldLi;
    if (disp & 0x02000000)
      disp |= 0xFC000000;
    out.Add("0x%08X", BranchTarget(op, vpc, disp));
    break;
  }
  case Layout::CondBranch:
  case Layout::BranchReg:
    // BO bit 0x10 means the condition is ignored: show BI as a number.
    out.Add("0x%02X,", d);
    if (d & 0x10)
      out.Add("%u", a);
    else
      AddCrBit(out, a);
    if (layout == Layout::CondBranch)
      out.Add(",0x%08X", BranchTarget(op, vpc, ShortDisp(op)));
    break;
  case Layout::AddImm:
    if (a)
      out.Add("r%u,r%u,", d, a);
    else
      out.Add("r%u,0,", d);
    AddSigned16(out, op);
    break;
  case Layout::AddImmHigh:
    if (a)
      out.Add("r%u,r%u,0x%04X", d, a, Imm(op));
    else
      out.Add("r%u,0,0x%04X", d, Imm(op));
    break;
  case Layout::RegRegSimm:
    out.Add("r%u,r%u,", d, a);
    AddSigned16(out, op);
    break;
  case Layout::LogicImm:
    out.Add("r%u,r%u,0x%04X", a, d, Imm(op));
    break;
  case Layout::CmpSimm:
    out.Add("cr%u,%u,r%u,", CrfD(op), LBit(op), a);
    AddSigned16(out, op);
    break;
  case Layout::CmpUimm:
    out.Add("cr%u,%u,r%u,0x%04X", CrfD(op), LBit(op), a, Imm(op));
    break;
  case Layout::Trap:
    out.Add("%u,r%u,r%u", d, a, b);
    break;
  case Layout::TrapImm:
    out.Add("%u,r%u,", d, a);
    AddSigned16(out, op);
    break;
  case Layout::Cmp:
    out.Add("cr%u,%u,r%u,r%u", CrfD(op), LBit(op), a, b);
    break;
  case Layout::RegRegReg:
    out.Add("r%u,r%u,r%u", d, a, b);
    break;
  case Layout::RegReg0Reg:
    if (a)
      out.Add("r%u,r%u,r%u", d, a, b);
    else
      out.Add("r%u,0,r%u", d, b);
    break;
  case Layout::LogicReg:
    out.Add("r%u,r%u,r%u", a, d, b);
    break;
  case Layout::Unary:
    out.Add("r%u,r%u", d, a);
    break;
  case Layout::LogicUnary:
    out.Add("r%u,r%u", a, d);
    break;
  case Layout::Reg:
    out.Add("r%u", d);
    break;
  case Layout::RegRegB:
    out.Add("r%u,r%u", d, b);
    break;
  case Layout::CacheOp:
    if (a)
      out.Add("r%u,r%u", a, b);
    else
      out.Add("0,r%u", b);
    break;
  case Layout::OnlyB:
    out.Add("r%u", b);
    break;
  case Layout::Load:
  case Layout::FLoad:
  {
    const char reg = (layout == Layout::Load) ? 'r' : 'f';
    if (a)
    {
      out.Add("%c%u,", reg, d);
      AddSigned16(out, op);
      out.Add("(r%u)", a);
    }
    else
      out.Add("%c%u,0x%08X", reg, d, SignedImm(op));   // absolute address
    break;
  }
  case Layout::LoadUpdate:
  case Layout::FLoadUpdate:
    out.Add("%c%u,", (layout == Layout::LoadUpdate) ? 'r' : 'f', d);
    AddSigned16(out, op);
    out.Add("(r%u)", a);
    break;
  case Layout::FIndexed:
    if (a)
      out.Add("f%u,r%u,r%u", d, a, b);
    else
      out.Add("f%u,0,r%u", d, b);
    break;
  case Layout::FIndexedUpdate:
    out.Add("f%u,r%u,r%u", d, a, b);
    break;
  case Layout::StringImm:
    if (a)
      out.Add("r%u,r%u,%u", d, a, b ? b : 32);
    else
      out.Add("r%u,0,%u", d, b ? b : 32);
    break;
  case Layout::ShiftImm:
    out.Add("r%u,r%u,%u", a, d, b);
    break;
  case Layout::Rotate:
    out.Add("r%u,r%u,%u,%u,%u", a, d, b, c, Me(op));
    break;
  case Layout::RotateReg:
    out.Add("r%u,r%u,r%u,%u,%u", a, d, b, c, Me(op));
    break;
  case Layout::CrLogic:
    AddCrBit(out, d);
    out.Add(",");
    AddCrBit(out, a);
    out.Add(",");
    AddCrBit(out, b);
    break;
  case Layout::CrField:
    out.Add("cr%u,cr%u", CrfD(op), CrfS(op));
    break;
  case Layout::CrFieldOnly:
    out.Add("cr%u", CrfD(op));
    break;
  case Layout::MoveFromSpr:
    out.Add("r%u,", d);
    AddSpr(out, op);
    break;
  case Layout::MoveToSpr:
    AddSpr(out, op);
    out.Add(",r%u", d);
    break;
  case Layout::MoveFromSr:
    out.Add("r%u,sr%u", d, (op >> 16) & 15);
    break;
  case Layout::MoveToSr:
    out.Add("sr%u,r%u", (op >> 16) & 15, d);
    break;
  case Layout::MoveToCr:
    out.Add("0x%02X,r%u", (op >> 12) & 0xFF, d);
    break;
  case Layout::FUnary:
    out.Add("f%u,f%u", d, b);
    break;
  case Layout::FBinary:
    out.Add("f%u,f%u,f%u", d, a, b);
    break;
  case Layout::FMul:
    out.Add("f%u,f%u,f%u", d, a, c);
    break;
  case Layout::FMulAdd:
    out.Add("f%u,f%u,f%u,f%u", d, a, c, b);
    break;
  case Layout::FCompare:
    out.Add("cr%u,f%u,f%u", CrfD(op), a, b);
    break;
  case Layout::FMoveFromFpscr:
    out.Add("f%u", d);
    break;
  case Layout::FpscrBit:
    out.Add("fpscr[%u]", d);
    break;
  case Layout::FpscrField:
    out.Add("cr%u,0x%X", CrfD(op), (op >> 12) & 15);
    break;
  case Layout::FpscrFields:
    out.Add("0x%02X,f%u", (op >> 17) & 0xFF, b);
    break;
  }
}

// Register-usage rules that make a recognised instruction an invalid form.
bool ValidForm(uint32_t op, uint16_t opts)
{
  const unsigned d = Rd(op), a = Ra(op);
  if ((opts & kOptBaseNotRd) && (a == 0 || a == d))
    return false;
  if ((opts & kOptBaseNot0) && a == 0)
    return false;
  if (opts & kOptLswi)
  {
    // rA must not be among the registers loaded. The register count is
    // taken from NB as written (so NB = 0 counts as no registers here).
    unsigned last = d + Rb(op) - 1;
    if (a >= d && a <= last)
      return false;
    if (last > 31 && a < last - 31)
      return false;
  }
  if ((opts & kOptLswx) && (d == a || d == Rb(op) || (d == 0 && a == 0)))
    return false;
  return true;
}

// Shorter idioms, tried first when asked for. Returns false to fall back to
// the plain form.
bool Simplified(uint32_t op, uint32_t vpc, Out &mnem, Out &oprs)
{
  const unsigned d = Rd(op), a = Ra(op), b = Rb(op);
  const char *dot = (op & kBitRc) ? "." : "";
  const unsigned primary = op >> 26;
  const unsigned ext = (op >> 1) & 0x3FF;

  if (op == 0x60000000)                            // ori r0,r0,0
  {
    mnem.Add("nop");
    return true;
  }

  switch (primary)
  {
  case 14:                                         // addi rD,0,v
    if (a != 0)
      return false;
    mnem.Add("li");
    oprs.Add("r%u,0x%08X", d, SignedImm(op));
    return true;
  case 15:                                         // addis
    if (a == 0)
    {
      mnem.Add("li");
      oprs.Add("r%u,0x%08X", d, Imm(op) << 16);
    }
    else
    {
      mnem.Add("addi");
      oprs.Add("r%u,r%u,0x%08X", d, a, Imm(op) << 16);
    }
    return true;
  case 25: case 27: case 29:                       // oris, xoris, andis.
    mnem.Add(primary == 25 ? "ori" : primary == 27 ? "xori" : "andi.");
    oprs.Add("r%u,r%u,0x%08X", a, d, Imm(op) << 16);
    return true;
  case 20: case 21:                                // rlwimi, rlwinm: show the mask
  {
    uint32_t mask = RotateMask(Rc5(op), Me(op));
    if (primary == 21 && b == 0)
    {
      mnem.Add("and%s", dot);
      oprs.Add("r%u,r%u,0x%08X", a, d, mask);
    }
    else
    {
      mnem.Add("%s%s", primary == 20 ? "rlwimi" : "rlwinm", dot);
      oprs.Add("r%u,r%u,%u,0x%08X", a, d, b, mask);
    }
    return true;
  }
  case 23:                                         // rlwnm
    mnem.Add("rlwnm%s", dot);
    oprs.Add("r%u,r%u,r%u,0x%08X", a, d, b, RotateMask(Rc5(op), Me(op)));
    return true;
  case 16:                                         // bc with a plain true/false test
  {
    const char *base;
    if ((d & 0x1C) == 0x04)
      base = "bf";
    else if ((d & 0x1C) == 0x0C)
      base = "bt";
    else
      return false;
    mnem.Add("%s%s%s", base, (op & kBitRc) ? "l" : "", (op & kBitAa) ? "a" : "");
    AddCrBit(oprs, a);
    oprs.Add(",0x%08X", BranchTarget(op, vpc, ShortDisp(op)));
    return true;
  }
  case 31:
    if ((op & kBitOe) == 0 && (ext == 444 || ext == 124))   // or/nor rA,rS,rS
    {
      if (d != b)
        return false;
      mnem.Add("%s%s", ext == 444 ? "mr" : "not", dot);
      oprs.Add("r%u,r%u", a, d);
      return true;
    }
    if ((ext & 0x1FF) == 40 || (ext & 0x1FF) == 8)          // subf(c): operands swapped
    {
      mnem.Add("%s%s%s", (ext & 0x1FF) == 40 ? "sub" : "subc", (op & kBitOe) ? "o" : "", dot);
      oprs.Add("r%u,r%u,r%u", d, b, a);
      return true;
    }
    return false;
  default:
    return false;
  }
}

} // anonymous namespace

Result DisassemblePowerPC(UINT32 op, UINT32 vpc, char *mnem, char *oprs, size_t oprs_size, bool simplify)
{
  Out mnemOut(mnem, 16);
  Out oprsOut(oprs, oprs_size);

  if (simplify && Simplified(op, vpc, mnemOut, oprsOut))
    return Result::OKAY;

  const Insn *insn = Find(op);
  if (!insn || !FitsForm(op, *insn))
    return Result::FAIL;

  mnemOut.Add("%s%s%s%s%s", insn->name,
              ((insn->opts & kOptOe) && (op & kBitOe)) ? "o" : "",
              ((insn->opts & kOptRc) && (op & kBitRc)) ? "." : "",
              ((insn->opts & kOptLk) && (op & kBitRc)) ? "l" : "",
              ((insn->opts & kOptAa) && (op & kBitAa)) ? "a" : "");
  AddOperands(oprsOut, op, vpc, insn->layout);
  return ValidForm(op, insn->opts) ? Result::OKAY : Result::FAIL;
}

/******************************************************************************
 Command line tool (built with -DSTANDALONE)
******************************************************************************/

#ifdef STANDALONE

static void Usage()
{
  puts("ppcd: PowerPC 603e disassembler (RetroPixels)");
  puts("usage: ppcd <file> [options]");
  puts("  -s <hex>     offset in the file to start at (default 0)");
  puts("  -l <count>   number of instructions (default: to the end of the file)");
  puts("  -o <hex>     address of the first instruction (default: the start offset)");
  puts("  -big         words are big-endian (default)");
  puts("  -little      words are little-endian");
  puts("  -simple      show simplified forms (default)");
  puts("  -nosimple    show only the basic forms");
  puts("  -h, -?       this text");
  puts("Invalid forms are marked with *, unknown words with ?.");
}

int main(int argc, char **argv)
{
  const char *path = nullptr;
  unsigned long start = 0, origin = 0, count = 0;
  bool haveCount = false, haveOrigin = false, little = false, simple = true;

  for (int i = 1; i < argc; i++)
  {
    const char *arg = argv[i];
    bool takesValue = !strcmp(arg, "-s") || !strcmp(arg, "-l") || !strcmp(arg, "-o");
    if (takesValue && i + 1 >= argc)
    {
      fprintf(stderr, "ppcd: %s needs a value\n", arg);
      return 1;
    }
    if (!strcmp(arg, "-h") || !strcmp(arg, "-?"))
    {
      Usage();
      return 0;
    }
    else if (!strcmp(arg, "-s"))
      start = strtoul(argv[++i], nullptr, 16);
    else if (!strcmp(arg, "-l"))
    {
      count = strtoul(argv[++i], nullptr, 10);
      haveCount = true;
    }
    else if (!strcmp(arg, "-o"))
    {
      origin = strtoul(argv[++i], nullptr, 16);
      haveOrigin = true;
    }
    else if (!strcmp(arg, "-big"))
      little = false;
    else if (!strcmp(arg, "-little"))
      little = true;
    else if (!strcmp(arg, "-simple"))
      simple = true;
    else if (!strcmp(arg, "-nosimple"))
      simple = false;
    else
      path = arg;
  }
  if (!path)
  {
    Usage();
    return 1;
  }

  FILE *fp = fopen(path, "rb");
  if (!fp)
  {
    fprintf(stderr, "ppcd: cannot open %s\n", path);
    return 1;
  }
  if (start && fseek(fp, (long) start, SEEK_SET) != 0)
  {
    fprintf(stderr, "ppcd: cannot seek to 0x%lX in %s\n", start, path);
    fclose(fp);
    return 1;
  }

  UINT32 addr = (UINT32) (haveOrigin ? origin : start);
  unsigned char word[4];
  for (unsigned long n = 0; (!haveCount || n < count) && fread(word, 1, 4, fp) == 4; n++, addr += 4)
  {
    UINT32 op = little ? ((UINT32) word[3] << 24) | ((UINT32) word[2] << 16) | ((UINT32) word[1] << 8) | word[0]
                       : ((UINT32) word[0] << 24) | ((UINT32) word[1] << 16) | ((UINT32) word[2] << 8) | word[3];
    char mnem[16], oprs[64];
    Result r = DisassemblePowerPC(op, addr, mnem, oprs, sizeof(oprs), simple);
    if (r == Result::OKAY)
      printf("0x%08X: 0x%08X\t%s\t%s\n", addr, op, mnem, oprs);
    else if (mnem[0] != '\0')
      printf("0x%08X: 0x%08X\t%s*\t%s\n", addr, op, mnem, oprs);
    else
      printf("0x%08X: 0x%08X\t?\n", addr, op);
  }
  fclose(fp);
  return 0;
}

#endif // STANDALONE
