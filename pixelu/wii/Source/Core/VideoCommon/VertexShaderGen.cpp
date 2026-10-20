// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexShaderGen.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
using TexGenInfo = std::remove_reference_t<decltype(vertex_shader_uid_data::texGenInfo[0])>;

// Writes text that contains no format fields (single braces are literal here).
void Put(ShaderCode& out, std::string_view text)
{
  out.Write("{}", text);
}

constexpr std::string_view VERTEX_FUNCTION_PARAMS =
    "(in DolphinVertexInput vertex_input, out DolphinVertexOutput vertex_output)";

constexpr std::array<std::string_view, 3> XYZ = {"x", "y", "z"};

// The three rows of a 3x4 matrix stored as consecutive vec4s starting at `base`.
std::string RowRef(std::string_view array, std::string_view base, u32 row, std::string_view sep)
{
  if (row == 0)
    return fmt::format("{}[{}]", array, base);
  return fmt::format("{}[{}{}{}]", array, base, sep, row);
}

// Copies the texgen state of one texture coordinate generator into the key.
void FillTexGenKey(vertex_shader_uid_data& key, u32 index)
{
  TexGenInfo& gen = key.texGenInfo[index];
  const TexMtxInfo& info = xfmem.texMtxInfo[index];

  // The XF fields are a bit wider than the meaningful range; only the useful values are kept.
  ASSERT(info.sourcerow <= SourceRow::Tex7);
  gen.sourcerow = info.sourcerow;
  gen.inputform = (info.inputform == TexInputForm::ABC1) ? TexInputForm::ABC1 : TexInputForm::AB11;

  const TexGenType type = info.texgentype;
  if (type == TexGenType::EmbossMap || type == TexGenType::Color0 || type == TexGenType::Color1)
  {
    gen.is_regular_texgen = false;
    gen.other.texgentype = type;
    if (type != TexGenType::EmbossMap)
      return;
    gen.other.emboss_sourceshift = info.embosssourceshift;
    // Without tangent/binormal data the light shift cannot change the result.
    if ((key.components & (VB_HAS_TANGENT | VB_HAS_BINORMAL)) != 0)
      gen.other.emboss_lightshift = info.embosslightshift;
    return;
  }

  // Regular (and the undefined encodings, which behave like it).
  gen.is_regular_texgen = true;
  gen.regular.projection = info.projection;
  // The dual-transform flag only matters when at least one regular texgen exists.
  key.dualTexTrans_enabled = xfmem.dualTexTrans.enabled;
  if (key.dualTexTrans_enabled)
  {
    gen.regular.postmtx_index = xfmem.postMtxInfo[index].index;
    gen.regular.postmtx_normalize = xfmem.postMtxInfo[index].normalize;
  }
}

class VertexShaderWriter
{
public:
  VertexShaderWriter(APIType api, const ShaderHostConfig& host, const vertex_shader_uid_data& uid,
                     ShaderCode& out)
      : m_api(api), m_host(host), m_uid(uid), m_out(out),
        m_pixel_lighting(g_ActiveConfig.bEnablePixelLighting)
  {
  }

  void WriteShader(const CustomVertexContents& custom)
  {
    WriteUniforms(custom.uniforms);
    Put(m_out, "struct VS_OUTPUT {\n");
    GenerateVSOutputMembers(m_out, m_api, m_uid.numTexGens, m_host, "", ShaderStage::Vertex);
    Put(m_out, "};\n\n");
    WriteIsNanHeader(m_out, m_api);
    GenerateLightingShaderHeader(m_out, m_uid.lighting);

    if (Expanding())
      WriteBufferInputs();
    else
      WriteAttributeInputs();
    WriteOutputs();

    WriteExpandFetch();
    WriteMatrixFunctions();
    WriteTexCoordFunctions();
    WriteFeatureDefines();
    WriteVertexStructs();

    m_out.Write("void dolphin_process_emulated_vertex{}\n", VERTEX_FUNCTION_PARAMS);
    Put(m_out, "{\n");
    WriteBody();
    Put(m_out, "}\n");

    if (!custom.shader.empty())
    {
      m_out.Write("{}\n", custom.shader);
    }
    else
    {
      m_out.Write("void process_vertex{}\n", VERTEX_FUNCTION_PARAMS);
      Put(m_out, "{\n\tdolphin_process_emulated_vertex(vertex_input, vertex_output);\n}\n");
    }

    WriteMain();
  }

  // dolphin_process_emulated_vertex(): what the console's transform unit computes.
  void WriteBody()
  {
    Put(m_out, "\tvertex_output.position = vec4(vertex_input.position * "
               "dolphin_position_matrix(), 1.0);\n");
    Put(m_out, "\tvertex_output.normal = normalize(vertex_input.normal * "
               "dolphin_normal_matrix());\n");
    for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; ++chan)
    {
      m_out.Write("\tvec4 vertex_lighting_{0} = dolphin_calculate_lighting_chn{0}"
                  "(vertex_input.color_{0}, vertex_output.position.xyz, vertex_output.normal);\n"
                  "\tvertex_output.color_{0} = vertex_lighting_{0};\n",
                  chan);
    }

    if (m_host.per_pixel_lighting)
    {
      // The pixel shader lights the raw vertex colours itself.
      for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; ++chan)
        m_out.Write("\tvertex_output.color_{0} = vertex_input.color_{0};\n", chan);
    }
    else
    {
      // Channels beyond numColorChans still exist in the interface; they read as zero.
      for (u32 chan = m_uid.numColorChans; chan < NUM_XF_COLOR_CHANNELS; ++chan)
        m_out.Write("\tvertex_output.color_{} = vec4(0.0, 0.0, 0.0, 0.0);\n", chan);
    }

    for (u32 i = 0; i < m_uid.numTexGens; ++i)
      WriteTexGenOutput(i, m_uid.texGenInfo[i]);
    for (u32 i = m_uid.numTexGens; i < 8; ++i)
      m_out.Write("\tvertex_output.texture_coord_{} = vec3(0, 0, 0);\n", i);
  }

private:
  bool Has(u32 mask) const { return (m_uid.components & mask) != 0; }
  bool Expanding() const { return m_uid.vs_expand != VSExpand::None; }

  void WriteUniforms(std::string_view custom_uniforms)
  {
    WriteLightStruct(m_out);
    Put(m_out, "UBO_BINDING(std140, 2) uniform VSBlock {\n");
    WriteVertexUniformMembers(m_out);
    Put(m_out, "};\n");

    if (!custom_uniforms.empty())
    {
      Put(m_out, "UBO_BINDING(std140, 3) uniform CustomShaderBlock {\n");
      Put(m_out, custom_uniforms);
      Put(m_out, "} custom_uniforms;\n");
    }

    if (!Expanding())
      return;
    Put(m_out, "UBO_BINDING(std140, 4) uniform GSBlock {\n");
    WriteGeometryUniformMembers(m_out);
    Put(m_out, "};\n");
    // SV_VertexID does not include the base vertex on D3D, so it is passed in separately.
    if (m_api == APIType::D3D)
      Put(m_out, "UBO_BINDING(std140, 5) uniform DX_Constants {\n  uint base_vertex;\n};\n\n");
  }

  // Ordinary vertex attributes.
  void WriteAttributeInputs()
  {
    struct Attribute
    {
      u32 mask;
      ShaderAttrib slot;
      std::string_view decl;
    };
    static constexpr std::array<Attribute, 6> optional_attributes = {{
        {VB_HAS_POSMTXIDX, ShaderAttrib::PositionMatrix, "uint4 posmtx"},
        {VB_HAS_NORMAL, ShaderAttrib::Normal, "float3 rawnormal"},
        {VB_HAS_TANGENT, ShaderAttrib::Tangent, "float3 rawtangent"},
        {VB_HAS_BINORMAL, ShaderAttrib::Binormal, "float3 rawbinormal"},
        {VB_HAS_COL0, ShaderAttrib::Color0, "float4 rawcolor0"},
        {VB_HAS_COL1, ShaderAttrib::Color1, "float4 rawcolor1"},
    }};

    m_out.Write("ATTRIBUTE_LOCATION({:s}) in {};\n", ShaderAttrib::Position, "float4 rawpos");
    for (const Attribute& attribute : optional_attributes)
    {
      if (Has(attribute.mask))
        m_out.Write("ATTRIBUTE_LOCATION({:s}) in {};\n", attribute.slot, attribute.decl);
    }
    for (u32 i = 0; i < 8; ++i)
    {
      // A texture matrix index travels in the z component of its texture coordinate.
      const bool has_index = Has(VB_HAS_TEXMTXIDX0 << i);
      if (!has_index && m_uid.texGenInfo[i].texcoord_elem_count == 0)
        continue;
      m_out.Write("ATTRIBUTE_LOCATION({:s}) in float{} rawtex{};\n",
                  ShaderAttrib::TexCoord0 + static_cast<int>(i), has_index ? 3 : 2, i);
    }
  }

  // With VS expansion the vertices are read from a storage buffer of 4-byte words; the code that
  // unpacks them into the usual raw* names is collected in m_unpack and emitted inside main().
  void WriteBufferInputs()
  {
    Put(m_out, R"(uint4 unpack_ubyte4(uint value) {
  return uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);
}

struct InputData {
)");
    // The matrix index is read by the matrix functions, not unpacked here.
    if (Has(VB_HAS_POSMTXIDX))
      Put(m_out, "  uint posmtx;\n");

    const u32 position_words = m_uid.position_has_3_elems ? 3 : 2;
    for (u32 k = 0; k < position_words; ++k)
      m_out.Write("  float pos{};\n", k);
    m_unpack += fmt::format("float4 rawpos = float4(i.pos0, i.pos1, {}, 1.0f);\n",
                            m_uid.position_has_3_elems ? "i.pos2" : "0.0f");

    // Note the pairing: "binormal" follows the tangent bit and "tangent" the binormal bit (the
    // bits are taken in order from VB_HAS_NORMAL). This only affects the expanded path.
    static constexpr std::array<std::pair<std::string_view, u32>, 3> vectors = {{
        {"normal", VB_HAS_NORMAL},
        {"binormal", VB_HAS_TANGENT},
        {"tangent", VB_HAS_BINORMAL},
    }};
    for (const auto& [name, mask] : vectors)
    {
      if (!Has(mask))
        continue;
      for (u32 k = 0; k < 3; ++k)
        m_out.Write("  float {}{};\n", name, k);
      m_unpack += fmt::format("float3 raw{0} = float3(i.{0}0, i.{0}1, i.{0}2);\n", name);
    }

    for (u32 c = 0; c < 2; ++c)
    {
      if (!Has(VB_HAS_COL0 << c))
        continue;
      m_out.Write("  uint color{};\n", c);
      m_unpack += fmt::format("float4 rawcolor{0} = float4(unpack_ubyte4(i.color{0})) / 255.0f;\n", c);
    }

    for (u32 t = 0; t < 8; ++t)
    {
      const u32 count = m_uid.texGenInfo[t].texcoord_elem_count;
      if (count == 0)
        continue;
      std::array<std::string, 3> parts;
      for (u32 k = 0; k < 3; ++k)
      {
        if (k >= count)
          parts[k] = "0.0f";
        else if (count == 1)
          parts[k] = fmt::format("i.tex{}", t);
        else
          parts[k] = fmt::format("i.tex{}_{}", t, k);
        if (k < count)
          m_out.Write("  float {};\n", std::string_view(parts[k]).substr(2));
      }
      m_unpack += fmt::format("float3 rawtex{} = float3({}, {}, {});\n", t, parts[0], parts[1],
                              parts[2]);
    }

    Put(m_out, "};\n\nSSBO_BINDING(1) readonly restrict buffer InputBuffer {\n"
               "  InputData input_buffer[];\n};\n\n");
  }

  void WriteOutputs()
  {
    const bool msaa = m_host.msaa;
    const bool ssaa = m_host.ssaa;
    if (m_host.backend_geometry_shaders)
    {
      Put(m_out, "VARYING_LOCATION(0) out VertexData {\n");
      GenerateVSOutputMembers(m_out, m_api, m_uid.numTexGens, m_host,
                              GetInterpolationQualifier(msaa, ssaa, true, false),
                              ShaderStage::Vertex);
      Put(m_out, "} vs;\n");
      return;
    }

    // Separate varyings, numbered in declaration order.
    const char* qualifier = GetInterpolationQualifier(msaa, ssaa);
    u32 location = 0;
    const auto varying = [&](std::string_view type, std::string_view name) {
      m_out.Write("VARYING_LOCATION({}) {} out {} {};\n", location++, qualifier, type, name);
    };
    varying("float4", "colors_0");
    varying("float4", "colors_1");
    for (u32 i = 0; i < m_uid.numTexGens; ++i)
      varying("float3", fmt::format("tex{}", i));
    if (!m_host.fast_depth_calc)
      varying("float4", "clipPos");
    if (m_pixel_lighting)
    {
      varying("float3", "Normal");
      varying("float3", "WorldPos");
    }
  }

  void WriteExpandFetch()
  {
    if (!Expanding())
      return;
    // Four expanded vertices share one input vertex.
    const std::string_view vertex_id = m_api == APIType::D3D ?
                                           "(gl_VertexID >> 2) + base_vertex" :
                                           "uint(gl_VertexID) >> 2u";
    Put(m_out, "InputData dolphin_primitive_expand_data(int index_offset)\n{\n");
    m_out.Write("\tuint vertex_id = {};\n", vertex_id);
    Put(m_out, "\treturn input_buffer[vertex_id + index_offset];\n}\n\n");
  }

  // dolphin_position_matrix() and dolphin_normal_matrix(): either the per-vertex matrix chosen by
  // the vertex's matrix index or the single shared one. The normal matrix is not rescaled: its
  // scale sizes the emboss effect, and lighting renormalises the transformed normal anyway.
  void WriteMatrixFunctions()
  {
    struct MatrixFunction
    {
      std::string_view type;
      std::string_view name;
      std::string_view indexed_array;
      std::string_view index_var;
      std::string_view swizzle;
      u32 shared_first_row;
    };
    static constexpr std::array<MatrixFunction, 2> functions = {{
        {"mat3x4", "dolphin_position_matrix", I_TRANSFORMMATRICES, "posidx", "", 0},
        {"mat3", "dolphin_normal_matrix", I_NORMALMATRICES, "normidx", ".xyz", 3},
    }};

    const bool indexed = Has(VB_HAS_POSMTXIDX);
    for (const MatrixFunction& fn : functions)
    {
      m_out.Write("{} {}()\n", fn.type, fn.name);
      Put(m_out, "{\n");
      m_out.Write("\t{} result;\n", fn.type);
      if (indexed)
      {
        if (Expanding())
        {
          Put(m_out, "\tInputData i = dolphin_primitive_expand_data(0);\n"
                     "\tuvec4 posmtx = unpack_ubyte4(i.posmtx);\n");
        }
        Put(m_out, "\tint posidx = int(posmtx.r);\n");
        if (fn.index_var != "posidx")
          m_out.Write("\tint {} = posidx & 31;\n", fn.index_var);
      }
      for (u32 row = 0; row < 3; ++row)
      {
        const std::string source =
            indexed ? RowRef(fn.indexed_array, fn.index_var, row, " + ") :
                      fmt::format("{}[{}]", I_POSNORMALMATRIX, fn.shared_first_row + row);
        m_out.Write("\tresult[{}] = {}{};\n", row, source, fn.swizzle);
      }
      Put(m_out, "\treturn result;\n}\n\n");
    }
  }

  // dolphin_transform_texcoordN(): the texture matrix (and optional post-transform) of texgen N.
  void WriteTexCoordFunctions()
  {
    for (u32 i = 0; i < m_uid.numTexGens; ++i)
    {
      const TexGenInfo& gen = m_uid.texGenInfo[i];
      m_out.Write("vec3 dolphin_transform_texcoord{}(vec4 coord)\n", i);
      Put(m_out, "{\n");
      if (gen.is_regular_texgen)
        WriteRegularTransform(i, gen);
      else
        Put(m_out, "\treturn vec3(coord.xyz);\n");
      Put(m_out, "}\n\n");
    }
  }

  void WriteRegularTransform(u32 i, const TexGenInfo& gen)
  {
    Put(m_out, "\tvec3 result;\n");

    const bool indexed = Has(VB_HAS_TEXMTXIDX0 << i);
    if (indexed)
      m_out.Write("\tint tmp = int(rawtex{}.z);\n", i);
    const u32 rows = gen.regular.projection == TexSize::STQ ? 3 : 2;
    std::string value;
    for (u32 row = 0; row < rows; ++row)
    {
      if (row != 0)
        value += ", ";
      if (indexed)
        value += fmt::format("dot(coord, {})", RowRef(I_TRANSFORMMATRICES, "tmp", row, "+"));
      else
        value += fmt::format("dot(coord, {}[{}])", I_TEXMATRICES, 3 * i + row);
    }
    if (rows == 2)
      value += ", 1";
    m_out.Write("\tresult = vec3({});\n", value);

    if (m_uid.dualTexTrans_enabled)
    {
      const u32 first = gen.regular.postmtx_index;
      std::string product;
      for (u32 row = 0; row < 3; ++row)
      {
        m_out.Write("\tvec4 P{} = {}[{}];\n", row, I_POSTTRANSFORMMATRICES, (first + row) & 0x3f);
        product += fmt::format("{0}dot(P{1}.xyz, result) + P{1}.w", row == 0 ? "" : ", ", row);
      }
      if (gen.regular.postmtx_normalize)
        Put(m_out, "\tresult = normalize(result);\n");
      m_out.Write("\tresult = vec3({});\n", product);
    }

    // A zero q makes the console halve and clamp the coordinates (seen in the neheGX Lesson08
    // sample, Rogue Squadron 3's Hoth sky and The Last Story's shadow culling).
    Put(m_out, "\tif(result.z == 0.0f)\n"
               "\t\tresult.xy = clamp(result.xy / 2.0f, vec2(-1.0f,-1.0f), vec2(1.0f,1.0f));\n"
               "\treturn result;\n");
  }

  // HAS_* macros for custom vertex shaders.
  void WriteFeatureDefines()
  {
    static constexpr std::array<std::pair<std::string_view, u32>, 5> features = {{
        {"COLOR_0", VB_HAS_COL0},
        {"COLOR_1", VB_HAS_COL1},
        {"NORMAL", VB_HAS_NORMAL},
        {"BINORMAL", VB_HAS_BINORMAL},
        {"TANGENT", VB_HAS_TANGENT},
    }};
    for (const auto& [name, mask] : features)
      m_out.Write("#define HAS_{} {}\n", name, Has(mask) ? 1 : 0);
    for (u32 i = 0; i < 8; ++i)
    {
      const bool present = i < m_uid.numTexGens && m_uid.texGenInfo[i].texcoord_elem_count != 0;
      m_out.Write("#define HAS_TEXTURE_COORD_{} {}\n", i, present ? 1 : 0);
    }
  }

  void WriteVertexStructs()
  {
    const auto write_struct = [this](std::string_view name, std::string_view fields,
                                     std::string_view coord_type) {
      m_out.Write("struct {}\n", name);
      Put(m_out, "{\n");
      Put(m_out, fields);
      for (u32 i = 0; i < 8; ++i)
        m_out.Write("\t{} texture_coord_{};\n", coord_type, i);
      Put(m_out, "};\n\n");
    };
    write_struct("DolphinVertexInput",
                 "\tvec4 color_0;\n\tvec4 color_1;\n\tvec4 position;\n"
                 "\tvec3 normal;\n\tvec3 binormal;\n\tvec3 tangent;\n",
                 "vec4");
    write_struct("DolphinVertexOutput",
                 "\tvec4 color_0;\n\tvec4 color_1;\n\tvec4 position;\n\tvec3 normal;\n", "vec3");
  }

  // The raw vector that feeds texgen i (before the input form and NaN fix-ups).
  void WriteTexGenSource(const TexGenInfo& gen)
  {
    const SourceRow row = gen.sourcerow;
    std::string_view source;
    switch (row)
    {
    case SourceRow::Geom:
      source = "rawpos";
      break;
    case SourceRow::Normal:
      source = Has(VB_HAS_NORMAL) ? "rawnormal" : "";
      break;
    case SourceRow::BinormalT:
      source = Has(VB_HAS_TANGENT) ? "rawtangent" : "";
      break;
    case SourceRow::BinormalB:
      source = Has(VB_HAS_BINORMAL) ? "rawbinormal" : "";
      break;
    case SourceRow::Colors:
      // Only the colour texgens read this row; the coordinate itself is unused.
      ASSERT(!gen.is_regular_texgen);
      ASSERT(gen.other.texgentype == TexGenType::Color0 ||
             gen.other.texgentype == TexGenType::Color1);
      return;
    default:
    {
      const u32 tex = static_cast<u32>(row) - static_cast<u32>(SourceRow::Tex0);
      if (m_uid.texGenInfo[tex].texcoord_elem_count != 0)
        m_out.Write("\t\tcoord = vec4(rawtex{0}.x, rawtex{0}.y, 1.0, 1.0);\n", tex);
      return;
    }
    }
    if (!source.empty())
      m_out.Write("\t\tcoord.xyz = {}.xyz;\n", source);
  }

  void WriteTexGenOutput(u32 i, const TexGenInfo& gen)
  {
    const TexGenType type = gen.is_regular_texgen ? TexGenType::Regular : gen.other.texgentype;
    if (type == TexGenType::Regular)
    {
      m_out.Write("\tvertex_output.texture_coord_{0} = "
                  "dolphin_transform_texcoord{0}(vertex_input.texture_coord_{0});\n",
                  i);
    }
    else if (type == TexGenType::EmbossMap)
    {
      // Offset the source coordinates by the light direction in tangent space.
      const u32 light = gen.other.emboss_lightshift;
      const u32 source = gen.other.emboss_sourceshift;
      Put(m_out, "\t{\n");
      m_out.Write("\t\tvec3 ldir = normalize({}[{}].pos.xyz - vertex_output.position.xyz);\n",
                  I_LIGHTS, light);
      for (const std::string_view axis : {"tangent", "binormal"})
        m_out.Write("\t\tvec3 {0} = vertex_input.{0} * dolphin_normal_matrix();\n", axis);
      m_out.Write("\t\tvertex_output.texture_coord_{}.xyz = vertex_output.texture_coord_{}.xyz + "
                  "vec3(dot(ldir, tangent), dot(ldir, binormal), 0.0);\n",
                  i, source);
      Put(m_out, "\t}\n");
    }
    else
    {
      const u32 chan = type == TexGenType::Color0 ? 0 : 1;
      m_out.Write("\tvertex_output.texture_coord_{0}.xyz = "
                  "vec3(vertex_lighting_{1}.x, vertex_lighting_{1}.y, 1);\n",
                  i, chan);
    }
  }

  void WriteVertexInput()
  {
    // All colour channels are produced even when TEV uses fewer, because texgens can read them.
    // Colour 1 needs both colours in the vertex; a lone colour 1 feeds channel 0.
    const bool has0 = Has(VB_HAS_COL0);
    const bool has1 = Has(VB_HAS_COL1);
    Put(m_out, "float4 vertex_color_0, vertex_color_1;\n");
    const std::string_view chan0 = has0 ? "rawcolor0" : has1 ? "rawcolor1" : "missing_color_value";
    const std::string_view chan1 = (has0 && has1) ? "rawcolor1" : "missing_color_value";
    m_out.Write("vertex_color_0 = {};\nvertex_color_1 = {};\n", chan0, chan1);

    Put(m_out, "\tDolphinVertexInput vertex_input;\n");
    for (u32 c = 0; c < 2; ++c)
      m_out.Write("\tvertex_input.color_{0} = vertex_color_{0};\n", c);
    Put(m_out, "\tvertex_input.position = rawpos;\n");

    // Vectors absent from the vertex come from the last values the game set.
    struct Vector
    {
      std::string_view name;
      u32 mask;
      std::string_view cached;
    };
    static constexpr std::array<Vector, 3> vectors = {{
        {"normal", VB_HAS_NORMAL, I_CACHED_NORMAL},
        {"binormal", VB_HAS_BINORMAL, I_CACHED_BINORMAL},
        {"tangent", VB_HAS_TANGENT, I_CACHED_TANGENT},
    }};
    for (const Vector& v : vectors)
    {
      if (Has(v.mask))
        m_out.Write("\tvertex_input.{0} = raw{0};\n", v.name);
      else
        m_out.Write("\tvertex_input.{} = {}.xyz;\n", v.name, v.cached);
    }

    for (u32 i = 0; i < m_uid.numTexGens; ++i)
    {
      const TexGenInfo& gen = m_uid.texGenInfo[i];
      Put(m_out, "\t{\n\t\tvec4 coord = vec4(0.0, 0.0, 1.0, 1.0);\n");
      WriteTexGenSource(gen);
      if (gen.inputform == TexInputForm::AB11)
        Put(m_out, "\t\tcoord.z = 1.0;\n");
      // NaN inputs become 1 (Shadow the Hedgehog's eyelids in cutscenes, Dolphin issue 11458).
      Put(m_out, "\t\t// Convert NaN to 1\n");
      for (const std::string_view c : XYZ)
        m_out.Write("\t\tif (dolphin_isnan(coord.{0})) coord.{0} = 1.0;\n", c);
      m_out.Write("\t\tvertex_input.texture_coord_{} = coord;\n", i);
      Put(m_out, "\t}\n");
    }
    for (u32 i = m_uid.numTexGens; i < 8; ++i)
      m_out.Write("\tvertex_input.texture_coord_{} = vec4(0, 0, 0, 0);\n", i);
  }

  void WriteLineExpansion()
  {
    Put(m_out, R"(bool is_bottom = (gl_VertexID & 2) != 0;
// Line expansion
int id_offset = 0;
if (is_bottom) {
  id_offset -= 1;
} else {
  id_offset += 1;
}
InputData other = dolphin_primitive_expand_data(id_offset);
)");
    m_out.Write("float4 other_pos = float4(other.pos0, other.pos1, {}, 1.0f);\n",
                m_uid.position_has_3_elems ? "other.pos2" : "0.0f");
    if (Has(VB_HAS_POSMTXIDX))
    {
      Put(m_out, "uint other_posidx = other.posmtx & 0xff;\n");
      for (u32 row = 0; row < 3; ++row)
      {
        m_out.Write("float4 other_p{} = {};\n", row,
                    RowRef(I_TRANSFORMMATRICES, "other_posidx", row, " + "));
      }
      Put(m_out, "other_pos = float4(dot(other_p0, other_pos), dot(other_p1, other_pos), "
                 "dot(other_p2, other_pos), 1.0f);\n");
    }
    else
    {
      Put(m_out, "other_pos = vec4(other_pos * dolphin_position_matrix(), 1.0);\n");
    }
    Put(m_out, "bool is_right = (gl_VertexID & 1) != 0;\n");
    GenerateVSLineExpansion(m_out, "", m_uid.numTexGens);
  }

  // Depth: own clipping when depth clamping is available, the Sonic depth tweak, the console
  // -1..0 depth range mapped (inverted) onto the host range, and the negative-viewport mirror.
  void WriteDepthAndPosition()
  {
    if (!m_host.fast_depth_calc)
      Put(m_out, "o.clipPos = o.pos;\n");

    // The 1e-7 shrink matches the software renderer's projection (Sonic Adventure/Unleashed).
    if (m_host.backend_depth_clamp)
    {
      Put(m_out, "float clipDepth = o.pos.z * (1.0 - 1e-7);\n"
                 "float clipDist0 = clipDepth + o.pos.w;\n"
                 "float clipDist1 = -clipDepth;\n");
      if (m_host.backend_geometry_shaders)
        Put(m_out, "o.clipDist0 = clipDist0;\no.clipDist1 = clipDist1;\n");
    }
    else
    {
      Put(m_out, "o.pos.z = o.pos.z * (1.0 - 1e-7);\n");
    }

    // Done before the divide so that depth ranges wider than the host allows still work (they act
    // as a depth bias once clipped).
    m_out.Write("o.pos.z = o.pos.w * {0}.w - o.pos.z * {0}.z;\n", I_PIXELCENTERCORRECTION);
    if (!m_host.backend_clip_control)
      Put(m_out, "o.pos.z = o.pos.z * 2.0 - o.pos.w;\n");

    // The backend negates the viewport height; mirror for negative viewports, then move the pixel
    // centre from the host's 0.5 to the console's 7/12.
    m_out.Write("o.pos.xy *= sign({}.xy * float2(1.0, -1.0));\n", I_PIXELCENTERCORRECTION);
    m_out.Write("o.pos.xy = o.pos.xy - o.pos.w * {}.xy;\n", I_PIXELCENTERCORRECTION);

    if (m_host.vertex_rounding)
    {
      // Snap 2D (w == 1) vertices to the console's pixel grid so upscaling adds no offset.
      Put(m_out, "if (o.pos.w == 1.0f)\n{\n");
      for (const std::string_view c : {"x", "y"})
      {
        m_out.Write("\tfloat ss_pixel_{0} = ((o.pos.{0} + 1.0f) * ({1}.{0} * 0.5f));\n", c,
                    I_VIEWPORT_SIZE);
      }
      for (const std::string_view c : {"x", "y"})
        m_out.Write("\tss_pixel_{0} = round(ss_pixel_{0});\n", c);
      for (const std::string_view c : {"x", "y"})
      {
        m_out.Write("\to.pos.{0} = ((ss_pixel_{0} / ({1}.{0} * 0.5f)) - 1.0f);\n", c,
                    I_VIEWPORT_SIZE);
      }
      Put(m_out, "}\n");
    }
  }

  void WriteMain()
  {
    Put(m_out, "void main()\n{\n");
    if (Expanding())
    {
      Put(m_out, "InputData i = dolphin_primitive_expand_data(0);\n");
      Put(m_out, m_unpack);
    }
    Put(m_out, "VS_OUTPUT o;\n");
    WriteVertexInput();

    Put(m_out, "\tDolphinVertexOutput vertex_output;\n"
               "\tprocess_vertex(vertex_input, vertex_output);\n");
    std::string projected;
    for (u32 row = 0; row < 4; ++row)
    {
      projected += fmt::format("{}dot({}[{}], vertex_output.position)", row == 0 ? "" : ", ",
                               I_PROJECTION, row);
    }
    m_out.Write("\to.pos = vec4({});\n", projected);
    for (u32 i = 0; i < m_uid.numTexGens; ++i)
      m_out.Write("\to.tex{0} = vertex_output.texture_coord_{0};\n", i);
    for (u32 c = 0; c < 2; ++c)
      m_out.Write("\to.colors_{0} = vertex_output.color_{0};\n", c);
    if (m_pixel_lighting)
    {
      // "WorldPos" is really view space.
      Put(m_out, "\to.Normal = vertex_output.normal;\n"
                 "\to.WorldPos = vertex_output.position.xyz;\n");
    }

    if (m_uid.vs_expand == VSExpand::Line)
    {
      WriteLineExpansion();
    }
    else if (m_uid.vs_expand == VSExpand::Point)
    {
      Put(m_out, "bool is_bottom = (gl_VertexID & 2) != 0;\n"
                 "bool is_right = (gl_VertexID & 1) != 0;\n"
                 "// Point expansion\n");
      GenerateVSPointExpansion(m_out, "", m_uid.numTexGens);
    }

    WriteDepthAndPosition();

    if (m_host.backend_geometry_shaders)
    {
      AssignVSOutputMembers(m_out, "vs", "o", m_uid.numTexGens, m_host);
    }
    else
    {
      for (u32 i = 0; i < m_uid.numTexGens; ++i)
        m_out.Write("tex{0}.xyz = o.tex{0};\n", i);
      if (!m_host.fast_depth_calc)
        Put(m_out, "clipPos = o.clipPos;\n");
      if (m_pixel_lighting)
        Put(m_out, "Normal = o.Normal;\nWorldPos = o.WorldPos;\n");
      Put(m_out, "colors_0 = o.colors_0;\ncolors_1 = o.colors_1;\n");
    }

    if (m_host.backend_depth_clamp)
    {
      for (u32 k = 0; k < 2; ++k)
        m_out.Write("gl_ClipDistance[{0}] = clipDist{0};\n", k);
    }

    // Vulkan's clip space has y pointing down.
    if (m_api == APIType::Vulkan)
      Put(m_out, "gl_Position = float4(o.pos.x, -o.pos.y, o.pos.z, o.pos.w);\n");
    else
      Put(m_out, "gl_Position = o.pos;\n");
    Put(m_out, "}\n");
  }

  const APIType m_api;
  const ShaderHostConfig& m_host;
  const vertex_shader_uid_data& m_uid;
  ShaderCode& m_out;
  // The varyings follow the live setting, while the body follows the host config.
  const bool m_pixel_lighting;
  std::string m_unpack;
};
}  // namespace

VertexShaderUid GetVertexShaderUid()
{
  ASSERT(bpmem.genMode.numtexgens == xfmem.numTexGen.numTexGens);
  ASSERT(bpmem.genMode.numcolchans == xfmem.numChan.numColorChans);

  VertexShaderUid uid;
  vertex_shader_uid_data& key = *uid.GetUidData();
  key.numTexGens = xfmem.numTexGen.numTexGens;
  key.numColorChans = xfmem.numChan.numColorChans;

  const u32 components = VertexLoaderManager::g_current_components;
  key.components = components & (VB_HAS_SHARED | VB_HAS_TEXMTXIDXALL);
  // Every present UV starts out with two components; the exact count is only filled in later for
  // drivers that need it.
  for (u32 t = 0; t < 8; ++t)
  {
    if ((components & (VB_HAS_UV0 << t)) != 0)
      key.texGenInfo[t].texcoord_elem_count = 2;
  }

  GetLightingShaderUid(key.lighting);

  // The key holds at most eight texgens; larger register values are invalid.
  const u32 texgens = std::min<u32>(key.numTexGens, 8);
  for (u32 i = 0; i < texgens; ++i)
    FillTexGenKey(key, i);

  return uid;
}

ShaderCode GenerateVertexShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                    const vertex_shader_uid_data* uid_data,
                                    CustomVertexContents custom_contents)
{
  ShaderCode out;
  VertexShaderWriter(api_type, host_config, *uid_data, out).WriteShader(custom_contents);
  return out;
}

void WriteVertexBody(APIType api_type, const ShaderHostConfig& host_config,
                     const vertex_shader_uid_data* uid_data, ShaderCode& out)
{
  VertexShaderWriter(api_type, host_config, *uid_data, out).WriteBody();
}
