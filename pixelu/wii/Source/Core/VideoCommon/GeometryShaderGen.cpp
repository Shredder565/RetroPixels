// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GeometryShaderGen.h"

#include <array>
#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// Per input primitive type (points, lines, triangles, triangle strips).
struct PrimitiveInfo
{
  const char* glsl_layout;
  const char* hlsl_type;
  u32 vertices_in;
  u32 vertices_out;  // before adding the wireframe closing vertex
};
constexpr std::array<PrimitiveInfo, 4> PRIMITIVES{{
    {"points", "point", 1, 4},
    {"lines", "line", 2, 4},
    {"triangles", "triangle", 3, 4},
    {"triangles", "triangle", 3, 3},
}};

class GeometryShaderWriter
{
public:
  GeometryShaderWriter(APIType api_type, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data& uid)
      : m_api(api_type), m_host(host_config), m_texgens(uid.numTexGens),
        m_primitive(static_cast<PrimitiveType>(uid.primitive_type)),
        m_info(PRIMITIVES[uid.primitive_type]),
        m_glsl(api_type == APIType::OpenGL || api_type == APIType::Vulkan),
        m_wireframe(host_config.wireframe), m_stereo(host_config.stereo)
  {
    m_max_vertices = m_info.vertices_out + (m_wireframe ? 1 : 0);
  }

  ShaderCode Generate()
  {
    WriteDeclarations();
    WriteMainHead();
    WritePrimitiveSetup();
    WriteVertexLoop();
    m_out.Write("}}\n");
    return std::move(m_out);
  }

private:
  const char* StripKind() const { return m_wireframe ? "line" : "triangle"; }
  u32 Invocations() const { return m_stereo ? 2 : 1; }
  // Without instancing both eyes are emitted by one invocation, doubling the output.
  u32 VerticesPerInvocation() const
  {
    return m_host.backend_gs_instancing || !m_stereo ? m_max_vertices : m_max_vertices * 2;
  }

  // Text without format fields.
  void Put(std::string_view text) { m_out.Write("{}", text); }

  void Copy(std::string_view dst, std::string_view src)
  {
    AssignVSOutputMembers(m_out, dst, src, m_texgens, m_host);
  }

  void Members(const char* qualifier)
  {
    GenerateVSOutputMembers(m_out, m_api, m_texgens, m_host, qualifier, ShaderStage::Geometry);
  }

  void WriteDeclarations()
  {
    if (m_glsl)
    {
      if (m_host.backend_gs_instancing)
        m_out.Write("layout({}, invocations = {}) in;\n", m_info.glsl_layout, Invocations());
      else
        m_out.Write("layout({}) in;\n", m_info.glsl_layout);
      m_out.Write("layout({}_strip, max_vertices = {}) out;\n", StripKind(),
                  VerticesPerInvocation());
    }

    WriteLightStruct(m_out);
    Put(m_glsl ? "UBO_BINDING(std140, 4) uniform GSBlock {\n" : "cbuffer GSBlock {\n");
    WriteGeometryUniformMembers(m_out);
    m_out.Write("}};\n");

    m_out.Write("struct VS_OUTPUT {{\n");
    Members("");
    m_out.Write("}};\n");
  }

  void WriteMainHead()
  {
    const bool msaa = m_host.msaa;
    const bool ssaa = m_host.ssaa;
    if (m_glsl)
    {
      if (m_host.backend_gs_instancing)
        m_out.Write("#define InstanceID gl_InvocationID\n");

      m_out.Write("VARYING_LOCATION(0) in VertexData {{\n");
      Members(GetInterpolationQualifier(msaa, ssaa, true, true));
      m_out.Write("}} vs[{}];\n", m_info.vertices_in);

      m_out.Write("VARYING_LOCATION(0) out VertexData {{\n");
      Members(GetInterpolationQualifier(msaa, ssaa, true, false));
      m_out.Write("}} ps;\n");

      if (m_stereo && !m_host.backend_gl_layer_in_fs)
        m_out.Write("flat out int layer;");
      m_out.Write("void main()\n{{\n");
      return;
    }

    // HLSL: the output vertex wraps VS_OUTPUT with the layer and the position.
    m_out.Write("struct VertexData {{\n");
    m_out.Write("\tVS_OUTPUT o;\n");
    if (m_stereo)
      m_out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");
    m_out.Write("\tfloat4 posout : SV_Position;\n");
    m_out.Write("}};\n");

    const char* stream = m_wireframe ? "Line" : "Triangle";
    m_out.Write("[maxvertexcount({})]\n", VerticesPerInvocation());
    if (m_host.backend_gs_instancing)
    {
      m_out.Write("[instance({})]\n", Invocations());
      m_out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output, in uint "
                  "InstanceID : SV_GSInstanceID)\n{{\n",
                  m_info.hlsl_type, m_info.vertices_in, stream);
    }
    else
    {
      m_out.Write("void main({} VS_OUTPUT o[{}], inout {}Stream<VertexData> output)\n{{\n",
                  m_info.hlsl_type, m_info.vertices_in, stream);
    }
    m_out.Write("\tVertexData ps;\n");
  }

  // The per-primitive values computed once: the line offset or the point quad size.
  void WritePrimitiveSetup()
  {
    if (m_primitive == PrimitiveType::Lines)
    {
      if (m_glsl)
      {
        m_out.Write("\tVS_OUTPUT start, end;\n");
        Copy("start", "vs[0]");
        Copy("end", "vs[1]");
      }
      else
      {
        m_out.Write("\tVS_OUTPUT start = o[0];\n");
        m_out.Write("\tVS_OUTPUT end = o[1];\n");
      }
      GenerateLineOffset(m_out, "\t", "\t\t", "end.pos", "start.pos", "");
    }
    else if (m_primitive == PrimitiveType::Points)
    {
      if (m_glsl)
      {
        m_out.Write("\tVS_OUTPUT center;\n");
        Copy("center", "vs[0]");
      }
      else
      {
        m_out.Write("\tVS_OUTPUT center = o[0];\n");
      }
      // Half the point size, from pixels to clip space (y flipped), towards the top right.
      m_out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
                  ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
    }
  }

  void WriteVertexLoop()
  {
    // With instancing, each invocation renders one eye; otherwise both eyes are looped over.
    const bool eye_loop = m_stereo && !m_host.backend_gs_instancing;
    if (m_stereo)
      Put(eye_loop ? "\tfor (int eye = 0; eye < 2; ++eye) {\n" : "\tint eye = InstanceID;\n");

    if (m_wireframe)
      m_out.Write("\tVS_OUTPUT first;\n");

    // A loop over a single vertex would make the D3D compiler warn about forced unrolling.
    const bool vertex_loop = m_info.vertices_in > 1;
    if (vertex_loop)
      m_out.Write("\tfor (int i = 0; i < {}; ++i) {{\n", m_info.vertices_in);
    else
      m_out.Write("\tint i = 0;\n");

    if (m_glsl)
    {
      m_out.Write("\tVS_OUTPUT f;\n");
      Copy("f", "vs[i]");
      // Some drivers corrupt the other outputs unless the clip distances are read.
      if (m_host.backend_depth_clamp &&
          DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
      {
        m_out.Write("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n");
        m_out.Write("\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
      }
    }
    else
    {
      m_out.Write("\tVS_OUTPUT f = o[i];\n");
    }

    if (m_stereo)
    {
      // Stereo separation: shift x in proportion to the distance beyond the convergence plane
      // (w is the view-space depth), in opposite directions for the two eyes.
      m_out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
      m_out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
    }

    switch (m_primitive)
    {
    case PrimitiveType::Lines:
      WriteLineQuad();
      break;
    case PrimitiveType::Points:
      WritePointQuad();
      break;
    default:
      Emit("f", true);
      break;
    }

    if (vertex_loop)
      m_out.Write("\t}}\n");

    if (m_wireframe)
      Emit("first", false);
    Put(m_glsl ? "\tEndPrimitive();\n" : "\toutput.RestartStrip();\n");

    if (eye_loop)
      m_out.Write("\t}}\n");
  }

  void WriteLineQuad()
  {
    m_out.Write("\tVS_OUTPUT l = f;\n");
    m_out.Write("\tVS_OUTPUT r = f;\n");
    m_out.Write("\tl.pos.xy -= offset * l.pos.w;\n");
    m_out.Write("\tr.pos.xy += offset * r.pos.w;\n");

    m_out.Write("\tif (" I_TEXOFFSET "[2] != 0) {{\n");
    m_out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");
    for (u32 i = 0; i < m_texgens; ++i)
    {
      m_out.Write("\tif (((" I_TEXOFFSET "[0] >> {}) & 0x1) != 0)\n", i);
      m_out.Write("\t\tr.tex{}.x += texOffset;\n", i);
    }
    m_out.Write("\t}}\n");

    Emit("l", true);
    Emit("r", false);
  }

  void WritePointQuad()
  {
    // Corners: lower left/right, upper left/right; offset points to the upper right one.
    static constexpr std::array<std::pair<const char*, const char*>, 4> corners{{
        {"ll", "float2(-1,-1) * offset"},
        {"lr", "float2(1,-1) * offset"},
        {"ul", "float2(-1,1) * offset"},
        {"ur", "offset"},
    }};
    for (const auto& corner : corners)
      m_out.Write("\tVS_OUTPUT {} = f;\n", corner.first);
    for (const auto& corner : corners)
      m_out.Write("\t{}.pos.xy += {};\n", corner.first, corner.second);

    m_out.Write("\tif (" I_TEXOFFSET "[3] != 0) {{\n");
    m_out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET "[3]), 1.0 / float(" I_TEXOFFSET
                "[3]));\n");
    for (u32 i = 0; i < m_texgens; ++i)
    {
      m_out.Write("\tif (((" I_TEXOFFSET "[1] >> {}) & 0x1) != 0) {{\n", i);
      m_out.Write("\t\tul.tex{}.xy += float2(0,1) * texOffset;\n", i);
      m_out.Write("\t\tur.tex{}.xy += texOffset;\n", i);
      m_out.Write("\t\tlr.tex{}.xy += float2(1,0) * texOffset;\n", i);
      m_out.Write("\t}}\n");
    }
    m_out.Write("\t}}\n");

    bool first = true;
    for (const auto& corner : corners)
    {
      Emit(corner.first, first);
      first = false;
    }
  }

  // Outputs one vertex. In wireframe mode the first vertex of the primitive is remembered so the
  // line strip can be closed.
  void Emit(std::string_view vertex, bool first_of_primitive)
  {
    if (m_wireframe && first_of_primitive)
      m_out.Write("\tif (i == 0) first = {};\n", vertex);

    if (m_glsl)
    {
      // Vulkan's clip space has y pointing down.
      if (m_api == APIType::Vulkan)
        m_out.Write("\tgl_Position = float4({0}.pos.x, -{0}.pos.y, {0}.pos.z, {0}.pos.w);\n", vertex);
      else
        m_out.Write("\tgl_Position = {}.pos;\n", vertex);
      if (m_host.backend_depth_clamp)
      {
        m_out.Write("\tgl_ClipDistance[0] = {}.clipDist0;\n", vertex);
        m_out.Write("\tgl_ClipDistance[1] = {}.clipDist1;\n", vertex);
      }
      Copy("ps", vertex);
    }
    else
    {
      m_out.Write("\tps.o = {};\n", vertex);
      m_out.Write("\tps.posout = {}.pos;\n", vertex);
    }

    if (m_stereo)
    {
      Put(m_glsl ? "\tgl_Layer = eye;\n" : "\tps.layer = eye;\n");
      if (!m_host.backend_gl_layer_in_fs)
        m_out.Write("\tlayer = eye;\n");
    }

    Put(m_glsl ? "\tEmitVertex();\n" : "\toutput.Append(ps);\n");
  }

  ShaderCode m_out;
  const APIType m_api;
  const ShaderHostConfig& m_host;
  const u32 m_texgens;
  const PrimitiveType m_primitive;
  const PrimitiveInfo& m_info;
  const bool m_glsl;
  const bool m_wireframe;
  const bool m_stereo;
  u32 m_max_vertices = 0;
};
}  // namespace

bool geometry_shader_uid_data::IsPassthrough() const
{
  if (primitive_type < static_cast<u32>(PrimitiveType::Triangles))
    return false;
  return g_ActiveConfig.stereo_mode == StereoMode::Off && !g_ActiveConfig.bWireFrame;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  GeometryShaderUid uid;
  geometry_shader_uid_data& data = *uid.GetUidData();
  data.numTexGens = xfmem.numTexGen.numTexGens;
  data.primitive_type = static_cast<u32>(primitive_type);
  return uid;
}

ShaderCode GenerateGeometryShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data)
{
  return GeometryShaderWriter(api_type, host_config, *uid_data).Generate();
}

void EnumerateGeometryShaderUids(const std::function<void(const GeometryShaderUid&)>& callback)
{
  // Triangles are drawn as strips when the backend has primitive restart.
  const PrimitiveType triangles = g_backend_info.bSupportsPrimitiveRestart ?
                                      PrimitiveType::TriangleStrip :
                                      PrimitiveType::Triangles;

  GeometryShaderUid uid;
  for (const PrimitiveType primitive : {triangles, PrimitiveType::Lines, PrimitiveType::Points})
  {
    uid.GetUidData()->primitive_type = static_cast<u32>(primitive);
    for (u32 texgens = 0; texgens <= 8; texgens++)
    {
      uid.GetUidData()->numTexGens = texgens;
      callback(uid);
    }
  }
}
