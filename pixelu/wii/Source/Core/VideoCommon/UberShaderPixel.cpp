// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/UberShaderPixel.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "Common/Assert.h"
#include "Common/EnumMap.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/UberShaderCommon.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
namespace
{
void Put(ShaderCode& out, std::string_view text)
{
  out.Write("{}", text);
}

// WriteSwitch over a table given as plain strings, case k being cases[k].
template <auto last_member, std::size_t N>
void WriteCases(ShaderCode& out, APIType api_type, std::string_view variable,
                const std::array<std::string, N>& cases, int indent)
{
  static_assert(N == static_cast<std::size_t>(last_member) + 1);
  using Key = decltype(last_member);
  Common::EnumMap<std::string_view, last_member> table;
  for (std::size_t k = 0; k < N; ++k)
    table[static_cast<Key>(k)] = cases[k];
  WriteSwitch(out, api_type, variable, table, indent, true);
}

constexpr u32 Bit(std::size_t position)
{
  return 1u << position;
}

// One of the 16 indices 0..count-1 picked by `var`, as nested ifs over pairs (count <= 8 is
// expected; higher counts reuse the last pair). With D3D a switch is used instead.
template <typename Leaf>
void WriteIndexSelect(ShaderCode& out, APIType api_type, std::string_view var, u32 count,
                      Leaf leaf)
{
  if (api_type == APIType::D3D)
  {
    out.Write("  switch ({}) {{\n", var);
    for (u32 i = 0; i < count; ++i)
      out.Write("  case {}u:\n    return {};\n", i, leaf(i));
    out.Write("  default:\n    return {};\n  }}\n", leaf(0));
    return;
  }

  out.Write("  if ({} >= {}u) {{\n    return {};\n  }}\n", var, count, leaf(0));
  // Leaves sit at six spaces whatever the nesting depth.
  const auto pair = [&](u32 first) {
    if (count > first + 1)
      out.Write("      return ({} == {}u) ? {} : {};\n", var, first, leaf(first), leaf(first + 1));
    else
      out.Write("      return {};\n", leaf(first));
  };
  // A group of four: the two pairs split at first + 2.
  const auto quad = [&](u32 first, std::string_view outer_indent) {
    const bool split = count > first + 2;
    if (split)
      out.Write("    if ({} < {}u) {{\n", var, first + 2);
    pair(first);
    if (split)
    {
      Put(out, "    } else {\n");
      pair(first + 2);
      Put(out, "    }\n");
    }
    (void)outer_indent;
  };
  if (count > 4)
    out.Write("  if ({} < 4u) {{\n", var);
  quad(0, "  ");
  if (count > 4)
  {
    Put(out, "  } else {\n");
    quad(4, "  ");
    Put(out, "  }\n");
  }
}

// The blend factor expression (with its ';'), as in the specialised shader.
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
  const std::string_view source =
      pair == 2 ? "src_color" : (pair == 1 && destination ? "ocol0" : "initial_ocol0");
  const std::string_view swizzle = alpha ? "a" : (pair == 1 ? "rgb" : "aaa");
  if (inverted)
    return fmt::format("{} - {}.{};", alpha ? "1.0" : "float3(1,1,1)", source, swizzle);
  return fmt::format("{}.{};", source, swizzle);
}

class UberPixelWriter
{
public:
  UberPixelWriter(APIType api, const ShaderHostConfig& host, const pixel_ubershader_uid_data& uid,
                  ShaderCode& out)
      : m_api(api), m_host(host), m_uid(uid), m_out(out), m_texgens(uid.num_texgens),
        m_fb_fetch(host.backend_shader_framebuffer_fetch),
        m_dual_source(host.backend_dual_source_blend && !uid.no_dual_src)
  {
  }

  void Write()
  {
    ASSERT_MSG(VIDEO, !(m_dual_source && m_fb_fetch),
               "If you're using framebuffer fetch, you shouldn't need dual source blend!");

    m_out.Write("// {}\n", m_uid);
    WriteBitfieldExtractHeader(m_out, m_api, m_host);
    WritePixelShaderCommonHeader(m_out, m_api, m_host, m_host.bounding_box);
    if (m_host.per_pixel_lighting)
      WriteLightingFunction(m_out);

    WriteInterface();
    if (m_texgens > 0)
      WriteTexCoordSelection();
    WriteHelpers();
    WriteSelectFunctions();

    // The fixed-point coordinates are locals of main(), so they are passed to the selector.
    if (m_texgens > 0)
    {
      Put(m_out, "#define getTexCoord(index) selectTexCoord((index)");
      for (u32 i = 0; i < m_texgens; ++i)
        m_out.Write(", fixpoint_uv{}", i);
      Put(m_out, ")\n\n");
    }
    if (m_uid.early_depth && m_host.backend_early_z)
      Put(m_out, "FORCE_EARLY_Z;\n");

    WriteMain();
    WriteRasAndKonst();
  }

private:
  void WriteInterface()
  {
    const std::string_view color_type = m_uid.uint_output ? "uvec4" : "vec4";
    if (m_fb_fetch)
      Put(m_out, "FRAGMENT_OUTPUT_LOCATION(0) FRAGMENT_INOUT vec4 real_ocol0;\n");
    else
      m_out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out {} ocol0;\n", color_type);
    if (m_dual_source)
      m_out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out {} ocol1;\n", color_type);

    if (m_uid.per_pixel_depth)
      Put(m_out, "#define depth gl_FragDepth\n");

    const bool msaa = m_host.msaa;
    const bool ssaa = m_host.ssaa;
    if (m_host.backend_geometry_shaders)
    {
      Put(m_out, "VARYING_LOCATION(0) in VertexData {\n");
      GenerateVSOutputMembers(m_out, m_api, m_texgens, m_host,
                              GetInterpolationQualifier(msaa, ssaa, true, true),
                              ShaderStage::Pixel);
      Put(m_out, "};\n\n");
      if (m_host.stereo && !m_host.backend_gl_layer_in_fs)
        Put(m_out, "flat in int layer;");
      return;
    }

    const char* qualifier = GetInterpolationQualifier(msaa, ssaa);
    u32 location = 0;
    const auto varying = [&](std::string_view type, std::string_view name) {
      m_out.Write("VARYING_LOCATION({}) {} in {} {};\n", location++, qualifier, type, name);
    };
    varying("float4", "colors_0");
    varying("float4", "colors_1");
    for (u32 i = 0; i < m_texgens; ++i)
      varying("float3", fmt::format("tex{}", i));
    if (!m_host.fast_depth_calc)
      varying("float4", "clipPos");
    if (m_host.per_pixel_lighting)
    {
      varying("float3", "Normal");
      varying("float3", "WorldPos");
    }
  }

  // selectTexCoord() and selectTexCoordIndex(): a texture coordinate that does not exist reads
  // coordinate 0 (the Mario portrait in Luigi's Mansion, bug 11462).
  void WriteTexCoordSelection()
  {
    Put(m_out, "int2 selectTexCoord(uint index");
    for (u32 i = 0; i < m_texgens; ++i)
      m_out.Write(", int2 fixpoint_uv{}", i);
    Put(m_out, ") {\n");
    WriteIndexSelect(m_out, m_api, "index", m_texgens,
                     [](u32 i) { return fmt::format("fixpoint_uv{}", i); });
    Put(m_out, "}\n\nuint selectTexCoordIndex(uint texmap){\n");
    WriteIndexSelect(m_out, m_api, "texmap", m_texgens, [](u32 i) { return fmt::format("{}u", i); });
    Put(m_out, "}\n\n");
  }

  void WriteHelpers()
  {
    // Sampler arrays can only be indexed dynamically on some backends.
    if (m_host.backend_dynamic_sampler_indexing)
    {
      Put(m_out, "int4 sampleTextureWrapper(uint texmap, int2 uv, int layer) {\n"
                 "  return sampleTexture(texmap, samp[texmap], uv, layer);\n}\n\n");
    }
    else
    {
      Put(m_out, R"(int4 sampleTextureWrapper(uint sampler_num, int2 uv, int layer) {
  // This is messy, but DirectX, OpenGL 3.3, and OpenGL ES 3.0 don't support dynamic indexing of the sampler array
  // With any luck the shader compiler will optimise this if the hardware supports dynamic indexing.
  switch(sampler_num) {
)");
      for (u32 i = 0; i < 8; ++i)
        m_out.Write("  case {0}u: return sampleTexture({0}u, samp[{0}u], uv, layer);\n", i);
      Put(m_out, "  }\n}\n\n");
    }

    // Channel swapping through the swap tables in tevksel.
    Put(m_out, R"(int4 Swizzle(uint s, int4 color) {
  // AKA: Color Channel Swapping

  int4 ret;
)");
    const std::array<std::pair<char, std::string>, 4> swaps = {{
        {'r', BitfieldExtract<&TevKSel::swap_rb>("bpmem_tevksel(s * 2u)")},
        {'g', BitfieldExtract<&TevKSel::swap_ga>("bpmem_tevksel(s * 2u)")},
        {'b', BitfieldExtract<&TevKSel::swap_rb>("bpmem_tevksel(s * 2u + 1u)")},
        {'a', BitfieldExtract<&TevKSel::swap_ga>("bpmem_tevksel(s * 2u + 1u)")},
    }};
    for (const auto& [channel, index] : swaps)
      m_out.Write("  ret.{} = color[{}];\n", channel, index);
    Put(m_out, "  return ret;\n}\n\n");

    // Indirect wrapping: off, 256 down to 16, or zero.
    Put(m_out, R"(int Wrap(int coord, uint mode) {
  if (mode == 0u) // ITW_OFF
    return coord;
  else if (mode < 6u) // ITW_256 to ITW_16
    return coord & (0xfffe >> mode);
  else // ITW_0
    return 0;
}

)");

    // The TEV lerp for int and int3 operands; see the specialised shader for the arithmetic.
    for (const std::string_view width : {"", "3"})
    {
      m_out.Write(R"(// TEV's Linear Interpolate, plus bias, add/subtract and scale
int{0} tevLerp{0}(int{0} A, int{0} B, int{0} C, int{0} D, uint bias, bool op, uint scale) {{
 // Scale C from 0..255 to 0..256
  C += C >> 7;

 // Add bias to D
  if (bias == 1u) D += 128;
  else if (bias == 2u) D -= 128;

  int{0} lerp = (A << 8) + (B - A)*C;
  if (scale != 3u) {{
    lerp = lerp << scale;
    D = D << scale;
  }}

  // This rounding bias is not added when the scale is divide by 2
  if (scale != 3u)
    lerp = lerp + (op ? 127 : 128);

  int{0} result = lerp >> 8;

  // Add/Subtract D
  if (op) // Subtract
    result = D - result;
  else // Add
    result = D + result;

  // Most of the Scale was moved inside the lerp for improved precision
  // But we still do the divide by 2 here
  if (scale == 3u)
    result = result >> 1;
  return result;
}}

)",
                  width);
    }

    // Compare operations 0-5 (shared by colour and alpha): R8, GR16 and BGR24, each GT then EQ.
    Put(m_out, "// Implements operations 0-5 of TEV's compare mode,\n"
               "// which are common to both color and alpha channels\n"
               "bool tevCompare(uint op, int3 color_A, int3 color_B) {\n  switch (op) {\n");
    Put(m_out, "  case 0u: // TevCompareMode::R8, TevComparison::GT\n"
               "    return (color_A.r > color_B.r);\n"
               "  case 1u: // TevCompareMode::R8, TevComparison::EQ\n"
               "    return (color_A.r == color_B.r);\n");
    struct Wide
    {
      std::string_view mode;
      u32 bits;
      std::string_view eq_test;
    };
    static constexpr std::array<Wide, 2> wide = {{
        {"GR16", 16, "color_A.r == color_B.r && color_A.g == color_B.g"},
        {"BGR24", 24, "color_A.r == color_B.r && color_A.g == color_B.g && color_A.b == color_B.b"},
    }};
    u32 op = 2;
    for (const Wide& w : wide)
    {
      std::string packed_a = "color_A.r | (color_A.g << 8)";
      std::string packed_b = "color_B.r | (color_B.g << 8)";
      if (w.bits == 24)
      {
        packed_a += " | (color_A.b << 16)";
        packed_b += " | (color_B.b << 16)";
      }
      m_out.Write("  case {0}u: // TevCompareMode::{1}, TevComparison::GT\n"
                  "    int A_{2} = ({3});\n    int B_{2} = ({4});\n    return A_{2} > B_{2};\n",
                  op, w.mode, w.bits, packed_a, packed_b);
      m_out.Write("  case {}u: // TevCompareMode::{}, TevComparison::EQ\n    return ({});\n", op + 1,
                  w.mode, w.eq_test);
      op += 2;
    }
    Put(m_out, "  default:\n    return false;\n  }\n}\n\n");

    Put(m_out, R"(struct State {
  int4 Reg[4];
  int4 RawTexColor;
  int4 TexColor;
  int AlphaBump;
};
struct StageState {
  uint stage;
  uint order;
  uint cc;
  uint ac;
};

int4 getRasColor(State s, StageState ss, float4 colors_0, float4 colors_1);
int4 getKonstColor(State s, StageState ss);

)");
  }

  // Run-time selection of comparisons, TEV inputs and registers.
  void WriteSelectFunctions()
  {
    std::array<std::string, 8> compares;
    static constexpr std::array<std::string_view, 6> compare_ops = {"< ", "==", "<=",
                                                                    "> ", "!=", ">="};
    compares[0] = "return false;";
    for (u32 k = 0; k < 6; ++k)
      compares[k + 1] = fmt::format("return a {} b;", compare_ops[k]);
    compares[7] = "return true;";

    static constexpr std::string_view ras = "getRasColor(s, ss, colors_0, colors_1)";
    std::array<std::string, 16> color_inputs;
    for (u32 k = 0; k < 12; ++k)
    {
      const std::string_view swizzle = (k & 1) != 0 ? "aaa" : "rgb";
      std::string source;
      if (k < 8)
        source = fmt::format("s.Reg[{}]", k / 2);
      else if (k < 10)
        source = "s.TexColor";
      else
        source = ras;
      color_inputs[k] = fmt::format("return {}.{};", source, swizzle);
    }
    color_inputs[12] = "return int3(255, 255, 255);";
    color_inputs[13] = "return int3(128, 128, 128);";
    color_inputs[14] = "return getKonstColor(s, ss).rgb;";
    color_inputs[15] = "return int3(0, 0, 0);";

    std::array<std::string, 8> alpha_inputs;
    for (u32 k = 0; k < 4; ++k)
      alpha_inputs[k] = fmt::format("return s.Reg[{}].a;", k);
    alpha_inputs[4] = "return s.TexColor.a;";
    alpha_inputs[5] = fmt::format("return {}.a;", ras);
    alpha_inputs[6] = "return getKonstColor(s, ss).a;";
    alpha_inputs[7] = "return 0;";

    std::array<std::string, 4> registers;
    for (u32 k = 0; k < 4; ++k)
      registers[k] = fmt::format("return s.Reg[{}];", k);

    Put(m_out, "// Helper function for Alpha Test\nbool alphaCompare(int a, int b, uint compare) {\n");
    WriteCases<CompareMode::Always>(m_out, m_api, "compare", compares, 2);
    Put(m_out, "}\n\nint3 selectColorInput(State s, StageState ss, float4 colors_0, "
               "float4 colors_1, uint index) {\n");
    WriteCases<TevColorArg::Zero>(m_out, m_api, "index", color_inputs, 2);
    Put(m_out, "}\n\nint selectAlphaInput(State s, StageState ss, float4 colors_0, "
               "float4 colors_1, uint index) {\n");
    WriteCases<TevAlphaArg::Zero>(m_out, m_api, "index", alpha_inputs, 2);
    Put(m_out, "}\n\nint4 getTevReg(in State s, uint index) {\n");
    WriteCases<TevOutput::Color2>(m_out, m_api, "index", registers, 2);
    Put(m_out, "}\n\n");
  }

  // The indirect stage of one TEV stage, inside the main loop.
  void WriteIndirectStage()
  {
    m_out.Write("\n    uint tex_coord = {};\n",
                BitfieldExtract<&TwoTevStageOrders::texcoord_even>("ss.order"));
    m_out.Write("    int2 fixedPoint_uv = getTexCoord(tex_coord);\n\n"
                "    bool texture_enabled = (ss.order & {}u) != 0u;\n",
                Bit(TwoTevStageOrders().enable_tex_even.StartBit()));
    m_out.Write("\n    // Indirect textures\n    uint tevind = bpmem_tevind(stage);\n"
                "    if (tevind != 0u)\n    {{\n      uint bs = {};\n",
                BitfieldExtract<&TevStageIndirect::bs>("tevind"));
    const std::array<std::pair<std::string_view, std::string>, 5> fields = {{
        {"fmt", BitfieldExtract<&TevStageIndirect::fmt>("tevind")},
        {"bias", BitfieldExtract<&TevStageIndirect::bias>("tevind")},
        {"bt", BitfieldExtract<&TevStageIndirect::bt>("tevind")},
        {"matrix_index", BitfieldExtract<&TevStageIndirect::matrix_index>("tevind")},
        {"matrix_id", BitfieldExtract<&TevStageIndirect::matrix_id>("tevind")},
    }};
    for (const auto& [name, value] : fields)
      m_out.Write("      uint {} = {};\n", name, value);
    Put(m_out, "      int2 indtevtrans = int2(0, 0);\n\n");

    // bpmem_iref is non-zero exactly when the indirect stage is valid; otherwise the offset is
    // skipped (undefined on console, Viewtiful Joe bug 12525) but wrapping still applies.
    Put(m_out, "      if (bpmem_iref(bt) != 0u) {\n        int3 indcoord;\n");
    m_out.Write("{{\n  uint iref = bpmem_iref(bt);\n"
                "  uint texcoord = bitfieldExtract(iref, 0, 3);\n"
                "  uint texmap = bitfieldExtract(iref, 8, 3);\n"
                "  int2 fixedPoint_uv = getTexCoord(texcoord);\n\n"
                "  if ((bt & 1u) == 0u)\n    fixedPoint_uv = fixedPoint_uv >> {0}[bt >> 1].xy;\n"
                "  else\n    fixedPoint_uv = fixedPoint_uv >> {0}[bt >> 1].zw;\n\n"
                "  indcoord = sampleTextureWrapper(texmap, fixedPoint_uv, layer).abg;\n}}\n",
                I_INDTEXSCALE);
    Put(m_out, "        if (bs != 0u)\n          s.AlphaBump = indcoord[bs - 1u];\n"
               "        switch(fmt)\n        {\n");

    // Per format: the offset shift (and bias value) and what happens to the bump alpha.
    struct Format
    {
      IndTexFormat format;
      u32 shift;
      std::string_view bump;
    };
    static constexpr std::array<Format, 4> formats = {{
        {IndTexFormat::ITF_8, 0, "s.AlphaBump & 0xf8"},
        {IndTexFormat::ITF_5, 3, "s.AlphaBump << 5"},
        {IndTexFormat::ITF_4, 4, "s.AlphaBump << 4"},
        {IndTexFormat::ITF_3, 5, "s.AlphaBump << 3"},
    }};
    for (const Format& f : formats)
    {
      m_out.Write("        case {:s}:\n", f.format);
      for (u32 c = 0; c < 3; ++c)
      {
        const char axis = "xyz"[c];
        if (f.shift == 0)
        {
          m_out.Write("          indcoord.{0} = indcoord.{0} + ((bias & {1}u) != 0u ? -128 : 0);\n",
                      axis, 1u << c);
        }
        else
        {
          m_out.Write("          indcoord.{0} = (indcoord.{0} >> {2}) + ((bias & {1}u) != 0u ? 1 : "
                      "0);\n",
                      axis, 1u << c, f.shift);
        }
      }
      m_out.Write("          s.AlphaBump = {};\n          break;\n", f.bump);
    }

    m_out.Write(R"(        }}

        // Matrix multiply
        if (matrix_index != 0u)
        {{
          uint mtxidx = 2u * (matrix_index - 1u);
          int shift = {0}[mtxidx].w;

          switch (matrix_id)
          {{
          case 0u: // 3x2 S0.10 matrix
            indtevtrans = int2(idot({0}[mtxidx].xyz, indcoord), idot({0}[mtxidx + 1u].xyz, indcoord)) >> 3;
            break;
          case 1u: // S matrix, S17.7 format
            indtevtrans = (fixedPoint_uv * indcoord.xx) >> 8;
            break;
          case 2u: // T matrix, S17.7 format
            indtevtrans = (fixedPoint_uv * indcoord.yy) >> 8;
            break;
          }}

          if (shift >= 0)
            indtevtrans = indtevtrans >> shift;
          else
            indtevtrans = indtevtrans << ((-shift) & 31);
        }}
      }}

      // Wrapping
)",
                I_INDTEXMTX);
    m_out.Write("      uint sw = {};\n", BitfieldExtract<&TevStageIndirect::sw>("tevind"));
    m_out.Write("      uint tw = {}; \n", BitfieldExtract<&TevStageIndirect::tw>("tevind"));
    m_out.Write("      int2 wrapped_coord = int2(Wrap(fixedPoint_uv.x, sw), Wrap(fixedPoint_uv.y, "
                "tw));\n\n      if ((tevind & {}u) != 0u) // add previous tevcoord\n",
                Bit(TevStageIndirect().fb_addprev.StartBit()));
    Put(m_out, R"(        tevcoord.xy += wrapped_coord + indtevtrans;
      else
        tevcoord.xy = wrapped_coord + indtevtrans;

      // Emulate s24 overflows
      tevcoord.xy = (tevcoord.xy << 8) >> 8;
    }
    else
    {
      tevcoord.xy = fixedPoint_uv;
    }

    // Sample texture for stage
    if (texture_enabled) {
)");
    m_out.Write("      uint sampler_num = {};\n\n",
                BitfieldExtract<&TwoTevStageOrders::texmap_even>("ss.order"));
    m_out.Write("      s.RawTexColor = sampleTextureWrapper(sampler_num, tevcoord.xy, layer);\n"
                "      uint swap = {};\n      s.TexColor = Swizzle(swap, s.RawTexColor);\n",
                BitfieldExtract<&TevStageCombiner::AlphaCombiner::tswap>("ss.ac"));
    Put(m_out, "    } else {\n      // Texture is disabled\n"
               "      s.TexColor = int4(255, 255, 255, 255);\n    }\n\n");
  }

  // Declarations of the combiner fields of one stage (prefix "color" or "alpha").
  template <typename Combiner>
  void WriteCombinerFields(std::string_view prefix, std::string_view reg)
  {
    struct Field
    {
      std::string_view name;
      bool boolean;
      std::string value;
    };
    const std::array<Field, 9> fields = {{
        {"a", false, BitfieldExtract<&Combiner::a>(reg)},
        {"b", false, BitfieldExtract<&Combiner::b>(reg)},
        {"c", false, BitfieldExtract<&Combiner::c>(reg)},
        {"d", false, BitfieldExtract<&Combiner::d>(reg)},
        {"bias", false, BitfieldExtract<&Combiner::bias>(reg)},
        {"op", true, BitfieldExtract<&Combiner::op>(reg)},
        {"clamp", true, BitfieldExtract<&Combiner::clamp>(reg)},
        {"scale", false, BitfieldExtract<&Combiner::scale>(reg)},
        {"dest", false, BitfieldExtract<&Combiner::dest>(reg)},
    }};
    for (const Field& f : fields)
    {
      if (f.boolean)
        m_out.Write("      bool {}_{} = bool({});\n", prefix, f.name, f.value);
      else
        m_out.Write("      uint {}_{} = {};\n", prefix, f.name, f.value);
    }
    m_out.Write("      uint {0}_compare_op = {0}_scale << 1 | uint({0}_op);\n\n", prefix);
  }

  void WriteCombiners(std::string_view lit)
  {
    std::array<std::string, 4> color_dest;
    std::array<std::string, 4> alpha_dest;
    for (u32 k = 0; k < 4; ++k)
    {
      color_dest[k] = fmt::format("s.Reg[{}].rgb = color;", k);
      alpha_dest[k] = fmt::format("s.Reg[{}].a = alpha;", k);
    }

    Put(m_out, "    // This is the Meat of TEV\n    {\n      // Color Combiner\n");
    WriteCombinerFields<TevStageCombiner::ColorCombiner>("color", "ss.cc");
    for (const char input : {'A', 'B', 'C'})
    {
      m_out.Write("      int3 color_{0} = selectColorInput(s, ss, {1}colors_0, {1}colors_1, "
                  "color_{2}) & int3(255, 255, 255);\n",
                  input, lit, static_cast<char>(input - 'A' + 'a'));
    }
    m_out.Write("      int3 color_D = selectColorInput(s, ss, {0}colors_0, {0}colors_1, color_d);"
                "  // 10 bits + sign\n\n",
                lit);
    Put(m_out, R"(      int3 color;
      if (color_bias != 3u) { // Normal mode
        color = tevLerp3(color_A, color_B, color_C, color_D, color_bias, color_op, color_scale);
      } else { // Compare mode
        // op 6 and 7 do a select per color channel
)");
    for (u32 op = 6; op <= 7; ++op)
    {
      const bool eq = op == 7;
      m_out.Write("        {}if (color_compare_op == {}u) {{\n"
                  "          // TevCompareMode::RGB8, TevComparison::{}\n",
                  eq ? "} else " : "", op, eq ? "EQ" : "GT");
      for (const char c : {'r', 'g', 'b'})
      {
        m_out.Write("          color.{0} = (color_A.{0} {1} color_B.{0}) ? color_C.{0} : 0;\n", c,
                    eq ? "==" : ">");
      }
    }
    Put(m_out, R"(        } else {
          // The remaining ops do one compare which selects all 3 channels
          color = tevCompare(color_compare_op, color_A, color_B) ? color_C : int3(0, 0, 0);
        }
        color = color_D + color;
      }

      // Clamp result
      if (color_clamp)
        color = clamp(color, 0, 255);
      else
        color = clamp(color, -1024, 1023);

      // Write result to the correct input register of the next stage
)");
    WriteCases<TevOutput::Color2>(m_out, m_api, "color_dest", color_dest, 6);
    Put(m_out, "\n      // Alpha Combiner\n");

    WriteCombinerFields<TevStageCombiner::AlphaCombiner>("alpha", "ss.ac");
    Put(m_out, "      int alpha_A = 0;\n      int alpha_B = 0;\n"
               "      if (alpha_bias != 3u || alpha_compare_op > 5u) {\n"
               "        // Small optimisation here: alpha_A and alpha_B are unused by compare ops "
               "0-5\n");
    for (const char input : {'A', 'B'})
    {
      m_out.Write("        alpha_{0} = selectAlphaInput(s, ss, {1}colors_0, {1}colors_1, "
                  "alpha_{2}) & 255;\n",
                  input, lit, static_cast<char>(input - 'A' + 'a'));
    }
    m_out.Write("      }};\n      int alpha_C = selectAlphaInput(s, ss, {0}colors_0, {0}colors_1, "
                "alpha_c) & 255;\n"
                "      int alpha_D = selectAlphaInput(s, ss, {0}colors_0, {0}colors_1, alpha_d); "
                "// 10 bits + sign\n\n",
                lit);
    Put(m_out, R"(
      int alpha;
      if (alpha_bias != 3u) { // Normal mode
        alpha = tevLerp(alpha_A, alpha_B, alpha_C, alpha_D, alpha_bias, alpha_op, alpha_scale);
      } else { // Compare mode
        if (alpha_compare_op == 6u) {
          // TevCompareMode::A8, TevComparison::GT
          alpha = (alpha_A > alpha_B) ? alpha_C : 0;
        } else if (alpha_compare_op == 7u) {
          // TevCompareMode::A8, TevComparison::EQ
          alpha = (alpha_A == alpha_B) ? alpha_C : 0;
        } else {
          // All remaining alpha compare ops actually compare the color channels
          alpha = tevCompare(alpha_compare_op, color_A, color_B) ? alpha_C : 0;
        }
        alpha = alpha_D + alpha;
      }

      // Clamp result
      if (alpha_clamp)
        alpha = clamp(alpha, 0, 255);
      else
        alpha = clamp(alpha, -1024, 1023);

      // Write result to the correct input register of the next stage
)");
    WriteCases<TevOutput::Color2>(m_out, m_api, "alpha_dest", alpha_dest, 6);
    Put(m_out, "    }\n    } // Main TEV loop\n\n");
  }

  void WriteDepth()
  {
    if (!m_host.fast_depth_calc)
    {
      m_out.Write("\tint zCoord = {0}[1].x + int((clipPos.z / clipPos.w) * float({0}[1].y));\n",
                  I_ZBIAS);
    }
    else
    {
      Put(m_out, m_host.backend_reversed_depth_range ?
                     "  int zCoord = int(rawpos.z * 16777216.0);\n" :
                     "  int zCoord = int((1.0 - rawpos.z) * 16777216.0);\n");
      Put(m_out, "  zCoord = clamp(zCoord, 0, 0xFFFFFF);\n\n");
    }

    const bool per_pixel_depth = m_uid.per_pixel_depth;
    if (per_pixel_depth)
    {
      // zfreeze disables early depth, so it only matters with per-pixel depth.
      m_out.Write("  // ZFreeze\n  if ((bpmem_genmode & {}u) != 0u) {{\n",
                  Bit(GenMode().zfreeze.StartBit()));
      m_out.Write("    float2 screenpos = rawpos.xy * {}.xy;\n", I_EFBSCALE);
      if (m_api == APIType::OpenGL)
      {
        Put(m_out, "    // OpenGL has reversed vertical screenspace coordinates\n"
                   "    screenpos.y = 528.0 - screenpos.y;\n");
      }
      m_out.Write("    zCoord = int({0}.z + {0}.x * screenpos.x + {0}.y * screenpos.y);\n }}\n\n",
                  I_ZSLOPE);
    }

    m_out.Write(R"(  // Depth Texture
  int early_zCoord = zCoord;
  if (bpmem_ztex_op != 0u) {{
    int ztex = int({0}[1].w); // fixed bias

    // Whatever texture was in our last stage, it's now our depth texture
    ztex += idot(s.RawTexColor.xyzw, {0}[0].xyzw);
    ztex += (bpmem_ztex_op == 1u) ? zCoord : 0;
    zCoord = ztex & 0xFFFFFF;
  }}

)",
                I_ZBIAS);

    if (per_pixel_depth)
    {
      Put(m_out, "  // If early depth is enabled, write to zbuffer before depth textures\n"
                 "  // If early depth isn't enabled, we write to the zbuffer here\n"
                 "  int zbuffer_zCoord = bpmem_late_ztest ? zCoord : early_zCoord;\n");
      Put(m_out, m_host.backend_reversed_depth_range ?
                     "  depth = float(zbuffer_zCoord) / 16777216.0;\n" :
                     "  depth = 1.0 - float(zbuffer_zCoord) / 16777216.0;\n");
    }
  }

  void WriteAlphaTestAndDither()
  {
    Put(m_out, "  // Alpha Test\n");
    // Drivers that break discard with early depth get the framebuffer value written back.
    if (m_uid.early_depth && DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DISCARD_WITH_EARLY_Z) &&
        m_host.backend_shader_framebuffer_fetch)
    {
      Put(m_out, "  #define discard_fragment { real_ocol0 = float4(initial_ocol0.xyz, 1.0); "
                 "return; }\n");
    }
    else
    {
      Put(m_out, "  #define discard_fragment discard\n");
    }

    m_out.Write("  if (bpmem_alphaTest != 0u) {{\n"
                "    bool comp0 = alphaCompare(TevResult.a, {0}.r, {1});\n"
                "    bool comp1 = alphaCompare(TevResult.a, {0}.g, {2});\n",
                I_ALPHA, BitfieldExtract<&AlphaTest::comp0>("bpmem_alphaTest"),
                BitfieldExtract<&AlphaTest::comp1>("bpmem_alphaTest"));
    m_out.Write("\n    // These if statements are written weirdly to work around intel and "
                "Qualcomm bugs with handling booleans.\n    switch ({}) {{\n",
                BitfieldExtract<&AlphaTest::logic>("bpmem_alphaTest"));
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> logic = {{
        {"AND", "&&"},
        {"OR", "||"},
        {"XOR", "!="},
        {"XNOR", "=="},
    }};
    for (u32 k = 0; k < 4; ++k)
    {
      m_out.Write("    case {}u: // {}\n"
                  "      if (comp0 {} comp1) break; else discard_fragment; break;\n",
                  k, logic[k].first, logic[k].second);
    }
    Put(m_out, "    }\n  }\n\n");

    Put(m_out, R"(  // Hardware testing indicates that an alpha of 1 can pass an alpha test,
  // but doesn't do anything in blending
  if (TevResult.a == 1) TevResult.a = 0;
  if (bpmem_dither) {
    // Flipper uses a standard 2x2 Bayer Matrix for 6 bit dithering
    // Here the matrix is encoded into the two factor constants
    int2 dither = int2(rawpos.xy) & 1;
    TevResult.rgb = (TevResult.rgb - (TevResult.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;
  }

)");
  }

  void WriteFog()
  {
    m_out.Write("  // Fog\n  uint fog_function = {};\n",
                BitfieldExtract<&FogParam3::fsel>("bpmem_fogParam3"));
    m_out.Write("  if (fog_function != {:s}) {{\n", FogType::Off);
    m_out.Write("    // TODO: This all needs to be converted from float to fixed point\n"
                "    float ze;\n    if ({} == 0u) {{\n",
                BitfieldExtract<&FogParam3::proj>("bpmem_fogParam3"));
    m_out.Write(R"(      // perspective
      // ze = A/(B - (Zs >> B_SHF)
      ze = ({0}.x * 16777216.0) / float({1}.y - (zCoord >> {1}.w));
    }} else {{
      // orthographic
      // ze = a*Zs    (here, no B_SHF)
      ze = {0}.x * float(zCoord) / 16777216.0;
    }}

)",
                I_FOGF, I_FOGI);
    m_out.Write("    if (bool({})) {{\n",
                BitfieldExtract<&FogRangeParams::RangeBase::Enabled>("bpmem_fogRangeBase"));
    m_out.Write(R"(      // x_adjust = sqrt((x-center)^2 + k^2)/k
      // ze *= x_adjust
      float offset = (2.0 * (rawpos.x / {0}.w)) - 1.0 - {0}.z;
      float floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
      uint indexlower = uint(floatindex);
      uint indexupper = indexlower + 1u;
      float klower = {1}[indexlower >> 2u][indexlower & 3u];
      float kupper = {1}[indexupper >> 2u][indexupper & 3u];
      float k = lerp(klower, kupper, frac(floatindex));
      float x_adjust = sqrt(offset * offset + k * k) / k;
      ze *= x_adjust;
    }}

    float fog = clamp(ze - {0}.y, 0.0, 1.0);

)",
                I_FOGF, I_FOGRANGE);

    m_out.Write("    if (fog_function >= {:s}) {{\n      switch (fog_function) {{\n", FogType::Exp);
    const std::array<std::pair<FogType, std::string_view>, 4> functions = {{
        {FogType::Exp, "fog = 1.0 - exp2(-8.0 * fog);\n"},
        {FogType::ExpSq, "fog = 1.0 - exp2(-8.0 * fog * fog);\n"},
        {FogType::BackwardsExp, "fog = exp2(-8.0 * (1.0 - fog));\n"},
        {FogType::BackwardsExpSq, "fog = 1.0 - fog;\n        fog = exp2(-8.0 * fog * fog);\n"},
    }};
    for (const auto& [type, body] : functions)
      m_out.Write("      case {:s}:\n        {}        break;\n", type, body);
    m_out.Write("      }}\n    }}\n\n    int ifog = iround(fog * 256.0);\n"
                "    TevResult.rgb = (TevResult.rgb * (256 - ifog) + {}.rgb * ifog) >> 8;\n  }}\n\n",
                I_FOGCOLOR);
  }

  void WriteLogicOps()
  {
    if (m_fb_fetch)
    {
      // Indexed by LogicOp: Clear, And, AndReverse, Copy, AndInverted, NoOp, Xor, Or,
      // Nor, Equiv, Invert, OrReverse, CopyInverted, OrInverted, Nand, Set.
      static constexpr std::array<std::string_view, 16> expressions = {
          "int4(0, 0, 0, 0)",
          "TevResult & fb_value",
          "TevResult & ~fb_value",
          "TevResult",
          "~TevResult & fb_value",
          "fb_value",
          "TevResult ^ fb_value",
          "TevResult | fb_value",
          "~(TevResult | fb_value)",
          "~(TevResult ^ fb_value)",
          "~fb_value",
          "TevResult | ~fb_value",
          "~TevResult",
          "~TevResult | fb_value",
          "~(TevResult & fb_value)",
          "int4(255, 255, 255, 255)",
      };
      Put(m_out, "  // Logic Ops\n  if (logic_op_enable) {\n"
                 "    int4 fb_value = iround(initial_ocol0 * 255.0);    switch (logic_op_mode) {\n");
      for (std::size_t k = 0; k < expressions.size(); ++k)
        m_out.Write("      case {}u: TevResult = {}; break;\n", k, expressions[k]);
      Put(m_out, "    }\n    TevResult &= 0xff;\n  }\n");
      return;
    }
    if (m_host.backend_logic_op)
      return;

    // Without logic op support, blending approximates some ops given these source colours.
    Put(m_out, "  // Helpers for logic op blending approximations\n  if (logic_op_enable) {\n"
               "    switch (logic_op_mode) {\n");
    m_out.Write("      case {}: // Clear\n        TevResult = int4(0, 0, 0, 0);\n        break;\n",
                static_cast<u32>(LogicOp::Clear));
    m_out.Write("      case {}: // Copy Inverted\n        TevResult ^= 0xff;\n        break;\n",
                static_cast<u32>(LogicOp::CopyInverted));
    m_out.Write("      case {}: // Set\n      case {}: // Invert\n"
                "        TevResult = int4(255, 255, 255, 255);\n        break;\n",
                static_cast<u32>(LogicOp::Set), static_cast<u32>(LogicOp::Invert));
    Put(m_out, "      default:\n        break;\n    }\n  }\n");
  }

  void WriteOutputColor()
  {
    if (m_uid.uint_output)
    {
      Put(m_out, "  if (bpmem_rgba6_format)\n    ocol0 = uint4(TevResult & 0xFC);\n"
                 "  else\n    ocol0 = uint4(TevResult);\n\n");
      return;
    }
    Put(m_out, R"(  if (bpmem_rgba6_format)
    ocol0.rgb = float3(TevResult.rgb >> 2) / 63.0;
  else
    ocol0.rgb = float3(TevResult.rgb) / 255.0;

  if (bpmem_dstalpha != 0u)
)");
    m_out.Write("    ocol0.a = float({} >> 2) / 63.0;\n",
                BitfieldExtract<&ConstantAlpha::alpha>("bpmem_dstalpha"));
    // (The blank line keeps its two spaces.)
    Put(m_out, "  else\n    ocol0.a = float(TevResult.a >> 2) / 63.0;\n  \n");
    if (m_dual_source || m_fb_fetch)
    {
      Put(m_out, "  // Dest alpha override (dual source blending)\n"
                 "  // Colors will be blended against the alpha from ocol1 and\n"
                 "  // the alpha from ocol0 will be written to the framebuffer.\n"
                 "  ocol1 = float4(0.0, 0.0, 0.0, float(TevResult.a) / 255.0);\n");
    }
  }

  void WriteShaderBlend()
  {
    std::array<std::string, 8> src_rgb;
    std::array<std::string, 8> src_alpha;
    std::array<std::string, 8> dst_rgb;
    std::array<std::string, 8> dst_alpha;
    for (u32 f = 0; f < 8; ++f)
    {
      src_rgb[f] = "blend_src.rgb = " + BlendFactor(false, false, f);
      src_alpha[f] = "blend_src.a = " + BlendFactor(true, false, f);
      dst_rgb[f] = "blend_dst.rgb = " + BlendFactor(false, true, f);
      dst_alpha[f] = "blend_dst.a = " + BlendFactor(true, true, f);
    }
    Put(m_out, "  if (blend_enable) {\n    float4 src_color;\n    if (bpmem_dstalpha != 0u) {\n"
               "      src_color = ocol1;\n    } else {\n      src_color = ocol0;\n    }"
               "    float4 blend_src;\n");
    WriteCases<SrcBlendFactor::InvDstAlpha>(m_out, m_api, "blend_src_factor", src_rgb, 4);
    WriteCases<SrcBlendFactor::InvDstAlpha>(m_out, m_api, "blend_src_factor_alpha", src_alpha, 4);
    Put(m_out, "    float4 blend_dst;\n");
    WriteCases<DstBlendFactor::InvDstAlpha>(m_out, m_api, "blend_dst_factor", dst_rgb, 4);
    WriteCases<DstBlendFactor::InvDstAlpha>(m_out, m_api, "blend_dst_factor_alpha", dst_alpha, 4);

    Put(m_out, "    float4 blend_result;\n");
    for (const std::string_view part : {"rgb", "a"})
    {
      m_out.Write("    if (blend_subtract{0})\n"
                  "      blend_result.{1} = initial_ocol0.{1} * blend_dst.{1} - ocol0.{1} * "
                  "blend_src.{1};\n    else\n"
                  "      blend_result.{1} = initial_ocol0.{1} * blend_dst.{1} + ocol0.{1} * "
                  "blend_src.{1};\n",
                  part == "a" ? "_alpha" : "", part);
    }
    Put(m_out, "    real_ocol0 = blend_result;\n  } else {\n    real_ocol0 = ocol0;\n  }\n");
  }

  void WriteMain()
  {
    Put(m_out, "void main()\n{\n  float4 rawpos = gl_FragCoord;\n");
    m_out.Write("  uint num_stages = {};\n\n",
                BitfieldExtract<&GenMode::numtevstages>("bpmem_genmode"));
    if (m_fb_fetch)
    {
      // Read the framebuffer once and write it once (Adreno drivers dislike more).
      Put(m_out, "#ifdef FB_FETCH_VALUE\n  float4 initial_ocol0 = FB_FETCH_VALUE;\n#else\n"
                 "  float4 initial_ocol0 = real_ocol0;\n#endif\n  float4 ocol0;\n  float4 ocol1;\n");
    }
    if (!m_host.backend_geometry_shaders || !m_host.stereo)
      Put(m_out, "\tint layer = 0;\n");
    else if (m_host.backend_gl_layer_in_fs)
      Put(m_out, "\tint layer = gl_Layer;\n");

    Put(m_out, R"(  int3 tevcoord = int3(0, 0, 0);
  State s;
  s.TexColor = int4(0, 0, 0, 0);
  s.RawTexColor = int4(0, 0, 0, 0);
  s.AlphaBump = 0;

)");
    for (u32 k = 0; k < 4; ++k)
      m_out.Write("  s.Reg[{0}] = {1}[{0}];\n", k, I_COLORS);

    std::string_view lit;
    if (m_host.per_pixel_lighting)
    {
      Put(m_out, "  float4 lit_colors_0 = colors_0;\n  float4 lit_colors_1 = colors_1;\n"
                 "  float3 lit_normal = normalize(Normal.xyz);\n  float3 lit_pos = WorldPos.xyz;\n");
      WriteVertexLighting(m_out, m_api, "lit_pos", "lit_normal", "colors_0", "colors_1",
                          "lit_colors_0", "lit_colors_1");
      lit = "lit_";
      Put(m_out, R"(  // The number of colors available to TEV is determined by numColorChans.
  // Normally this is performed in the vertex shader after lighting,
  // but with per-pixel lighting, we need to perform it here.
  // TODO: Actually implement this for ubershaders
  // if (xfmem_numColorChans == 0u)
  //   o.colors_0 = float4(0.0, 0.0, 0.0, 0.0);
  // if (xfmem_numColorChans <= 1u)
  //   o.colors_1 = float4(0.0, 0.0, 0.0, 0.0);
)");
    }

    // Odd stages use the upper half of the tevorder register.
    const int odd_shift = static_cast<int>(TwoTevStageOrders().enable_tex_odd.StartBit() -
                                           TwoTevStageOrders().enable_tex_even.StartBit());
    m_out.Write(R"(  // Main tev loop
  for(uint stage = 0u; stage <= num_stages; stage++)
  {{
    StageState ss;
    ss.stage = stage;
    ss.cc = bpmem_combiners(stage).x;
    ss.ac = bpmem_combiners(stage).y;
    ss.order = bpmem_tevorder(stage>>1);
    if ((stage & 1u) == 1u)
      ss.order = ss.order >> {};

)",
                odd_shift);

    // Texturing is disabled without texgens (for now).
    if (m_texgens != 0)
    {
      for (u32 i = 0; i < m_texgens; ++i)
      {
        m_out.Write("    int2 fixpoint_uv{0} = int2((tex{0}.z == 0.0 ? tex{0}.xy : tex{0}.xy / "
                    "tex{0}.z) * float2({1}[{0}].zw * 128));\n",
                    i, I_TEXDIMS);
      }
      WriteIndirectStage();
    }
    WriteCombiners(lit);

    // The last stage's destination registers are the result.
    m_out.Write("  int4 TevResult;\n  TevResult.xyz = getTevReg(s, {}).xyz;\n"
                "  TevResult.w = getTevReg(s, {}).w;\n  TevResult &= 255;\n\n",
                BitfieldExtract<&TevStageCombiner::ColorCombiner::dest>(
                    "bpmem_combiners(num_stages).x"),
                BitfieldExtract<&TevStageCombiner::AlphaCombiner::dest>(
                    "bpmem_combiners(num_stages).y"));

    WriteDepth();
    WriteAlphaTestAndDither();
    WriteFog();
    WriteLogicOps();
    WriteOutputColor();
    if (m_host.bounding_box)
      Put(m_out, "  if (bpmem_bounding_box) {\n    UpdateBoundingBox(rawpos.xy);\n  }\n");
    if (m_fb_fetch)
      WriteShaderBlend();
    Put(m_out, "}\n\n");
  }

  void WriteRasAndKonst()
  {
    m_out.Write("int4 getRasColor(State s, StageState ss, float4 colors_0, float4 colors_1) {{\n"
                "  // Select Ras for stage\n  uint ras = {};\n",
                BitfieldExtract<&TwoTevStageOrders::colorchan_even>("ss.order"));
    m_out.Write("  if (ras < 2u) {{ // Lighting Channel 0 or 1\n"
                "    int4 color = iround(((ras == 0u) ? colors_0 : colors_1) * 255.0);\n"
                "    uint swap = {};\n    return Swizzle(swap, color);\n",
                BitfieldExtract<&TevStageCombiner::AlphaCombiner::rswap>("ss.ac"));
    Put(m_out, R"(  } else if (ras == 5u) { // Alpha Bump
    return int4(s.AlphaBump, s.AlphaBump, s.AlphaBump, s.AlphaBump);
  } else if (ras == 6u) { // Normalzied Alpha Bump
    int normalized = s.AlphaBump | s.AlphaBump >> 5;
    return int4(normalized, normalized, normalized, normalized);
  } else {
    return int4(0, 0, 0, 0);
  }
}

int4 getKonstColor(State s, StageState ss) {
  // Select Konst for stage
)");
    // (The TODO comment runs into the next one without a line break.)
    Put(m_out, "  // TODO: a switch case might be better here than an dynamically"
               "  // indexed uniform lookup\n"
               "  uint tevksel = bpmem_tevksel(ss.stage>>1);\n");
    m_out.Write("  if ((ss.stage & 1u) == 0u)\n    return int4(konstLookup[{}].rgb, konstLookup[{}].a);\n",
                BitfieldExtract<&TevKSel::kcsel_even>("tevksel"),
                BitfieldExtract<&TevKSel::kasel_even>("tevksel"));
    m_out.Write("  else\n    return int4(konstLookup[{}].rgb, konstLookup[{}].a);\n}}\n",
                BitfieldExtract<&TevKSel::kcsel_odd>("tevksel"),
                BitfieldExtract<&TevKSel::kasel_odd>("tevksel"));
  }

  const APIType m_api;
  const ShaderHostConfig& m_host;
  const pixel_ubershader_uid_data& m_uid;
  ShaderCode& m_out;
  const u32 m_texgens;
  const bool m_fb_fetch;
  const bool m_dual_source;
};
}  // namespace

PixelShaderUid GetPixelShaderUid()
{
  PixelShaderUid uid;
  pixel_ubershader_uid_data& key = *uid.GetUidData();

  const bool depth_test = bpmem.zmode.test_enable;
  const bool zfreeze = bpmem.genMode.zfreeze;
  const EmulatedZ ztest = bpmem.GetEmulatedZ();

  key.num_texgens = xfmem.numTexGen.numTexGens;
  // Early depth is only usable when the alpha test cannot discard reliably anyway (or fast depth
  // is on), and never with zfreeze, whose depth comes from the shader.
  const bool early = ztest == EmulatedZ::Early &&
                     (g_ActiveConfig.bFastDepthCalc ||
                      bpmem.alpha_test.TestResult() == AlphaTestResult::Undetermined) &&
                     !(depth_test && zfreeze);
  key.early_depth = early;
  key.per_pixel_depth = (bpmem.ztex2.op != ZTexOp::Disabled && ztest == EmulatedZ::Late) ||
                        (depth_test && !g_ActiveConfig.bFastDepthCalc && !early) ||
                        (depth_test && zfreeze);
  key.uint_output = bpmem.blendmode.UseLogicOp();
  return uid;
}

void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid)
{
  pixel_ubershader_uid_data& key = *uid->GetUidData();

  // With framebuffer fetch the shader blends itself; without dual-source support it cannot use
  // it. Otherwise dual source is always used unless the driver breaks it.
  if (host_config.backend_shader_framebuffer_fetch || !host_config.backend_dual_source_blend)
    key.no_dual_src = 1;
  else if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING))
    key.no_dual_src = 0;

  // Only D3D with logic op support needs integer outputs.
  if (api_type != APIType::D3D || !host_config.backend_logic_op)
    key.uint_output = 0;
}

ShaderCode GenPixelShader(APIType api_type, const ShaderHostConfig& host_config,
                          const pixel_ubershader_uid_data* uid_data)
{
  ShaderCode out;
  UberPixelWriter(api_type, host_config, *uid_data, out).Write();
  return out;
}

void EnumeratePixelShaderUids(const std::function<void(const PixelShaderUid&)>& callback)
{
  PixelShaderUid uid;
  pixel_ubershader_uid_data& key = *uid.GetUidData();
  for (u32 texgens = 0; texgens <= 8; ++texgens)
  {
    key.num_texgens = texgens;
    for (u32 early = 0; early < 2; ++early)
    {
      key.early_depth = early;
      for (u32 depth = 0; depth < 2; ++depth)
      {
        // Early depth tests and shader-written depth exclude each other.
        if (early != 0 && depth != 0)
          continue;
        key.per_pixel_depth = depth;
        for (u32 uint_output = 0; uint_output < 2; ++uint_output)
        {
          key.uint_output = uint_output;
          for (u32 single_source = 0; single_source < 2; ++single_source)
          {
            key.no_dual_src = single_source;
            callback(uid);
          }
        }
      }
    }
  }
}
}  // namespace UberShader
