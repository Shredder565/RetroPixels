// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/UberShaderVertex.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/UberShaderCommon.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
namespace
{
void Put(ShaderCode& out, std::string_view text)
{
  out.Write("{}", text);
}

// "uintBitsToFloat(vertex_buffer[offset + 0]),\n<indent>uintBitsToFloat(...)" for `count` words.
std::string BufferFloats(u32 count, u32 indent)
{
  std::string result;
  for (u32 k = 0; k < count; ++k)
  {
    if (k != 0)
      result += fmt::format(",\n{:{}}", "", indent);
    result += fmt::format("uintBitsToFloat(vertex_buffer[offset + {}])", k);
  }
  return result;
}

// Loader functions for the dynamic vertex loader: vertices are read from a storage buffer
// instead of vertex attributes, using per-attribute offsets from the uniforms. Position and
// texture coordinate offsets carry their component count in the upper 16 bits.
void WriteBufferLoaders(ShaderCode& out, APIType api_type)
{
  Put(out, "\nSSBO_BINDING(1) readonly restrict buffer Vertices {\n  uint vertex_buffer[];\n};\n");
  // OpenGL and Vulkan count gl_VertexID from the base vertex, D3D's SV_VertexID starts at 0, so
  // D3D (12; 11 has no dynamic vertex loader) adds the base vertex from a root constant.
  if (api_type == APIType::D3D)
  {
    Put(out, "UBO_BINDING(std140, 5) uniform DX_Constants {\n  uint base_vertex;\n};\n\n");
  }
  out.Write("uint GetVertexBaseOffset(uint vertex_id) {{\n  return {} * vertex_stride;\n}}\n",
            api_type == APIType::D3D ? "(vertex_id + base_vertex)" : "vertex_id");

  Put(out, R"(
uint4 load_input_uint4_ubyte4(uint vtx_offset, uint attr_offset) {
  uint value = vertex_buffer[vtx_offset + attr_offset];
  return uint4(value & 0xffu, (value >> 8) & 0xffu, (value >> 16) & 0xffu, value >> 24);
}

float4 load_input_float4_ubyte4(uint vtx_offset, uint attr_offset) {
  return float4(load_input_uint4_ubyte4(vtx_offset, attr_offset)) / 255.0f;
}

float3 load_input_float3_float3(uint vtx_offset, uint attr_offset) {
  uint offset = vtx_offset + attr_offset;
)");
  out.Write("  return float3({});\n}}\n\n", BufferFloats(3, 16));

  constexpr std::string_view sized_offset = "  uint components = attr_offset >> 16;\n"
                                            "  uint offset = vtx_offset + (attr_offset & 0xffff);\n";
  const std::string pad(18, ' ');

  Put(out, "float4 load_input_float4_rawpos(uint vtx_offset, uint attr_offset) {\n");
  Put(out, sized_offset);
  out.Write("  if (components < 3)\n    return float4({},\n{}0.0f, 1.0f);\n", BufferFloats(2, 18),
            pad);
  out.Write("  else\n    return float4({},\n{}1.0f);\n}}\n\n", BufferFloats(3, 18), pad);

  Put(out, "float3 load_input_float3_rawtex(uint vtx_offset, uint attr_offset) {\n");
  Put(out, sized_offset);
  out.Write("  if (components < 2)\n    return float3({}, 0.0f, 0.0f);\n", BufferFloats(1, 18));
  out.Write("  else if (components < 3)\n    return float3({},\n{}0.0f);\n", BufferFloats(2, 18),
            pad);
  out.Write("  else\n    return float3({});\n}}\n\n", BufferFloats(3, 18));
}

class UberVertexWriter
{
public:
  UberVertexWriter(APIType api, const ShaderHostConfig& host, u32 texgens, ShaderCode& out)
      : m_api(api), m_host(host), m_texgens(texgens), m_out(out),
        m_loader(host.backend_dynamic_vertex_loader || host.backend_vs_point_line_expand)
  {
  }

  void Write(const vertex_ubershader_uid_data& uid)
  {
    m_out.Write("// {}\n\n", uid);
    WriteLightStruct(m_out);
    Put(m_out, "UBO_BINDING(std140, 2) uniform VSBlock {\n");
    WriteVertexUniformMembers(m_out);
    Put(m_out, "};\n");
    if (m_loader)
    {
      Put(m_out, "UBO_BINDING(std140, 4) uniform GSBlock {\n");
      WriteGeometryUniformMembers(m_out);
      Put(m_out, "};\n");
    }

    Put(m_out, "struct VS_OUTPUT {\n");
    GenerateVSOutputMembers(m_out, m_api, m_texgens, m_host, "", ShaderStage::Vertex);
    Put(m_out, "};\n\n");

    WriteIsNanHeader(m_out, m_api);
    WriteBitfieldExtractHeader(m_out, m_api, m_host);
    WriteLightingFunction(m_out);

    if (m_loader)
      WriteBufferLoaders(m_out, m_api);
    else
      WriteAttributes();
    WriteOutputs();

    Put(m_out, "void main()\n{\nVS_OUTPUT o;\n\n");
    if (m_host.backend_vs_point_line_expand)
    {
      // Expanded primitives use four shader invocations per input vertex.
      Put(m_out, R"(uint vertex_id = gl_VertexID;
if (vs_expand != 0u) {
  vertex_id = vertex_id >> 2;
}
uint vertex_base_offset = GetVertexBaseOffset(vertex_id);
)");
    }
    else if (m_host.backend_dynamic_vertex_loader)
    {
      Put(m_out, "uint vertex_base_offset = GetVertexBaseOffset(gl_VertexID);\n");
    }

    Load(0, "rawpos", "float4", "rawpos");
    WriteTransforms();
    WriteVertexColors();
    WriteVertexLighting(m_out, m_api, "pos.xyz", "_normal", "vertex_color_0", "vertex_color_1",
                        "o.colors_0", "o.colors_1");
    if (m_texgens > 0)
      WriteTexGens();
    if (m_host.backend_vs_point_line_expand)
      WriteExpansion();
    WriteFinish();
  }

private:
  // Declares and loads an attribute from the vertex buffer; with ordinary vertex attributes the
  // variable already exists.
  void Load(u32 indent, std::string_view name, std::string_view type, std::string_view stored,
            std::string_view offset_name = {})
  {
    if (!m_loader)
      return;
    m_out.Write("{:{}}{} {} = load_input_{}_{}(vertex_base_offset, vertex_offset_{});\n", "",
                indent, type, name, type, stored, offset_name.empty() ? name : offset_name);
  }

  void WriteAttributes()
  {
    struct Attribute
    {
      ShaderAttrib slot;
      std::string_view decl;
    };
    static constexpr std::array<Attribute, 7> attributes = {{
        {ShaderAttrib::Position, "float4 rawpos"},
        {ShaderAttrib::PositionMatrix, "uint4 posmtx"},
        {ShaderAttrib::Normal, "float3 rawnormal"},
        {ShaderAttrib::Tangent, "float3 rawtangent"},
        {ShaderAttrib::Binormal, "float3 rawbinormal"},
        {ShaderAttrib::Color0, "float4 rawcolor0"},
        {ShaderAttrib::Color1, "float4 rawcolor1"},
    }};
    for (const Attribute& a : attributes)
      m_out.Write("ATTRIBUTE_LOCATION({:s}) in {};\n", a.slot, a.decl);
    for (u32 i = 0; i < 8; ++i)
    {
      m_out.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawtex{};\n",
                  ShaderAttrib::TexCoord0 + static_cast<int>(i), i);
    }
  }

  void WriteOutputs()
  {
    const bool msaa = m_host.msaa;
    const bool ssaa = m_host.ssaa;
    if (m_host.backend_geometry_shaders)
    {
      Put(m_out, "VARYING_LOCATION(0) out VertexData {\n");
      GenerateVSOutputMembers(m_out, m_api, m_texgens, m_host,
                              GetInterpolationQualifier(msaa, ssaa, true, false),
                              ShaderStage::Vertex);
      Put(m_out, "} vs;\n");
      return;
    }
    const char* qualifier = GetInterpolationQualifier(msaa, ssaa);
    u32 location = 0;
    const auto varying = [&](std::string_view type, std::string_view name) {
      m_out.Write("VARYING_LOCATION({}) {} out {} {};\n", location++, qualifier, type, name);
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

  // Position/normal matrices (per vertex or shared), projection, and the normal, tangent and
  // binormal with their cached fallbacks.
  void WriteTransforms()
  {
    Put(m_out, "// Position matrix\nfloat4 P0;\nfloat4 P1;\nfloat4 P2;\n\n"
               "// Normal matrix\nfloat3 N0;\nfloat3 N1;\nfloat3 N2;\n\n");
    m_out.Write("if ((components & {}u) != 0u) {{ // VB_HAS_POSMTXIDX\n",
                std::to_underlying(VB_HAS_POSMTXIDX));
    Load(2, "posmtx", "uint4", "ubyte4");
    Put(m_out, "  // Vertex format has a per-vertex matrix\n  int posidx = int(posmtx.r);\n");
    for (u32 row = 0; row < 3; ++row)
    {
      m_out.Write("  P{} = {}[posidx{}];\n", row, I_TRANSFORMMATRICES,
                  row == 0 ? std::string() : fmt::format("+{}", row));
    }
    // Normal matrices are stored after the 32 position rows of the same index.
    Put(m_out, "\n  int normidx = posidx >= 32 ? (posidx - 32) : posidx;\n");
    for (u32 row = 0; row < 3; ++row)
    {
      m_out.Write("  N{} = {}[normidx{}].xyz;\n", row, I_NORMALMATRICES,
                  row == 0 ? std::string() : fmt::format("+{}", row));
    }
    Put(m_out, "} else {\n  // One shared matrix\n");
    for (u32 row = 0; row < 3; ++row)
      m_out.Write("  P{} = {}[{}];\n", row, I_POSNORMALMATRIX, row);
    for (u32 row = 0; row < 3; ++row)
      m_out.Write("  N{} = {}[{}].xyz;\n", row, I_POSNORMALMATRIX, row + 3);
    Put(m_out, "}\n\n// Multiply the position vector by the position matrix\n"
               "float4 pos = float4(dot(P0, rawpos), dot(P1, rawpos), dot(P2, rawpos), 1.0);\n");
    m_out.Write("o.pos = float4(dot({0}[0], pos), dot({0}[1], pos), dot({0}[2], pos), "
                "dot({0}[3], pos));\n\n",
                I_PROJECTION);

    struct Vector
    {
      std::string_view name;
      u32 mask;
      std::string_view flag;
      std::string_view cached;
    };
    const std::array<Vector, 3> vectors = {{
        {"normal", std::to_underlying(VB_HAS_NORMAL), "NORMAL", I_CACHED_NORMAL},
        {"tangent", std::to_underlying(VB_HAS_TANGENT), "TANGENT", I_CACHED_TANGENT},
        {"binormal", std::to_underlying(VB_HAS_BINORMAL), "BINORMAL", I_CACHED_BINORMAL},
    }};
    for (const Vector& v : vectors)
      m_out.Write("float3 _raw{};\n", v.name);
    for (const Vector& v : vectors)
    {
      m_out.Write("if ((components & {}u) != 0u) // VB_HAS_{}\n{{\n", v.mask, v.flag);
      Load(2, fmt::format("raw{}", v.name), "float3", "float3");
      m_out.Write("  _raw{0} = raw{0};\n}}\nelse\n{{\n  _raw{0} = {1}.xyz;\n}}\n\n", v.name,
                  v.cached);
    }

    Put(m_out, R"(// The scale of the transform matrix is used to control the size of the emboss map
// effect by changing the scale of the transformed binormals (which only get used by
// emboss map texgens). By normalising the first transformed normal (which is used
// by lighting calculations and needs to be unit length), the same transform matrix
// can do double duty, scaling for emboss mapping, and not scaling for lighting.
)");
    for (const Vector& v : vectors)
    {
      const std::string product =
          fmt::format("float3(dot(N0, _raw{0}), dot(N1, _raw{0}), dot(N2, _raw{0}))", v.name);
      if (v.name == "normal")
        m_out.Write("float3 _{} = normalize({});\n", v.name, product);
      else
        m_out.Write("float3 _{} = {};\n", v.name, product);
    }
  }

  // Colour 1 needs both vertex colours; a single colour feeds both channels.
  void WriteVertexColors()
  {
    const u32 col0 = std::to_underlying(VB_HAS_COL0);
    const u32 col1 = std::to_underlying(VB_HAS_COL1);
    const u32 both = col0 | col1;
    Put(m_out, R"(// xfmem.numColorChans controls the number of color channels available to TEV,
// but we still need to generate all channels here, as it can be used in texgen.
// Cel-damage is an example of this.
float4 vertex_color_0, vertex_color_1;

// To use color 1, the vertex descriptor must have color 0 and 1.
// If color 1 is present but not color 0, it is used for lighting channel 0.
)");
    m_out.Write("bool use_color_1 = ((components & {0}u) == {0}u); // VB_HAS_COL0 | VB_HAS_COL1\n",
                both);

    m_out.Write("if ((components & {0}u) == {0}u) // VB_HAS_COL0 | VB_HAS_COL1\n{{\n", both);
    Load(2, "rawcolor0", "float4", "ubyte4");
    Load(2, "rawcolor1", "float4", "ubyte4");
    WriteColorPair("rawcolor0", "rawcolor1");
    for (u32 c = 0; c < 2; ++c)
    {
      m_out.Write("else if ((components & {}u) != 0u) // VB_HAS_COL{}\n{{\n", c == 0 ? col0 : col1,
                  c);
      const std::string name = fmt::format("rawcolor{}", c);
      Load(2, name, "float4", "ubyte4");
      WriteColorPair(name, name);
    }
    Put(m_out, "else\n{\n");
    WriteColorPair("missing_color_value", "missing_color_value");
  }

  void WriteColorPair(std::string_view first, std::string_view second)
  {
    m_out.Write("  vertex_color_0 = {};\n  vertex_color_1 = {};\n}}\n", first, second);
  }

  // The texture matrix product, for either the STQ (three rows) or ST (two rows plus 1) form.
  void WriteTexMatrixProduct(std::string_view array, const std::array<std::string_view, 3>& rows)
  {
    const auto row = [&](u32 k) { return fmt::format("dot(coord, {}[{}])", array, rows[k]); };
    const std::string indent(34, ' ');
    m_out.Write("        if ({} == {:s}) {{\n",
                BitfieldExtract<&TexMtxInfo::projection>("texMtxInfo"), TexSize::STQ);
    m_out.Write("          output_tex.xyz = float3({},\n{}{},\n{}{});\n", row(0), indent, row(1),
                indent, row(2));
    m_out.Write("        }} else {{\n          output_tex.xyz = float3({},\n{}{},\n{}1.0);\n"
                "        }}\n",
                row(0), indent, row(1), indent);
  }

  void WriteTexGens()
  {
    // HLSL rejects dynamically indexed outputs that might be uninitialised.
    for (u32 i = 0; i < m_texgens; ++i)
      m_out.Write("o.tex{} = float3(0.0, 0.0, 0.0);\n", i);

    Put(m_out, "// Texture coordinate generation\n");
    if (m_texgens == 1)
      Put(m_out, "{ const uint texgen = 0u;\n");
    else
      m_out.Write("for (uint texgen = 0u; texgen < {}u; texgen++) {{\n", m_texgens);

    Put(m_out, "  // Texcoord transforms\n  float4 coord = float4(0.0, 0.0, 1.0, 1.0);\n"
               "  uint texMtxInfo = xfmem_texMtxInfo(texgen);\n");
    m_out.Write("  switch ({}) {{\n", BitfieldExtract<&TexMtxInfo::sourcerow>("texMtxInfo"));
    m_out.Write("  case {:s}:\n    coord.xyz = rawpos.xyz;\n    break;\n\n", SourceRow::Geom);

    struct Row
    {
      SourceRow row;
      u32 mask;
      std::string_view flag;
      std::string_view name;
    };
    const std::array<Row, 3> vector_rows = {{
        {SourceRow::Normal, std::to_underlying(VB_HAS_NORMAL), "NORMAL", "rawnormal"},
        {SourceRow::BinormalT, std::to_underlying(VB_HAS_TANGENT), "TANGENT", "rawtangent"},
        {SourceRow::BinormalB, std::to_underlying(VB_HAS_BINORMAL), "BINORMAL", "rawbinormal"},
    }};
    for (const Row& r : vector_rows)
    {
      m_out.Write("  case {:s}:\n    if ((components & {}u) != 0u) // VB_HAS_{}\n    {{\n", r.row,
                  r.mask, r.flag);
      Load(6, r.name, "float3", "float3");
      m_out.Write("      coord.xyz = {}.xyz;\n    }}\n    break;\n\n", r.name);
    }
    for (u32 t = 0; t < 8; ++t)
    {
      const auto row = static_cast<SourceRow>(std::to_underlying(SourceRow::Tex0) + t);
      const u32 mask = std::to_underlying(VB_HAS_UV0) << t;
      m_out.Write("  case {:s}:\n    if ((components & {}u) != 0u) // VB_HAS_UV{}\n    {{\n", row,
                  mask, t);
      Load(6, fmt::format("rawtex{}", t), "float3", "rawtex",
           fmt::format("rawtex[{}][{}]", t / 4, t % 4));
      m_out.Write("      coord = float4(rawtex{0}.x, rawtex{0}.y, 1.0f, 1.0f);\n    }}\n"
                  "    break;\n\n",
                  t);
    }
    Put(m_out, "  }\n\n  // Input form of AB11 sets z element to 1.0\n");
    m_out.Write("  if ({} == {:s}) // inputform == AB11\n    coord.z = 1.0f;\n\n",
                BitfieldExtract<&TexMtxInfo::inputform>("texMtxInfo"), TexInputForm::AB11);

    // NaN inputs become 1 (Shadow the Hedgehog's eyelids, Dolphin issue 11458).
    Put(m_out, "  // Convert NaN to 1\n");
    for (const char c : {'x', 'y', 'z'})
      m_out.Write("  if (dolphin_isnan(coord.{0})) coord.{0} = 1.0;\n", c);

    m_out.Write("  // first transformation\n  uint texgentype = {};\n",
                BitfieldExtract<&TexMtxInfo::texgentype>("texMtxInfo"));
    Put(m_out, "  float3 output_tex;\n  switch (texgentype)\n  {\n");

    // Emboss: offset an earlier texgen's result by the light direction in tangent space.
    m_out.Write("  case {:s}:\n    {{\n", TexGenType::EmbossMap);
    m_out.Write("      uint light = {};\n      uint source = {};\n      switch (source) {{\n",
                BitfieldExtract<&TexMtxInfo::embosslightshift>("texMtxInfo"),
                BitfieldExtract<&TexMtxInfo::embosssourceshift>("texMtxInfo"));
    for (u32 i = 0; i < m_texgens; ++i)
      m_out.Write("      case {0}u: output_tex.xyz = o.tex{0}; break;\n", i);
    Put(m_out, "      default: output_tex.xyz = float3(0.0, 0.0, 0.0); break;\n      }\n");
    m_out.Write("      float3 ldir = normalize({}[light].pos.xyz - pos.xyz);\n", I_LIGHTS);
    Put(m_out, "      output_tex.xyz += float3(dot(ldir, _tangent), dot(ldir, _binormal), 0.0);\n"
               "    }\n    break;\n\n");

    for (u32 c = 0; c < 2; ++c)
    {
      m_out.Write("  case {0:s}:\n    output_tex.xyz = float3(o.colors_{1}.x, o.colors_{1}.y, 1.0);\n"
                  "    break;\n\n",
                  c == 0 ? TexGenType::Color0 : TexGenType::Color1, c);
    }

    m_out.Write("  case {:s}:\n  default:\n    {{\n", TexGenType::Regular);
    m_out.Write("      if ((components & ({}u /* VB_HAS_TEXMTXIDX0 */ << texgen)) != 0u) {{\n",
                std::to_underlying(VB_HAS_TEXMTXIDX0));
    // The matrix index travels in the z component of the texture coordinate.
    if (m_loader)
    {
      Put(m_out, "        int tmp = int(load_input_float3_rawtex(vertex_base_offset, "
                 "vertex_offset_rawtex[texgen / 4][texgen % 4]).z);\n\n");
    }
    else
    {
      Put(m_out, R"(        // This is messy, due to dynamic indexing of the input texture coordinates.
        // Hopefully the compiler will unroll this whole loop anyway and the switch.
        int tmp = 0;
        switch (texgen) {
)");
      for (u32 i = 0; i < m_texgens; ++i)
        m_out.Write("        case {0}u: tmp = int(rawtex{0}.z); break;\n", i);
      Put(m_out, "        }\n\n");
    }
    WriteTexMatrixProduct(I_TRANSFORMMATRICES, {"tmp", "tmp + 1", "tmp + 2"});
    Put(m_out, "      } else {\n");
    WriteTexMatrixProduct(I_TEXMATRICES, {"3u * texgen", "3u * texgen + 1u", "3u * texgen + 2u"});
    Put(m_out, "      }\n    }\n    break;\n\n  }\n\n");

    // Post-transform matrices (dual texture transform).
    Put(m_out, "  if (xfmem_dualTexInfo != 0u) {\n"
               "    uint postMtxInfo = xfmem_postMtxInfo(texgen);");
    m_out.Write("    uint base_index = {};\n", BitfieldExtract<&PostMtxInfo::index>("postMtxInfo"));
    for (u32 row = 0; row < 3; ++row)
    {
      const std::string index =
          row == 0 ? std::string("base_index") : fmt::format("(base_index + {}u)", row);
      m_out.Write("    float4 P{} = {}[{} & 0x3fu];\n", row, I_POSTTRANSFORMMATRICES, index);
    }
    m_out.Write("\n    if ({} != 0u)\n      output_tex.xyz = normalize(output_tex.xyz);\n\n",
                BitfieldExtract<&PostMtxInfo::normalize>("postMtxInfo"));
    Put(m_out, "    // multiply by postmatrix\n    output_tex.xyz = float3(");
    for (u32 row = 0; row < 3; ++row)
    {
      m_out.Write("{}dot(P{}.xyz, output_tex.xyz) + P{}.w", row == 0 ? "" : ",\n" + std::string(28, ' '),
                  row, row);
    }
    Put(m_out, ");\n  }\n\n");

    // A zero q makes the console halve and clamp regular texgen coordinates.
    m_out.Write("  if (texgentype == {:s} && output_tex.z == 0.0)\n", TexGenType::Regular);
    Put(m_out, "    output_tex.xy = clamp(output_tex.xy / 2.0f, float2(-1.0f,-1.0f), "
               "float2(1.0f,1.0f));\n\n");

    Put(m_out, "  // Hopefully GPUs that can support dynamic indexing will optimize this.\n"
               "  switch (texgen) {\n");
    for (u32 i = 0; i < m_texgens; ++i)
      m_out.Write("  case {0}u: o.tex{0} = output_tex; break;\n", i);
    Put(m_out, "  }\n}\n");
  }

  void WriteExpansion()
  {
    m_out.Write("if (vs_expand == {}u) {{ // Line\n", std::to_underlying(VSExpand::Line));
    // The other end of the line is the neighbouring vertex in the buffer.
    Put(m_out, R"(  bool is_bottom = (gl_VertexID & 2) != 0;
  bool is_right = (gl_VertexID & 1) != 0;
  uint other_base_offset = vertex_base_offset;
  if (is_bottom) {
    other_base_offset -= vertex_stride;
  } else {
    other_base_offset += vertex_stride;
  }
  float4 other_rawpos = load_input_float4_rawpos(other_base_offset, vertex_offset_rawpos);
)");
    for (u32 row = 0; row < 3; ++row)
      m_out.Write("  float4 other_p{0} = P{0};\n", row);
    m_out.Write("  if ((components & {}u) != 0u) {{ // VB_HAS_POSMTXIDX\n",
                std::to_underlying(VB_HAS_POSMTXIDX));
    Put(m_out, "    uint other_posidx = load_input_uint4_ubyte4(other_base_offset, "
               "vertex_offset_posmtx).r;\n");
    for (u32 row = 0; row < 3; ++row)
    {
      m_out.Write("    other_p{} = {}[other_posidx{}];\n", row, I_TRANSFORMMATRICES,
                  row == 0 ? std::string() : fmt::format("+{}", row));
    }
    Put(m_out, "  }\n  float4 other_pos = float4(dot(other_p0, other_rawpos), "
               "dot(other_p1, other_rawpos), dot(other_p2, other_rawpos), 1.0);\n");
    GenerateVSLineExpansion(m_out, "  ", m_texgens);
    m_out.Write("}} else if (vs_expand == {}u) {{ // Point\n", std::to_underlying(VSExpand::Point));
    Put(m_out, "  bool is_bottom = (gl_VertexID & 2) != 0;\n"
               "  bool is_right = (gl_VertexID & 1) != 0;\n");
    GenerateVSPointExpansion(m_out, "  ", m_texgens);
    Put(m_out, "}\n");
  }

  void WriteFinish()
  {
    const bool pixel_lighting = m_host.per_pixel_lighting;
    if (pixel_lighting)
    {
      Put(m_out, R"(// When per-pixel lighting is enabled, the vertex colors are passed through
// unmodified so we can evaluate the lighting in the pixel shader.
// Lighting is also still computed in the vertex shader since it can be used to
// generate texture coordinates. We generated them above, so now the colors can
// be reverted to their previous stage.
o.colors_0 = vertex_color_0;
o.colors_1 = vertex_color_1;
// Note that the numColorChans logic should be (but currently isn't)
// performed in the pixel shader.
)");
    }
    else
    {
      Put(m_out, R"(// The number of colors available to TEV is determined by numColorChans.
// We have to provide the fields to match the interface, so set to zero
// if it's not enabled.
if (xfmem_numColorChans == 0u)
  o.colors_0 = float4(0.0, 0.0, 0.0, 0.0);
if (xfmem_numColorChans <= 1u)
  o.colors_1 = float4(0.0, 0.0, 0.0, 0.0);
)");
    }

    if (!m_host.fast_depth_calc)
      Put(m_out, "o.clipPos = o.pos;\n");
    if (pixel_lighting)
      Put(m_out, "o.Normal = _normal;\no.WorldPos = pos.xyz;\n");

    // Own depth clipping when depth clamping is available (with the Sonic 1e-7 tweak); unlike
    // the specialised shader there is no tweak without it.
    if (m_host.backend_depth_clamp)
    {
      Put(m_out, R"(float clipDepth = o.pos.z * (1.0 - 1e-7);
float clipDist0 = clipDepth + o.pos.w;
float clipDist1 = -clipDepth;
)");
      if (m_host.backend_geometry_shaders)
        Put(m_out, "o.clipDist0 = clipDist0;\no.clipDist1 = clipDist1;\n");
    }

    // Console depth range (inverted) mapped before the divide, negative-viewport mirroring and the
    // 7/12 pixel centre, as in the specialised shader.
    m_out.Write("o.pos.z = o.pos.w * {0}.w - o.pos.z * {0}.z;\n", I_PIXELCENTERCORRECTION);
    if (!m_host.backend_clip_control)
      Put(m_out, "o.pos.z = o.pos.z * 2.0 - o.pos.w;\n");
    m_out.Write("o.pos.xy *= sign({0}.xy * float2(1.0, -1.0));\n"
                "o.pos.xy = o.pos.xy - o.pos.w * {0}.xy;\n",
                I_PIXELCENTERCORRECTION);

    if (m_host.vertex_rounding)
    {
      Put(m_out, "if (o.pos.w == 1.0f)\n{\n");
      for (const char c : {'x', 'y'})
      {
        m_out.Write("\tfloat ss_pixel_{0} = ((o.pos.{0} + 1.0f) * ({1}.{0} * 0.5f));\n", c,
                    I_VIEWPORT_SIZE);
      }
      for (const char c : {'x', 'y'})
        m_out.Write("\tss_pixel_{0} = round(ss_pixel_{0});\n", c);
      for (const char c : {'x', 'y'})
      {
        m_out.Write("\to.pos.{0} = ((ss_pixel_{0} / ({1}.{0} * 0.5f)) - 1.0f);\n", c,
                    I_VIEWPORT_SIZE);
      }
      Put(m_out, "}\n");
    }

    if (m_host.backend_geometry_shaders)
    {
      AssignVSOutputMembers(m_out, "vs", "o", m_texgens, m_host);
    }
    else
    {
      for (u32 i = 0; i < m_texgens; ++i)
        m_out.Write("tex{0}.xyz = o.tex{0};\n", i);
      if (!m_host.fast_depth_calc)
        Put(m_out, "clipPos = o.clipPos;\n");
      if (pixel_lighting)
        Put(m_out, "Normal = o.Normal;\nWorldPos = o.WorldPos;\n");
      Put(m_out, "colors_0 = o.colors_0;\ncolors_1 = o.colors_1;\n");
    }

    if (m_host.backend_depth_clamp)
      Put(m_out, "gl_ClipDistance[0] = clipDist0;\ngl_ClipDistance[1] = clipDist1;\n");

    // Vulkan's clip space has y pointing down.
    Put(m_out, m_api == APIType::Vulkan ?
                   "gl_Position = float4(o.pos.x, -o.pos.y, o.pos.z, o.pos.w);\n" :
                   "gl_Position = o.pos;\n");
    Put(m_out, "}\n");
  }

  const APIType m_api;
  const ShaderHostConfig& m_host;
  const u32 m_texgens;
  ShaderCode& m_out;
  const bool m_loader;
};
}  // namespace

VertexShaderUid GetVertexShaderUid()
{
  VertexShaderUid uid;
  uid.GetUidData()->num_texgens = xfmem.numTexGen.numTexGens;
  return uid;
}

ShaderCode GenVertexShader(APIType api_type, const ShaderHostConfig& host_config,
                           const vertex_ubershader_uid_data* uid_data)
{
  ShaderCode out;
  UberVertexWriter(api_type, host_config, uid_data->num_texgens, out).Write(*uid_data);
  return out;
}

void EnumerateVertexShaderUids(const std::function<void(const VertexShaderUid&)>& callback)
{
  VertexShaderUid uid;
  for (u32 texgens = 0; texgens <= 8; ++texgens)
  {
    uid.GetUidData()->num_texgens = texgens;
    callback(uid);
  }
}
}  // namespace UberShader
