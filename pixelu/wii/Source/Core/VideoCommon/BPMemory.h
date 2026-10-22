// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Layout of the BP register file (the pixel pipeline's 256 registers: indirect texturing, TEV,
// texture units, scissor, fog, blending and EFB copies), plus debug formatters for each register
// used by the FIFO analyzer and logging.

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "Common/BitField.h"
#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/EnumMap.h"
#include "Common/Inline.h"

// Xlib's None (0) and Always (2) macros would otherwise replace enumerators below.
#undef None
#undef Always

enum class TextureFormat;
enum class EFBCopyFormat;
enum class TLUTFormat;

#pragma pack(4)

// BP register addresses. Comments give the number of consecutive registers for ranges.
enum
{
  BPMEM_GENMODE = 0x00,
  BPMEM_DISPLAYCOPYFILTER = 0x01,  // 4 registers
  BPMEM_IND_MTXA = 0x06,           // every third register, 3 matrices
  BPMEM_IND_MTXB = 0x07,           // every third register, 3 matrices
  BPMEM_IND_MTXC = 0x08,           // every third register, 3 matrices
  BPMEM_IND_IMASK = 0x0F,
  BPMEM_IND_CMD = 0x10,  // 16 registers
  BPMEM_SCISSORTL = 0x20,
  BPMEM_SCISSORBR = 0x21,
  BPMEM_LINEPTWIDTH = 0x22,
  BPMEM_PERF0_TRI = 0x23,
  BPMEM_PERF0_QUAD = 0x24,
  BPMEM_RAS1_SS0 = 0x25,
  BPMEM_RAS1_SS1 = 0x26,
  BPMEM_IREF = 0x27,
  BPMEM_TREF = 0x28,      // 8 registers
  BPMEM_SU_SSIZE = 0x30,  // every other register, 8 coordinates
  BPMEM_SU_TSIZE = 0x31,  // every other register, 8 coordinates
  BPMEM_ZMODE = 0x40,
  BPMEM_BLENDMODE = 0x41,
  BPMEM_CONSTANTALPHA = 0x42,
  BPMEM_ZCOMPARE = 0x43,
  BPMEM_FIELDMASK = 0x44,
  BPMEM_SETDRAWDONE = 0x45,
  BPMEM_BUSCLOCK0 = 0x46,
  BPMEM_PE_TOKEN_ID = 0x47,
  BPMEM_PE_TOKEN_INT_ID = 0x48,
  BPMEM_EFB_TL = 0x49,
  BPMEM_EFB_WH = 0x4A,
  BPMEM_EFB_ADDR = 0x4B,
  BPMEM_EFB_STRIDE = 0x4D,
  BPMEM_COPYYSCALE = 0x4E,
  BPMEM_CLEAR_AR = 0x4F,
  BPMEM_CLEAR_GB = 0x50,
  BPMEM_CLEAR_Z = 0x51,
  BPMEM_TRIGGER_EFB_COPY = 0x52,
  BPMEM_COPYFILTER0 = 0x53,
  BPMEM_COPYFILTER1 = 0x54,
  BPMEM_CLEARBBOX1 = 0x55,
  BPMEM_CLEARBBOX2 = 0x56,
  BPMEM_CLEAR_PIXEL_PERF = 0x57,
  BPMEM_REVBITS = 0x58,
  BPMEM_SCISSOROFFSET = 0x59,
  BPMEM_PRELOAD_ADDR = 0x60,
  BPMEM_PRELOAD_TMEMEVEN = 0x61,
  BPMEM_PRELOAD_TMEMODD = 0x62,
  BPMEM_PRELOAD_MODE = 0x63,
  BPMEM_LOADTLUT0 = 0x64,
  BPMEM_LOADTLUT1 = 0x65,
  BPMEM_TEXINVALIDATE = 0x66,
  BPMEM_PERF1 = 0x67,
  BPMEM_FIELDMODE = 0x68,
  BPMEM_BUSCLOCK1 = 0x69,
  // Texture units 0-3 occupy 0x80-0x9f and units 4-7 occupy 0xa0-0xbf; each group has one
  // register per unit for each kind below.
  BPMEM_TX_SETMODE0 = 0x80,
  BPMEM_TX_SETMODE1 = 0x84,
  BPMEM_TX_SETIMAGE0 = 0x88,
  BPMEM_TX_SETIMAGE1 = 0x8C,
  BPMEM_TX_SETIMAGE2 = 0x90,
  BPMEM_TX_SETIMAGE3 = 0x94,
  BPMEM_TX_SETTLUT = 0x98,
  BPMEM_TX_SETMODE0_4 = 0xA0,
  BPMEM_TX_SETMODE1_4 = 0xA4,
  BPMEM_TX_SETIMAGE0_4 = 0xA8,
  BPMEM_TX_SETIMAGE1_4 = 0xAC,
  BPMEM_TX_SETIMAGE2_4 = 0xB0,
  BPMEM_TX_SETIMAGE3_4 = 0xB4,
  BPMEM_TX_SETTLUT_4 = 0xB8,
  BPMEM_TEV_COLOR_ENV = 0xC0,  // every other register, 16 stages
  BPMEM_TEV_ALPHA_ENV = 0xC1,  // every other register, 16 stages
  BPMEM_TEV_COLOR_RA = 0xE0,   // every other register, 4 TEV registers
  BPMEM_TEV_COLOR_BG = 0xE1,   // every other register, 4 TEV registers
  BPMEM_FOGRANGE = 0xE8,       // 6 registers
  BPMEM_FOGPARAM0 = 0xEE,
  BPMEM_FOGBMAGNITUDE = 0xEF,
  BPMEM_FOGBEXPONENT = 0xF0,
  BPMEM_FOGPARAM3 = 0xF1,
  BPMEM_FOGCOLOR = 0xF2,
  BPMEM_ALPHACOMPARE = 0xF3,
  BPMEM_BIAS = 0xF4,
  BPMEM_ZTEX2 = 0xF5,
  BPMEM_TEV_KSEL = 0xF6,  // 8 registers
  BPMEM_BP_MASK = 0xFE,
};

// Shared pieces of the register formatters below.
namespace BPMemoryFormat
{
// Base for formatters that accept only an empty format spec.
struct PlainSpec
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
};

constexpr const char* YesNo(bool value)
{
  return value ? "Yes" : "No";
}
}  // namespace BPMemoryFormat

// ---------------------------------------------------------------------------------------------
// TEV (texture environment) combiner enums

enum class TevScale : u32
{
  Scale1 = 0,
  Scale2 = 1,
  Scale4 = 2,
  Divide2 = 3
};
template <>
struct fmt::formatter<TevScale> : EnumFormatter<TevScale::Divide2>
{
  constexpr formatter() : EnumFormatter({"1", "2", "4", "0.5"}) {}
};

enum class TevOp : u32
{
  Add = 0,
  Sub = 1,
};
template <>
struct fmt::formatter<TevOp> : EnumFormatter<TevOp::Sub>
{
  constexpr formatter() : EnumFormatter({"Add", "Subtract"}) {}
};

// Which channels a compare-mode stage compares (packed into one value).
enum class TevCompareMode : u32
{
  R8 = 0,
  GR16 = 1,
  BGR24 = 2,
  RGB8 = 3,
  A8 = RGB8,
};
template <>
struct fmt::formatter<TevCompareMode> : EnumFormatter<TevCompareMode::RGB8>
{
  constexpr formatter() : EnumFormatter({"R8", "GR16", "BGR24", "RGB8 / A8"}) {}
};

enum class TevComparison : u32
{
  GT = 0,
  EQ = 1,
};
template <>
struct fmt::formatter<TevComparison> : EnumFormatter<TevComparison::EQ>
{
  constexpr formatter() : EnumFormatter({"Greater than", "Equal to"}) {}
};

// Inputs of the color combiner.
enum class TevColorArg : u32
{
  PrevColor = 0,
  PrevAlpha = 1,
  Color0 = 2,
  Alpha0 = 3,
  Color1 = 4,
  Alpha1 = 5,
  Color2 = 6,
  Alpha2 = 7,
  TexColor = 8,
  TexAlpha = 9,
  RasColor = 10,
  RasAlpha = 11,
  One = 12,
  Half = 13,
  Konst = 14,
  Zero = 15
};
template <>
struct fmt::formatter<TevColorArg> : EnumFormatter<TevColorArg::Zero>
{
  static constexpr array_type names = {
      "prev.rgb", "prev.aaa", "c0.rgb",  "c0.aaa",  "c1.rgb", "c1.aaa", "c2.rgb",    "c2.aaa",
      "tex.rgb",  "tex.aaa",  "ras.rgb", "ras.aaa", "ONE",    "HALF",   "konst.rgb", "ZERO",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Inputs of the alpha combiner.
enum class TevAlphaArg : u32
{
  PrevAlpha = 0,
  Alpha0 = 1,
  Alpha1 = 2,
  Alpha2 = 3,
  TexAlpha = 4,
  RasAlpha = 5,
  Konst = 6,
  Zero = 7
};
template <>
struct fmt::formatter<TevAlphaArg> : EnumFormatter<TevAlphaArg::Zero>
{
  static constexpr array_type names = {
      "prev", "c0", "c1", "c2", "tex", "ras", "konst", "ZERO",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Where a stage writes its result.
enum class TevOutput : u32
{
  Prev = 0,
  Color0 = 1,
  Color1 = 2,
  Color2 = 3,
};
template <>
struct fmt::formatter<TevOutput> : EnumFormatter<TevOutput::Color2>
{
  constexpr formatter() : EnumFormatter({"prev", "c0", "c1", "c2"}) {}
};

// Depth texture formats.
enum class ZTexFormat : u32
{
  U8 = 0,
  U16 = 1,
  U24 = 2
};
template <>
struct fmt::formatter<ZTexFormat> : EnumFormatter<ZTexFormat::U24>
{
  constexpr formatter() : EnumFormatter({"u8", "u16", "u24"}) {}
};

// How a depth texture modifies the fragment depth.
enum class ZTexOp : u32
{
  Disabled = 0,
  Add = 1,
  Replace = 2
};
template <>
struct fmt::formatter<ZTexOp> : EnumFormatter<ZTexOp::Replace>
{
  constexpr formatter() : EnumFormatter({"Disabled", "Add", "Replace"}) {}
};

// Bias added to a stage's result; Compare switches the stage to compare mode.
enum class TevBias : u32
{
  Zero = 0,
  AddHalf = 1,
  SubHalf = 2,
  Compare = 3
};
template <>
struct fmt::formatter<TevBias> : EnumFormatter<TevBias::Compare>
{
  constexpr formatter() : EnumFormatter({"0", "+0.5", "-0.5", "compare"}) {}
};

// ---------------------------------------------------------------------------------------------
// Indirect texturing

// Number of bits kept from each indirect texture component.
enum class IndTexFormat : u32
{
  ITF_8 = 0,
  ITF_5 = 1,
  ITF_4 = 2,
  ITF_3 = 3
};
template <>
struct fmt::formatter<IndTexFormat> : EnumFormatter<IndTexFormat::ITF_3>
{
  constexpr formatter() : EnumFormatter({"ITF_8", "ITF_5", "ITF_4", "ITF_3"}) {}
};

// Components that get a bias applied (one bit each for S, T and U).
enum class IndTexBias : u32
{
  None = 0,
  S = 1,
  T = 2,
  ST = 3,
  U = 4,
  SU = 5,
  TU_ = 6,  // underscore avoids a clash with a PowerPC.h macro
  STU = 7
};
template <>
struct fmt::formatter<IndTexBias> : EnumFormatter<IndTexBias::STU>
{
  constexpr formatter() : EnumFormatter({"None", "S", "T", "ST", "U", "SU", "TU", "STU"}) {}
};

enum class IndMtxIndex : u32
{
  Off = 0,
  Matrix0 = 1,
  Matrix1 = 2,
  Matrix2 = 3,
};
template <>
struct fmt::formatter<IndMtxIndex> : EnumFormatter<IndMtxIndex::Matrix2>
{
  constexpr formatter() : EnumFormatter({"Off", "Matrix 0", "Matrix 1", "Matrix 2"}) {}
};

enum class IndMtxId : u32
{
  Indirect = 0,
  S = 1,
  T = 2,
};
template <>
struct fmt::formatter<IndMtxId> : EnumFormatter<IndMtxId::T>
{
  constexpr formatter() : EnumFormatter({"Indirect", "S", "T"}) {}
};

// Which indirect component supplies the "bump alpha" rasterizer input.
enum class IndTexBumpAlpha : u32
{
  Off = 0,
  S = 1,
  T = 2,
  U = 3
};
template <>
struct fmt::formatter<IndTexBumpAlpha> : EnumFormatter<IndTexBumpAlpha::U>
{
  constexpr formatter() : EnumFormatter({"Off", "S", "T", "U"}) {}
};

// Wrapping applied to the regular texture coordinate before the offset is added.
enum class IndTexWrap : u32
{
  ITW_OFF = 0,
  ITW_256 = 1,
  ITW_128 = 2,
  ITW_64 = 3,
  ITW_32 = 4,
  ITW_16 = 5,
  ITW_0 = 6
};
template <>
struct fmt::formatter<IndTexWrap> : EnumFormatter<IndTexWrap::ITW_0>
{
  constexpr formatter() : EnumFormatter({"Off", "256", "128", "64", "32", "16", "0"}) {}
};

// An indirect matrix is 2x3 with 11-bit signed entries (1.10 fixed point), sent as three
// columns. Each column also carries part of the matrix's scale exponent.
union IND_MTXA
{
  BitField<0, 11, s32> ma;
  BitField<11, 11, s32> mb;
  BitField<22, 2, u8, u32> s0;  // scale exponent bits 0-1
  u32 hex;
};

union IND_MTXB
{
  BitField<0, 11, s32> mc;
  BitField<11, 11, s32> md;
  BitField<22, 2, u8, u32> s1;  // scale exponent bits 2-3
  u32 hex;
};

union IND_MTXC
{
  BitField<0, 11, s32> me;
  BitField<11, 11, s32> mf;
  BitField<22, 1, u8, u32> s2;  // scale exponent bit 4
  // The SDK writes two scale bits here like in the other columns, but hardware only uses the
  // lower one.
  BitField<22, 2, u8, u32> sdk_s2;
  u32 hex;
};

namespace BPMemoryFormat
{
// The two matrix rows held by one column, named m<first> and m<second>.
template <typename Out>
Out WriteIndMtxRows(Out out, char first, s32 row0, char second, s32 row1)
{
  return fmt::format_to(out, "Row 0 (m{}): {} ({})\nRow 1 (m{}): {} ({})\n", first,
                        static_cast<float>(row0) / 1024.0f, row0, second,
                        static_cast<float>(row1) / 1024.0f, row1);
}
}  // namespace BPMemoryFormat

template <>
struct fmt::formatter<IND_MTXA> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const IND_MTXA& col, FormatContext& ctx) const
  {
    auto out = BPMemoryFormat::WriteIndMtxRows(ctx.out(), 'a', col.ma, 'b', col.mb);
    // These bits already sit at the bottom of the exponent, so both values match.
    const u8 bits = col.s0;
    return fmt::format_to(out, "Scale bits: {} (shifted: {})", bits, bits);
  }
};
template <>
struct fmt::formatter<IND_MTXB> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const IND_MTXB& col, FormatContext& ctx) const
  {
    auto out = BPMemoryFormat::WriteIndMtxRows(ctx.out(), 'c', col.mc, 'd', col.md);
    const u8 bits = col.s1;
    return fmt::format_to(out, "Scale bits: {} (shifted: {})", bits, bits << 2);
  }
};
template <>
struct fmt::formatter<IND_MTXC> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const IND_MTXC& col, FormatContext& ctx) const
  {
    auto out = BPMemoryFormat::WriteIndMtxRows(ctx.out(), 'e', col.me, 'f', col.mf);
    const u8 bits = col.s2;
    const u8 sdk_bits = col.sdk_s2;
    return fmt::format_to(out, "Scale bits: {} (shifted: {}), given to SDK as {} ({})", bits,
                          bits << 4, sdk_bits, sdk_bits << 4);
  }
};

struct IND_MTX
{
  IND_MTXA col0;
  IND_MTXB col1;
  IND_MTXC col2;

  // The 5-bit scale exponent assembled from the three columns.
  u8 GetScale() const
  {
    const u32 low = col0.s0;
    const u32 mid = col1.s1;
    const u32 high = col2.s2;
    return static_cast<u8>(low | (mid << 2) | (high << 4));
  }
};

union IND_IMASK
{
  BitField<0, 24, u32> mask;
  u32 hex;
};

// ---------------------------------------------------------------------------------------------
// TEV stages

struct TevStageCombiner
{
  // Color stage: dest = (d op lerp(a, b, c) + bias) * scale, or a comparison when bias is Compare.
  // a, b and c are 8-bit inputs; d keeps 10 bits.
  union ColorCombiner
  {
    BitField<0, 4, TevColorArg> d;
    BitField<4, 4, TevColorArg> c;
    BitField<8, 4, TevColorArg> b;
    BitField<12, 4, TevColorArg> a;

    BitField<16, 2, TevBias> bias;
    BitField<18, 1, TevOp> op;                  // regular stages
    BitField<18, 1, TevComparison> comparison;  // compare stages
    BitField<19, 1, bool, u32> clamp;

    BitField<20, 2, TevScale> scale;               // regular stages
    BitField<20, 2, TevCompareMode> compare_mode;  // compare stages
    BitField<22, 2, TevOutput> dest;

    u32 hex;
  };
  // Alpha stage: same equation on alpha, plus the swap tables used for this stage's inputs.
  union AlphaCombiner
  {
    BitField<0, 2, u32> rswap;
    BitField<2, 2, u32> tswap;
    BitField<4, 3, TevAlphaArg> d;
    BitField<7, 3, TevAlphaArg> c;
    BitField<10, 3, TevAlphaArg> b;
    BitField<13, 3, TevAlphaArg> a;

    BitField<16, 2, TevBias> bias;
    BitField<18, 1, TevOp> op;                  // regular stages
    BitField<18, 1, TevComparison> comparison;  // compare stages
    BitField<19, 1, bool, u32> clamp;

    BitField<20, 2, TevScale> scale;               // regular stages
    BitField<20, 2, TevCompareMode> compare_mode;  // compare stages
    BitField<22, 2, TevOutput> dest;

    u32 hex;
  };

  ColorCombiner colorC;
  AlphaCombiner alphaC;
};

namespace BPMemoryFormat
{
// Per-combiner details for EquationOf.
struct ColorTerms
{
  using Arg = TevColorArg;
  static constexpr std::string_view channel = "rgb";

  // Constants are spelled as numbers in the equation, unlike the enum's own names.
  static std::string Name(TevColorArg arg)
  {
    static constexpr std::array<const char*, 16> spelled = {
        "prev.rgb", "prev.aaa", "c0.rgb",  "c0.aaa",  "c1.rgb", "c1.aaa", "c2.rgb",    "c2.aaa",
        "tex.rgb",  "tex.aaa",  "ras.rgb", "ras.aaa", "1",      ".5",     "konst.rgb", "0",
    };
    return spelled[static_cast<u32>(arg)];
  }
  static bool IsOne(TevColorArg arg) { return arg == TevColorArg::One; }
  static bool IsHalf(TevColorArg arg) { return arg == TevColorArg::Half; }
};

struct AlphaTerms
{
  using Arg = TevAlphaArg;
  static constexpr std::string_view channel = "a";

  // Spelled with ".a" so the terms do not read as whole vectors; Zero is never printed.
  static std::string Name(TevAlphaArg arg) { return fmt::format("{:n}.a", arg); }
  // Alpha inputs have no 1 or 0.5 constants.
  static bool IsOne(TevAlphaArg) { return false; }
  static bool IsHalf(TevAlphaArg) { return false; }
};

// A readable form of a regular (non-compare) stage, e.g. "dest.rgb = (c0.rgb + tex.rgb) * 2",
// leaving out terms that are zero or multiplied by zero. The trailing blank line separates it
// from the field list.
template <typename Terms, typename Stage>
std::string EquationOf(const Stage& stage)
{
  using Arg = typename Terms::Arg;
  const Arg a = stage.a;
  const Arg b = stage.b;
  const Arg c = stage.c;
  const Arg d = stage.d;

  // The interpolation is (1 - c) * a + c * b.
  const bool uses_a = a != Arg::Zero && !Terms::IsOne(c);
  const bool uses_b = b != Arg::Zero && c != Arg::Zero;

  std::string blend;
  if (uses_a && uses_b)
  {
    blend = Terms::IsHalf(c) ?
                fmt::format("({} + {})/2", Terms::Name(a), Terms::Name(b)) :
                fmt::format("lerp({}, {}, {})", Terms::Name(a), Terms::Name(b), Terms::Name(c));
  }
  else if (uses_a)
  {
    if (c == Arg::Zero)
      blend = Terms::Name(a);
    else if (Terms::IsHalf(c))
      blend = ".5*" + Terms::Name(a);
    else
      blend = fmt::format("(1 - {})*{}", Terms::Name(c), Terms::Name(a));
  }
  else if (uses_b)
  {
    blend = Terms::IsOne(c) ? Terms::Name(b) : fmt::format("{}*{}", Terms::Name(c), Terms::Name(b));
  }

  const bool subtract = stage.op == TevOp::Sub;
  std::string sum = d != Arg::Zero ? Terms::Name(d) : std::string();
  if (!blend.empty())
  {
    if (!sum.empty())
      sum += subtract ? " - " : " + ";
    else if (subtract)
      sum += '-';
    sum += blend;
  }

  const TevBias bias = stage.bias;
  if (bias == TevBias::AddHalf)
    sum += sum.empty() ? ".5" : " + .5";
  else if (bias == TevBias::SubHalf)
    sum += sum.empty() ? "-.5" : " - .5";
  else if (sum.empty())
    sum = "0";

  std::string result;
  const TevOutput dest = stage.dest;
  if (dest == TevOutput::Prev)
    result = fmt::format("dest.{} = ", Terms::channel);
  else
    result = fmt::format("{:n}.{} = ", dest, Terms::channel);

  const TevScale scale = stage.scale;
  if (scale == TevScale::Scale1)
    result += sum;
  else
    result += fmt::format("({}) * {:n}", sum, scale);
  result += "\n\n";
  return result;
}
}  // namespace BPMemoryFormat

template <>
struct fmt::formatter<TevStageCombiner::ColorCombiner> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevStageCombiner::ColorCombiner& cc, FormatContext& ctx) const
  {
    auto out = ctx.out();
    if (cc.bias != TevBias::Compare)
      out = fmt::format_to(out, "{}", BPMemoryFormat::EquationOf<BPMemoryFormat::ColorTerms>(cc));
    return fmt::format_to(out,
                          "a: {}\nb: {}\nc: {}\nd: {}\nBias: {}\nOp: {} / Comparison: {}\n"
                          "Clamp: {}\nScale factor: {} / Compare mode: {}\nDest: {}",
                          cc.a, cc.b, cc.c, cc.d, cc.bias, cc.op, cc.comparison,
                          BPMemoryFormat::YesNo(cc.clamp), cc.scale, cc.compare_mode, cc.dest);
  }
};
template <>
struct fmt::formatter<TevStageCombiner::AlphaCombiner> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevStageCombiner::AlphaCombiner& ac, FormatContext& ctx) const
  {
    auto out = ctx.out();
    if (ac.bias != TevBias::Compare)
      out = fmt::format_to(out, "{}", BPMemoryFormat::EquationOf<BPMemoryFormat::AlphaTerms>(ac));
    return fmt::format_to(out,
                          "a: {}\nb: {}\nc: {}\nd: {}\nBias: {}\nOp: {} / Comparison: {}\n"
                          "Clamp: {}\nScale factor: {} / Compare mode: {}\nDest: {}\n"
                          "Rasterized color swaptable: {}\nTexture color swaptable: {}",
                          ac.a, ac.b, ac.c, ac.d, ac.bias, ac.op, ac.comparison,
                          BPMemoryFormat::YesNo(ac.clamp), ac.scale, ac.compare_mode, ac.dest,
                          ac.rswap, ac.tswap);
  }
};

// Indirect setup of one TEV stage. For reference, the SDK's bump-mapping helper programs three
// consecutive stages: two with bias ST (3), matrix S then T, wrap 0 and the second adding the
// previous result, then a third that only adds the previous result.
union TevStageIndirect
{
  BitField<0, 2, u32> bt;  // indirect stage supplying the offsets
  BitField<2, 2, IndTexFormat> fmt;
  BitField<4, 3, IndTexBias> bias;
  BitField<4, 1, bool, u32> bias_s;
  BitField<5, 1, bool, u32> bias_t;
  BitField<6, 1, bool, u32> bias_u;
  BitField<7, 2, IndTexBumpAlpha> bs;  // component used as bump alpha
  // Matrix applied for the Indirect matrix type; for the S and T types it still selects the
  // matrix whose scale is used.
  BitField<9, 2, IndMtxIndex> matrix_index;
  // Expected to be Indirect when matrix_index is Off.
  BitField<11, 2, IndMtxId> matrix_id;
  BitField<13, 3, IndTexWrap> sw;         // wrap for the regular S coordinate
  BitField<16, 3, IndTexWrap> tw;         // wrap for the regular T coordinate
  BitField<19, 1, bool, u32> lb_utclod;   // LOD from the modified (true) or plain coordinates
  BitField<20, 1, bool, u32> fb_addprev;  // add the previous stage's coordinate result

  struct
  {
    u32 hex : 21;
    u32 unused : 11;
  };

  u32 fullhex;

  // Without bump alpha and without a matrix the indirect lookup cannot affect anything, so the
  // indirect texture need not be sampled.
  bool IsActive() const { return bs != IndTexBumpAlpha::Off || matrix_index != IndMtxIndex::Off; }
};
template <>
struct fmt::formatter<TevStageIndirect> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevStageIndirect& tevind, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    return fmt::format_to(ctx.out(),
                          "Indirect tex stage ID: {}\nFormat: {}\nBias: {}\nBump alpha: {}\n"
                          "Offset matrix index: {}\nOffset matrix ID: {}\n"
                          "Regular coord S wrapping factor: {}\n"
                          "Regular coord T wrapping factor: {}\n"
                          "Use modified texture coordinates for LOD computation: {}\n"
                          "Add texture coordinates from previous TEV stage: {}",
                          tevind.bt, tevind.fmt, tevind.bias, tevind.bs, tevind.matrix_index,
                          tevind.matrix_id, tevind.sw, tevind.tw, YesNo(tevind.lb_utclod),
                          YesNo(tevind.fb_addprev));
  }
};

// Rasterized color fed to a TEV stage.
enum class RasColorChan : u32
{
  Color0 = 0,
  Color1 = 1,
  AlphaBump = 5,
  NormalizedAlphaBump = 6,
  Zero = 7,
};
template <>
struct fmt::formatter<RasColorChan> : EnumFormatter<RasColorChan::Zero>
{
  static constexpr array_type names = {
      "Color chan 0", "Color chan 1", nullptr,           nullptr,
      nullptr,        "Alpha bump",   "Norm alpha bump", "Zero",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Texture and color inputs of a pair of TEV stages (even stage in the low half).
union TwoTevStageOrders
{
  BitField<0, 3, u32> texmap_even;
  BitField<3, 3, u32> texcoord_even;
  BitField<6, 1, bool, u32> enable_tex_even;  // stage samples its texture
  BitField<7, 3, RasColorChan> colorchan_even;

  BitField<12, 3, u32> texmap_odd;
  BitField<15, 3, u32> texcoord_odd;
  BitField<18, 1, bool, u32> enable_tex_odd;  // stage samples its texture
  BitField<19, 3, RasColorChan> colorchan_odd;

  u32 hex;

  // i selects the stage: 0 for the even one, anything else for the odd one.
  u32 getTexMap(int i) const
  {
    if (i != 0)
      return texmap_odd;
    return texmap_even;
  }
  u32 getTexCoord(int i) const
  {
    if (i != 0)
      return texcoord_odd;
    return texcoord_even;
  }
  u32 getEnable(int i) const
  {
    const bool enabled = (i != 0) ? enable_tex_odd.Value() : enable_tex_even.Value();
    return enabled ? 1u : 0u;
  }
  RasColorChan getColorChan(int i) const
  {
    if (i != 0)
      return colorchan_odd;
    return colorchan_even;
  }
};
// Formats a TREF register; the u8 is its BP address, which gives the stage numbers.
template <>
struct fmt::formatter<std::pair<u8, TwoTevStageOrders>> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const std::pair<u8, TwoTevStageOrders>& p, FormatContext& ctx) const
  {
    const TwoTevStageOrders& orders = p.second;
    const u8 first_stage = static_cast<u8>((p.first - BPMEM_TREF) * 2);
    auto out = ctx.out();
    for (int half = 0; half < 2; ++half)
    {
      const u8 stage = static_cast<u8>(first_stage + half);
      out = fmt::format_to(out,
                           "Stage {0} texmap: {1}\nStage {0} tex coord: {2}\n"
                           "Stage {0} enable texmap: {3}\n"
                           "Stage {0} rasterized color channel: {4}\n",
                           stage, orders.getTexMap(half), orders.getTexCoord(half),
                           BPMemoryFormat::YesNo(orders.getEnable(half) != 0),
                           orders.getColorChan(half));
    }
    return out;
  }
};

// Coordinate scales (as negative powers of two) for a pair of indirect stages.
union TEXSCALE
{
  BitField<0, 4, u32> ss0;   // S of indirect stage 0 (or 2): scaled by 2^-ss0
  BitField<4, 4, u32> ts0;   // T of indirect stage 0 (or 2)
  BitField<8, 4, u32> ss1;   // S of indirect stage 1 (or 3)
  BitField<12, 4, u32> ts1;  // T of indirect stage 1 (or 3)
  u32 hex;
};
// Formats a RAS1_SS register; the u8 is its BP address.
template <>
struct fmt::formatter<std::pair<u8, TEXSCALE>> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const std::pair<u8, TEXSCALE>& p, FormatContext& ctx) const
  {
    const TEXSCALE& scale = p.second;
    const std::array<std::pair<u32, u32>, 2> shifts = {{{scale.ss0, scale.ts0},
                                                        {scale.ss1, scale.ts1}}};
    const u8 first_stage = static_cast<u8>((p.first - BPMEM_RAS1_SS0) * 2);
    const auto factor = [](u32 shift) { return 1.f / static_cast<float>(1 << shift); };

    auto out = ctx.out();
    for (u32 half = 0; half < 2; ++half)
    {
      const auto [s_shift, t_shift] = shifts[half];
      out = fmt::format_to(out,
                           "{0}Indirect stage {1} S coord scale: {2} ({3})\n"
                           "Indirect stage {1} T coord scale: {4} ({5})",
                           half != 0 ? "\n" : "", static_cast<u8>(first_stage + half),
                           factor(s_shift), s_shift, factor(t_shift), t_shift);
    }
    return out;
  }
};

// Texture map and coordinate used by each of the four indirect stages.
union RAS1_IREF
{
  BitField<0, 3, u32> bi0;  // stage 0 texture map
  BitField<3, 3, u32> bc0;  // stage 0 texture coordinate
  BitField<6, 3, u32> bi1;
  BitField<9, 3, u32> bc1;
  BitField<12, 3, u32> bi2;
  BitField<15, 3, u32> bc2;
  BitField<18, 3, u32> bi3;
  BitField<21, 3, u32> bc3;
  u32 hex;

  // Each stage owns 6 bits: the map in the low 3 and the coordinate in the high 3.
  u32 getTexMap(int i) const { return Field(i, 0); }
  u32 getTexCoord(int i) const { return Field(i, 3); }

private:
  u32 Field(int stage, int offset) const { return (hex >> (stage * 6 + offset)) & 0b111; }
};
template <>
struct fmt::formatter<RAS1_IREF> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const RAS1_IREF& indref, FormatContext& ctx) const
  {
    auto out = ctx.out();
    for (int stage = 0; stage < 4; ++stage)
    {
      out = fmt::format_to(out, "{0}Indirect stage {1} texmap: {2}\nIndirect stage {1} tex coord: {3}",
                           stage != 0 ? "\n" : "", stage, indref.getTexMap(stage),
                           indref.getTexCoord(stage));
    }
    return out;
  }
};

// ---------------------------------------------------------------------------------------------
// Texture units

enum class WrapMode : u32
{
  Clamp = 0,
  Repeat = 1,
  Mirror = 2,
  // 3 is not a valid setting; hardware tests show it behaving like Clamp.
};
template <>
struct fmt::formatter<WrapMode> : EnumFormatter<WrapMode::Mirror>
{
  constexpr formatter() : EnumFormatter({"Clamp", "Repeat", "Mirror"}) {}
};

enum class MipMode : u32
{
  None = 0,
  Point = 1,
  Linear = 2,
};
template <>
struct fmt::formatter<MipMode> : EnumFormatter<MipMode::Linear>
{
  constexpr formatter() : EnumFormatter({"None", "Mip point", "Mip linear"}) {}
};

enum class FilterMode : u32
{
  Near = 0,
  Linear = 1,
};
template <>
struct fmt::formatter<FilterMode> : EnumFormatter<FilterMode::Linear>
{
  constexpr formatter() : EnumFormatter({"Near", "Linear"}) {}
};

enum class LODType : u32
{
  Edge = 0,
  Diagonal = 1,
};
template <>
struct fmt::formatter<LODType> : EnumFormatter<LODType::Diagonal>
{
  constexpr formatter() : EnumFormatter({"Edge LOD", "Diagonal LOD"}) {}
};

enum class MaxAniso
{
  One = 0,
  Two = 1,
  Four = 2,
};
template <>
struct fmt::formatter<MaxAniso> : EnumFormatter<MaxAniso::Four>
{
  constexpr formatter() : EnumFormatter({"1", "2", "4"}) {}
};

union TexMode0
{
  BitField<0, 2, WrapMode> wrap_s;
  BitField<2, 2, WrapMode> wrap_t;
  BitField<4, 1, FilterMode> mag_filter;
  BitField<5, 2, MipMode> mipmap_filter;
  BitField<7, 1, FilterMode> min_filter;
  BitField<8, 1, LODType> diag_lod;
  BitField<9, 8, s32> lod_bias;  // in 1/32 units
  BitField<19, 2, MaxAniso> max_aniso;
  BitField<21, 1, bool, u32> lod_clamp;
  u32 hex;
};
template <>
struct fmt::formatter<TexMode0> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexMode0& mode, FormatContext& ctx) const
  {
    const s32 bias = mode.lod_bias;
    return fmt::format_to(ctx.out(),
                          "Wrap S: {}\nWrap T: {}\nMag filter: {}\nMipmap filter: {}\n"
                          "Min filter: {}\nLOD type: {}\nLOD bias: {} ({})\n"
                          "Max anisotropic filtering: {}\nLOD/bias clamp: {}",
                          mode.wrap_s, mode.wrap_t, mode.mag_filter, mode.mipmap_filter,
                          mode.min_filter, mode.diag_lod, bias, static_cast<float>(bias) / 32.f,
                          mode.max_aniso, BPMemoryFormat::YesNo(mode.lod_clamp));
  }
};

union TexMode1
{
  BitField<0, 8, u32> min_lod;  // in 1/16 units
  BitField<8, 8, u32> max_lod;  // in 1/16 units
  u32 hex;
};
template <>
struct fmt::formatter<TexMode1> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexMode1& mode, FormatContext& ctx) const
  {
    const u32 min_lod = mode.min_lod;
    const u32 max_lod = mode.max_lod;
    return fmt::format_to(ctx.out(), "Min LOD: {} ({})\nMax LOD: {} ({})", min_lod,
                          static_cast<float>(min_lod) / 16.f, max_lod,
                          static_cast<float>(max_lod) / 16.f);
  }
};

union TexImage0
{
  BitField<0, 10, u32> width;    // width - 1
  BitField<10, 10, u32> height;  // height - 1
  BitField<20, 4, TextureFormat> format;
  u32 hex;
};
template <>
struct fmt::formatter<TexImage0> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexImage0& teximg, FormatContext& ctx) const
  {
    const u32 width = teximg.width + 1;
    const u32 height = teximg.height + 1;
    return fmt::format_to(ctx.out(), "Width: {}\nHeight: {}\nFormat: {}", width, height,
                          teximg.format);
  }
};

// TMEM placement for even mip levels.
union TexImage1
{
  BitField<0, 15, u32> tmem_even;  // in 32-byte lines
  BitField<15, 3, u32> cache_width;
  BitField<18, 3, u32> cache_height;
  // When set the game preloads TMEM itself; otherwise the cache is filled on demand.
  BitField<21, 1, bool, u32> cache_manually_managed;
  u32 hex;
};

// TMEM placement for odd mip levels.
union TexImage2
{
  BitField<0, 15, u32> tmem_odd;  // in 32-byte lines
  BitField<15, 3, u32> cache_width;
  BitField<18, 3, u32> cache_height;
  u32 hex;
};

namespace BPMemoryFormat
{
template <typename Out>
Out WriteTmemPlacement(Out out, std::string_view parity, u32 line, u32 width, u32 height)
{
  return fmt::format_to(out,
                        "{0} TMEM Line: 0x{1:04x} (byte 0x{2:05x})\n{0} TMEM Width: {3}\n"
                        "{0} TMEM Height: {4}",
                        parity, line, line * 32, width, height);
}
}  // namespace BPMemoryFormat

template <>
struct fmt::formatter<TexImage1> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexImage1& teximg, FormatContext& ctx) const
  {
    auto out = BPMemoryFormat::WriteTmemPlacement(ctx.out(), "Even", teximg.tmem_even,
                                                  teximg.cache_width, teximg.cache_height);
    return fmt::format_to(out, "\nCache is manually managed: {}",
                          BPMemoryFormat::YesNo(teximg.cache_manually_managed));
  }
};
template <>
struct fmt::formatter<TexImage2> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexImage2& teximg, FormatContext& ctx) const
  {
    return BPMemoryFormat::WriteTmemPlacement(ctx.out(), "Odd", teximg.tmem_odd,
                                              teximg.cache_width, teximg.cache_height);
  }
};

union TexImage3
{
  BitField<0, 24, u32> image_base;  // physical address / 32
  u32 hex;
};
template <>
struct fmt::formatter<TexImage3> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexImage3& teximg, FormatContext& ctx) const
  {
    const u32 address = teximg.image_base << 5;
    return fmt::format_to(ctx.out(), "Source address (32 byte aligned): 0x{:06x}", address);
  }
};

union TexTLUT
{
  BitField<0, 10, u32> tmem_offset;  // in 512-byte units
  BitField<10, 2, TLUTFormat> tlut_format;
  u32 hex;
};
template <>
struct fmt::formatter<TexTLUT> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TexTLUT& tlut, FormatContext& ctx) const
  {
    const u32 address = tlut.tmem_offset << 9;
    return fmt::format_to(ctx.out(), "Tmem address: 0x{:05x}\nFormat: {}", address,
                          tlut.tlut_format);
  }
};

union ZTex1
{
  BitField<0, 24, u32> bias;
  u32 hex;
};
template <>
struct fmt::formatter<ZTex1> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ZTex1& ztex1, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Bias: 0x{:06x}", ztex1.bias);
  }
};

union ZTex2
{
  BitField<0, 2, ZTexFormat> type;
  BitField<2, 2, ZTexOp> op;
  u32 hex;
};
template <>
struct fmt::formatter<ZTex2> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ZTex2& ztex2, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Type: {}\nOperation: {}", ztex2.type, ztex2.op);
  }
};

// ---------------------------------------------------------------------------------------------
// Geometry and setup

enum class CullMode : u32
{
  None = 0,
  Back = 1,   // drop back faces
  Front = 2,  // drop front faces
  All = 3,    // drop every triangle
};
template <>
struct fmt::formatter<CullMode> : EnumFormatter<CullMode::All>
{
  static constexpr array_type names = {
      "None",
      "Back-facing primitives only",
      "Front-facing primitives only",
      "All primitives",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

union GenMode
{
  BitField<0, 4, u32> numtexgens;
  BitField<4, 3, u32> numcolchans;
  BitField<7, 1, u32> unused;              // purpose unknown
  BitField<8, 1, bool, u32> flat_shading;  // not confirmed on hardware
  BitField<9, 1, bool, u32> multisampling;
  // Stage count minus one: there is always at least one TEV stage.
  BitField<10, 4, u32> numtevstages;
  BitField<14, 2, CullMode> cull_mode;
  BitField<16, 3, u32> numindstages;
  BitField<19, 1, bool, u32> zfreeze;

  u32 hex;
};
template <>
struct fmt::formatter<GenMode> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const GenMode& mode, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    const u32 stages = mode.numtevstages + 1;
    return fmt::format_to(ctx.out(),
                          "Num tex gens: {}\nNum color channels: {}\nUnused bit: {}\n"
                          "Flat shading (unconfirmed): {}\nMultisampling: {}\n"
                          "Num TEV stages: {}\nCull mode: {}\nNum indirect stages: {}\n"
                          "ZFreeze: {}",
                          mode.numtexgens, mode.numcolchans, mode.unused,
                          YesNo(mode.flat_shading), YesNo(mode.multisampling), stages,
                          mode.cull_mode, mode.numindstages, YesNo(mode.zfreeze));
  }
};

enum class AspectRatioAdjustment
{
  DontAdjust = 0,
  Adjust = 1,
};
template <>
struct fmt::formatter<AspectRatioAdjustment> : EnumFormatter<AspectRatioAdjustment::Adjust>
{
  constexpr formatter() : EnumFormatter({"Don't adjust", "Adjust"}) {}
};

union LPSize
{
  BitField<0, 8, u32> linesize;   // sixths of a pixel
  BitField<8, 8, u32> pointsize;  // sixths of a pixel
  BitField<16, 3, u32> lineoff;
  BitField<19, 3, u32> pointoff;
  // For interlaced output, where pixels are twice as tall as wide.
  BitField<22, 1, AspectRatioAdjustment> adjust_for_aspect_ratio;
  u32 hex;
};
template <>
struct fmt::formatter<LPSize> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const LPSize& lp, FormatContext& ctx) const
  {
    const u32 line = lp.linesize;
    const u32 point = lp.pointsize;
    return fmt::format_to(ctx.out(),
                          "Line size: {} ({:.3} pixels)\nPoint size: {} ({:.3} pixels)\n"
                          "Line offset: {}\nPoint offset: {}\nAdjust line aspect ratio: {}",
                          line, static_cast<float>(line) / 6.f, point,
                          static_cast<float>(point) / 6.f, lp.lineoff, lp.pointoff,
                          lp.adjust_for_aspect_ratio);
  }
};

// A scissor corner. Coordinates carry the SDK's +342 bias. Hardware ignores the 12th bit (the SDK
// masks it off too, except libogc for the bottom edge); the *_full fields keep it for the FIFO
// analyzer.
union ScissorPos
{
  BitField<0, 11, u32> y;
  BitField<0, 12, u32> y_full;
  BitField<12, 11, u32> x;
  BitField<12, 12, u32> x_full;
  u32 hex;
};
template <>
struct fmt::formatter<ScissorPos> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ScissorPos& pos, FormatContext& ctx) const
  {
    // Unsigned on purpose: values below the bias wrap around, as they always have.
    const u32 x = pos.x;
    const u32 y = pos.y;
    return fmt::format_to(ctx.out(), "X: {} (raw: {})\nY: {} (raw: {})", x - 342, pos.x_full,
                          y - 342, pos.y_full);
  }
};

// The scissor offset, stored halved (0-511 covers 0-1022) and carrying the SDK's +342 bias
// (applied before halving). The 10th bit is ignored by hardware; *_full keeps it.
union ScissorOffset
{
  BitField<0, 9, u32> x;
  BitField<0, 10, u32> x_full;
  BitField<10, 9, u32> y;
  BitField<10, 10, u32> y_full;
  u32 hex;
};
template <>
struct fmt::formatter<ScissorOffset> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ScissorOffset& off, FormatContext& ctx) const
  {
    const u32 x = off.x;
    const u32 y = off.y;
    return fmt::format_to(ctx.out(), "X: {} (raw: {})\nY: {} (raw: {})", x * 2 - 342, off.x_full,
                          y * 2 - 342, off.y_full);
  }
};

union X10Y10
{
  BitField<0, 10, u32> x;
  BitField<10, 10, u32> y;
  u32 hex;
};

// ---------------------------------------------------------------------------------------------
// Pixel engine: blending, fog, depth

enum class SrcBlendFactor : u32
{
  Zero = 0,
  One = 1,
  DstClr = 2,
  InvDstClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};
template <>
struct fmt::formatter<SrcBlendFactor> : EnumFormatter<SrcBlendFactor::InvDstAlpha>
{
  static constexpr array_type names = {"0",         "1",           "dst_color", "1-dst_color",
                                       "src_alpha", "1-src_alpha", "dst_alpha", "1-dst_alpha"};
  constexpr formatter() : EnumFormatter(names) {}
};

enum class DstBlendFactor : u32
{
  Zero = 0,
  One = 1,
  SrcClr = 2,
  InvSrcClr = 3,
  SrcAlpha = 4,
  InvSrcAlpha = 5,
  DstAlpha = 6,
  InvDstAlpha = 7
};
template <>
struct fmt::formatter<DstBlendFactor> : EnumFormatter<DstBlendFactor::InvDstAlpha>
{
  static constexpr array_type names = {"0",         "1",           "src_color", "1-src_color",
                                       "src_alpha", "1-src_alpha", "dst_alpha", "1-dst_alpha"};
  constexpr formatter() : EnumFormatter(names) {}
};

enum class LogicOp : u32
{
  Clear = 0,
  And = 1,
  AndReverse = 2,
  Copy = 3,
  AndInverted = 4,
  NoOp = 5,
  Xor = 6,
  Or = 7,
  Nor = 8,
  Equiv = 9,
  Invert = 10,
  OrReverse = 11,
  CopyInverted = 12,
  OrInverted = 13,
  Nand = 14,
  Set = 15
};
template <>
struct fmt::formatter<LogicOp> : EnumFormatter<LogicOp::Set>
{
  static constexpr array_type names = {
      "Clear (0)",
      "And (src & dst)",
      "And Reverse (src & ~dst)",
      "Copy (src)",
      "And Inverted (~src & dst)",
      "NoOp (dst)",
      "Xor (src ^ dst)",
      "Or (src | dst)",
      "Nor (~(src | dst))",
      "Equiv (~(src ^ dst))",
      "Invert (~dst)",
      "Or Reverse (src | ~dst)",
      "Copy Inverted (~src)",
      "Or Inverted (~src | dst)",
      "Nand (~(src & dst))",
      "Set (1)",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

union BlendMode
{
  BitField<0, 1, bool, u32> blend_enable;
  BitField<1, 1, bool, u32> logic_op_enable;
  BitField<2, 1, bool, u32> dither;
  BitField<3, 1, bool, u32> color_update;
  BitField<4, 1, bool, u32> alpha_update;
  BitField<5, 3, DstBlendFactor> dst_factor;
  BitField<8, 3, SrcBlendFactor> src_factor;
  BitField<11, 1, bool, u32> subtract;
  BitField<12, 4, LogicOp> logic_mode;

  u32 hex;

  bool UseLogicOp() const;
};
template <>
struct fmt::formatter<BlendMode> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const BlendMode& mode, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    return fmt::format_to(ctx.out(),
                          "Enable: {}\nLogic ops: {}\nDither: {}\nColor write: {}\n"
                          "Alpha write: {}\nDest factor: {}\nSource factor: {}\nSubtract: {}\n"
                          "Logic mode: {}",
                          YesNo(mode.blend_enable), YesNo(mode.logic_op_enable),
                          YesNo(mode.dither), YesNo(mode.color_update), YesNo(mode.alpha_update),
                          mode.dst_factor, mode.src_factor, YesNo(mode.subtract),
                          mode.logic_mode);
  }
};

// Fog A parameter: a float with an 11-bit mantissa.
union FogParam0
{
  BitField<0, 11, u32> mant;
  BitField<11, 8, u32> exp;
  BitField<19, 1, u32> sign;

  u32 hex;
  float FloatValue() const;
};
template <>
struct fmt::formatter<FogParam0> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FogParam0& param, FormatContext& ctx) const
  {
    const char sign = param.sign != 0 ? '-' : '+';
    return fmt::format_to(ctx.out(), "A value: {}\nMantissa: {}\nExponent: {}\nSign: {}",
                          param.FloatValue(), param.mant, param.exp, sign);
  }
};

enum class FogProjection : u32
{
  Perspective = 0,
  Orthographic = 1,
};
template <>
struct fmt::formatter<FogProjection> : EnumFormatter<FogProjection::Orthographic>
{
  constexpr formatter() : EnumFormatter({"Perspective", "Orthographic"}) {}
};

enum class FogType : u32
{
  Off = 0,
  Linear = 2,
  Exp = 4,
  ExpSq = 5,
  BackwardsExp = 6,
  BackwardsExpSq = 7,
};
template <>
struct fmt::formatter<FogType> : EnumFormatter<FogType::BackwardsExpSq>
{
  // The misspelt last name is the established debugger text.
  static constexpr array_type names = {
      "Off (no fog)",
      nullptr,
      "Linear fog",
      nullptr,
      "Exponential fog",
      "Exponential-squared fog",
      "Backwards exponential fog",
      "Backwards exponenential-sequared fog",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Fog C parameter (same float format as A) together with the projection and fog function.
union FogParam3
{
  BitField<0, 11, u32> c_mant;
  BitField<11, 8, u32> c_exp;
  BitField<19, 1, u32> c_sign;
  BitField<20, 1, FogProjection> proj;
  BitField<21, 3, FogType> fsel;

  u32 hex;
  float FloatValue() const;
};
template <>
struct fmt::formatter<FogParam3> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FogParam3& param, FormatContext& ctx) const
  {
    const char sign = param.c_sign != 0 ? '-' : '+';
    return fmt::format_to(ctx.out(),
                          "C value: {}\nMantissa: {}\nExponent: {}\nSign: {}\nProjection: {}\n"
                          "Fsel: {}",
                          param.FloatValue(), param.c_mant, param.c_exp, sign, param.proj,
                          param.fsel);
  }
};

// Two 12-bit range adjustment factors.
union FogRangeKElement
{
  BitField<0, 12, u32> HI;
  BitField<12, 12, u32> LO;

  // The 1/256 scale is an assumption; it has not been checked against hardware.
  float GetValue(int i) const
  {
    const u32 raw = (i != 0) ? HI.Value() : LO.Value();
    return static_cast<float>(raw) / 256.f;
  }
  u32 HEX;
};

struct FogRangeParams
{
  union RangeBase
  {
    BitField<0, 10, u32> Center;  // viewport center, +342 bias
    BitField<10, 1, bool, u32> Enabled;
    u32 hex;
  };
  RangeBase Base;
  FogRangeKElement K[5];
};
template <>
struct fmt::formatter<FogRangeParams::RangeBase> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FogRangeParams::RangeBase& range, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Center: {}\nEnabled: {}", range.Center,
                          BPMemoryFormat::YesNo(range.Enabled));
  }
};
template <>
struct fmt::formatter<FogRangeKElement> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FogRangeKElement& range, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "High: {}\nLow: {}", range.HI, range.LO);
  }
};

// Fog parameters. Eye-space depth is reconstructed as A / (B_magnitude - (Zscreen >> B_shift)).
struct FogParams
{
  FogParam0 a;
  u32 b_magnitude;
  u32 b_shift;  // possibly B's exponent plus one
  FogParam3 c_proj_fsel;

  union FogColor
  {
    BitField<0, 8, u32> b;
    BitField<8, 8, u32> g;
    BitField<16, 8, u32> r;
    u32 hex;
  };

  FogColor color;

  // True when A and C both have the all-ones exponent (inf/NaN); see BPMemory.cpp.
  bool IsNaNCase() const;
  float GetA() const;

  // Offset subtracted from the range-adjusted eye-space depth.
  float GetC() const;
};
template <>
struct fmt::formatter<FogParams::FogColor> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FogParams::FogColor& color, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Red: {}\nGreen: {}\nBlue: {}", color.r, color.g, color.b);
  }
};

enum class CompareMode : u32
{
  Never = 0,
  Less = 1,
  Equal = 2,
  LEqual = 3,
  Greater = 4,
  NEqual = 5,
  GEqual = 6,
  Always = 7
};
template <>
struct fmt::formatter<CompareMode> : EnumFormatter<CompareMode::Always>
{
  static constexpr array_type names = {"Never",   "Less",   "Equal",  "LEqual",
                                       "Greater", "NEqual", "GEqual", "Always"};
  constexpr formatter() : EnumFormatter(names) {}
};

union ZMode
{
  BitField<0, 1, bool, u32> test_enable;
  BitField<1, 3, CompareMode> func;
  BitField<4, 1, bool, u32> update_enable;

  u32 hex;
};
template <>
struct fmt::formatter<ZMode> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ZMode& mode, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    return fmt::format_to(ctx.out(), "Enable test: {}\nCompare function: {}\nEnable updates: {}",
                          YesNo(mode.test_enable), mode.func, YesNo(mode.update_enable));
  }
};

union ConstantAlpha
{
  BitField<0, 8, u32> alpha;
  BitField<8, 1, bool, u32> enable;
  u32 hex;
};
template <>
struct fmt::formatter<ConstantAlpha> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const ConstantAlpha& c, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Enable: {}\nAlpha value: {:02x}",
                          BPMemoryFormat::YesNo(c.enable), c.alpha);
  }
};

union FieldMode
{
  // Compensates texture LOD selection for interlaced (half-height) fields.
  BitField<0, 1, AspectRatioAdjustment> texLOD;
  u32 hex;
};
template <>
struct fmt::formatter<FieldMode> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FieldMode& mode, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(),
                          "Adjust vertex tex LOD computation to account for interlacing: {}",
                          mode.texLOD);
  }
};

enum class FieldMaskState : u32
{
  Skip = 0,
  Write = 1,
};
template <>
struct fmt::formatter<FieldMaskState> : EnumFormatter<FieldMaskState::Write>
{
  constexpr formatter() : EnumFormatter({"Skipped", "Written"}) {}
};

// Which interlaced fields reach the EFB.
union FieldMask
{
  BitField<0, 1, FieldMaskState> odd;
  BitField<1, 1, FieldMaskState> even;
  u32 hex;
};
template <>
struct fmt::formatter<FieldMask> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const FieldMask& mask, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Odd field: {}\nEven field: {}", mask.odd, mask.even);
  }
};

enum class PixelFormat : u32
{
  RGB8_Z24 = 0,
  RGBA6_Z24 = 1,
  RGB565_Z16 = 2,
  Z24 = 3,
  Y8 = 4,
  U8 = 5,
  V8 = 6,
  YUV420 = 7,
  INVALID_FMT = 0xffffffff,  // not a hardware value: marks "no format known yet"
};
template <>
struct fmt::formatter<PixelFormat> : EnumFormatter<PixelFormat::YUV420>
{
  static constexpr array_type names = {"RGB8_Z24", "RGBA6_Z24", "RGB565_Z16", "Z24",
                                       "Y8",       "U8",        "V8",         "YUV420"};
  constexpr formatter() : EnumFormatter(names) {}
};

enum class DepthFormat : u32
{
  ZLINEAR = 0,
  ZNEAR = 1,
  ZMID = 2,
  ZFAR = 3,

  // The inverted variants appear to be unsupported by hardware.
  ZINV_LINEAR = 4,
  ZINV_NEAR = 5,
  ZINV_MID = 6,
  ZINV_FAR = 7
};
template <>
struct fmt::formatter<DepthFormat> : EnumFormatter<DepthFormat::ZINV_FAR>
{
  static constexpr array_type names = {
      "linear",     "compressed (near)",     "compressed (mid)",     "compressed (far)",
      "inv linear", "compressed (inv near)", "compressed (inv mid)", "compressed (inv far)",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

union PEControl
{
  BitField<0, 3, PixelFormat> pixel_format;
  BitField<3, 3, DepthFormat> zformat;
  BitField<6, 1, bool, u32> early_ztest;

  u32 hex;
};
template <>
struct fmt::formatter<PEControl> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const PEControl& config, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "EFB pixel format: {}\nDepth format: {}\nEarly depth test: {}",
                          config.pixel_format, config.zformat,
                          BPMemoryFormat::YesNo(config.early_ztest));
  }
};

// ---------------------------------------------------------------------------------------------
// Texture coordinate scaling

union TCInfo
{
  BitField<0, 16, u32> scale_minus_1;
  BitField<16, 1, bool, u32> range_bias;
  BitField<17, 1, bool, u32> cylindric_wrap;
  // Only meaningful in the S half of a TCoordInfo.
  BitField<18, 1, bool, u32> line_offset;
  BitField<19, 1, bool, u32> point_offset;
  u32 hex;
};
// The bool says whether this is the S (true) or T (false) half.
template <>
struct fmt::formatter<std::pair<bool, TCInfo>> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const std::pair<bool, TCInfo>& p, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    const bool is_s = p.first;
    const TCInfo& info = p.second;
    const u32 scale = info.scale_minus_1 + 1;
    auto out = fmt::format_to(ctx.out(),
                              "{0} coord scale: {1}\n{0} coord range bias: {2}\n"
                              "{0} coord cylindric wrap: {3}",
                              is_s ? 'S' : 'T', scale, YesNo(info.range_bias),
                              YesNo(info.cylindric_wrap));
    if (!is_s)
      return out;
    return fmt::format_to(out, "\nUse line offset: {}\nUse point offset: {}",
                          YesNo(info.line_offset), YesNo(info.point_offset));
  }
};

struct TCoordInfo
{
  TCInfo s;
  TCInfo t;
};

// ---------------------------------------------------------------------------------------------
// TEV registers and konstant selection

enum class TevRegType : u32
{
  Color = 0,
  Constant = 1,
};
template <>
struct fmt::formatter<TevRegType> : EnumFormatter<TevRegType::Constant>
{
  constexpr formatter() : EnumFormatter({"Color", "Constant"}) {}
};

// A TEV color register (or konstant color, depending on type), split over two BP registers.
// Components are 11-bit signed; konstants may only use the low 8 bits (unverified).
struct TevReg
{
  union RA
  {
    u32 hex;

    BitField<0, 11, s32> red;
    BitField<12, 11, s32> alpha;
    BitField<23, 1, TevRegType, u32> type;
  };
  union BG
  {
    u32 hex;

    BitField<0, 11, s32> blue;
    BitField<12, 11, s32> green;
    BitField<23, 1, TevRegType, u32> type;
  };

  RA ra;
  BG bg;
};
template <>
struct fmt::formatter<TevReg::RA> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevReg::RA& ra, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Type: {}\nAlpha: {:03x}\nRed: {:03x}", ra.type, ra.alpha,
                          ra.red);
  }
};
template <>
struct fmt::formatter<TevReg::BG> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevReg::BG& bg, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "Type: {}\nGreen: {:03x}\nBlue: {:03x}", bg.type, bg.green,
                          bg.blue);
  }
};
template <>
struct fmt::formatter<TevReg> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const TevReg& reg, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{}\n{}", reg.ra, reg.bg);
  }
};

enum class ColorChannel : u32
{
  Red = 0,
  Green = 1,
  Blue = 2,
  Alpha = 3,
};
template <>
struct fmt::formatter<ColorChannel> : EnumFormatter<ColorChannel::Alpha>
{
  constexpr formatter() : EnumFormatter({"Red", "Green", "Blue", "Alpha"}) {}
};

// Konstant input selection: fixed fractions, a whole konstant color, or one of its components.
enum class KonstSel : u32
{
  V1 = 0,
  V7_8 = 1,
  V3_4 = 2,
  V5_8 = 3,
  V1_2 = 4,
  V3_8 = 5,
  V1_4 = 6,
  V1_8 = 7,
  // 8-11 are invalid and give 0 (for alpha, 12-15 are invalid as well).
  K0 = 12,  // color stages only
  K1 = 13,  // color stages only
  K2 = 14,  // color stages only
  K3 = 15,  // color stages only
  K0_R = 16,
  K1_R = 17,
  K2_R = 18,
  K3_R = 19,
  K0_G = 20,
  K1_G = 21,
  K2_G = 22,
  K3_G = 23,
  K0_B = 24,
  K1_B = 25,
  K2_B = 26,
  K3_B = 27,
  K0_A = 28,
  K1_A = 29,
  K2_A = 30,
  K3_A = 31,
};
template <>
struct fmt::formatter<KonstSel> : EnumFormatter<KonstSel::K3_A>
{
  static constexpr array_type names = {
      "1",
      "7/8",
      "3/4",
      "5/8",
      "1/2",
      "3/8",
      "1/4",
      "1/8",
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      "Konst 0 RGB (invalid for alpha)",
      "Konst 1 RGB (invalid for alpha)",
      "Konst 2 RGB (invalid for alpha)",
      "Konst 3 RGB (invalid for alpha)",
      "Konst 0 Red",
      "Konst 1 Red",
      "Konst 2 Red",
      "Konst 3 Red",
      "Konst 0 Green",
      "Konst 1 Green",
      "Konst 2 Green",
      "Konst 3 Green",
      "Konst 0 Blue",
      "Konst 1 Blue",
      "Konst 2 Blue",
      "Konst 3 Blue",
      "Konst 0 Alpha",
      "Konst 1 Alpha",
      "Konst 2 Alpha",
      "Konst 3 Alpha",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Each KSEL register holds half of a swap table (red/green for even registers, blue/alpha for
// odd ones) and the konstant selections of two TEV stages.
union TevKSel
{
  BitField<0, 2, ColorChannel> swap_rb;  // red (even register) or blue (odd register)
  BitField<2, 2, ColorChannel> swap_ga;  // green (even register) or alpha (odd register)
  BitField<4, 5, KonstSel> kcsel_even;
  BitField<9, 5, KonstSel> kasel_even;
  BitField<14, 5, KonstSel> kcsel_odd;
  BitField<19, 5, KonstSel> kasel_odd;
  u32 hex;
};
// Formats a KSEL register; the u8 is its BP address.
template <>
struct fmt::formatter<std::pair<u8, TevKSel>> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const std::pair<u8, TevKSel>& p, FormatContext& ctx) const
  {
    const TevKSel& ksel = p.second;
    const int index = p.first - BPMEM_TEV_KSEL;
    const bool second_half = (index & 1) != 0;
    const u8 table = static_cast<u8>(index / 2);
    const u8 first_stage = static_cast<u8>(index * 2);
    const u8 second_stage = static_cast<u8>(first_stage + 1);

    return fmt::format_to(ctx.out(),
                          "Swap table {0}: {1} channel comes from input's {2} channel\n"
                          "Swap table {0}: {3} channel comes from input's {4} channel\n"
                          "TEV stage {5} konst color: {6}\nTEV stage {5} konst alpha: {7}\n"
                          "TEV stage {8} konst color: {9}\nTEV stage {8} konst alpha: {10}",
                          table, second_half ? "Blue" : "Red", ksel.swap_rb,
                          second_half ? "Alpha" : "Green", ksel.swap_ga, first_stage,
                          ksel.kcsel_even, ksel.kasel_even, second_stage, ksel.kcsel_odd,
                          ksel.kasel_odd);
  }
};

struct AllTevKSels
{
  std::array<TevKSel, 8> ksel;

  KonstSel GetKonstColor(u32 tev_stage) const
  {
    const TevKSel& reg = ksel[tev_stage / 2];
    return (tev_stage % 2 != 0) ? reg.kcsel_odd.Value() : reg.kcsel_even.Value();
  }
  KonstSel GetKonstAlpha(u32 tev_stage) const
  {
    const TevKSel& reg = ksel[tev_stage / 2];
    return (tev_stage % 2 != 0) ? reg.kasel_odd.Value() : reg.kasel_even.Value();
  }
  // Source channel for each output channel (red, green, blue, alpha) of a swap table.
  Common::EnumMap<ColorChannel, ColorChannel::Alpha> GetSwapTable(u32 swap_table_id) const
  {
    const TevKSel& red_green = ksel[swap_table_id * 2];
    const TevKSel& blue_alpha = ksel[swap_table_id * 2 + 1];
    return {red_green.swap_rb.Value(), red_green.swap_ga.Value(), blue_alpha.swap_rb.Value(),
            blue_alpha.swap_ga.Value()};
  }
};

// ---------------------------------------------------------------------------------------------
// Alpha test

enum class AlphaTestOp : u32
{
  And = 0,
  Or = 1,
  Xor = 2,
  Xnor = 3
};
template <>
struct fmt::formatter<AlphaTestOp> : EnumFormatter<AlphaTestOp::Xnor>
{
  constexpr formatter() : EnumFormatter({"And", "Or", "Xor", "Xnor"}) {}
};

enum class AlphaTestResult
{
  Undetermined = 0,
  Fail = 1,
  Pass = 2,
};

union AlphaTest
{
  BitField<0, 8, u32> ref0;
  BitField<8, 8, u32> ref1;
  BitField<16, 3, CompareMode> comp0;
  BitField<19, 3, CompareMode> comp1;
  BitField<22, 2, AlphaTestOp> logic;

  u32 hex;

  // Whether the test outcome is fixed regardless of the fragment's alpha; only Always and Never
  // comparisons have a known result.
  DOLPHIN_FORCE_INLINE AlphaTestResult TestResult() const
  {
    const bool always0 = comp0 == CompareMode::Always;
    const bool never0 = comp0 == CompareMode::Never;
    const bool always1 = comp1 == CompareMode::Always;
    const bool never1 = comp1 == CompareMode::Never;

    switch (logic)
    {
    case AlphaTestOp::And:
      if (always0 && always1)
        return AlphaTestResult::Pass;
      if (never0 || never1)
        return AlphaTestResult::Fail;
      return AlphaTestResult::Undetermined;

    case AlphaTestOp::Or:
      if (always0 || always1)
        return AlphaTestResult::Pass;
      if (never0 && never1)
        return AlphaTestResult::Fail;
      return AlphaTestResult::Undetermined;

    case AlphaTestOp::Xor:
    case AlphaTestOp::Xnor:
    {
      // Both sides must be constant for the combination to be.
      if (!(always0 || never0) || !(always1 || never1))
        return AlphaTestResult::Undetermined;
      const bool sides_differ = always0 != always1;
      const bool passes = (logic == AlphaTestOp::Xor) == sides_differ;
      return passes ? AlphaTestResult::Pass : AlphaTestResult::Fail;
    }
    }
    return AlphaTestResult::Undetermined;
  }
};
template <>
struct fmt::formatter<AlphaTest> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const AlphaTest& test, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(),
                          "Test 1: {} (ref: 0x{:02x})\nTest 2: {} (ref: 0x{:02x})\nLogic: {}\n",
                          test.comp0, test.ref0, test.comp1, test.ref1, test.logic);
  }
};

// ---------------------------------------------------------------------------------------------
// EFB copies

enum class FrameToField : u32
{
  Progressive = 0,
  InterlacedEven = 2,
  InterlacedOdd = 3,
};
template <>
struct fmt::formatter<FrameToField> : EnumFormatter<FrameToField::InterlacedOdd>
{
  static constexpr array_type names = {"Progressive", nullptr, "Interlaced (even lines)",
                                       "Interlaced (odd lines)"};
  constexpr formatter() : EnumFormatter(names) {}
};

enum class GammaCorrection : u32
{
  Gamma1_0 = 0,
  Gamma1_7 = 1,
  Gamma2_2 = 2,
  // Not a documented setting; hardware treats it like 2.2.
  Invalid2_2 = 3,
};
template <>
struct fmt::formatter<GammaCorrection> : EnumFormatter<GammaCorrection::Invalid2_2>
{
  constexpr formatter() : EnumFormatter({"1.0", "1.7", "2.2", "Invalid 2.2"}) {}
};

// The EFB copy trigger register.
union UPE_Copy
{
  u32 Hex;

  BitField<0, 1, bool, u32> clamp_top;
  BitField<1, 1, bool, u32> clamp_bottom;
  BitField<2, 1, u32> unknown_bit;
  // Stored rotated: the lowest bit is the format's top bit. See tp_realFormat.
  BitField<3, 4, u32> target_pixel_format;
  BitField<7, 2, GammaCorrection> gamma;
  // Box-filters 2x2 blocks into one pixel (a mipmap-style downscale) when set.
  BitField<9, 1, bool, u32> half_scale;
  BitField<10, 1, bool, u32> scale_invert;  // enables vertical scaling
  BitField<11, 1, bool, u32> clear;
  BitField<12, 2, FrameToField> frame_to_field;
  BitField<14, 1, bool, u32> copy_to_xfb;
  BitField<15, 1, bool, u32> intensity_fmt;  // I4, I8, IA4 or IA8 output
  // When clear, colors are converted automatically from the EFB format to the texture format.
  BitField<16, 1, bool, u32> auto_conv;

  EFBCopyFormat tp_realFormat() const
  {
    // Undo the rotation: move the low bit up to bit 3.
    const u32 stored = target_pixel_format;
    return static_cast<EFBCopyFormat>((stored >> 1) | ((stored & 1) << 3));
  }
};
template <>
struct fmt::formatter<UPE_Copy> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const UPE_Copy& copy, FormatContext& ctx) const
  {
    using BPMemoryFormat::YesNo;
    // Indexed by (top << 1) | bottom.
    static constexpr std::array<std::string_view, 4> clamp_names = {"None", "Bottom only",
                                                                    "Top only", "Top and Bottom"};
    const size_t clamp_index = (copy.clamp_top ? 2u : 0u) + (copy.clamp_bottom ? 1u : 0u);

    return fmt::format_to(ctx.out(),
                          "Clamping: {}\nUnknown bit: {}\nTarget pixel format: {}\n"
                          "Gamma correction: {}\nHalf scale: {}\nVertical scaling: {}\n"
                          "Clear: {}\nFrame to field: {}\nCopy to XFB: {}\n"
                          "Intensity format: {}\nAutomatic color conversion: {}",
                          clamp_names[clamp_index], copy.unknown_bit, copy.tp_realFormat(),
                          copy.gamma, YesNo(copy.half_scale), YesNo(copy.scale_invert),
                          YesNo(copy.clear), copy.frame_to_field, YesNo(copy.copy_to_xfb),
                          YesNo(copy.intensity_fmt), YesNo(copy.auto_conv));
  }
};

// Seven 6-bit vertical filter weights, spread over two BP registers (four in the first, three
// in the second).
union CopyFilterCoefficients
{
  using Values = std::array<u8, 7>;

  u64 Hex;

  BitField<0, 32, u32, u64> Low;
  BitField<0, 6, u64> w0;
  BitField<6, 6, u64> w1;
  BitField<12, 6, u64> w2;
  BitField<18, 6, u64> w3;
  BitField<32, 32, u32, u64> High;
  BitField<32, 6, u64> w4;
  BitField<38, 6, u64> w5;
  BitField<44, 6, u64> w6;

  Values GetCoefficients() const
  {
    Values weights{};
    for (size_t i = 0; i < weights.size(); ++i)
    {
      const u64 shift = i < 4 ? 6 * i : 32 + 6 * (i - 4);
      weights[i] = static_cast<u8>((Hex >> shift) & 0x3f);
    }
    return weights;
  }
};

// ---------------------------------------------------------------------------------------------
// TMEM loads

union BPU_PreloadTileInfo
{
  BitField<0, 15, u32> count;  // in 32-byte lines
  BitField<15, 2, u32> type;
  u32 hex;
};
template <>
struct fmt::formatter<BPU_PreloadTileInfo> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const BPU_PreloadTileInfo& info, FormatContext& ctx) const
  {
    const u32 count = info.count;
    const u32 type = info.type;
    // The SDK's GX_TexModeSync writes this register with everything zero.
    if (count == 0 && type == 0)
      return fmt::format_to(ctx.out(), "GX_TexModeSync (type and count are both 0)");
    return fmt::format_to(ctx.out(), "Type: {}\nCount: 0x{:x} lines (0x{:x} bytes)", type, count,
                          count * 32);
  }
};

union BPU_LoadTlutInfo
{
  BitField<0, 10, u32> tmem_addr;         // in 512-byte units
  BitField<10, 11, u32> tmem_line_count;  // in 32-byte lines
  u32 hex;
};
template <>
struct fmt::formatter<BPU_LoadTlutInfo> : BPMemoryFormat::PlainSpec
{
  template <typename FormatContext>
  auto format(const BPU_LoadTlutInfo& info, FormatContext& ctx) const
  {
    const u32 address = info.tmem_addr << 9;
    const u32 lines = info.tmem_line_count;
    return fmt::format_to(ctx.out(), "Tmem address: 0x{:05x}\nCount: 0x{:x} lines (0x{:x} bytes)",
                          address, lines, lines * 32);
  }
};

// Registers 0x60-0x66.
struct BPS_TmemConfig
{
  u32 preload_addr;
  u32 preload_tmem_even;
  u32 preload_tmem_odd;
  BPU_PreloadTileInfo preload_tile_info;
  u32 tlut_src;
  BPU_LoadTlutInfo tlut_dest;
  u32 texinvalidate;
};

union AllTexUnits;

// Location of one texture-unit register within 0x80-0xbf. Bits 0-1 are the unit number's low
// bits, bits 2-4 the register kind and bit 5 the unit number's high bit.
union TexUnitAddress
{
  enum class Register : u32
  {
    SETMODE0 = 0,
    SETMODE1 = 1,
    SETIMAGE0 = 2,
    SETIMAGE1 = 3,
    SETIMAGE2 = 4,
    SETIMAGE3 = 5,
    SETTLUT = 6,
    UNKNOWN = 7,
  };

  BitField<0, 2, u32> UnitIdLow;
  BitField<2, 3, Register> Reg;
  BitField<5, 1, u32> UnitIdHigh;

  BitField<0, 6, u32> FullAddress;
  u32 hex;

  TexUnitAddress() : hex(0) {}
  TexUnitAddress(u32 unit_id, Register reg = Register::SETMODE0)
      : hex((unit_id & 3) | ((static_cast<u32>(reg) & 7) << 2) | (((unit_id >> 2) & 1) << 5))
  {
  }

  static TexUnitAddress FromBPAddress(u32 Address)
  {
    // Only the low six bits vary; the top bits are always 0x80 for these registers.
    TexUnitAddress result;
    result.hex = Address & 0x3f;
    return result;
  }

  u32 GetUnitID() const { return UnitIdHigh * 4 + UnitIdLow; }

private:
  friend AllTexUnits;

  size_t GetOffset() const { return FullAddress; }
  size_t GetBPAddress() const { return size_t{FullAddress} + 0x80; }

  // Register offset of a unit's SETMODE0, usable in constant expressions.
  static constexpr size_t ComputeOffset(u32 unit_id)
  {
    return size_t{unit_id & 3} + size_t{(unit_id >> 2) & 1} * 32;
  }
};
static_assert(sizeof(TexUnitAddress) == sizeof(u32));

// The registers of one texture unit as they appear in BP memory: each kind is four registers
// apart from the next (the gaps belong to the neighbouring units).
struct TexUnit
{
  TexMode0 texMode0;
  u32 : 32;  // unnamed padding, one line per word since MSVC rejects u32 : 96
  u32 : 32;
  u32 : 32;
  TexMode1 texMode1;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage0 texImage0;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage1 texImage1;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage2 texImage2;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexImage3 texImage3;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  TexTLUT texTlut;
  u32 : 32;
  u32 : 32;
  u32 : 32;
  u32 unknown;
};
static_assert(sizeof(TexUnit) == sizeof(u32) * 4 * 7 + sizeof(u32));

// Registers 0x80-0xbf, viewable per texture unit.
union AllTexUnits
{
  std::array<u32, 8 * 8> AllRegisters;

  const TexUnit& GetUnit(u32 UnitId) const
  {
    const size_t first = TexUnitAddress(UnitId).GetOffset();
    return *reinterpret_cast<const TexUnit*>(&AllRegisters[first]);
  }

private:
  // Named views of every unit, so they are visible in a debugger even when GetUnit is inlined.
  template <u32 unit_id>
  struct PlacedUnit
  {
    static_assert(unit_id != 0, "unit 0 needs no leading padding (and arrays cannot be empty)");
    std::array<u32, TexUnitAddress::ComputeOffset(unit_id)> leading;
    TexUnit unit;
  };

  TexUnit unit0;
  PlacedUnit<1> unit1;
  PlacedUnit<2> unit2;
  PlacedUnit<3> unit3;
  PlacedUnit<4> unit4;
  PlacedUnit<5> unit5;
  PlacedUnit<6> unit6;
  PlacedUnit<7> unit7;
};
static_assert(sizeof(AllTexUnits) == 8 * 8 * sizeof(u32));

// ---------------------------------------------------------------------------------------------
// The whole register file

// A BP write being applied: its address, the bits that changed and the new value.
struct BPCmd
{
  int address;
  int changes;
  int newvalue;
};

enum class EmulatedZ : u32
{
  Disabled = 0,
  Early = 1,
  Late = 2,
  ForcedEarly = 3,
  EarlyWithFBFetch = 4,
  EarlyWithZComplocHack = 5,
};

// Every member sits at its BP address (one u32 per register), noted on the right.
struct BPMemory
{
  GenMode genMode;             // 0x00
  u32 display_copy_filter[4];  // 0x01-0x04
  u32 unknown;                 // 0x05
  // Three 2x3 indirect matrices (GXSetIndTexMtx), each with a 5-bit scale exponent spread over
  // its columns; the effective scale is 2^(exponent - 17). TevStageIndirect::matrix_index picks
  // one, applied as a plain, S-type or T-type matrix.
  IND_MTX indmtx[3];                  // 0x06-0x0e
  IND_IMASK imask;                    // 0x0f
  TevStageIndirect tevind[16];        // 0x10-0x1f: GXSetTevIndirect
  ScissorPos scissorTL;               // 0x20
  ScissorPos scissorBR;               // 0x21
  LPSize lineptwidth;                 // 0x22
  u32 sucounter;                      // 0x23
  u32 rascounter;                     // 0x24
  TEXSCALE texscale[2];               // 0x25-0x26: GXSetIndTexCoordScale
  RAS1_IREF tevindref;                // 0x27: GXSetIndTexOrder
  TwoTevStageOrders tevorders[8];     // 0x28-0x2f
  TCoordInfo texcoords[8];            // 0x30-0x3f: S and T for each coordinate
  ZMode zmode;                        // 0x40
  BlendMode blendmode;                // 0x41
  ConstantAlpha dstalpha;             // 0x42
  PEControl zcontrol;                 // 0x43: GXSetZCompLoc, GXPixModeSync
  FieldMask fieldmask;                // 0x44
  u32 drawdone;                       // 0x45: bit 1 marks the end of a display list
  u32 unknown5;                       // 0x46: bus clock
  u32 petoken;                        // 0x47
  u32 petokenint;                     // 0x48
  X10Y10 copyTexSrcXY;                // 0x49
  X10Y10 copyTexSrcWH;                // 0x4a
  u32 copyTexDest;                    // 0x4b: destination of display and texture copies
  u32 unknown6;                       // 0x4c
  u32 copyDestStride;                 // 0x4d
  u32 dispcopyyscale;                 // 0x4e
  u32 clearcolorAR;                   // 0x4f
  u32 clearcolorGB;                   // 0x50
  u32 clearZValue;                    // 0x51
  UPE_Copy triggerEFBCopy;            // 0x52
  CopyFilterCoefficients copyfilter;  // 0x53-0x54
  u32 boundbox0;                      // 0x55
  u32 boundbox1;                      // 0x56
  u32 unknown7[2];                    // 0x57-0x58
  ScissorOffset scissorOffset;        // 0x59
  u32 unknown8[6];                    // 0x5a-0x5f
  BPS_TmemConfig tmem_config;         // 0x60-0x66
  u32 metric;                         // 0x67
  FieldMode fieldmode;                // 0x68
  u32 unknown10[7];                   // 0x69-0x6f
  u32 unknown11[16];                  // 0x70-0x7f
  AllTexUnits tex;                    // 0x80-0xbf
  TevStageCombiner combiners[16];     // 0xc0-0xdf
  TevReg tevregs[4];                  // 0xe0-0xe7
  FogRangeParams fogRange;            // 0xe8-0xed
  FogParams fog;                      // 0xee-0xf2
  AlphaTest alpha_test;               // 0xf3
  ZTex1 ztex1;                        // 0xf4
  ZTex2 ztex2;                        // 0xf5
  AllTevKSels tevksel;                // 0xf6-0xfd
  u32 bpMask;                         // 0xfe
  u32 unknown18;                      // 0xff

  // The depth test placement the registers ask for (backends may refine it).
  EmulatedZ GetEmulatedZ() const
  {
    if (!zmode.test_enable)
      return EmulatedZ::Disabled;
    return zcontrol.early_ztest ? EmulatedZ::Early : EmulatedZ::Late;
  }
};

#pragma pack()

extern BPMemory bpmem;

void LoadBPReg(u8 reg, u32 value, int cycles_into_future);
void LoadBPRegPreprocess(u8 reg, u32 value, int cycles_into_future);

std::pair<std::string, std::string> GetBPRegInfo(u8 cmd, u32 cmddata);
