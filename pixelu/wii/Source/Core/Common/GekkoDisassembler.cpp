// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GekkoDisassembler.h"

#include <array>
#include <bit>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

namespace Common
{
namespace
{
// ---------------------------------------------------------------------------------------------
// Instruction fields. Bit 0 is the least significant bit of the word.

constexpr u32 Primary(u32 w)
{
  return w >> 26;
}
constexpr u32 FieldD(u32 w)
{
  return (w >> 21) & 31;
}
constexpr u32 FieldA(u32 w)
{
  return (w >> 16) & 31;
}
constexpr u32 FieldB(u32 w)
{
  return (w >> 11) & 31;
}
constexpr u32 FieldC(u32 w)
{
  return (w >> 6) & 31;
}
constexpr u32 FieldME(u32 w)
{
  return (w >> 1) & 31;
}
constexpr u32 CrDest(u32 w)
{
  return (w >> 23) & 7;
}
constexpr u32 CrSrc(u32 w)
{
  return (w >> 18) & 7;
}
constexpr u32 CmpL(u32 w)
{
  return (w >> 21) & 3;
}
constexpr u32 Ext10(u32 w)
{
  return (w >> 1) & 0x3ff;
}
constexpr u32 Ext5(u32 w)
{
  return (w >> 1) & 31;
}
constexpr bool RcBit(u32 w)
{
  return (w & 1) != 0;
}
constexpr bool OeBit(u32 w)
{
  return (w & 0x400) != 0;
}

constexpr u32 kMaskD = 31u << 21;
constexpr u32 kMaskA = 31u << 16;
constexpr u32 kMaskB = 31u << 11;

// Integer register names as the disassembly prints them.
constexpr std::array<const char*, 32> kGpr = {
    "r0",  "sp",  "rtoc", "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10",
    "r11", "r12", "r13",  "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21",
    "r22", "r23", "r24",  "r25", "r26", "r27", "r28", "r29", "r30", "r31",
};

// Padded labels for the register views.
constexpr std::array<const char*, 32> kGprLabels = {
    " r0", " r1 (sp)", " r2 (rtoc)", " r3", " r4", " r5", " r6", " r7", " r8", " r9", "r10",
    "r11", "r12",      "r13",        "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21",
    "r22", "r23",      "r24",        "r25", "r26", "r27", "r28", "r29", "r30", "r31",
};

constexpr std::array<const char*, 32> kFprLabels = {
    " f0", " f1", " f2", " f3", " f4", " f5", " f6", " f7", " f8", " f9", "f10",
    "f11", "f12", "f13", "f14", "f15", "f16", "f17", "f18", "f19", "f20", "f21",
    "f22", "f23", "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",
};

// Simplified trap mnemonics, indexed by the TO field. Empty means "no short form".
constexpr std::array<std::string_view, 32> kTrapSuffix = [] {
  std::array<std::string_view, 32> t{};
  t[1] = "lgt";
  t[2] = "llt";
  t[4] = "eq";
  t[5] = "lge";
  t[6] = "lle";
  t[8] = "gt";
  t[12] = "ge";
  t[16] = "lt";
  t[20] = "le";
  t[24] = "ne";
  return t;
}();

struct SprName
{
  u16 number;
  const char* name;
};

// Special purpose register names, sorted by number.
constexpr SprName kSprNames[] = {
    {1, "XER"},       {8, "LR"},        {9, "CTR"},       {18, "DSISR"},    {19, "DAR"},
    {22, "DEC"},      {25, "SDR1"},     {26, "SRR0"},     {27, "SRR1"},     {268, "TBLr"},
    {269, "TBUr"},    {272, "SPRG0"},   {273, "SPRG1"},   {274, "SPRG2"},   {275, "SPRG3"},
    {282, "EAR"},     {284, "TBLw"},    {285, "TBUw"},    {287, "PVR"},     {528, "IBAT0U"},
    {529, "IBAT0L"},  {530, "IBAT1U"},  {531, "IBAT1L"},  {532, "IBAT2U"},  {533, "IBAT2L"},
    {534, "IBAT3U"},  {535, "IBAT3L"},  {536, "DBAT0U"},  {537, "DBAT0L"},  {538, "DBAT1U"},
    {539, "DBAT1L"},  {540, "DBAT2U"},  {541, "DBAT2L"},  {542, "DBAT3U"},  {543, "DBAT3L"},
    {560, "IBAT4U"},  {561, "IBAT4L"},  {562, "IBAT5U"},  {563, "IBAT5L"},  {564, "IBAT6U"},
    {565, "IBAT6L"},  {566, "IBAT7U"},  {567, "IBAT7L"},  {568, "DBAT4U"},  {569, "DBAT4L"},
    {570, "DBAT5U"},  {571, "DBAT5L"},  {572, "DBAT6U"},  {573, "DBAT6L"},  {574, "DBAT7U"},
    {575, "DBAT7L"},  {912, "GQR0"},    {913, "GQR1"},    {914, "GQR2"},    {915, "GQR3"},
    {916, "GQR4"},    {917, "GQR5"},    {918, "GQR6"},    {919, "GQR7"},    {920, "HID2"},
    {921, "WPAR"},    {922, "DMAU"},    {923, "DMAL"},    {924, "ECID_U"},  {925, "ECID_M"},
    {926, "ECID_L"},  {936, "UMMCR0"},  {937, "UPMC1"},   {938, "UPMC2"},   {939, "USIA"},
    {940, "UMMCR1"},  {941, "UPMC3"},   {942, "UPMC4"},   {943, "USDA"},    {952, "MMCR0"},
    {953, "PMC1"},    {954, "PMC2"},    {955, "SIA"},     {956, "MMCR1"},   {957, "PMC3"},
    {958, "PMC4"},    {959, "SDA"},     {1008, "HID0"},   {1009, "HID1"},   {1010, "IABR"},
    {1011, "HID4"},   {1012, "TDCL"},   {1013, "DABR"},   {1017, "L2CR"},   {1018, "TDCH"},
    {1019, "ICTC"},   {1020, "THRM1"},  {1021, "THRM2"},  {1022, "THRM3"},
};

std::string SprText(u32 number)
{
  std::size_t lo = 0;
  std::size_t hi = std::size(kSprNames);
  while (lo < hi)
  {
    const std::size_t mid = (lo + hi) / 2;
    if (kSprNames[mid].number < number)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < std::size(kSprNames) && kSprNames[lo].number == number)
    return kSprNames[lo].name;
  return std::to_string(number);
}

// Signed 16-bit displacement of a D-form load/store, as "0", "0x1234" or "-0x1234".
std::string Disp16(u32 raw)
{
  raw &= 0xffff;
  if (raw == 0)
    return "0";
  if (raw >= 0x8000)
    return fmt::format("-0x{:04X}", 0x10000 - raw);
  return fmt::format("0x{:04X}", raw);
}

// Same for the 12-bit displacement of the paired-single quantized loads/stores.
std::string Disp12(u32 raw)
{
  raw &= 0xfff;
  if (raw == 0)
    return "0";
  if (raw >= 0x800)
    return fmt::format("-0x{:04X}", 0x1000 - raw);
  return fmt::format("0x{:04X}", raw);
}

// The mask an rlwinm-style instruction applies, rotated right by the shift for display.
u32 RotateMaskForDisplay(u32 shift, u32 mb, u32 me)
{
  const auto from = [](u32 bit) -> u32 { return bit > 31 ? 0u : (0xFFFFFFFFu >> bit); };
  const u32 mask = mb <= me ? (from(mb) & ~from(me + 1)) : (from(mb) | ~from(me + 1));
  return std::rotr(mask, static_cast<int>(shift));
}

// ---------------------------------------------------------------------------------------------
// Primary opcode 31 is decoded from a table indexed by the 10-bit extended opcode.

enum class Kind : u8
{
  None,
  Regs,       // registers chosen by `regs`, with the optional o/. suffixes
  RegsShift,  // like Regs, but the last operand is the raw B field (lswi, srawi)
  Compare,
  Trap,
  TrapDouble,
  MoveToCr,
  SegReg,
  SprMove,
  TimeBase,
  NoOperands,
  McrXer,
  FloatIndexed,
  OrOrMove,
  NorOrNot,
};

// What happens to the record bit of a register-form instruction.
enum class Rc : u8
{
  Optional,  // a set bit adds "."
  MustBe0,
  MustBe1,
};

enum : u8
{
  kRegD = 4,
  kRegA = 2,
  kRegB = 1,
};

struct Op31
{
  Kind kind = Kind::None;
  const char* name = nullptr;
  u8 regs = 0;          // kRegD | kRegA | kRegB: which of the three fields print
  bool source_first = false;  // the "rS" field is printed after rA (logical/shift ops)
  bool swap_ab = false;       // operands are printed as rD, rB, rA (sub/subc)
  bool has_oe = false;        // "o" suffix when OE is set; also decoded at xo + 512
  Rc rc = Rc::MustBe0;
  u32 zero_bits = 0;          // bits that must be clear, otherwise the word is illegal
  int param = 0;              // to/from direction for the move kinds
};

constexpr Op31 Load(const char* n)
{
  return {Kind::Regs, n, kRegD | kRegA | kRegB};
}
constexpr Op31 Cache(const char* n)
{
  return {Kind::Regs, n, kRegA | kRegB, false, false, false, Rc::MustBe0, kMaskD};
}
constexpr Op31 Arith(const char* n, bool oe, u8 regs = 7, u32 zero = 0)
{
  return {Kind::Regs, n, regs, false, false, oe, Rc::Optional, zero};
}
constexpr Op31 Logic(const char* n, u8 regs = 7, u32 zero = 0)
{
  return {Kind::Regs, n, regs, true, false, false, Rc::Optional, zero};
}
constexpr Op31 Special(Kind k, const char* n = nullptr, int param = 0, u32 zero = 0)
{
  Op31 op{k, n};
  op.param = param;
  op.zero_bits = zero;
  return op;
}

constexpr std::array<Op31, 1024> kOp31 = [] {
  std::array<Op31, 1024> t{};
  const auto put = [&t](u32 xo, Op31 op) {
    t[xo] = op;
    if (op.has_oe)
      t[xo + 512] = op;
  };

  put(0, Special(Kind::Compare, nullptr, 0, 1));
  put(32, Special(Kind::Compare, nullptr, 0, 1));
  put(4, Special(Kind::Trap, nullptr, 0, 1));
  put(68, Special(Kind::TrapDouble));

  Op31 subc = Arith("subc", true);
  subc.swap_ab = true;
  put(8, subc);
  Op31 sub = Arith("sub", true);
  sub.swap_ab = true;
  put(40, sub);

  put(9, Arith("mulhdu", false));
  put(10, Arith("addc", true));
  put(11, Arith("mulhwu", false));
  put(73, Arith("mulhd", false));
  put(75, Arith("mulhw", false));
  put(104, Arith("neg", true, kRegD | kRegA, kMaskB));
  put(136, Arith("subfe", true));
  put(138, Arith("adde", true));
  put(200, Arith("subfze", true, kRegD | kRegA, kMaskB));
  put(202, Arith("addze", true, kRegD | kRegA, kMaskB));
  put(232, Arith("subfme", true, kRegD | kRegA, kMaskB));
  put(233, Arith("mulld", true));
  put(234, Arith("addme", true, kRegD | kRegA, kMaskB));
  put(235, Arith("mullw", true));
  put(266, Arith("add", true));
  put(457, Arith("divdu", true));
  put(459, Arith("divwu", true));
  put(489, Arith("divd", true));
  put(491, Arith("divw", true));

  put(24, Logic("slw"));
  put(26, Logic("cntlzw", kRegD | kRegA, kMaskB));
  put(27, Logic("sld"));
  put(28, Logic("and"));
  put(58, Logic("cntlzd", kRegD | kRegA, kMaskB));
  put(60, Logic("andc"));
  put(284, Logic("eqv"));
  put(316, Logic("xor"));
  put(412, Logic("orc"));
  put(476, Logic("nand"));
  put(536, Logic("srw"));
  put(539, Logic("srd"));
  put(792, Logic("sraw"));
  put(794, Logic("srad"));
  put(922, Logic("extsh", kRegD | kRegA, kMaskB));
  put(954, Logic("extsb", kRegD | kRegA, kMaskB));
  put(986, Logic("extsw", kRegD | kRegA, kMaskB));
  put(124, Special(Kind::NorOrNot));
  put(444, Special(Kind::OrOrMove));

  using Named = std::pair<u32, const char*>;
  const Named loads[] = {{20, "lwarx"},
                              {21, "ldx"},     {23, "lwzx"},    {53, "ldux"},   {55, "lwzux"},
                              {84, "ldarx"},   {87, "lbzx"},    {119, "lbzux"}, {149, "stdx"},
                              {151, "stwx"},   {181, "stdux"},  {183, "stwux"}, {215, "stbx"},
                              {247, "stbux"},  {279, "lhzx"},   {310, "eciwx"}, {311, "lhzux"},
                              {341, "lwax"},   {343, "lhax"},   {373, "lwaux"}, {375, "lhaux"},
                              {407, "sthx"},   {438, "ecowx"},  {439, "sthux"}, {533, "lswx"},
                              {534, "lwbrx"},  {661, "stswx"},  {662, "stwbrx"},
                              {790, "lhbrx"},  {918, "sthbrx"}};
  for (const auto& [xo, n] : loads)
  {
    put(xo, Load(n));
  }
  Op31 stwcx = Load("stwcx.");
  stwcx.rc = Rc::MustBe1;
  put(150, stwcx);
  Op31 stdcx = Load("stdcx.");
  stdcx.rc = Rc::MustBe1;
  put(214, stdcx);

  const Named caches[] = {{54, "dcbst"}, {86, "dcbf"},  {246, "dcbtst"}, {278, "dcbt"},
                          {470, "dcbi"},  {982, "icbi"}, {1014, "dcbz"}};
  for (const auto& [xo, n] : caches)
  {
    put(xo, Cache(n));
  }

  put(19, {Kind::Regs, "mfcr", kRegD, false, false, false, Rc::MustBe0, kMaskA | kMaskB});
  put(83, {Kind::Regs, "mfmsr", kRegD, false, false, false, Rc::MustBe0, kMaskA | kMaskB});
  put(146, {Kind::Regs, "mtmsr", kRegD, false, false, false, Rc::MustBe0, kMaskA | kMaskB});
  put(242, {Kind::Regs, "mtsrin", kRegD | kRegB, false, false, false, Rc::MustBe0, kMaskA});
  put(659, {Kind::Regs, "mfsrin", kRegD | kRegB, false, false, false, Rc::MustBe0, kMaskA});
  put(306, {Kind::Regs, "tlbie", kRegB, false, false, false, Rc::MustBe0, kMaskD | kMaskA});
  put(434, {Kind::Regs, "slbie", kRegB, false, false, false, Rc::MustBe0, kMaskD | kMaskA});

  put(597, {Kind::RegsShift, "lswi", kRegD | kRegA});
  put(725, {Kind::RegsShift, "stswi", kRegD | kRegA});
  put(824, {Kind::RegsShift, "srawi", kRegD | kRegA, true, false, false, Rc::Optional});

  put(144, Special(Kind::MoveToCr));
  put(210, Special(Kind::SegReg, nullptr, 1));
  put(595, Special(Kind::SegReg, nullptr, 0));
  put(339, Special(Kind::SprMove, nullptr, 0));
  put(467, Special(Kind::SprMove, nullptr, 1));
  put(371, Special(Kind::TimeBase));
  put(512, Special(Kind::McrXer, nullptr, 0, 0x007ff801));

  put(370, Special(Kind::NoOperands, "tlbia"));
  put(498, Special(Kind::NoOperands, "slbia"));
  put(566, Special(Kind::NoOperands, "tlbsync"));
  put(598, Special(Kind::NoOperands, "sync"));
  put(854, Special(Kind::NoOperands, "eieio"));

  const Named float_indexed[] = {{535, "lfsx"},   {567, "lfsux"},  {599, "lfdx"},
                                 {631, "lfdux"},  {663, "stfsx"},  {695, "stfsux"},
                                 {727, "stfdx"},  {759, "stfdux"}, {983, "stfiwx"}};
  for (const auto& [xo, n] : float_indexed)
  {
    put(xo, Special(Kind::FloatIndexed, n));
  }
  return t;
}();

// ---------------------------------------------------------------------------------------------
// Floating-point arithmetic (primary opcodes 59 and 63).

enum : u8
{
  kFprA = 4,
  kFprC = 2,
  kFprB = 1,
  kIgnoreUnused = 8,  // fields that are not printed may hold anything
};

struct FloatOp
{
  const char* name = nullptr;
  u8 fields = 0;
};

// A-form, indexed by the 5-bit extended opcode.
constexpr std::array<FloatOp, 32> kSingleA = [] {
  std::array<FloatOp, 32> t{};
  t[18] = {"divs", kFprA | kFprB};
  t[20] = {"subs", kFprA | kFprB};
  t[21] = {"adds", kFprA | kFprB};
  t[22] = {"sqrts", kFprB};
  t[24] = {"res", kFprB};
  t[25] = {"muls", kFprA | kFprC};
  t[28] = {"msubs", kFprA | kFprC | kFprB};
  t[29] = {"madds", kFprA | kFprC | kFprB};
  t[30] = {"nmsubs", kFprA | kFprC | kFprB};
  t[31] = {"nmadds", kFprA | kFprC | kFprB};
  return t;
}();

constexpr std::array<FloatOp, 32> kDoubleA = [] {
  std::array<FloatOp, 32> t{};
  t[18] = {"div", kFprA | kFprB};
  t[20] = {"sub", kFprA | kFprB};
  t[21] = {"add", kFprA | kFprB};
  t[22] = {"sqrt", kFprB};
  t[23] = {"sel", kFprA | kFprC | kFprB};
  t[25] = {"mul", kFprA | kFprC};
  t[26] = {"rsqrte", kFprB};
  t[28] = {"msub", kFprA | kFprC | kFprB};
  t[29] = {"madd", kFprA | kFprC | kFprB};
  t[30] = {"nmsub", kFprA | kFprC | kFprB};
  t[31] = {"nmadd", kFprA | kFprC | kFprB};
  return t;
}();

// ---------------------------------------------------------------------------------------------
// Paired-single arithmetic (primary opcode 4). Operand templates use D, A, B and C as
// placeholders for "p<field>".

struct PairedOp
{
  const char* name = nullptr;
  const char* operands = nullptr;
};

constexpr std::array<PairedOp, 32> kPaired5 = [] {
  std::array<PairedOp, 32> t{};
  t[10] = {"ps_sum0", "D, 0=A+B, 1=C"};
  t[11] = {"ps_sum1", "D, 0=C, 1=A+B"};
  t[12] = {"ps_muls0", "D, A*C[0]"};
  t[13] = {"ps_muls1", "D, A*C[1]"};
  t[14] = {"ps_madds0", "D, A*C[0]+B"};
  t[15] = {"ps_madds1", "D, A*C[1]+B"};
  t[18] = {"ps_div", "D, A/B"};
  t[20] = {"ps_sub", "D, A-B"};
  t[21] = {"ps_add", "D, A+B"};
  t[23] = {"ps_sel", "D, A>=0?C:B"};
  t[24] = {"ps_res", "D, (1/B)"};
  t[25] = {"ps_mul", "D, A*C"};
  t[26] = {"ps_rsqrte", "D, B"};
  t[28] = {"ps_msub", "D, A*C-B"};
  t[29] = {"ps_madd", "D, A*C+B"};
  t[30] = {"ps_nmsub", "D, -(A*C-B)"};
  t[31] = {"ps_nmadd", "D, -(A*C+B)"};
  return t;
}();

struct Paired10
{
  u16 xo;
  PairedOp op;
};

constexpr Paired10 kPaired10[] = {
    {40, {"ps_neg", "D, -B"}},
    {72, {"ps_mr", "D, B"}},
    {136, {"ps_nabs", "D, -|B|"}},
    {264, {"ps_abs", "D, |B|"}},
    {528, {"ps_merge00", "D, A[0], B[0]"}},
    {560, {"ps_merge01", "D, A[0], B[1]"}},
    {592, {"ps_merge10", "D, A[1], B[0]"}},
    {624, {"ps_merge11", "D, A[1], B[1]"}},
};

std::string ExpandPaired(u32 w, std::string_view pattern)
{
  std::string out;
  for (const char c : pattern)
  {
    u32 field;
    switch (c)
    {
    case 'D':
      field = FieldD(w);
      break;
    case 'A':
      field = FieldA(w);
      break;
    case 'B':
      field = FieldB(w);
      break;
    case 'C':
      field = FieldC(w);
      break;
    default:
      out += c;
      continue;
    }
    out += 'p';
    out += std::to_string(field);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------

class Decoder
{
public:
  Decoder(u32 word, u32 pc) : m_word(word), m_pc(pc) {}

  std::string Run()
  {
    Decode();
    std::string text = std::move(m_mnemonic);
    text += '\t';
    text += m_args;
    return text;
  }

private:
  void Illegal()
  {
    if (m_word == 0)
    {
      m_mnemonic.clear();
      m_args = "---";
    }
    else
    {
      m_mnemonic = "(ill)";
      m_args = fmt::format("{:08x}", m_word);
    }
  }

  static const char* Gpr(u32 n) { return kGpr[n]; }

  static s32 SImm(u32 w) { return static_cast<s16>(w & 0xffff); }
  static u32 UImm(u32 w) { return w & 0xffff; }

  void Decode()
  {
    const u32 w = m_word;
    switch (Primary(w))
    {
    case 2:
      TrapImmediate(true);
      return;
    case 3:
      TrapImmediate(false);
      return;
    case 4:
      Paired();
      return;
    case 7:
      m_mnemonic = "mulli";
      m_args = fmt::format("{}, {}, {}", Gpr(FieldD(w)), Gpr(FieldA(w)), SImm(w));
      return;
    case 8:
      m_mnemonic = "subfic";
      m_args = fmt::format("{}, {}, {}", Gpr(FieldD(w)), Gpr(FieldA(w)), SImm(w));
      return;
    case 10:
      CompareImmediate(true);
      return;
    case 11:
      CompareImmediate(false);
      return;
    case 12:
      AddImmediate("ic", false);
      return;
    case 13:
      AddImmediate("ic.", false);
      return;
    case 14:
      AddImmediate("i", true);
      return;
    case 15:
      AddImmediate("is", true);
      return;
    case 16:
      ConditionalBranch();
      return;
    case 17:
      if ((w & 0x03ffffff) == 2)
        m_mnemonic = "sc";
      else
        Illegal();
      return;
    case 18:
      Branch();
      return;
    case 19:
      Opcode19();
      return;
    case 20:
      Rotate("imi", false);
      return;
    case 21:
      Rotate("inm", false);
      return;
    case 23:
      Rotate("nm", true);
      return;
    case 24:
      if ((w & 0x03ffffff) == 0)
        m_mnemonic = "nop";
      else
        LogicalImmediate("ori");
      return;
    case 25:
      LogicalImmediate("oris");
      return;
    case 26:
      LogicalImmediate("xori");
      return;
    case 27:
      LogicalImmediate("xoris");
      return;
    case 28:
      LogicalImmediate("andi.");
      return;
    case 29:
      LogicalImmediate("andis.");
      return;
    case 31:
      Opcode31();
      return;
    case 56:
      PairedMemory("psq_l");
      return;
    case 57:
      PairedMemory("psq_lu");
      return;
    case 60:
      PairedMemory("psq_st");
      return;
    case 61:
      PairedMemory("psq_stu");
      return;
    case 58:
    {
      static constexpr const char* names[] = {"ld", "ldu", "lwa"};
      if ((w & 3) == 3)
        Illegal();
      else
        Memory(w & ~3u, names[w & 3], false);
      return;
    }
    case 62:
      if ((w & 3) > 1)
        Illegal();
      else
        Memory(w & ~3u, (w & 1) ? "stdu" : "std", false);
      return;
    case 59:
      FloatArith(kSingleA[Ext5(w)]);
      return;
    case 63:
      Opcode63();
      return;
    default:
      break;
    }

    const u32 primary = Primary(w);
    if (primary >= 32 && primary <= 55)
    {
      static constexpr const char* names[] = {
          "lwz",  "lwzu",  "lbz",  "lbzu",  "stw",  "stwu", "stb", "stbu",
          "lhz",  "lhzu",  "lha",  "lhau",  "sth",  "sthu", "lmw", "stmw",
          "lfs",  "lfsu",  "lfd",  "lfdu",  "stfs", "stfsu", "stfd", "stfdu",
      };
      Memory(w, names[primary - 32], primary >= 48);
      return;
    }
    Illegal();
  }

  // --- immediates ----------------------------------------------------------------------------

  void TrapImmediate(bool doubleword)
  {
    const u32 w = m_word;
    const char width = doubleword ? 'd' : 'w';
    const std::string_view cond = kTrapSuffix[FieldD(w)];
    m_mnemonic = 't';
    m_mnemonic += width;
    if (!cond.empty())
    {
      m_mnemonic += cond;
      m_mnemonic += 'i';
    }
    else
    {
      m_mnemonic += 'i';
      m_args = fmt::format("{}, ", FieldD(w));
    }
    m_args += fmt::format("{}, {}", Gpr(FieldA(w)), SImm(w));
  }

  void CompareImmediate(bool logical)
  {
    const u32 w = m_word;
    const u32 l = CmpL(w);
    if (l > 1)
    {
      Illegal();
      return;
    }
    m_mnemonic = logical ? "cmpl" : "cmp";
    m_mnemonic += l ? "di" : "wi";
    if (CrDest(w) != 0)
      m_args = fmt::format("cr{}, ", CrDest(w));
    if (logical)
      m_args += fmt::format("{}, {}", Gpr(FieldA(w)), UImm(w));
    else
      m_args += fmt::format("{}, {}", Gpr(FieldA(w)), SImm(w));
  }

  // addic, addic., addi, addis and their simplified forms.
  void AddImmediate(std::string_view suffix, bool load_form_allowed)
  {
    const u32 w = m_word;
    if (load_form_allowed && FieldA(w) == 0)
    {
      m_mnemonic = 'l';
      m_mnemonic += suffix;
      if (suffix == "i")
        m_args = fmt::format("{}, {}", Gpr(FieldD(w)), SImm(w));
      else
        m_args = fmt::format("{}, 0x{:04X}", Gpr(FieldD(w)), UImm(w));
      return;
    }
    const bool negative = (w & 0x8000) != 0;
    const u32 magnitude = negative ? 0x10000 - UImm(w) : UImm(w);
    m_mnemonic = negative ? "sub" : "add";
    m_mnemonic += suffix;
    m_args = fmt::format("{}, {}, {}", Gpr(FieldD(w)), Gpr(FieldA(w)), magnitude);
  }

  void LogicalImmediate(const char* name)
  {
    const u32 w = m_word;
    m_mnemonic = name;
    m_args = fmt::format("{}, {}, 0x{:04X}", Gpr(FieldA(w)), Gpr(FieldD(w)), UImm(w));
  }

  // --- branches ------------------------------------------------------------------------------

  // Builds the mnemonic (and BO/BI operands) of a conditional branch. `target` is a register
  // name ("lr", "ctr") or empty for bc; `backward` flips the meaning of the prediction bit.
  void BranchMnemonic(std::string_view target, bool absolute, bool backward)
  {
    static constexpr const char* link_suffix[] = {"", "l", "a", "la"};
    static constexpr const char* cond_names[] = {"ge", "le", "ne", "ns", "lt", "gt", "eq", "so"};
    static constexpr const char* ctr_names[] = {"nzf", "zf", "", "", "nzt", "zt", "", "",
                                                "nz",  "z",  "", "", "nz",  "z",  "", ""};

    const u32 w = m_word;
    const u32 bo = FieldD(w);
    const u32 bi = FieldA(w);
    const char* suffix = link_suffix[(absolute ? 2 : 0) + (w & 1)];
    const bool predict_flag = ((bo & 1) != 0) != backward;
    const char hint = predict_flag ? '+' : '-';

    if ((bo & 4) == 0)
    {
      m_mnemonic = fmt::format("bd{}{}{}{}", ctr_names[bo >> 1], target, suffix, hint);
      if ((bo & 16) == 0)
        m_args = std::to_string(bi);
      return;
    }

    if ((bo & 16) == 0)
    {
      const u32 index = ((bo & 8) ? 4 : 0) + (bi & 3);
      m_mnemonic = fmt::format("b{}{}{}{}", cond_names[index], target, suffix, hint);
      if (bi >= 4)
        m_args = fmt::format("cr{}", bi >> 2);
      return;
    }

    // Unconditional.
    if (Primary(w) == 16)
    {
      m_mnemonic = fmt::format("bc{}", suffix);
      m_args = fmt::format("{}, {}", bo, bi);
    }
    else
    {
      m_mnemonic = fmt::format("b{}{}", target, suffix);
    }
  }

  void ConditionalBranch()
  {
    const u32 w = m_word;
    const bool absolute = (w & 2) != 0;
    const u32 disp = static_cast<u32>(static_cast<s32>(static_cast<s16>(w & 0xfffc)));
    BranchMnemonic("", absolute, static_cast<s32>(disp) < 0);
    m_args += fmt::format(" ->0x{:08X}", absolute ? disp : m_pc + disp);
  }

  void Branch()
  {
    static constexpr const char* link_suffix[] = {"", "l", "a", "la"};
    const u32 w = m_word;
    const u32 disp = static_cast<u32>(static_cast<s32>(w << 6) >> 6) & ~3u;
    m_mnemonic = 'b';
    m_mnemonic += link_suffix[w & 3];
    m_args = fmt::format("->0x{:08X}", (w & 2) ? disp : m_pc + disp);
  }

  // --- opcode 19 -----------------------------------------------------------------------------

  void MoveCrField(std::string_view suffix)
  {
    const u32 w = m_word;
    if ((w & 0x0063f801) != 0)
    {
      Illegal();
      return;
    }
    m_mnemonic = "mcrf";
    m_mnemonic += suffix;
    m_args = fmt::format("cr{}, cr{}", CrDest(w), CrSrc(w));
  }

  // Condition register logic. `alias` is the two-operand form used when crbA == crbB.
  void CrLogic(const char* name, const char* alias)
  {
    const u32 w = m_word;
    if (RcBit(w))
    {
      Illegal();
      return;
    }
    const u32 d = FieldD(w);
    const u32 a = FieldA(w);
    const u32 b = FieldB(w);
    m_mnemonic = "cr";
    if (alias != nullptr && a == b)
    {
      m_mnemonic += alias;
      m_args = fmt::format("{}, {}", d, a);
    }
    else
    {
      m_mnemonic += name;
      m_args = fmt::format("{}, {}, {}", d, a, b);
    }
  }

  void NoOperands(const char* name)
  {
    if ((m_word & (kMaskD | kMaskA | kMaskB | 1)) != 0)
      Illegal();
    else
      m_mnemonic = name;
  }

  void Opcode19()
  {
    switch (Ext10(m_word))
    {
    case 0:
      MoveCrField("");
      break;
    case 16:
      BranchMnemonic("lr", false, false);
      break;
    case 528:
      BranchMnemonic("ctr", false, false);
      break;
    case 33:
      CrLogic("nor", "not");
      break;
    case 129:
      CrLogic("andc", nullptr);
      break;
    case 193:
      CrLogic("xor", "clr");
      break;
    case 225:
      CrLogic("nand", nullptr);
      break;
    case 257:
      CrLogic("and", nullptr);
      break;
    case 289:
      CrLogic("eqv", "set");
      break;
    case 417:
      CrLogic("orc", nullptr);
      break;
    case 449:
      CrLogic("or", "move");
      break;
    case 50:
      NoOperands("rfi");
      break;
    case 150:
      NoOperands("isync");
      break;
    default:
      Illegal();
      break;
    }
  }

  // --- rotates -------------------------------------------------------------------------------

  void Rotate(const char* form, bool by_register)
  {
    const u32 w = m_word;
    const u32 shift = FieldB(w);
    const u32 mb = FieldC(w);
    const u32 me = FieldME(w);
    m_mnemonic = fmt::format("rlw{}{}", form, RcBit(w) ? "." : "");
    m_args = fmt::format("{}, {}, {}{}, {}, {} ({:08x})", Gpr(FieldA(w)), Gpr(FieldD(w)),
                         by_register ? "r" : "", shift, mb, me,
                         RotateMaskForDisplay(shift, mb, me));
  }

  // --- loads and stores ----------------------------------------------------------------------

  void Memory(u32 w, const char* name, bool fpr)
  {
    m_mnemonic = name;
    if (fpr)
      m_args = fmt::format("f{}, {} ({})", FieldD(w), Disp16(w), Gpr(FieldA(w)));
    else
      m_args = fmt::format("{}, {} ({})", Gpr(FieldD(w)), Disp16(w), Gpr(FieldA(w)));
  }

  void PairedMemory(const char* name)
  {
    const u32 w = m_word;
    m_mnemonic = name;
    m_args = fmt::format("p{}, {}(r{}), {}, qr{}", FieldD(w), Disp12(w), FieldA(w),
                         (w >> 15) & 1, (w >> 12) & 7);
  }

  // --- opcode 31 -----------------------------------------------------------------------------

  // Prints the chosen integer register fields of `w`, comma separated.
  static std::string RegList(u32 w, u8 regs)
  {
    std::string out;
    const auto add = [&out](u32 reg) {
      if (!out.empty())
        out += ", ";
      out += Gpr(reg);
    };
    if (regs & kRegD)
      add(FieldD(w));
    if (regs & kRegA)
      add(FieldA(w));
    if (regs & kRegB)
      add(FieldB(w));
    return out;
  }

  // Register-form instruction. Returns false (after marking the word illegal) when the record
  // bit does not match what the instruction requires.
  bool RegisterForm(const Op31& op, const char* name, u8 regs)
  {
    u32 w = m_word;
    if ((op.rc == Rc::MustBe0 && RcBit(w)) || (op.rc == Rc::MustBe1 && !RcBit(w)))
    {
      Illegal();
      return false;
    }
    // Shown as rA, rS, rB for logical ops and rD, rB, rA for the subtract aliases: swap the
    // field contents so the generic D, A, B order prints them.
    if (op.source_first)
      w = (w & ~(kMaskD | kMaskA)) | (FieldA(w) << 21) | (FieldD(w) << 16);
    if (op.swap_ab)
      w = (w & ~(kMaskA | kMaskB)) | (FieldB(w) << 16) | (FieldA(w) << 11);

    m_mnemonic = name;
    if (op.has_oe && OeBit(w))
      m_mnemonic += 'o';
    if (op.rc == Rc::Optional && RcBit(w))
      m_mnemonic += '.';
    m_args = RegList(w, regs);
    m_operand_word = w;
    return true;
  }

  void Opcode31()
  {
    const u32 w = m_word;
    const Op31& op = kOp31[Ext10(w)];
    if (op.kind == Kind::None || (w & op.zero_bits) != 0)
    {
      Illegal();
      return;
    }

    switch (op.kind)
    {
    case Kind::Regs:
      RegisterForm(op, op.name, op.regs);
      break;

    case Kind::RegsShift:
      if (RegisterForm(op, op.name, op.regs))
        m_args += fmt::format(",{}", FieldB(m_operand_word));
      break;

    case Kind::NorOrNot:
    case Kind::OrOrMove:
    {
      const bool alias = FieldD(w) == FieldB(w);
      const char* name = op.kind == Kind::NorOrNot ? (alias ? "not" : "nor") : (alias ? "mr" : "or");
      RegisterForm(Logic(name), name, alias ? (kRegD | kRegA) : (kRegD | kRegA | kRegB));
      break;
    }

    case Kind::Compare:
    {
      const u32 l = CmpL(w);
      if (l > 1)
      {
        Illegal();
        break;
      }
      m_mnemonic = Ext10(w) != 0 ? "cmpl" : "cmp";
      m_mnemonic += l ? 'd' : 'w';
      if (CrDest(w) != 0)
        m_args = fmt::format("cr{},", CrDest(w));
      m_args += fmt::format("{}, {}", Gpr(FieldA(w)), Gpr(FieldB(w)));
      break;
    }

    case Kind::Trap:
    case Kind::TrapDouble:
    {
      const bool doubleword = op.kind == Kind::TrapDouble;
      const u32 to = FieldD(w);
      const std::string_view cond = kTrapSuffix[to];
      if (!cond.empty())
      {
        m_mnemonic = fmt::format("t{}{}", doubleword ? 'd' : 'w', cond);
        m_args = fmt::format("{}, {}", Gpr(FieldA(w)), Gpr(FieldB(w)));
      }
      else if (to != 31)
      {
        Illegal();
      }
      else if (doubleword)
      {
        m_mnemonic = "td";
        m_args = "31,0,0";
      }
      else
      {
        m_mnemonic = "trap";
      }
      break;
    }

    case Kind::MoveToCr:
    {
      if ((w & 0x00100801) != 0)
      {
        Illegal();
        break;
      }
      const u32 crm = (w >> 12) & 0xff;
      if (crm == 0xff)
      {
        m_mnemonic = "mtcr";
        m_args = Gpr(FieldD(w));
      }
      else
      {
        m_mnemonic = "mtcrf";
        m_args = fmt::format("0x{:02x},{}", crm, Gpr(FieldD(w)));
      }
      break;
    }

    case Kind::SegReg:
    {
      if ((w & 0x0010f801) != 0)
      {
        Illegal();
        break;
      }
      const u32 sr = (w >> 16) & 15;
      if (op.param)
      {
        m_mnemonic = "mtsr";
        m_args = fmt::format("{}, {}", sr, Gpr(FieldD(w)));
      }
      else
      {
        m_mnemonic = "mfsr";
        m_args = fmt::format("{}, {}", Gpr(FieldD(w)), sr);
      }
      break;
    }

    case Kind::SprMove:
    {
      if (RcBit(w))
      {
        Illegal();
        break;
      }
      const bool to_spr = op.param != 0;
      const u32 spr = (FieldB(w) << 5) | FieldA(w);
      const char* reg = Gpr(FieldD(w));
      m_mnemonic = to_spr ? "mt" : "mf";
      if (spr == 1 || spr == 8 || spr == 9)
      {
        m_mnemonic += spr == 1 ? "xer" : (spr == 8 ? "lr" : "ctr");
        m_args = reg;
      }
      else
      {
        m_mnemonic += "spr";
        const std::string name = SprText(spr);
        m_args = to_spr ? fmt::format("{}, {}", name, reg) : fmt::format("{}, {}", reg, name);
      }
      break;
    }

    case Kind::TimeBase:
    {
      if (RcBit(w))
      {
        Illegal();
        break;
      }
      const u32 tbr = (FieldB(w) << 5) | FieldA(w);
      m_args = Gpr(FieldD(w));
      m_mnemonic = "mftb";
      if (tbr == 268)
        m_mnemonic += 'l';
      else if (tbr == 269)
        m_mnemonic += 'u';
      else
        m_args += fmt::format(",{}", tbr);
      break;
    }

    case Kind::NoOperands:
      NoOperands(op.name);
      break;

    case Kind::McrXer:
      m_mnemonic = "mcrxr";
      m_args = fmt::format("cr{}", CrDest(w));
      break;

    case Kind::FloatIndexed:
      m_mnemonic = op.name;
      m_args = fmt::format("f{}, {}, {}", FieldD(w), Gpr(FieldA(w)), Gpr(FieldB(w)));
      break;

    case Kind::None:
      break;
    }
  }

  // --- floating point ------------------------------------------------------------------------

  void FloatArith(const FloatOp& op)
  {
    if (op.name == nullptr)
    {
      Illegal();
      return;
    }
    const u32 w = m_word;
    m_mnemonic = fmt::format("f{}{}", op.name, RcBit(w) ? "." : "");
    m_args = fmt::format("f{}", FieldD(w));

    bool stray_bits = false;
    const auto operand = [&](u8 flag, u32 value) {
      if (op.fields & flag)
        m_args += fmt::format(", f{}", value);
      else if (!(op.fields & kIgnoreUnused) && value != 0)
        stray_bits = true;
    };
    operand(kFprA, FieldA(w));
    operand(kFprC, FieldC(w));
    operand(kFprB, FieldB(w));
    if (stray_bits)
      Illegal();
  }

  void Opcode63()
  {
    const u32 w = m_word;
    if (Ext5(w) & 16)
    {
      FloatArith(kDoubleA[Ext5(w)]);
      return;
    }

    switch (Ext10(w))
    {
    case 0:
    case 32:
      if ((w & 0x00600001) != 0)
      {
        Illegal();
        break;
      }
      m_mnemonic = Ext10(w) == 0 ? "fcmpu" : "fcmpo";
      m_args = fmt::format("cr{}, f{}, f{}", CrDest(w), FieldA(w), FieldB(w));
      break;
    case 12:
      FloatArith({"rsp", kFprB});
      break;
    case 14:
      FloatArith({"ctiw", kFprB});
      break;
    case 15:
      FloatArith({"ctiwz", kFprB});
      break;
    case 40:
      FloatArith({"neg", kFprB | kIgnoreUnused});
      break;
    case 136:
      FloatArith({"nabs", kFprB | kIgnoreUnused});
      break;
    case 264:
      FloatArith({"abs", kFprB | kIgnoreUnused});
      break;
    case 72:
      m_mnemonic = RcBit(w) ? "fmr." : "fmr";
      m_args = fmt::format("f{}, f{}", FieldD(w), FieldB(w));
      break;
    case 38:
    case 70:
      if ((w & (kMaskA | kMaskB)) != 0)
      {
        Illegal();
        break;
      }
      m_mnemonic = fmt::format("mtfsb{}{}", Ext10(w) == 38 ? 1 : 0, RcBit(w) ? "." : "");
      m_args = std::to_string(FieldD(w));
      break;
    case 64:
      MoveCrField("s");
      break;
    case 134:
      if ((w & 0x006f0800) != 0)
      {
        Illegal();
        break;
      }
      m_mnemonic = RcBit(w) ? "mtfsfi." : "mtfsfi";
      m_args = fmt::format("cr{},{}", CrDest(w), (w >> 12) & 15);
      break;
    case 583:
      if ((w & (kMaskA | kMaskB)) != 0)
      {
        Illegal();
        break;
      }
      m_mnemonic = RcBit(w) ? "mffs." : "mffs";
      m_args = Gpr(FieldD(w));
      break;
    case 711:
      if ((w & 0x02010000) != 0)
      {
        Illegal();
        break;
      }
      m_mnemonic = RcBit(w) ? "mtfsf." : "mtfsf";
      m_args = fmt::format("0x{:x}, f{}", (w >> 17) & 0xff, FieldB(w));
      break;
    default:
      Illegal();
      break;
    }
  }

  // --- paired singles ------------------------------------------------------------------------

  void Paired()
  {
    const u32 w = m_word;
    const u32 xo5 = Ext5(w);

    if (xo5 == 6 || xo5 == 7)
    {
      const bool update = (w & 0x40) != 0;
      m_mnemonic = xo5 == 6 ? (update ? "psq_lux" : "psq_lx") : (update ? "psq_stux" : "psq_stx");
      m_args = fmt::format("p{}, r{}, r{}, {}, qr{}", FieldD(w), FieldA(w), FieldB(w),
                           (w >> 10) & 1, (w >> 7) & 7);
      return;
    }
    if (kPaired5[xo5].name != nullptr)
    {
      m_mnemonic = kPaired5[xo5].name;
      m_args = ExpandPaired(w, kPaired5[xo5].operands);
      return;
    }

    const u32 xo10 = Ext10(w);
    if (xo10 == 0 || xo10 == 32 || xo10 == 64 || xo10 == 96)
    {
      static constexpr const char* names[] = {"ps_cmpu0", "ps_cmpo0", "ps_cmpu1", "ps_cmpo1"};
      m_mnemonic = names[xo10 >> 5];
      if (CrDest(w) != 0)
        m_args = fmt::format("cr{}, ", CrDest(w));
      m_args += fmt::format("p{}, p{}", FieldA(w), FieldB(w));
      return;
    }
    for (const Paired10& entry : kPaired10)
    {
      if (entry.xo == xo10)
      {
        m_mnemonic = entry.op.name;
        m_args = ExpandPaired(w, entry.op.operands);
        return;
      }
    }
    if (xo10 == 1014)
    {
      if ((w & kMaskD) != 0 || RcBit(w))
      {
        Illegal();
        return;
      }
      m_mnemonic = "dcbz_l";
      m_args = RegList(w, kRegA | kRegB);
      return;
    }

    m_mnemonic = fmt::format("ps_{}", xo5);
    m_args = "---";
  }

  u32 m_word;
  u32 m_pc;
  u32 m_operand_word = 0;
  std::string m_mnemonic;
  std::string m_args;
};
}  // namespace

std::string GekkoDisassembler::Disassemble(u32 opcode, u32 current_instruction_address,
                                           bool big_endian)
{
  const u32 word = big_endian ? opcode :
                                (opcode >> 24) | ((opcode >> 8) & 0xff00) |
                                    ((opcode << 8) & 0xff0000) | (opcode << 24);
  return Decoder(word, current_instruction_address).Run();
}

const char* GekkoDisassembler::GetGPRName(u32 index)
{
  return index < kGprLabels.size() ? kGprLabels[index] : nullptr;
}

const char* GekkoDisassembler::GetFPRName(u32 index)
{
  return index < kFprLabels.size() ? kFprLabels[index] : nullptr;
}
}  // namespace Common
