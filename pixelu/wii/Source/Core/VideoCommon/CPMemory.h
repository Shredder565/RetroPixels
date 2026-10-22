// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>
#include <type_traits>
#include <utility>

#include "Common/BitField.h"
#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/EnumMap.h"
#include "Common/MsgHandler.h"

// Command processor (CP) registers: vertex descriptors, vertex attribute tables, array pointers
// and matrix indices, as loaded by GX_LOAD_CP_REG.

// Register numbers. The high nybble selects the register; the low nybble is a sub-index where one
// exists (VAT number or array number). For the single registers any low nybble is accepted, but
// anything other than 0 is reported.
enum
{
  // Probably performance-counter controls; the SDK writes 0x20 = 0 at start-up. None of them is
  // emulated.
  UNKNOWN_00 = 0x00,
  UNKNOWN_10 = 0x10,
  UNKNOWN_20 = 0x20,
  MATINDEX_A = 0x30,
  MATINDEX_B = 0x40,
  VCD_LO = 0x50,
  VCD_HI = 0x60,
  // One per vertex attribute table: 0x70-0x77, 0x80-0x87, 0x90-0x97.
  CP_VAT_REG_A = 0x70,
  CP_VAT_REG_B = 0x80,
  CP_VAT_REG_C = 0x90,
  // One per array: 0xa0-0xaf and 0xb0-0xbf.
  ARRAY_BASE = 0xa0,
  ARRAY_STRIDE = 0xb0,

  CP_COMMAND_MASK = 0xf0,
  CP_NUM_VAT_REG = 0x08,
  CP_VAT_MASK = 0x07,
  CP_NUM_ARRAYS = 0x10,
  CP_ARRAY_MASK = 0x0f,
};

// Indexable arrays. The first twelve hold vertex attributes; XF_A-XF_D are sources for indexed
// XF loads (conventionally position matrices, normal matrices, texture matrices and lights).
enum class CPArray : u8
{
  Position = 0,
  Normal = 1,

  Color0 = 2,
  Color1 = 3,

  TexCoord0 = 4,
  TexCoord1 = 5,
  TexCoord2 = 6,
  TexCoord3 = 7,
  TexCoord4 = 8,
  TexCoord5 = 9,
  TexCoord6 = 10,
  TexCoord7 = 11,

  XF_A = 12,
  XF_B = 13,
  XF_C = 14,
  XF_D = 15,
};
template <>
struct fmt::formatter<CPArray> : EnumFormatter<CPArray::XF_D>
{
  static constexpr array_type names = {
      "Position",    "Normal",      "Color 0",     "Color 1",     "Tex Coord 0", "Tex Coord 1",
      "Tex Coord 2", "Tex Coord 3", "Tex Coord 4", "Tex Coord 5", "Tex Coord 6", "Tex Coord 7",
      "XF A",        "XF B",        "XF C",        "XF D"};
  constexpr formatter() : EnumFormatter(names) {}
};
// Steps from Color0 or TexCoord0 to the n-th colour or texture coordinate array.
constexpr CPArray operator+(CPArray array, u8 offset)
{
  return static_cast<CPArray>(static_cast<u8>(array) + offset);
}

// Arrays Position..TexCoord7 (the ones a vertex can index).
constexpr u8 NUM_VERTEX_COMPONENT_ARRAYS = 12;

// How a vertex attribute is supplied.
enum class VertexComponentFormat
{
  NotPresent = 0,
  Direct = 1,
  Index8 = 2,
  Index16 = 3,
};
template <>
struct fmt::formatter<VertexComponentFormat> : EnumFormatter<VertexComponentFormat::Index16>
{
  constexpr formatter() : EnumFormatter({"Not present", "Direct", "8-bit index", "16-bit index"}) {}
};

constexpr bool IsIndexed(VertexComponentFormat format)
{
  return format == VertexComponentFormat::Index8 || format == VertexComponentFormat::Index16;
}

// Element type of a coordinate-like attribute.
enum class ComponentFormat
{
  UByte = 0,  // not allowed for normals
  Byte = 1,
  UShort = 2,  // not allowed for normals
  Short = 3,
  Float = 4,
  // 5 is set by a few commercial games and treated as float; 6 and 7 have not been seen.
  InvalidFloat5 = 5,
  InvalidFloat6 = 6,
  InvalidFloat7 = 7,
};
// The invalid values are deliberately left unnamed so they are printed as invalid.
template <>
struct fmt::formatter<ComponentFormat> : EnumFormatter<ComponentFormat::Float>
{
  static constexpr array_type names = {"Unsigned Byte", "Byte", "Unsigned Short", "Short", "Float"};
  constexpr formatter() : EnumFormatter(names) {}
};

// Bytes per element.
constexpr u32 GetElementSize(ComponentFormat format)
{
  switch (format)
  {
  case ComponentFormat::UByte:
  case ComponentFormat::Byte:
    return 1;
  case ComponentFormat::UShort:
  case ComponentFormat::Short:
    return 2;
  case ComponentFormat::Float:
  case ComponentFormat::InvalidFloat5:
  case ComponentFormat::InvalidFloat6:
  case ComponentFormat::InvalidFloat7:
    return 4;
  }
  PanicAlertFmt("Unknown format {}", format);
  return 0;
}

enum class CoordComponentCount
{
  XY = 0,
  XYZ = 1,
};
template <>
struct fmt::formatter<CoordComponentCount> : EnumFormatter<CoordComponentCount::XYZ>
{
  constexpr formatter() : EnumFormatter({"2 (x, y)", "3 (x, y, z)"}) {}
};

enum class NormalComponentCount
{
  N = 0,
  NTB = 1,
};
template <>
struct fmt::formatter<NormalComponentCount> : EnumFormatter<NormalComponentCount::NTB>
{
  constexpr formatter() : EnumFormatter({"1 (normal)", "3 (normal, tangent, binormal)"}) {}
};

enum class ColorComponentCount
{
  RGB = 0,
  RGBA = 1,
};
template <>
struct fmt::formatter<ColorComponentCount> : EnumFormatter<ColorComponentCount::RGBA>
{
  constexpr formatter() : EnumFormatter({"3 (r, g, b)", "4 (r, g, b, a)"}) {}
};

// Vertex colour encodings (bits per colour in the names).
enum class ColorFormat
{
  RGB565 = 0,
  RGB888 = 1,
  RGB888x = 2,
  RGBA4444 = 3,
  RGBA6666 = 4,
  RGBA8888 = 5,
};
template <>
struct fmt::formatter<ColorFormat> : EnumFormatter<ColorFormat::RGBA8888>
{
  static constexpr array_type names = {
      "RGB 16 bits 565",   "RGB 24 bits 888",   "RGB 32 bits 888x",
      "RGBA 16 bits 4444", "RGBA 24 bits 6666", "RGBA 32 bits 8888",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

enum class TexComponentCount
{
  S = 0,
  ST = 1,
};
template <>
struct fmt::formatter<TexComponentCount> : EnumFormatter<TexComponentCount::ST>
{
  constexpr formatter() : EnumFormatter({"1 (s)", "2 (s, t)"}) {}
};

// Vertex descriptor: which attributes a vertex has and how each is supplied.
struct TVtxDesc
{
  union Low
  {
    // Matrix index attributes: one byte each when set.
    BitField<0, 1, bool, u32> PosMatIdx;
    BitField<1, 1, bool, u32> Tex0MatIdx;
    BitField<2, 1, bool, u32> Tex1MatIdx;
    BitField<3, 1, bool, u32> Tex2MatIdx;
    BitField<4, 1, bool, u32> Tex3MatIdx;
    BitField<5, 1, bool, u32> Tex4MatIdx;
    BitField<6, 1, bool, u32> Tex5MatIdx;
    BitField<7, 1, bool, u32> Tex6MatIdx;
    BitField<8, 1, bool, u32> Tex7MatIdx;
    BitFieldArray<1, 1, 8, bool, u32> TexMatIdx;

    BitField<9, 2, VertexComponentFormat> Position;
    BitField<11, 2, VertexComponentFormat> Normal;
    BitField<13, 2, VertexComponentFormat> Color0;
    BitField<15, 2, VertexComponentFormat> Color1;
    BitFieldArray<13, 2, 2, VertexComponentFormat> Color;

    u32 Hex = 0;
  };
  union High
  {
    BitField<0, 2, VertexComponentFormat> Tex0Coord;
    BitField<2, 2, VertexComponentFormat> Tex1Coord;
    BitField<4, 2, VertexComponentFormat> Tex2Coord;
    BitField<6, 2, VertexComponentFormat> Tex3Coord;
    BitField<8, 2, VertexComponentFormat> Tex4Coord;
    BitField<10, 2, VertexComponentFormat> Tex5Coord;
    BitField<12, 2, VertexComponentFormat> Tex6Coord;
    BitField<14, 2, VertexComponentFormat> Tex7Coord;
    BitFieldArray<0, 2, 8, VertexComponentFormat> TexCoord;

    u32 Hex = 0;
  };

  Low low;
  High high;
};
template <>
struct fmt::formatter<TVtxDesc::Low>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TVtxDesc::Low& desc, FormatContext& ctx) const
  {
    const auto presence = [](bool present) { return present ? "Present" : "Not present"; };
    auto out =
        fmt::format_to(ctx.out(), "Position and normal matrix index: {}\n", presence(desc.PosMatIdx));
    for (u32 i = 0; i < 8; ++i)
      out = fmt::format_to(out, "Texture Coord {} matrix index: {}\n", i, presence(desc.TexMatIdx[i]));
    return fmt::format_to(out, "Position: {}\nNormal: {}\nColor 0: {}\nColor 1: {}", desc.Position,
                          desc.Normal, desc.Color0, desc.Color1);
  }
};
template <>
struct fmt::formatter<TVtxDesc::High>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TVtxDesc::High& desc, FormatContext& ctx) const
  {
    auto out = ctx.out();
    for (u32 i = 0; i < 8; ++i)
    {
      const VertexComponentFormat coord = desc.TexCoord[i];
      out = fmt::format_to(out, "{}Texture Coord {}: {}", i == 0 ? "" : "\n", i, coord);
    }
    return out;
  }
};
template <>
struct fmt::formatter<TVtxDesc>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TVtxDesc& desc, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{}\n{}", desc.low, desc.high);
  }
};

// Scale applied by a fixed-point shift ("frac") field, for display.
constexpr float CPFracScale(u32 frac)
{
  return 1.f / static_cast<float>(1 << frac);
}

// Vertex attribute table, word 0 (registers 0x70-0x77).
union UVAT_group0
{
  u32 Hex = 0;
  BitField<0, 1, CoordComponentCount> PosElements;
  BitField<1, 3, ComponentFormat> PosFormat;
  BitField<4, 5, u32> PosFrac;
  BitField<9, 1, NormalComponentCount> NormalElements;
  BitField<10, 3, ComponentFormat> NormalFormat;
  BitField<13, 1, ColorComponentCount> Color0Elements;
  BitField<14, 3, ColorFormat> Color0Comp;
  BitField<17, 1, ColorComponentCount> Color1Elements;
  BitField<18, 3, ColorFormat> Color1Comp;
  BitField<21, 1, TexComponentCount> Tex0CoordElements;
  BitField<22, 3, ComponentFormat> Tex0CoordFormat;
  BitField<25, 5, u8, u32> Tex0Frac;
  BitField<30, 1, bool, u32> ByteDequant;
  BitField<31, 1, bool, u32> NormalIndex3;
};
template <>
struct fmt::formatter<UVAT_group0>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const UVAT_group0& g, FormatContext& ctx) const
  {
    const char* const dequant = g.ByteDequant ? "shift applies to u8/s8 components" :
                                                "shift does not apply to u8/s8 components";
    const char* const indices =
        g.NormalIndex3 ? "three indices, one each for normal, tangent, and binormal" :
                         "single index shared by normal, tangent, and binormal";
    auto out = fmt::format_to(ctx.out(),
                              "Position elements: {}\nPosition format: {}\nPosition shift: {} ({})\n",
                              g.PosElements, g.PosFormat, g.PosFrac, CPFracScale(g.PosFrac));
    out = fmt::format_to(out, "Normal elements: {}\nNormal format: {}\n", g.NormalElements,
                         g.NormalFormat);
    out = fmt::format_to(out, "Color 0 elements: {}\nColor 0 format: {}\n", g.Color0Elements,
                         g.Color0Comp);
    out = fmt::format_to(out, "Color 1 elements: {}\nColor 1 format: {}\n", g.Color1Elements,
                         g.Color1Comp);
    out = fmt::format_to(out,
                         "Texture coord 0 elements: {}\nTexture coord 0 format: {}\n"
                         "Texture coord 0 shift: {} ({})\n",
                         g.Tex0CoordElements, g.Tex0CoordFormat, g.Tex0Frac,
                         CPFracScale(g.Tex0Frac));
    return fmt::format_to(out, "Byte dequant: {}\nNormal index 3: {}", dequant, indices);
  }
};

// Vertex attribute table, word 1 (registers 0x80-0x87).
union UVAT_group1
{
  u32 Hex = 0;
  BitField<0, 1, TexComponentCount> Tex1CoordElements;
  BitField<1, 3, ComponentFormat> Tex1CoordFormat;
  BitField<4, 5, u8, u32> Tex1Frac;
  BitField<9, 1, TexComponentCount> Tex2CoordElements;
  BitField<10, 3, ComponentFormat> Tex2CoordFormat;
  BitField<13, 5, u8, u32> Tex2Frac;
  BitField<18, 1, TexComponentCount> Tex3CoordElements;
  BitField<19, 3, ComponentFormat> Tex3CoordFormat;
  BitField<22, 5, u8, u32> Tex3Frac;
  BitField<27, 1, TexComponentCount> Tex4CoordElements;
  BitField<28, 3, ComponentFormat> Tex4CoordFormat;
  BitField<31, 1, bool, u32> VCacheEnhance;
};

// Vertex attribute table, word 2 (registers 0x90-0x97).
union UVAT_group2
{
  u32 Hex = 0;
  BitField<0, 5, u8, u32> Tex4Frac;
  BitField<5, 1, TexComponentCount> Tex5CoordElements;
  BitField<6, 3, ComponentFormat> Tex5CoordFormat;
  BitField<9, 5, u8, u32> Tex5Frac;
  BitField<14, 1, TexComponentCount> Tex6CoordElements;
  BitField<15, 3, ComponentFormat> Tex6CoordFormat;
  BitField<18, 5, u8, u32> Tex6Frac;
  BitField<23, 1, TexComponentCount> Tex7CoordElements;
  BitField<24, 3, ComponentFormat> Tex7CoordFormat;
  BitField<27, 5, u8, u32> Tex7Frac;
};

namespace CPMemoryDetail
{
// Writes the elements/format/shift lines for one texture coordinate.
template <typename Out>
Out FormatTexCoord(Out out, int n, TexComponentCount elements, ComponentFormat format, u8 frac)
{
  return fmt::format_to(out,
                        "Texture coord {0} elements: {1}\nTexture coord {0} format: {2}\n"
                        "Texture coord {0} shift: {3} ({4})",
                        n, elements, format, frac, CPFracScale(frac));
}
}  // namespace CPMemoryDetail

template <>
struct fmt::formatter<UVAT_group1>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const UVAT_group1& g, FormatContext& ctx) const
  {
    using CPMemoryDetail::FormatTexCoord;
    auto out = FormatTexCoord(ctx.out(), 1, g.Tex1CoordElements, g.Tex1CoordFormat, g.Tex1Frac);
    out = FormatTexCoord(fmt::format_to(out, "\n"), 2, g.Tex2CoordElements, g.Tex2CoordFormat,
                         g.Tex2Frac);
    out = FormatTexCoord(fmt::format_to(out, "\n"), 3, g.Tex3CoordElements, g.Tex3CoordFormat,
                         g.Tex3Frac);
    return fmt::format_to(out,
                          "\nTexture coord 4 elements: {}\nTexture coord 4 format: {}\n"
                          "Enhance VCache (must always be on): {}",
                          g.Tex4CoordElements, g.Tex4CoordFormat, g.VCacheEnhance ? "Yes" : "No");
  }
};
template <>
struct fmt::formatter<UVAT_group2>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const UVAT_group2& g, FormatContext& ctx) const
  {
    using CPMemoryDetail::FormatTexCoord;
    // Texture coordinate 4 is split: its shift lives in this word.
    auto out = fmt::format_to(ctx.out(), "Texture coord 4 shift: {} ({})\n", g.Tex4Frac,
                              CPFracScale(g.Tex4Frac));
    out = FormatTexCoord(out, 5, g.Tex5CoordElements, g.Tex5CoordFormat, g.Tex5Frac);
    out = FormatTexCoord(fmt::format_to(out, "\n"), 6, g.Tex6CoordElements, g.Tex6CoordFormat,
                         g.Tex6Frac);
    return FormatTexCoord(fmt::format_to(out, "\n"), 7, g.Tex7CoordElements, g.Tex7CoordFormat,
                          g.Tex7Frac);
  }
};

// One vertex attribute table: element counts, formats and fixed-point shifts per attribute.
struct VAT
{
  UVAT_group0 g0;
  UVAT_group1 g1;
  UVAT_group2 g2;

  constexpr ColorComponentCount GetColorElements(size_t idx) const
  {
    if (idx == 0)
      return g0.Color0Elements;
    if (idx == 1)
      return g0.Color1Elements;
    PanicAlertFmt("Invalid color index {}", idx);
    return ColorComponentCount::RGB;
  }
  constexpr ColorFormat GetColorFormat(size_t idx) const
  {
    if (idx == 0)
      return g0.Color0Comp;
    if (idx == 1)
      return g0.Color1Comp;
    PanicAlertFmt("Invalid color index {}", idx);
    return ColorFormat::RGB565;
  }

  constexpr TexComponentCount GetTexElements(size_t idx) const
  {
    TexComponentCount result = TexComponentCount::S;
    VisitTexCoord(*this, idx, [&](const auto& elements, const auto&, const auto&) {
      result = elements;
    });
    return result;
  }
  constexpr ComponentFormat GetTexFormat(size_t idx) const
  {
    ComponentFormat result = ComponentFormat::UByte;
    VisitTexCoord(*this, idx, [&](const auto&, const auto& format, const auto&) {
      result = format;
    });
    return result;
  }
  constexpr u8 GetTexFrac(size_t idx) const
  {
    u8 result = 0;
    VisitTexCoord(*this, idx, [&](const auto&, const auto&, const auto& frac) { result = frac; });
    return result;
  }
  void SetTexElements(size_t idx, TexComponentCount value)
  {
    VisitTexCoord(*this, idx, [value](auto& elements, auto&, auto&) { elements = value; });
  }
  void SetTexFormat(size_t idx, ComponentFormat value)
  {
    VisitTexCoord(*this, idx, [value](auto&, auto& format, auto&) { format = value; });
  }
  void SetTexFrac(size_t idx, u8 value)
  {
    VisitTexCoord(*this, idx, [value](auto&, auto&, auto& frac) { frac = value; });
  }

private:
  // Calls fn(elements, format, frac) with the fields of texture coordinate `idx`; reports an
  // out-of-range index instead.
  template <typename Self, typename Fn>
  static constexpr void VisitTexCoord(Self& vat, size_t idx, Fn&& fn)
  {
    switch (idx)
    {
    case 0:
      fn(vat.g0.Tex0CoordElements, vat.g0.Tex0CoordFormat, vat.g0.Tex0Frac);
      return;
    case 1:
      fn(vat.g1.Tex1CoordElements, vat.g1.Tex1CoordFormat, vat.g1.Tex1Frac);
      return;
    case 2:
      fn(vat.g1.Tex2CoordElements, vat.g1.Tex2CoordFormat, vat.g1.Tex2Frac);
      return;
    case 3:
      fn(vat.g1.Tex3CoordElements, vat.g1.Tex3CoordFormat, vat.g1.Tex3Frac);
      return;
    case 4:
      fn(vat.g1.Tex4CoordElements, vat.g1.Tex4CoordFormat, vat.g2.Tex4Frac);
      return;
    case 5:
      fn(vat.g2.Tex5CoordElements, vat.g2.Tex5CoordFormat, vat.g2.Tex5Frac);
      return;
    case 6:
      fn(vat.g2.Tex6CoordElements, vat.g2.Tex6CoordFormat, vat.g2.Tex6Frac);
      return;
    case 7:
      fn(vat.g2.Tex7CoordElements, vat.g2.Tex7CoordFormat, vat.g2.Tex7Frac);
      return;
    default:
      PanicAlertFmt("Invalid tex coord index {}", idx);
      return;
    }
  }
};
template <>
struct fmt::formatter<VAT>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const VAT& vat, FormatContext& ctx) const
  {
    return fmt::format_to(ctx.out(), "{}\n{}\n{}", vat.g0, vat.g1, vat.g2);
  }
};

// Matrix indices used when a vertex carries no index of its own.
union TMatrixIndexA
{
  BitField<0, 6, u32> PosNormalMtxIdx;
  BitField<6, 6, u32> Tex0MtxIdx;
  BitField<12, 6, u32> Tex1MtxIdx;
  BitField<18, 6, u32> Tex2MtxIdx;
  BitField<24, 6, u32> Tex3MtxIdx;
  u32 Hex;
};
template <>
struct fmt::formatter<TMatrixIndexA>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TMatrixIndexA& m, FormatContext& ctx) const
  {
    auto out = fmt::format_to(ctx.out(), "PosNormal: {}", m.PosNormalMtxIdx);
    const u32 tex[] = {m.Tex0MtxIdx, m.Tex1MtxIdx, m.Tex2MtxIdx, m.Tex3MtxIdx};
    for (u32 i = 0; i < 4; ++i)
      out = fmt::format_to(out, "\nTex{}: {}", i, tex[i]);
    return out;
  }
};

union TMatrixIndexB
{
  BitField<0, 6, u32> Tex4MtxIdx;
  BitField<6, 6, u32> Tex5MtxIdx;
  BitField<12, 6, u32> Tex6MtxIdx;
  BitField<18, 6, u32> Tex7MtxIdx;
  u32 Hex;
};
template <>
struct fmt::formatter<TMatrixIndexB>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TMatrixIndexB& m, FormatContext& ctx) const
  {
    const u32 tex[] = {m.Tex4MtxIdx, m.Tex5MtxIdx, m.Tex6MtxIdx, m.Tex7MtxIdx};
    auto out = ctx.out();
    for (u32 i = 0; i < 4; ++i)
      out = fmt::format_to(out, "{}Tex{}: {}", i == 0 ? "" : "\n", i + 4, tex[i]);
    return out;
  }
};

class VertexLoaderBase;

// The decoded CP register file. Saved in savestates as raw bytes, so the layout matters.
struct CPState final
{
  CPState() = default;
  // Builds the state from a 0x100-word register image (as produced by FillCPMemoryArray).
  explicit CPState(const u32* memory);

  // Applies a CP register write.
  void LoadCPReg(u8 sub_cmd, u32 value);
  // Writes the registers into a 0x100-word image indexed by register number.
  void FillCPMemoryArray(u32* memory) const;

  Common::EnumMap<u32, CPArray::XF_D> array_bases;
  Common::EnumMap<u32, CPArray::XF_D> array_strides;
  TMatrixIndexA matrix_index_a{};
  TMatrixIndexB matrix_index_b{};
  TVtxDesc vtx_desc;
  // Usually only table 0 is used, rewritten whenever the format changes.
  std::array<VAT, CP_NUM_VAT_REG> vtx_attr{};
};
static_assert(std::is_trivially_copyable_v<CPState>);

class PointerWrap;

// The main state is what the GPU side executes with; the preprocess copy is advanced ahead of it
// on the CPU thread in deterministic dual-core mode.
extern CPState g_main_cp_state;
extern CPState g_preprocess_cp_state;

void CopyPreprocessCPStateFromMain();

// (name, description) of a CP register write, for the FIFO tools.
std::pair<std::string, std::string> GetCPRegInfo(u8 cmd, u32 value);
