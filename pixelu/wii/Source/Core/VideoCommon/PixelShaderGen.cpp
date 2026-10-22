// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PixelShaderGen.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
using StageKey = std::remove_reference_t<decltype(pixel_shader_uid_data::stagehash[0])>;

// Writes text without format fields (single braces are literal here).
void Put(ShaderCode& out, std::string_view text)
{
  out.Write("{}", text);
}

// ---------------------------------------------------------------------------------------------
// Names of TEV operands in the generated code.

// The six registers a TEV input can read, in the order of the colour/alpha argument encodings.
constexpr std::array<std::string_view, 6> TEV_REGISTERS = {"prev", "c0",      "c1",
                                                           "c2",   "textemp", "rastemp"};
// Destination registers.
constexpr std::array<std::string_view, 4> TEV_OUTPUTS = {"prev", "c0", "c1", "c2"};

std::string ColorInput(TevColorArg arg)
{
  const u32 index = static_cast<u32>(arg);
  if (index < 12)
    return fmt::format("{}.{}", TEV_REGISTERS[index / 2], (index & 1) != 0 ? "aaa" : "rgb");
  switch (arg)
  {
  case TevColorArg::One:
    return "int3(255,255,255)";
  case TevColorArg::Half:
    return "int3(128,128,128)";
  case TevColorArg::Konst:
    return "konsttemp.rgb";
  default:
    return "int3(0,0,0)";
  }
}

std::string AlphaInput(TevAlphaArg arg)
{
  const u32 index = static_cast<u32>(arg);
  if (index < 6)
    return fmt::format("{}.a", TEV_REGISTERS[index]);
  return arg == TevAlphaArg::Konst ? "konsttemp.a" : "0";
}

std::string ColorOutput(TevOutput dest)
{
  return fmt::format("{}.rgb", TEV_OUTPUTS[static_cast<u32>(dest)]);
}

std::string AlphaOutput(TevOutput dest)
{
  return fmt::format("{}.a", TEV_OUTPUTS[static_cast<u32>(dest)]);
}

// The fixed konstant fractions 1, 7/8, ... 1/8 as 8-bit values.
constexpr std::array<u32, 8> KONST_FRACTIONS = {255, 223, 191, 159, 128, 96, 64, 32};
constexpr std::string_view RGBA = "rgba";

// Konstant colour selection: fractions, four invalid zero entries, whole k registers and then
// single components of them replicated.
std::string KonstColor(KonstSel sel)
{
  const u32 index = static_cast<u32>(sel);
  if (index < 8)
    return fmt::format("{0},{0},{0}", KONST_FRACTIONS[index]);
  if (index < 12)
    return "0,0,0";
  if (index < 16)
    return fmt::format("{}[{}].rgb", I_KCOLORS, index - 12);
  const char c = RGBA[(index - 16) / 4];
  return fmt::format("{}[{}].{}{}{}", I_KCOLORS, index % 4, c, c, c);
}

// Konstant alpha selection; 8-15 are all invalid here.
std::string KonstAlpha(KonstSel sel)
{
  const u32 index = static_cast<u32>(sel);
  if (index < 8)
    return fmt::format("{}", KONST_FRACTIONS[index]);
  if (index < 16)
    return "0";
  return fmt::format("{}[{}].{}", I_KCOLORS, index % 4, RGBA[(index - 16) / 4]);
}

// The rasterized colour a stage reads.
std::string_view RasterColor(RasColorChan chan)
{
  switch (chan)
  {
  case RasColorChan::Color0:
    return "iround(col0 * 255.0)";
  case RasColorChan::Color1:
    return "iround(col1 * 255.0)";
  case RasColorChan::AlphaBump:
    return "(int4(1, 1, 1, 1) * alphabump)";
  case RasColorChan::NormalizedAlphaBump:
    // Bump alpha is 0..248; replicating the top bits stretches it to 0..255.
    return "(int4(1, 1, 1, 1) * (alphabump | (alphabump >> 5)))";
  case RasColorChan::Zero:
    return "int4(0, 0, 0, 0)";
  default:
    break;
  }
  // Encodings 2-4 are invalid and produce code that does not compile, as before.
  static constexpr std::array<std::string_view, 3> invalid = {"ERROR13", "ERROR14", "ERROR15"};
  return invalid[static_cast<u32>(chan) - 2];
}

std::string Swizzle(ColorChannel r, ColorChannel g, ColorChannel b, ColorChannel a)
{
  const auto pick = [](ColorChannel c) { return RGBA[static_cast<u32>(c)]; };
  return {pick(r), pick(g), pick(b), pick(a)};
}

bool IsRasArg(TevColorArg arg)
{
  return arg == TevColorArg::RasAlpha || arg == TevColorArg::RasColor;
}

// Whether any input of the stage reads the rasterized colour.
bool StageReadsRaster(const TevStageCombiner::ColorCombiner& cc,
                      const TevStageCombiner::AlphaCombiner& ac)
{
  return IsRasArg(cc.a) || IsRasArg(cc.b) || IsRasArg(cc.c) || IsRasArg(cc.d) ||
         ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
         ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha;
}

// Whether any input of the stage reads the konstant register.
bool StageReadsKonst(const TevStageCombiner::ColorCombiner& cc,
                     const TevStageCombiner::AlphaCombiner& ac)
{
  const auto c = [](TevColorArg arg) { return arg == TevColorArg::Konst; };
  const auto a = [](TevAlphaArg arg) { return arg == TevAlphaArg::Konst; };
  return c(cc.a) || c(cc.b) || c(cc.c) || c(cc.d) || a(ac.a) || a(ac.b) || a(ac.c) || a(ac.d);
}

// ---------------------------------------------------------------------------------------------
// Key construction helpers.

void FillStageKey(StageKey& key, u32 n)
{
  const TwoTevStageOrders& orders = bpmem.tevorders[n / 2];
  const int half = static_cast<int>(n & 1);
  const TevStageCombiner::ColorCombiner& cc = bpmem.combiners[n].colorC;
  const TevStageCombiner::AlphaCombiner& ac = bpmem.combiners[n].alphaC;

  key.tevorders_texcoord = orders.getTexCoord(half);
  key.tevind = bpmem.tevind[n].hex;
  key.cc = cc.hex & 0xFFFFFF;
  // The low four bits (rswap and tswap) are replaced by the decoded tables below.
  key.ac = ac.hex & 0xFFFFF0;

  if (StageReadsRaster(cc, ac))
  {
    const auto swap = bpmem.tevksel.GetSwapTable(ac.rswap);
    key.ras_swap_r = swap[ColorChannel::Red];
    key.ras_swap_g = swap[ColorChannel::Green];
    key.ras_swap_b = swap[ColorChannel::Blue];
    key.ras_swap_a = swap[ColorChannel::Alpha];
    key.tevorders_colorchan = orders.getColorChan(half);
  }

  key.tevorders_enable = orders.getEnable(half);
  if (key.tevorders_enable)
  {
    const auto swap = bpmem.tevksel.GetSwapTable(ac.tswap);
    key.tex_swap_r = swap[ColorChannel::Red];
    key.tex_swap_g = swap[ColorChannel::Green];
    key.tex_swap_b = swap[ColorChannel::Blue];
    key.tex_swap_a = swap[ColorChannel::Alpha];
    key.tevorders_texmap = orders.getTexMap(half);
  }

  if (StageReadsKonst(cc, ac))
  {
    key.tevksel_kc = bpmem.tevksel.GetKonstColor(n);
    key.tevksel_ka = bpmem.tevksel.GetKonstAlpha(n);
  }
}

// The alpha test has to run in the shader when its result is not known in advance, or when it
// always fails but the depth test happens late (depth must then still be written correctly).
bool NeedsAlphaTest(AlphaTestResult pretest, EmulatedZ ztest)
{
  return pretest == AlphaTestResult::Undetermined ||
         (pretest == AlphaTestResult::Fail && ztest == EmulatedZ::Late);
}

// ---------------------------------------------------------------------------------------------
// Common header pieces.

void WritePSBlock(ShaderCode& out)
{
  struct Member
  {
    std::string_view type;
    std::string_view name;
  };
  // The layout of PixelShaderConstants (ConstantManager.h).
  static constexpr std::array<Member, 36> members = {{
      {"int4", I_COLORS "[4]"},
      {"int4", I_KCOLORS "[4]"},
      {"int4", I_ALPHA},
      {"int4", I_TEXDIMS "[8]"},
      {"int4", I_ZBIAS "[2]"},
      {"int4", I_INDTEXSCALE "[2]"},
      {"int4", I_INDTEXMTX "[6]"},
      {"int4", I_FOGCOLOR},
      {"int4", I_FOGI},
      {"float4", I_FOGF},
      {"float4", I_FOGRANGE "[3]"},
      {"float4", I_ZSLOPE},
      {"float2", I_EFBSCALE},
      {"uint ", "bpmem_genmode"},
      {"uint ", "bpmem_alphaTest"},
      {"uint ", "bpmem_fogParam3"},
      {"uint ", "bpmem_fogRangeBase"},
      {"uint ", "bpmem_dstalpha"},
      {"uint ", "bpmem_ztex_op"},
      {"bool ", "bpmem_late_ztest"},
      {"bool ", "bpmem_rgba6_format"},
      {"bool ", "bpmem_dither"},
      {"bool ", "bpmem_bounding_box"},
      {"uint4", "bpmem_pack1[16]"},  // combiners (xy), tevind (z), iref (w)
      {"uint4", "bpmem_pack2[8]"},   // tevorder (x), tevksel (y), sampler tm0/tm1 (zw)
      {"int4 ", "konstLookup[32]"},
      {"bool ", "blend_enable"},
      {"uint ", "blend_src_factor"},
      {"uint ", "blend_src_factor_alpha"},
      {"uint ", "blend_dst_factor"},
      {"uint ", "blend_dst_factor_alpha"},
      {"bool ", "blend_subtract"},
      {"bool ", "blend_subtract_alpha"},
      {"bool ", "logic_op_enable"},
      {"uint ", "logic_op_mode"},
      {"uint ", "time_ms"},
  }};
  Put(out, "UBO_BINDING(std140, 1) uniform PSBlock {\n");
  for (const Member& m : members)
    out.Write("\t{} {};\n", m.type, m.name);
  Put(out, "};\n\n");

  struct Packed
  {
    std::string_view name;
    std::string_view array;
    std::string_view swizzle;
  };
  static constexpr std::array<Packed, 7> packed = {{
      {"bpmem_combiners", "bpmem_pack1", "xy"},
      {"bpmem_tevind", "bpmem_pack1", "z"},
      {"bpmem_iref", "bpmem_pack1", "w"},
      {"bpmem_tevorder", "bpmem_pack2", "x"},
      {"bpmem_tevksel", "bpmem_pack2", "y"},
      {"samp_texmode0", "bpmem_pack2", "z"},
      {"samp_texmode1", "bpmem_pack2", "w"},
  }};
  for (const Packed& p : packed)
    out.Write("#define {}(i) ({}[(i)].{})\n", p.name, p.array, p.swizzle);
  Put(out, "\n");
}

void WriteBoundingBoxFunctions(ShaderCode& out)
{
  static constexpr std::array<std::string_view, 4> sides = {"left", "right", "top", "bottom"};
  Put(out, "SSBO_BINDING(0) coherent buffer BBox {\n  int bbox_data[4];\n};\n");
  for (u32 i = 0; i < 4; ++i)
    out.Write("#define bbox_{} bbox_data[{}]\n", sides[i], i);

  Put(out, "\nvoid UpdateBoundingBoxBuffer(int2 min_pos, int2 max_pos) {\n");
  for (u32 i = 0; i < 4; ++i)
  {
    // Even sides are minima, odd ones maxima; the first pair is x, the second y.
    const bool is_max = (i & 1) != 0;
    const char axis = i < 2 ? 'x' : 'y';
    out.Write("  if (bbox_{0} {1} {2}_pos.{3})\n    {4}(bbox_{0}, {2}_pos.{3});\n", sides[i],
              is_max ? '<' : '>', is_max ? "max" : "min", axis, is_max ? "atomicMax" : "atomicMin");
  }
  Put(out, "}\n\n");

  Put(out, "void UpdateBoundingBox(float2 rawpos) {\n"
           "  // We only want to include coordinates for pixels aligned with the native resolution "
           "pixel centers.\n"
           "  // This makes bounding box sizes more accurate (though not perfect) at higher "
           "resolutions,\n"
           "  // avoiding EFB copy buffer overflow in affected games.\n"
           "  //\n"
           "  // For a more detailed explanation, see https://dolp.in/pr9801\n");
  out.Write("  int2 int_efb_scale = iround(1.0 / {}.xy);\n", I_EFBSCALE);
  Put(out, "  if (int(rawpos.x) % int_efb_scale.x != int_efb_scale.x >> 1 ||\n"
           "      int(rawpos.y) % int_efb_scale.y != int_efb_scale.y >> 1)  "
           "// right shift for fast divide by two\n"
           "  {\n    return;\n  }\n\n"
           "  // The rightmost shaded pixel is not included in the right bounding box register,\n"
           "  // such that width = right - left + 1. This has been verified on hardware.\n");
  out.Write("  int2 pos = int2(rawpos * {}.xy);\n\n", I_EFBSCALE);
  Put(out, "#ifdef API_OPENGL\n"
           "  // We need to invert the Y coordinate due to OpenGL's lower-left origin\n");
  out.Write("  pos.y = {} - pos.y - 1;\n", EFB_HEIGHT);
  Put(out, "#endif\n\n"
           "  // The GC/Wii GPU rasterizes in 2x2 pixel groups, so bounding box values will be "
           "rounded to the\n"
           "  // extents of these groups, rather than the exact pixel.\n"
           "  int2 pos_tl = pos & ~1;  // round down to even\n"
           "  int2 pos_br = pos | 1;   // round up to odd\n\n"
           "#if defined(SUPPORTS_SUBGROUP_REDUCTION) && !defined(BROKEN_SUBGROUP_WITH_DISCARD)\n"
           "  if (!IS_HELPER_INVOCATION)\n  {\n"
           "    SUBGROUP_MIN(pos_tl);\n    SUBGROUP_MAX(pos_br);\n"
           "    if (IS_FIRST_ACTIVE_INVOCATION)\n"
           "      UpdateBoundingBoxBuffer(pos_tl, pos_br);\n  }\n"
           "#else\n  UpdateBoundingBoxBuffer(pos_tl, pos_br);\n#endif\n}\n\n");
}

// readTexture(), readTextureLinear() and WrapCoord() for manual texture sampling.
void WriteManualSamplingHelpers(ShaderCode& out, bool custom_texture_sizes)
{
  Put(out, "\nint4 readTexture(in sampler2DArray tex, uint u, uint v, int layer, int lod) {\n"
           "  return iround(texelFetch(tex, int3(u, v, layer), lod) * 255.0);\n}\n\n"
           "int4 readTextureLinear(in sampler2DArray tex, uint2 uv1, uint2 uv2, int layer, "
           "int lod, int2 frac_uv) {\n  int4 result =\n");
  // Bilinear weights in 1/128 steps; the four texels are summed and the 14 fraction bits dropped.
  for (u32 corner = 0; corner < 4; ++corner)
  {
    const bool right = (corner & 1) != 0;
    const bool bottom = (corner & 2) != 0;
    out.Write("    readTexture(tex, uv{}.x, uv{}.y, layer, lod) * ({}frac_uv.x) * ({}frac_uv.y){}\n",
              right ? 2 : 1, bottom ? 2 : 1, right ? "      " : "128 - ",
              bottom ? "      " : "128 - ", corner == 3 ? ";" : " +");
  }
  Put(out, "  return result >> 14;\n}\n");

  // Custom textures may have any size, so they need a real modulo; console textures are powers of
  // two and can be masked.
  std::string_view repeat;
  std::string_view mirror_test;
  if (custom_texture_sizes)
  {
    Put(out, "\n// Both GLSL and HLSL produce undefined values when the modulo operator (%) is "
             "used with a negative\n"
             "// dividend and a positive divisor.  We want a positive value such that "
             "SafeModulo(-1, 3) is 2.\n"
             "int SafeModulo(int dividend, int divisor) {\n"
             "  if (dividend >= 0) {\n    return dividend % divisor;\n  } else {\n"
             "    // This works because ~x is the same as -x - 1.\n"
             "    // `~x % 5` over -5 to -1 gives 4, 3, 2, 1, 0.  `4 - (~x % 5)` gives "
             "0, 1, 2, 3, 4.\n"
             "    return (divisor - 1) - (~dividend % divisor);\n  }\n}\n");
    repeat = "uint(SafeModulo(coord, size));  // coord % size";
    mirror_test = "SafeModulo(coord, 2 * size) >= size) {  // coord % (2 * size)";
  }
  else
  {
    repeat = "uint(coord & (size - 1));";
    mirror_test = "(coord & size) != 0) {";
  }
  out.Write("\nuint WrapCoord(int coord, uint wrap, int size) {{\n  switch (wrap) {{\n"
            "    case {:s}:\n"
            "    default: // confirmed that clamp is used for invalid (3) via hardware test\n"
            "      return uint(clamp(coord, 0, size - 1));\n",
            WrapMode::Clamp);
  out.Write("    case {:s}:\n      return {}\n", WrapMode::Repeat, repeat);
  out.Write("    case {:s}:\n      if ({}\n        coord = ~coord;\n      }}\n      return {}\n"
            "  }}\n}}\n",
            WrapMode::Mirror, mirror_test, repeat);
}

// The body of sampleTexture() with manual sampling: LOD selection, wrapping and filtering done in
// the shader to match the console exactly.
void WriteManualSampleTexture(ShaderCode& out, const ShaderHostConfig& host_config)
{
  out.Write("\n  uint texmode0 = samp_texmode0(texmap);\n"
            "  uint texmode1 = samp_texmode1(texmap);\n\n"
            "  uint wrap_s = {};\n  uint wrap_t = {};\n",
            BitfieldExtract<&SamplerState::TM0::wrap_u>("texmode0"),
            BitfieldExtract<&SamplerState::TM0::wrap_v>("texmode0"));
  out.Write("  bool mag_linear = {} != 0u;\n  bool mipmap_linear = {} != 0u;\n"
            "  bool min_linear = {} != 0u;\n  bool diag_lod = {} != 0u;\n",
            BitfieldExtract<&SamplerState::TM0::mag_filter>("texmode0"),
            BitfieldExtract<&SamplerState::TM0::mipmap_filter>("texmode0"),
            BitfieldExtract<&SamplerState::TM0::min_filter>("texmode0"),
            BitfieldExtract<&SamplerState::TM0::diag_lod>("texmode0"));
  out.Write("  int lod_bias = {};\n  // uint max_aniso = TODO;\n  bool lod_clamp = {} != 0u;\n",
            BitfieldExtract<&SamplerState::TM0::lod_bias>("texmode0"),
            BitfieldExtract<&SamplerState::TM0::lod_clamp>("texmode0"));
  out.Write("  int min_lod = int({});\n  int max_lod = int({});\n",
            BitfieldExtract<&SamplerState::TM1::min_lod>("texmode1"),
            BitfieldExtract<&SamplerState::TM1::max_lod>("texmode1"));

  if (host_config.manual_texture_sampling_custom_texture_sizes)
  {
    out.Write("\n  int native_size_s = {0}[texmap].x;\n  int native_size_t = {0}[texmap].y;\n",
              I_TEXDIMS);
    Put(out, "\n  int3 size = textureSize(tex, 0);\n"
             "  int size_s = size.x;\n  int size_t = size.y;\n  int num_layers = size.z;\n");
    if (!g_backend_info.bSupportsTextureQueryLevels)
    {
      Put(out, "  int number_of_levels = 256;  // textureQueryLevels is not supported\n");
      ERROR_LOG_FMT(VIDEO, "textureQueryLevels is not supported!  Odd graphical results may occur "
                           "if custom textures are in use!");
    }
    else
    {
      Put(out, "  int number_of_levels = textureQueryLevels(tex);\n");
    }
    Put(out, "\n  // Prevent out-of-bounds LOD values when using custom textures\n"
             "  max_lod = min(max_lod, (number_of_levels - 1) << 4);\n"
             "  // Rescale uv to account for the new texture size\n");
    for (const char axis : {'s', 't'})
    {
      out.Write("  uv.{} = (uv.{} * size_{}) / native_size_{};\n", axis == 's' ? 'x' : 'y',
                axis == 's' ? 'x' : 'y', axis, axis);
    }
    Put(out, "  // Clamp layer as well (texture() automatically clamps, but texelFetch() doesn't)\n"
             "  layer = clamp(layer, 0, num_layers - 1);\n");
  }
  else
  {
    out.Write("\n  int size_s = {0}[texmap].x;\n  int size_t = {0}[texmap].y;\n", I_TEXDIMS);
  }

  // Coarse derivatives match the software renderer (not hardware tested). Without the explicit
  // functions the driver may still choose coarse ones.
  const std::string_view coarse = g_backend_info.bSupportsCoarseDerivatives ? "Coarse" : "";
  out.Write("\n  float2 uv_delta_x = abs(dFdx{0}(float2(uv)));\n"
            "  float2 uv_delta_y = abs(dFdy{0}(float2(uv)));\n",
            coarse);

  // LOD in 1/16 steps; lod_bias carries 8 fraction bits.
  Put(out, "\n  float2 uv_delta = diag_lod ? uv_delta_x + uv_delta_y : max(uv_delta_x, "
           "uv_delta_y);\n"
           "  float max_delta = max(uv_delta.x / 128.0, uv_delta.y / 128.0);\n"
           "  // log2(x) is undefined if x <= 0, but in practice it seems log2(0) is -infinity, "
           "which becomes INT_MIN.\n"
           "  // If lod_bias is negative, adding it to INT_MIN causes an underflow, resulting in a "
           "large positive value.\n"
           "  // Hardware testing indicates that min_lod should be used when the derivative is "
           "0.\n"
           "  int lod = max_delta == 0.0 ? min_lod : int(floor(log2(max_delta) * 16.0)) + "
           "(lod_bias >> 4);\n\n"
           "  bool is_linear = (lod > 0) ? min_linear : mag_linear;\n"
           "  lod = clamp(lod, min_lod, max_lod);\n"
           "  int base_lod = lod >> 4;\n  int frac_lod = lod & 15;\n"
           "  if (!mipmap_linear && frac_lod >= 8) {\n"
           "    // Round to nearest LOD in point mode\n    base_lod++;\n  }\n\n");

  // One mip level of the bilinear path; `next` selects the following level.
  const auto bilinear_level = [&out](bool next) {
    const std::string_view lod = next ? "(base_lod + 1)" : "base_lod";
    const std::string_view indent = next ? "      " : "    ";
    for (const char sign : {'-', '+'})
    {
      out.Write("{}{}texuv{} = uint2(\n", indent, next ? "" : "uint2 ", sign == '-' ? 1 : 2);
      for (const char axis : {'x', 'y'})
      {
        const char st = axis == 'x' ? 's' : 't';
        out.Write("{0}    WrapCoord(((uv.{1} >> {2}) {3} 64) >> 7, wrap_{4}, size_{4} >> {2}){5}\n",
                  indent, axis, lod, sign, st, axis == 'x' ? "," : ");");
      }
    }
    out.Write("{0}{1}frac_uv = int2(((uv.x >> {2}) - 64) & 0x7f, ((uv.y >> {2}) - 64) & 0x7f);\n",
              indent, next ? "" : "int2 ", lod);
  };
  // One mip level of the point-sampled path.
  const auto point_level = [&out](bool next) {
    const std::string_view lod = next ? "(base_lod + 1)" : "base_lod";
    const std::string_view indent = next ? "      " : "    ";
    out.Write("{}{}texuv = uint2(\n", indent, next ? "" : "uint2 ");
    for (const char axis : {'x', 'y'})
    {
      const char st = axis == 'x' ? 's' : 't';
      out.Write("{0}    WrapCoord(uv.{1} >> (7 + base_lod{2}), wrap_{3}, size_{3} >> {4}){5}\n",
                indent, axis, next ? " + 1" : "", st, lod, axis == 'x' ? "," : ");");
    }
  };
  // Blends in the next mip level for trilinear filtering.
  const auto mip_blend = [&out](std::string_view read) {
    out.Write("      result *= 16 - frac_lod;\n"
              "      result += {} * frac_lod;\n"
              "      result >>= 4;\n    }}\n",
              read);
  };

  Put(out, "  if (is_linear) {\n");
  bilinear_level(false);
  Put(out, "\n    int4 result = readTextureLinear(tex, texuv1, texuv2, layer, base_lod, "
           "frac_uv);\n\n    if (frac_lod != 0 && mipmap_linear) {\n");
  bilinear_level(true);
  Put(out, "\n");
  mip_blend("readTextureLinear(tex, texuv1, texuv2, layer, base_lod + 1, frac_uv)");
  Put(out, "\n    return result;\n  } else {\n");
  point_level(false);
  Put(out, "\n    int4 result = readTexture(tex, texuv.x, texuv.y, layer, base_lod);\n\n"
           "    if (frac_lod != 0 && mipmap_linear) {\n");
  point_level(true);
  Put(out, "\n");
  mip_blend("readTexture(tex, texuv.x, texuv.y, layer, base_lod + 1)");
  Put(out, "    return result;\n  }\n}\n");
}

// ---------------------------------------------------------------------------------------------
// TEV stages.

// "(((d + bias) << scale) +/- (lerp(a, b, c) ...)) >> scale". The console scales c to 0..256 so
// that the lerp can divide by 256, applies scales above one inside the lerp for precision and adds
// a rounding bias before that division (not when halving).
void WriteTevRegular(ShaderCode& out, std::string_view components, TevBias bias, TevOp op,
                     TevScale scale)
{
  std::string_view bias_term;
  if (bias == TevBias::AddHalf)
    bias_term = " + 128";
  else if (bias == TevBias::SubHalf)
    bias_term = " - 128";

  std::string_view shift_left;
  if (scale == TevScale::Scale2)
    shift_left = " << 1";
  else if (scale == TevScale::Scale4)
    shift_left = " << 2";
  const bool halve = scale == TevScale::Divide2;
  const std::string_view rounding = halve ? "" : (op == TevOp::Sub ? " + 127" : " + 128");

  out.Write("(((tevin_d.{0}{1}){2}) {3} (((((tevin_a.{0}<<8) + (tevin_b.{0}-tevin_a.{0})*"
            "(tevin_c.{0}+(tevin_c.{0}>>7))){2}){4})>>8)){5}",
            components, bias_term, shift_left, op == TevOp::Sub ? '-' : '+', rounding,
            halve ? " >> 1" : "");
}

// Compare-mode stages: "d + (a OP b ? c : 0)" on the selected component width.
std::string CompareTerm(bool alpha, TevComparison comparison, TevCompareMode mode)
{
  const bool eq = comparison == TevComparison::EQ;
  const std::string_view op = eq ? "==" : ">";
  const std::string_view result = alpha ? "a" : "rgb";
  const std::string_view zero = alpha ? "0" : "int3(0,0,0)";

  switch (mode)
  {
  case TevCompareMode::R8:
    return fmt::format("((tevin_a.r {} tevin_b.r) ? tevin_c.{} : {})", op, result, zero);
  case TevCompareMode::GR16:
  case TevCompareMode::BGR24:
  {
    const u32 bits = mode == TevCompareMode::GR16 ? 16 : 24;
    // The colour equality variant is written without spaces after the commas.
    const std::string_view sep = (eq && !alpha) ? "," : ", ";
    return fmt::format("((idot(tevin_a.rgb{1}comp{0}) {2} idot(tevin_b.rgb{1}comp{0})) ? "
                       "tevin_c.{3} : {4})",
                       bits, sep, op, result, zero);
  }
  default:
    break;
  }
  // RGB8 compares per component; for alpha the same encoding is A8.
  if (alpha)
    return fmt::format("((tevin_a.a {} tevin_b.a) ? tevin_c.a : 0)", op);
  if (eq)
    return "((int3(1,1,1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)";
  return "(max(sign(tevin_a.rgb - tevin_b.rgb), int3(0,0,0)) * tevin_c.rgb)";
}

// The indirect texture part of a stage: computes tevcoord from the regular coordinates and the
// indirect offsets.
void WriteIndirectOp(ShaderCode& out, const pixel_shader_uid_data& uid, const StageKey& stage,
                     u32 n, u32 texcoord, bool has_tex_coord)
{
  TevStageIndirect ind;
  ind.fullhex = stage.tevind;
  Put(out, "\t// indirect op\n");

  // An indirect stage beyond numindstages is undefined (a noise pattern on console); skipping the
  // offset is close enough and avoids reading a missing iindtex. Wrapping and add-to-previous
  // still apply, as they do for disabled stages (Viewtiful Joe, bug 12525).
  const u32 bt = ind.bt;
  const bool has_ind_stage = bt < uid.genMode_numindstages;
  const u32 format = static_cast<u32>(ind.fmt.Value());

  if (has_ind_stage && ind.bs != IndTexBumpAlpha::Off)
  {
    // Bump alpha: the low bits of the selected component, moved to the top of five bits (ITF_8
    // has no alpha bits and uses no shift).
    static constexpr std::array<char, 4> alpha_shift = {'0', '5', '4', '3'};
    static constexpr std::array<char, 4> component = {' ', 'x', 'y', 'z'};
    out.Write("\talphabump = (iindtex{}.{} << {}) & 248;\n", bt,
              component[static_cast<u32>(ind.bs.Value())], alpha_shift[format]);
  }

  if (has_ind_stage && ind.matrix_index != IndMtxIndex::Off)
  {
    // Offsets: the high bits of each component, by format.
    static constexpr std::array<char, 4> offset_shift = {'0', '3', '4', '5'};
    out.Write("\tint3 iindtevcrd{} = iindtex{} >> {};\n", n, bt, offset_shift[format]);

    const u32 bias_mask = static_cast<u32>(ind.bias.Value());
    if (bias_mask != 0)
    {
      std::string fields;
      for (u32 bit = 0; bit < 3; ++bit)
      {
        if ((bias_mask & (1u << bit)) != 0)
          fields += "xyz"[bit];
      }
      const std::string_view add = format == 0 ? "-128" : "1";
      const int count = std::popcount(bias_mask);
      std::string value;
      if (count == 1)
      {
        value = fmt::format("int({})", add);
      }
      else
      {
        value = fmt::format("int{}({}", count, add);
        for (int k = 1; k < count; ++k)
          value += fmt::format(", {}", add);
        value += ')';
      }
      out.Write("\tiindtevcrd{}.{} += {};\n", n, fields, value);
    }

    // Each matrix has two rows; the fourth column of a row holds the scale exponent.
    const u32 row = 2 * (static_cast<u32>(ind.matrix_index.Value()) - 1);
    const bool negation_bug = DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION);
    const IndMtxId id = ind.matrix_id;
    if (id == IndMtxId::Indirect || id == IndMtxId::S || id == IndMtxId::T)
    {
      // The products overflow, but only the low 24 bits matter.
      if (id == IndMtxId::Indirect)
      {
        out.Write("\tint2 indtevtrans{0} = int2(idot({1}[{2}].xyz, iindtevcrd{0}), "
                  "idot({1}[{3}].xyz, iindtevcrd{0})) >> 3;\n",
                  n, I_INDTEXMTX, row, row + 1);
      }
      else
      {
        // Dynamic matrices scale the regular coordinate by one offset component.
        ASSERT(has_tex_coord);
        out.Write("\tint2 indtevtrans{} = int2(fixpoint_uv{} * iindtevcrd{}.{}) >> 8;\n", n,
                  texcoord, n, id == IndMtxId::S ? "xx" : "yy");
      }
      if (negation_bug)
      {
        // Drivers with this bug cannot negate inside the shift.
        const bool dynamic = id != IndMtxId::Indirect;
        out.Write("\tint{} indtexmtx_w_inverse_{} = -{}[{}].w;\n", dynamic ? " " : "", n,
                  I_INDTEXMTX, row);
        out.Write("\tif ({0}[{1}].w >= 0) indtevtrans{2} >>= {0}[{1}].w;\n", I_INDTEXMTX, row, n);
        if (dynamic)
          out.Write("\telse indtevtrans{0} <<= (indtexmtx_w_inverse_{0});\n", n);
        else
          out.Write("\telse indtevtrans{0} <<= indtexmtx_w_inverse_{0};\n", n);
      }
      else
      {
        out.Write("\tif ({0}[{1}].w >= 0) indtevtrans{2} >>= {0}[{1}].w;\n", I_INDTEXMTX, row, n);
        out.Write("\telse indtevtrans{} <<= (-{}[{}].w);\n", n, I_INDTEXMTX, row);
      }
    }
    else
    {
      out.Write("\tint2 indtevtrans{} = int2(0, 0);\n", n);
      ASSERT(false);  // invalid matrix_id
    }
  }
  else
  {
    out.Write("\tint2 indtevtrans{} = int2(0, 0);\n", n);
    // Without a matrix the id should be the default (Indirect).
    if (ind.matrix_index == IndMtxIndex::Off)
      ASSERT(ind.matrix_id == IndMtxId::Indirect);
  }

  // Wrapping of the regular coordinates: off, a power of two from 256 down to 16, or zero
  // (invalid encoding 7 behaves like zero).
  const auto write_wrap = [&](char axis, IndTexWrap wrap) {
    if (wrap == IndTexWrap::ITW_OFF)
    {
      out.Write("\twrappedcoord.{0} = fixpoint_uv{1}.{0};\n", axis, texcoord);
    }
    else if (wrap >= IndTexWrap::ITW_0)
    {
      out.Write("\twrappedcoord.{} = 0;\n", axis);
    }
    else
    {
      const u32 size = 512u >> static_cast<u32>(wrap);
      out.Write("\twrappedcoord.{0} = fixpoint_uv{1}.{0} & (({2}<<7) - 1);\n", axis, texcoord,
                size);
    }
  };
  write_wrap('x', ind.sw);
  write_wrap('y', ind.tw);

  out.Write("\ttevcoord.xy {}= wrappedcoord + indtevtrans{};\n", ind.fb_addprev ? "+" : "", n);
  // The coordinates are signed 24-bit values.
  Put(out, "\ttevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
}

void WriteStage(ShaderCode& out, const pixel_shader_uid_data& uid, u32 n)
{
  const StageKey& stage = uid.stagehash[n];
  out.Write("\n\t// TEV stage {}\n", n);

  // A texture coordinate the stage refers to that does not exist reads coordinate 0 (on console
  // that sometimes glitches).
  const bool has_tex_coord = stage.tevorders_texcoord < uid.genMode_numtexgens;
  const u32 texcoord = has_tex_coord ? stage.tevorders_texcoord : 0;
  WriteIndirectOp(out, uid, stage, n, texcoord, has_tex_coord);

  TevStageCombiner::ColorCombiner cc;
  TevStageCombiner::AlphaCombiner ac;
  cc.hex = stage.cc;
  ac.hex = stage.ac;

  if (StageReadsRaster(cc, ac))
  {
    out.Write("\trastemp = {}.{};\n", RasterColor(stage.tevorders_colorchan),
              Swizzle(stage.ras_swap_r, stage.ras_swap_g, stage.ras_swap_b, stage.ras_swap_a));
  }

  if (uid.genMode_numtexgens == 0)
  {
    // Without texgens the texture input appears to be black (needs more testing).
    Put(out, "\ttextemp = int4(0, 0, 0, 0);\n");
  }
  else if (!stage.tevorders_enable)
  {
    Put(out, "\ttextemp = int4(255, 255, 255, 255);\n");
  }
  else
  {
    const u32 texmap = stage.tevorders_texmap;
    out.Write("\trawtextemp = sampleTextureWrapper({}u, tevcoord.xy, layer);\n", texmap);
    out.Write("\ttextemp = rawtextemp.{};\n",
              Swizzle(stage.tex_swap_r, stage.tex_swap_g, stage.tex_swap_b, stage.tex_swap_a));
  }

  if (StageReadsKonst(cc, ac))
  {
    out.Write("\tkonsttemp = int4({}, {});\n", KonstColor(stage.tevksel_kc),
              KonstAlpha(stage.tevksel_ka));
  }

  // Inputs a-c are 8 bit; some drivers mishandle the vector form of the mask.
  const bool scalar_mask = DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VECTOR_BITWISE_AND);
  const std::array<std::pair<TevColorArg, TevAlphaArg>, 3> inputs = {{
      {cc.a, ac.a},
      {cc.b, ac.b},
      {cc.c, ac.c},
  }};
  for (u32 i = 0; i < 3; ++i)
  {
    const std::string color = ColorInput(inputs[i].first);
    const std::string alpha = AlphaInput(inputs[i].second);
    const char name = static_cast<char>('a' + i);
    if (scalar_mask)
      out.Write("\ttevin_{} = int4({} & 255, {} & 255);\n", name, color, alpha);
    else
      out.Write("\ttevin_{} = int4({}, {})&int4(255, 255, 255, 255);\n", name, color, alpha);
  }
  out.Write("\ttevin_d = int4({}, {});\n", ColorInput(cc.d), AlphaInput(ac.d));

  Put(out, "\t// color combine\n");
  out.Write("\t{} = clamp(", ColorOutput(cc.dest));
  if (cc.bias == TevBias::Compare)
    out.Write("   tevin_d.rgb + {}", CompareTerm(false, cc.comparison, cc.compare_mode));
  else
    WriteTevRegular(out, "rgb", cc.bias, cc.op, cc.scale);
  if (cc.clamp)
    Put(out, ", int3(0,0,0), int3(255,255,255))");
  else
    Put(out, ", int3(-1024,-1024,-1024), int3(1023,1023,1023))");
  Put(out, ";\n");

  Put(out, "\t// alpha combine\n");
  out.Write("\t{} = clamp(", AlphaOutput(ac.dest));
  if (ac.bias == TevBias::Compare)
    out.Write("   tevin_d.a + {}", CompareTerm(true, ac.comparison, ac.compare_mode));
  else
    WriteTevRegular(out, "a", ac.bias, ac.op, ac.scale);
  Put(out, ac.clamp ? ", 0, 255)" : ", -1024, 1023)");
  Put(out, ";\n");
}

// ---------------------------------------------------------------------------------------------
// Per-pixel operations after TEV.

std::string AlphaCompare(CompareMode mode, std::string_view ref)
{
  switch (mode)
  {
  case CompareMode::Never:
    return "(false)";
  case CompareMode::Always:
    return "(true)";
  default:
    break;
  }
  static constexpr Common::EnumMap<std::string_view, CompareMode::Always> ops{
      "", "< ", "==", "<=", "> ", "!=", ">=", "",
  };
  return fmt::format("(prev.a {} {})", ops[mode], ref);
}

void WriteAlphaTest(ShaderCode& out, const pixel_shader_uid_data& uid, APIType api_type,
                    bool per_pixel_depth, bool use_dual_source)
{
  static constexpr Common::EnumMap<std::string_view, AlphaTestOp::Xnor> logic{" && ", " || ",
                                                                             " != ", " == "};
  // Some drivers break on "!(...)", so the comparison against false is used there.
  const bool negation_bug = DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN);
  out.Write("\tif({}( {}{}{}", negation_bug ? "" : "!",
            AlphaCompare(uid.alpha_test_comp0, I_ALPHA ".r"), logic[uid.alpha_test_logic],
            AlphaCompare(uid.alpha_test_comp1, I_ALPHA ".g"));
  Put(out, negation_bug ? ") == false) {\n" : ")) {\n");

  const std::string_view zero = uid.uint_output ? "uint4(0, 0, 0, 0)" : "float4(0.0, 0.0, 0.0, 0.0)";
  out.Write("\t\tocol0 = {};\n", zero);
  if (use_dual_source)
    out.Write("\t\tocol1 = {};\n", zero);
  if (per_pixel_depth)
    out.Write("\t\tdepth = {};\n", g_backend_info.bSupportsReversedDepthRange ? "1.0" : "0.0");

  // With the zcomploc hack the fragment is kept so that its depth is still written.
  if (uid.ztest != EmulatedZ::EarlyWithZComplocHack)
  {
    Put(out, "\t\tdiscard;\n");
    if (api_type == APIType::D3D)
      Put(out, "\t\treturn;\n");
  }
  Put(out, "\t}\n");
}

void WriteFog(ShaderCode& out, const pixel_shader_uid_data& uid)
{
  if (uid.fog_fsel == FogType::Off)
    return;

  if (uid.fog_proj == FogProjection::Perspective)
  {
    // ze = A / (B - (Zs >> B_SHF)), dropping the low bits of Zs like the software renderer.
    out.Write("\tfloat ze = ({0}.x * 16777216.0) / float({1}.y - (zCoord >> {1}.w));\n", I_FOGF,
              I_FOGI);
  }
  else
  {
    out.Write("\tfloat ze = {}.x * float(zCoord) / 16777216.0;\n", I_FOGF);
  }

  if (uid.fog_RangeBaseEnabled)
  {
    // Range fog: ze *= sqrt((x - center)^2 + k^2) / k, with k interpolated from the 10-entry
    // table.
    out.Write("\tfloat offset = (2.0 * (rawpos.x / {0}.w)) - 1.0 - {0}.z;\n", I_FOGF);
    Put(out, "\tfloat floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
             "\tuint indexlower = uint(floatindex);\n"
             "\tuint indexupper = indexlower + 1u;\n");
    for (const std::string_view bound : {"lower", "upper"})
    {
      out.Write("\tfloat k{0} = {1}[index{0} >> 2u][index{0} & 3u];\n", bound, I_FOGRANGE);
    }
    Put(out, "\tfloat k = lerp(klower, kupper, frac(floatindex));\n"
             "\tfloat x_adjust = sqrt(offset * offset + k * k) / k;\n"
             "\tze *= x_adjust;\n");
  }

  out.Write("\tfloat fog = clamp(ze - {}.y, 0.0, 1.0);\n", I_FOGF);

  switch (uid.fog_fsel)
  {
  case FogType::Exp:
    Put(out, "\tfog = 1.0 - exp2(-8.0 * fog);\n");
    break;
  case FogType::ExpSq:
    Put(out, "\tfog = 1.0 - exp2(-8.0 * fog * fog);\n");
    break;
  case FogType::BackwardsExp:
    Put(out, "\tfog = exp2(-8.0 * (1.0 - fog));\n");
    break;
  case FogType::BackwardsExpSq:
    Put(out, "\tfog = 1.0 - fog;\n   fog = exp2(-8.0 * fog * fog);\n");
    break;
  case FogType::Linear:
    break;
  default:
    WARN_LOG_FMT(VIDEO, "Unknown Fog Type! {}", uid.fog_fsel);
    break;
  }

  Put(out, "\tint ifog = iround(fog * 256.0);\n");
  out.Write("\tprev.rgb = (prev.rgb * (256 - ifog) + {}.rgb * ifog) >> 8;\n", I_FOGCOLOR);
}

// Logic op against the framebuffer value read with framebuffer fetch.
void WriteLogicOp(ShaderCode& out, const pixel_shader_uid_data& uid)
{
  // Indexed by LogicOp: Clear, And, AndReverse, Copy, AndInverted, NoOp, Xor, Or,
  // Nor, Equiv, Invert, OrReverse, CopyInverted, OrInverted, Nand, Set.
  static constexpr std::array<std::string_view, 16> expressions = {
      "int4(0, 0, 0, 0)",   "prev & fb_value",  "prev & ~fb_value",   "prev",
      "~prev & fb_value",   "fb_value",         "prev ^ fb_value",    "prev | fb_value",
      "~(prev | fb_value)", "~(prev ^ fb_value)", "~fb_value",        "prev | ~fb_value",
      "~prev",              "~prev | fb_value", "~(prev & fb_value)", "int4(255, 255, 255, 255)",
  };
  Put(out, "\tint4 fb_value = iround(initial_ocol0 * 255.0);\n");
  out.Write("\tprev = ({}) & 0xff;\n", expressions[uid.logic_op_mode]);
}

// Logic ops approximated with fixed-function blending: the shader only supplies the constant
// source colour the blend equation needs.
void WriteLogicOpBlend(ShaderCode& out, const pixel_shader_uid_data& uid)
{
  const LogicOp op = static_cast<LogicOp>(uid.logic_op_mode);
  if (op == LogicOp::Clear || op == LogicOp::NoOp)
    Put(out, "\tprev = int4(0, 0, 0, 0);\n");
  else if (op == LogicOp::CopyInverted)
    Put(out, "\tprev ^= 255;\n");
  else if (op == LogicOp::Set || op == LogicOp::Invert)
    Put(out, "\tprev = int4(255, 255, 255, 255);\n");
}

void WriteColor(ShaderCode& out, const pixel_shader_uid_data& uid, bool use_dual_source)
{
  // Integer render targets (logic ops on some backends) take the value directly.
  if (uid.uint_output)
  {
    Put(out, uid.rgba6_format ? "\tocol0 = uint4(prev & 0xFC);\n" : "\tocol0 = uint4(prev);\n");
    return;
  }

  Put(out, uid.rgba6_format ? "\tocol0.rgb = float3(prev.rgb >> 2) / 63.0;\n" :
                              "\tocol0.rgb = float3(prev.rgb) / 255.0;\n");
  // With destination alpha the framebuffer gets the constant alpha, while blending uses the
  // 8-bit alpha through the second (dual-source) output.
  out.Write("\tocol0.a = float({}.a >> 2) / 63.0;\n", uid.useDstAlpha ? I_ALPHA : "prev");
  if (use_dual_source)
    Put(out, "\tocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
}

// One blend factor expression. Factors come in (value, 1 - value) pairs; pair 0 is zero/one.
std::string BlendFactor(bool alpha, bool destination, u32 factor)
{
  const u32 pair = factor / 2;
  const bool inverted = (factor & 1) != 0;
  if (pair == 0)
  {
    if (alpha)
      return inverted ? "1.0;" : "0.0;";
    return inverted ? "float3(1,1,1);" : "float3(0,0,0);";
  }
  std::string_view source;
  if (pair == 1)
    source = destination ? "ocol0" : "initial_ocol0";
  else if (pair == 2)
    source = "src_color";
  else
    source = "initial_ocol0";
  const std::string_view swizzle = alpha ? "a" : (pair == 1 ? "rgb" : "aaa");
  if (!inverted)
    return fmt::format("{}.{};", source, swizzle);
  return fmt::format("{} - {}.{};", alpha ? "1.0" : "float3(1,1,1)", source, swizzle);
}

// Blending done in the shader through framebuffer fetch.
void WriteBlend(ShaderCode& out, const pixel_shader_uid_data& uid)
{
  out.Write("\tfloat4 src_color = {};\n\tfloat4 blend_src;", uid.useDstAlpha ? "ocol1" : "ocol0");
  out.Write("\tblend_src.rgb = {}\n",
            BlendFactor(false, false, static_cast<u32>(uid.blend_src_factor)));
  out.Write("\tblend_src.a = {}\n",
            BlendFactor(true, false, static_cast<u32>(uid.blend_src_factor_alpha)));
  Put(out, "\tfloat4 blend_dst;\n");
  out.Write("\tblend_dst.rgb = {}\n",
            BlendFactor(false, true, static_cast<u32>(uid.blend_dst_factor)));
  out.Write("\tblend_dst.a = {}\n",
            BlendFactor(true, true, static_cast<u32>(uid.blend_dst_factor_alpha)));

  Put(out, "\tfloat4 blend_result;\n");
  out.Write("\tblend_result.rgb = initial_ocol0.rgb * blend_dst.rgb {} ocol0.rgb * blend_src.rgb;\n",
            uid.blend_subtract ? '-' : '+');
  out.Write("\tblend_result.a = initial_ocol0.a * blend_dst.a {} ocol0.a * blend_src.a;\n",
            uid.blend_subtract_alpha ? '-' : '+');
  Put(out, "\treal_ocol0 = blend_result;\n");
}

// Types shared with custom (graphics mod) pixel shaders.
void WriteFragmentDefinitions(ShaderCode& out)
{
  Put(out, "struct DolphinLightData\n{\n"
           "\tfloat3 position;\n\tfloat3 direction;\n\tfloat3 color;\n"
           "\tuint attenuation_type;\n\tfloat4 cosatt;\n\tfloat4 distatt;\n};\n\n");

  Put(out, "struct DolphinFragmentInput\n{\n"
           "\tvec4 color_0;\n\tvec4 color_1;\n\tint layer;\n\tvec3 normal;\n\tvec3 position;\n");
  for (u32 i = 0; i < 8; ++i)
    out.Write("\tvec3 tex{};\n", i);
  Put(out, "\n");
  for (u32 chan = 0; chan < 2; ++chan)
  {
    for (const std::string_view part : {"color", "alpha"})
      out.Write("\tDolphinLightData[8] lights_chan{}_{};\n", chan, part);
  }
  Put(out, "\tfloat4[2] ambient_lighting;\n\tfloat4[2] base_material;\n");
  for (u32 chan = 0; chan < 2; ++chan)
  {
    for (const std::string_view part : {"color", "alpha"})
      out.Write("\tuint light_chan{}_{}_count;\n", chan, part);
  }
  Put(out, "};\n\n");

  Put(out, "struct DolphinFragmentOutput\n{\n\tivec4 main;\n\tivec4 last_texture;\n};\n\n");

  const std::array<std::pair<std::string_view, AttenuationFunc>, 4> attenuation = {{
      {"NONE", AttenuationFunc::None},
      {"POINT", AttenuationFunc::Spec},
      {"DIR", AttenuationFunc::Dir},
      {"SPOT", AttenuationFunc::Spot},
  }};
  for (const auto& [name, func] : attenuation)
  {
    out.Write("const uint CUSTOM_SHADER_LIGHTING_ATTENUATION_TYPE_{} = {}u;\n", name,
              static_cast<u32>(func));
  }
}

constexpr std::string_view FRAGMENT_FUNCTION_PARAMS =
    "(in DolphinFragmentInput frag_input, out DolphinFragmentOutput frag_output)";

// Generates a complete specialised pixel shader.
class PixelShaderWriter
{
public:
  PixelShaderWriter(APIType api, const ShaderHostConfig& host, const pixel_shader_uid_data& uid,
                    ShaderCode& out)
      : m_api(api), m_host(host), m_uid(uid), m_out(out),
        m_pixel_lighting(g_ActiveConfig.bEnablePixelLighting),
        m_fb_fetch(uid.blend_enable || uid.logic_op_enable ||
                   uid.ztest == EmulatedZ::EarlyWithFBFetch)
  {
  }

  void Write(const CustomPixelContents& custom)
  {
    m_out.Write("// Pixel Shader for TEV stages\n// {} TEV stages, {} texgens, {} IND stages\n",
                m_uid.genMode_numtevstages + 1, m_uid.genMode_numtexgens,
                m_uid.genMode_numindstages);
    WriteBitfieldExtractHeader(m_out, m_api, m_host);
    WritePixelShaderCommonHeader(m_out, m_api, m_host, m_uid.bounding_box);
    Put(m_out, "\n#define sampleTextureWrapper(texmap, uv, layer) "
               "sampleTexture(texmap, samp[texmap], uv, layer)\n");

    // Depth test before the shader runs (zcomploc). The backend defines FORCE_EARLY_Z as
    // early_fragment_tests or, failing that, conservative depth_unchanged.
    if (m_uid.ztest == EmulatedZ::ForcedEarly)
      Put(m_out, "FORCE_EARLY_Z; \n");

    WriteInterface(custom.uniforms);
    if (m_pixel_lighting)
      GenerateLightingShaderHeader(m_out, m_uid.lighting);

    WriteFragmentDefinitions(m_out);
    m_out.Write("void dolphin_process_emulated_fragment{}\n", FRAGMENT_FUNCTION_PARAMS);
    Put(m_out, "{\n");
    WriteFragmentBody(m_api, m_host, &m_uid, m_out);
    Put(m_out, "}\n");

    if (!custom.shader.empty())
    {
      m_out.Write("{}\n", custom.shader);
    }
    else
    {
      m_out.Write("void process_fragment{}\n", FRAGMENT_FUNCTION_PARAMS);
      Put(m_out, "{\n\tdolphin_process_emulated_fragment(frag_input, frag_output);\n}\n");
    }

    WriteMain(!custom.shader.empty());
  }

private:
  void WriteInterface(std::string_view custom_uniforms)
  {
    const std::string_view color_type = m_uid.uint_output ? "uvec4" : "vec4";
    if (m_fb_fetch)
      Put(m_out, "FRAGMENT_OUTPUT_LOCATION(0) FRAGMENT_INOUT vec4 real_ocol0;\n");
    else
      m_out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out {} ocol0;\n", color_type);
    if (!m_uid.no_dual_src)
      m_out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out {} ocol1;\n", color_type);

    if (m_uid.per_pixel_depth)
      Put(m_out, "#define depth gl_FragDepth\n");

    const bool msaa = m_host.msaa;
    const bool ssaa = m_host.ssaa;
    if (m_host.backend_geometry_shaders)
    {
      Put(m_out, "VARYING_LOCATION(0) in VertexData {\n");
      GenerateVSOutputMembers(m_out, m_api, m_uid.genMode_numtexgens, m_host,
                              GetInterpolationQualifier(msaa, ssaa, true, true),
                              ShaderStage::Pixel);
      Put(m_out, "};\n");
      if (m_host.stereo && !m_host.backend_gl_layer_in_fs)
        Put(m_out, "flat in int layer;");
    }
    else
    {
      const char* qualifier = GetInterpolationQualifier(msaa, ssaa);
      u32 location = 0;
      const auto varying = [&](std::string_view type, std::string_view name) {
        m_out.Write("VARYING_LOCATION({}) {} in {} {};\n", location++, qualifier, type, name);
      };
      varying("float4", "colors_0");
      varying("float4", "colors_1");
      for (u32 i = 0; i < m_uid.genMode_numtexgens; ++i)
        varying("float3", fmt::format("tex{}", i));
      if (!m_host.fast_depth_calc)
        varying("float4", "clipPos");
      if (m_pixel_lighting)
      {
        varying("float3", "Normal");
        varying("float3", "WorldPos");
      }
    }

    if (!custom_uniforms.empty())
    {
      Put(m_out, "UBO_BINDING(std140, 3) uniform CustomShaderBlock {\n");
      Put(m_out, custom_uniforms);
      Put(m_out, "} custom_uniforms;\n");
    }
  }

  void WriteFragmentInput(bool custom_shader)
  {
    Put(m_out, "\tDolphinFragmentInput frag_input;\n");
    for (u32 c = 0; c < 2; ++c)
      m_out.Write("\tfrag_input.color_{0} = colors_{0};\n", c);
    Put(m_out, "\tfrag_input.layer = layer;\n");
    if (m_pixel_lighting)
      Put(m_out, "\tfrag_input.normal = normalize(Normal);\n\tfrag_input.position = WorldPos;\n");
    else
      Put(m_out, "\tfrag_input.normal = vec3(0, 0, 0);\n\tfrag_input.position = vec3(0, 0, 0);\n");
    // (A key with more than eight texgens copies the extra varyings too.)
    for (u32 i = 0; i < m_uid.genMode_numtexgens; ++i)
      m_out.Write("\tfrag_input.tex{0} = tex{0};\n", i);
    for (u32 i = m_uid.genMode_numtexgens; i < 8; ++i)
      m_out.Write("\tfrag_input.tex{} = vec3(0, 0, 0);\n", i);
    if (custom_shader)
      GenerateCustomLighting(&m_out, m_uid.lighting);
  }

  // The 24-bit depth of the fragment, as zCoord.
  void WriteDepth()
  {
    if (m_uid.zfreeze)
    {
      // Depth from the frozen plane equation.
      m_out.Write("\tfloat2 screenpos = rawpos.xy * {}.xy;\n", I_EFBSCALE);
      if (m_api == APIType::OpenGL)
        m_out.Write("\tscreenpos.y = {}.0 - screenpos.y;\n", EFB_HEIGHT);
      m_out.Write("\tint zCoord = int({0}.z + {0}.x * screenpos.x + {0}.y * screenpos.y);\n",
                  I_ZSLOPE);
    }
    else if (!m_host.fast_depth_calc)
    {
      // Recomputed from the clip position, which is more accurate than the rasterized depth.
      m_out.Write("\tint zCoord = {0}[1].x + int((clipPos.z / clipPos.w) * float({0}[1].y));\n",
                  I_ZBIAS);
    }
    else if (m_host.backend_reversed_depth_range)
    {
      Put(m_out, "\tint zCoord = int(rawpos.z * 16777216.0);\n");
    }
    else
    {
      Put(m_out, "\tint zCoord = int((1.0 - rawpos.z) * 16777216.0);\n");
    }
    Put(m_out, "\tzCoord = clamp(zCoord, 0, 0xFFFFFF);\n");

    const std::string_view depth_out = m_host.backend_reversed_depth_range ?
                                           "\tdepth = float(zCoord) / 16777216.0;\n" :
                                           "\tdepth = 1.0 - float(zCoord) / 16777216.0;\n";
    // With an early depth test, z textures never reach the depth buffer.
    const EmulatedZ ztest = m_uid.ztest;
    const bool early = ztest == EmulatedZ::Early || ztest == EmulatedZ::EarlyWithFBFetch ||
                       ztest == EmulatedZ::EarlyWithZComplocHack;
    if (m_uid.per_pixel_depth && early)
      Put(m_out, depth_out);

    // The z texture comes from the last stage's texture. It still matters for fog, so it is only
    // skipped when neither depth nor fog use it.
    const bool ztexture_used = m_uid.per_pixel_depth || m_uid.fog_fsel != FogType::Off;
    if (m_uid.ztex_op != ZTexOp::Disabled && ztexture_used)
    {
      m_out.Write("\tzCoord = idot({0}[0].xyzw, frag_output.last_texture.xyzw) + {0}[1].w {1};\n",
                  I_ZBIAS, m_uid.ztex_op == ZTexOp::Add ? "+ zCoord" : "");
      Put(m_out, "\tzCoord = zCoord & 0xFFFFFF;\n");
    }

    if (m_uid.per_pixel_depth && ztest == EmulatedZ::Late)
      Put(m_out, depth_out);
  }

  void WriteMain(bool custom_shader)
  {
    Put(m_out, "void main()\n{\n\tfloat4 rawpos = gl_FragCoord;\n");
    if (m_fb_fetch)
    {
      // The framebuffer value is read once up front and the result written once at the end
      // (Adreno drivers dislike repeated access to the inout variable).
      Put(m_out, "#ifdef FB_FETCH_VALUE\n\tfloat4 initial_ocol0 = FB_FETCH_VALUE;\n"
                 "#else\n\tfloat4 initial_ocol0 = real_ocol0;\n#endif\n");
      Put(m_out, "\tfloat4 ocol0;\n");
    }
    if (m_uid.blend_enable)
      Put(m_out, "\tfloat4 ocol1;\n");

    if (!m_host.backend_geometry_shaders || !m_host.stereo)
      Put(m_out, "\tint layer = 0;\n");
    else if (m_host.backend_gl_layer_in_fs)
      Put(m_out, "\tint layer = gl_Layer;\n");

    WriteFragmentInput(custom_shader);
    Put(m_out, "\tDolphinFragmentOutput frag_output;\n"
               "\tprocess_fragment(frag_input, frag_output);\n"
               "\tivec4 prev = frag_output.main & 255;\n");

    const bool use_dual_source = !m_uid.no_dual_src || m_uid.blend_enable;
    if (NeedsAlphaTest(m_uid.Pretest, m_uid.ztest))
      WriteAlphaTest(m_out, m_uid, m_api, m_uid.per_pixel_depth, use_dual_source);

    // An alpha of 1 passes alpha tests but blends like 0 (Mario Kart Wii menus, Fortune Street).
    Put(m_out, "\t// Hardware testing indicates that an alpha of 1 can pass an alpha test,\n"
               "\t// but doesn't do anything in blending\n"
               "\tif (prev.a == 1) prev.a = 0;\n");

    WriteDepth();

    if (m_uid.dither)
    {
      // 2x2 Bayer matrix for the 6-bit formats.
      Put(m_out, "\tint2 dither = int2(rawpos.xy) & 1;\n"
                 "\tprev.rgb = (prev.rgb - (prev.rgb >> 6)) + (dither.x ^ dither.y) * 2 + "
                 "dither.y;\n");
    }

    WriteFog(m_out, m_uid);
    if (m_uid.logic_op_enable)
      WriteLogicOp(m_out, m_uid);
    else if (m_uid.emulate_logic_op_with_blend)
      WriteLogicOpBlend(m_out, m_uid);

    WriteColor(m_out, m_uid, use_dual_source);
    if (m_uid.blend_enable)
      WriteBlend(m_out, m_uid);
    else if (m_fb_fetch)
      Put(m_out, "\treal_ocol0 = ocol0;\n");

    if (m_uid.bounding_box)
      Put(m_out, "\tUpdateBoundingBox(rawpos.xy);\n");
    Put(m_out, "}\n");
  }

  const APIType m_api;
  const ShaderHostConfig& m_host;
  const pixel_shader_uid_data& m_uid;
  ShaderCode& m_out;
  // Varyings and lighting follow the live setting; the body follows the host config.
  const bool m_pixel_lighting;
  const bool m_fb_fetch;
};
}  // namespace

PixelShaderUid GetPixelShaderUid()
{
  PixelShaderUid uid;
  pixel_shader_uid_data& key = *uid.GetUidData();

  const bool rgba6 = bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
  key.useDstAlpha = bpmem.dstalpha.enable && bpmem.blendmode.alpha_update && rgba6;
  key.genMode_numindstages = bpmem.genMode.numindstages;
  key.genMode_numtevstages = bpmem.genMode.numtevstages;
  key.genMode_numtexgens = bpmem.genMode.numtexgens;
  key.bounding_box = g_ActiveConfig.bBBoxEnable && g_bounding_box->IsEnabled();
  key.rgba6_format = rgba6 && !g_ActiveConfig.bForceTrueColor;
  key.dither = bpmem.blendmode.dither && key.rgba6_format;
  key.uint_output = bpmem.blendmode.UseLogicOp();

  const AlphaTestResult pretest = bpmem.alpha_test.TestResult();
  key.Pretest = pretest;

  // An early depth test is forced when the alpha test cannot discard reliably anyway (or fast
  // depth is on), except with zfreeze, whose depth is computed per pixel.
  EmulatedZ ztest = bpmem.GetEmulatedZ();
  if (ztest == EmulatedZ::Early && !bpmem.genMode.zfreeze &&
      (g_ActiveConfig.bFastDepthCalc || pretest == AlphaTestResult::Undetermined))
  {
    ztest = EmulatedZ::ForcedEarly;
  }
  key.ztest = ztest;

  const bool depth_test = bpmem.zmode.test_enable;
  key.per_pixel_depth =
      (bpmem.ztex2.op != ZTexOp::Disabled && ztest == EmulatedZ::Late) ||
      (depth_test && !g_ActiveConfig.bFastDepthCalc && ztest != EmulatedZ::ForcedEarly) ||
      (depth_test && bpmem.genMode.zfreeze);

  if (g_ActiveConfig.bEnablePixelLighting)
  {
    key.numColorChans = xfmem.numChan.numColorChans;
    GetLightingShaderUid(key.lighting);
  }

  // Perspective divide bit per texgen (only eight fit; the XF state has no more anyway).
  const u32 texgens = std::min<u32>(key.genMode_numtexgens, 8);
  for (u32 i = 0; i < texgens; ++i)
  {
    key.texMtxInfo_n_projection |= static_cast<u32>(xfmem.texMtxInfo[i].projection.Value()) << i;
  }

  const u32 stages = key.genMode_numtevstages + 1;
  u32 indirect_used = 0;
  for (u32 n = 0; n < stages; ++n)
  {
    if (bpmem.tevind[n].IsActive())
      indirect_used |= 1u << bpmem.tevind[n].bt;
  }
  key.nIndirectStagesUsed = indirect_used;
  for (u32 i = 0; i < key.genMode_numindstages; ++i)
  {
    if ((indirect_used & (1u << i)) != 0)
    {
      const int stage = static_cast<int>(i);
      key.SetTevindrefValues(stage, bpmem.tevindref.getTexCoord(stage),
                             bpmem.tevindref.getTexMap(stage));
    }
  }

  for (u32 n = 0; n < stages; ++n)
    FillStageKey(key.stagehash[n], n);

  // Without per-pixel lighting the key ends after the last used stage.
  if (g_ActiveConfig.bEnablePixelLighting)
  {
    key.num_values = sizeof(key);
  }
  else
  {
    key.num_values = static_cast<u32>(offsetof(pixel_shader_uid_data, stagehash) +
                                      stages * sizeof(key.stagehash[0]));
  }

  if (NeedsAlphaTest(pretest, ztest))
  {
    key.alpha_test_comp0 = bpmem.alpha_test.comp0;
    key.alpha_test_comp1 = bpmem.alpha_test.comp1;
    key.alpha_test_logic = bpmem.alpha_test.logic;
  }

  key.zfreeze = bpmem.genMode.zfreeze;
  key.ztex_op = bpmem.ztex2.op;
  key.fog_fsel = bpmem.fog.c_proj_fsel.fsel;
  key.fog_proj = bpmem.fog.c_proj_fsel.proj;
  key.fog_RangeBaseEnabled = bpmem.fogRange.Base.Enabled;

  return uid;
}

void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid)
{
  pixel_shader_uid_data& key = *uid->GetUidData();

  // Only D3D with logic op support needs integer outputs; OpenGL and Vulkan convert normalized
  // outputs to integer targets themselves.
  if (api_type != APIType::D3D || !host_config.backend_logic_op)
    key.uint_output = 0;

  // Keys cached while bounding box was on must not emit it once it is off.
  if (!host_config.bounding_box || !host_config.backend_bbox)
    key.bounding_box = 0;
}

void WritePixelShaderCommonHeader(ShaderCode& out, [[maybe_unused]] APIType api_type,
                                  const ShaderHostConfig& host_config, bool bounding_box)
{
  // Integer dot products.
  for (u32 n = 3; n <= 4; ++n)
  {
    std::string sum = "tmp.x";
    for (u32 k = 1; k < n; ++k)
      sum += fmt::format(" + tmp.{}", "xyzw"[k]);
    out.Write("int idot(int{0} x, int{0} y)\n{{\n\tint{0} tmp = x * y;\n\treturn {1};\n}}\n", n,
              sum);
  }
  Put(out, "\n");

  // Round and convert in one call.
  for (const std::string_view size : {" ", "2", "3", "4"})
    out.Write("int{0} iround(float{0} x) {{ return int{0}(round(x)); }}\n", size);
  Put(out, "\nSAMPLER_BINDING(0) uniform sampler2DArray samp[8];\n\n");

  WritePSBlock(out);

  if (host_config.per_pixel_lighting)
  {
    WriteLightStruct(out);
    Put(out, "UBO_BINDING(std140, 2) uniform VSBlock {\n");
    WriteVertexUniformMembers(out);
    Put(out, "};\n");
  }

  if (bounding_box)
    WriteBoundingBoxFunctions(out);

  if (host_config.manual_texture_sampling)
    WriteManualSamplingHelpers(out, host_config.manual_texture_sampling_custom_texture_sizes);

  Put(out, "\nint4 sampleTexture(uint texmap, in sampler2DArray tex, int2 uv, int layer) {\n");
  if (host_config.manual_texture_sampling)
  {
    WriteManualSampleTexture(out, host_config);
    return;
  }

  // Hardware sampling: texdim holds the size in texels, uv is in 1/128 texel units.
  for (const char axis : {'s', 't'})
  {
    out.Write("  float size_{} = float({}[texmap].{} * 128);\n", axis, I_TEXDIMS,
              axis == 's' ? 'x' : 'y');
  }
  Put(out, "  float3 coords = float3(float(uv.x) / size_s, float(uv.y) / size_t, layer);\n");
  if (host_config.backend_sampler_lod_bias)
  {
    Put(out, "  return iround(255.0 * texture(tex, coords));\n");
  }
  else
  {
    // The bias is applied in the shader when samplers cannot carry it.
    out.Write("  uint texmode0 = samp_texmode0(texmap);\n"
              "  float lod_bias = float({}) / 256.0f;\n"
              "  return iround(255.0 * texture(tex, coords, lod_bias));\n",
              BitfieldExtract<&SamplerState::TM0::lod_bias>("texmode0"));
  }
  Put(out, "}\n");
}

void WriteFragmentBody([[maybe_unused]] APIType api_type, const ShaderHostConfig& host_config,
                       const pixel_shader_uid_data* uid_data, ShaderCode& out)
{
  const pixel_shader_uid_data& uid = *uid_data;
  Put(out, "\tvec4 col0 = frag_input.color_0;\n"
           "\tvec4 col1 = frag_input.color_1;\n"
           "\tint layer = frag_input.layer;\n");

  // TEV registers and temporaries.
  out.Write("\tint4 c0 = {0}[1], c1 = {0}[2], c2 = {0}[3], prev = {0}[0];\n", I_COLORS);
  Put(out, "\tint4 rastemp = int4(0, 0, 0, 0), rawtextemp = int4(0, 0, 0, 0), "
           "textemp = int4(0, 0, 0, 0), konsttemp = int4(0, 0, 0, 0);\n");
  Put(out, "\tint3 comp16 = int3(1, 256, 0), comp24 = int3(1, 256, 256*256);\n"
           "\tint alphabump=0;\n\tint3 tevcoord=int3(0, 0, 0);\n"
           "\tint2 wrappedcoord=int2(0,0), tempcoord=int2(0,0);\n\tint4 ");
  for (const char input : {'a', 'b', 'c', 'd'})
    out.Write("{}tevin_{}=int4(0,0,0,0)", input == 'a' ? "" : ",", input);
  Put(out, ";\n\n");

  if (host_config.per_pixel_lighting)
  {
    // Channels beyond numColorChans read as zero.
    for (u32 chan = 0; chan < 2; ++chan)
    {
      if (chan == 0 ? uid.numColorChans != 0 : uid.numColorChans == 2)
      {
        out.Write("\tcol{0} = dolphin_calculate_lighting_chn{0}(col{0}, frag_input.position, "
                  "frag_input.normal);\n",
                  chan);
      }
      else
      {
        out.Write("\tcol{} = vec4(0.0, 0.0, 0.0, 0.0);\n", chan);
      }
    }
  }

  // Texture coordinates in 1/128 texel fixed point (with the perspective divide).
  if (uid.genMode_numtexgens == 0)
  {
    // Keeps out-of-range coordinate references (which read coordinate 0) compiling.
    Put(out, "\tint2 fixpoint_uv0 = int2(0, 0);\n\n");
  }
  for (u32 i = 0; i < uid.genMode_numtexgens; ++i)
  {
    out.Write("\tint2 fixpoint_uv{0} = int2((frag_input.tex{0}.z == 0.0 ? frag_input.tex{0}.xy : "
              "frag_input.tex{0}.xy / frag_input.tex{0}.z) * float2({1}[{0}].zw * 128));\n",
              i, I_TEXDIMS);
  }

  // Indirect lookups.
  for (u32 i = 0; i < uid.genMode_numindstages; ++i)
  {
    if ((uid.nIndirectStagesUsed & (1u << i)) == 0)
      continue;
    const int stage = static_cast<int>(i);
    // A missing coordinate reads coordinate 0 (the Mario portrait in Luigi's Mansion, bug 11462).
    u32 texcoord = uid.GetTevindirefCoord(stage);
    if (texcoord >= uid.genMode_numtexgens)
      texcoord = 0;
    out.Write("\ttempcoord = fixpoint_uv{} >> {}[{}].{};\n", texcoord, I_INDTEXSCALE, i / 2,
              (i & 1) != 0 ? "zw" : "xy");
    out.Write("\tint3 iindtex{} = sampleTextureWrapper({}u, tempcoord, layer).abg;\n", i,
              uid.GetTevindirefMap(stage));
  }

  const u32 stages = uid.genMode_numtevstages + 1;
  for (u32 n = 0; n < stages; ++n)
    WriteStage(out, uid, n);

  // The last stage's result is the output, whatever register it was written to.
  TevStageCombiner::ColorCombiner last_cc;
  TevStageCombiner::AlphaCombiner last_ac;
  last_cc.hex = uid.stagehash[uid.genMode_numtevstages].cc;
  last_ac.hex = uid.stagehash[uid.genMode_numtevstages].ac;
  if (last_cc.dest != TevOutput::Prev)
    out.Write("\tprev.rgb = {};\n", ColorOutput(last_cc.dest));
  if (last_ac.dest != TevOutput::Prev)
    out.Write("\tprev.a = {};\n", AlphaOutput(last_ac.dest));

  Put(out, "\tfrag_output.last_texture = rawtextemp;\n\tfrag_output.main = prev;\n");
}

ShaderCode GeneratePixelShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                   const pixel_shader_uid_data* uid_data,
                                   CustomPixelContents custom_contents)
{
  ShaderCode out;
  PixelShaderWriter(api_type, host_config, *uid_data, out).Write(custom_contents);
  return out;
}
