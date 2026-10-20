// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/ShaderGenCommon.h"

#include <array>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Core/ConfigManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// Appends text that contains no format fields.
void Append(ShaderCode& out, std::string_view text)
{
  out.Write("{}", text);
}

const char* ApiFilePrefix(APIType api_type)
{
  switch (api_type)
  {
  case APIType::D3D:
    return "D3D";
  case APIType::Metal:
    return "Metal";
  case APIType::OpenGL:
    return "OpenGL";
  case APIType::Vulkan:
    return "Vulkan";
  default:
    return "";
  }
}

// One member of the vertex output interface.
struct InterfaceMember
{
  const char* type;
  const char* name;
  int index;  // appended to the name unless negative
};

// The interface members in declaration order. D3D geometry shaders also give every member a
// TEXCOORD semantic numbered in this order, so the order is part of the stage interface.
std::vector<InterfaceMember> OutputInterface(u32 texgens, const ShaderHostConfig& host_config)
{
  std::vector<InterfaceMember> members{
      {"float4", "pos", -1}, {"float4", "colors_", 0}, {"float4", "colors_", 1}};
  if (host_config.backend_geometry_shaders)
  {
    members.push_back({"float", "clipDist", 0});
    members.push_back({"float", "clipDist", 1});
  }
  for (u32 i = 0; i < texgens; ++i)
    members.push_back({"float3", "tex", static_cast<int>(i)});
  if (!host_config.fast_depth_calc)
    members.push_back({"float4", "clipPos", -1});
  if (host_config.per_pixel_lighting)
  {
    members.push_back({"float3", "Normal", -1});
    members.push_back({"float3", "WorldPos", -1});
  }
  return members;
}

// VSBlock members, one declaration per entry (written with a leading tab and a semicolon).
constexpr std::string_view VERTEX_UNIFORMS[] = {
    "uint    components",
    "uint    xfmem_dualTexInfo",
    "uint    xfmem_numColorChans",
    "uint    missing_color_hex",
    "float4  missing_color_value",
    "float4 " I_POSNORMALMATRIX "[6]",
    "float4 " I_PROJECTION "[4]",
    "int4 " I_MATERIALS "[4]",
    "Light " I_LIGHTS "[8]",
    "float4 " I_TEXMATRICES "[24]",
    "float4 " I_TRANSFORMMATRICES "[64]",
    "float4 " I_NORMALMATRICES "[32]",
    "float4 " I_POSTTRANSFORMMATRICES "[64]",
    "float4 " I_PIXELCENTERCORRECTION,
    "float2 " I_VIEWPORT_SIZE,
    "uint4   xfmem_pack1[8]",
    "float4 " I_CACHED_NORMAL,
    "float4 " I_CACHED_TANGENT,
    "float4 " I_CACHED_BINORMAL,
    "uint vertex_stride",
    "uint vertex_offset_rawnormal",
    "uint vertex_offset_rawtangent",
    "uint vertex_offset_rawbinormal",
    "uint vertex_offset_rawpos",
    "uint vertex_offset_posmtx",
    "uint vertex_offset_rawcolor0",
    "uint vertex_offset_rawcolor1",
    // std140 pads arrays of scalars to vec4, so the eight offsets are two uint4s.
    "uint4 vertex_offset_rawtex[2]",
};
}  // namespace

ShaderHostConfig ShaderHostConfig::GetCurrent()
{
  const VideoConfig& cfg = g_ActiveConfig;
  const BackendInfo& backend = g_backend_info;

  ShaderHostConfig result = {};

  // From the user's settings.
  const bool multisampled = cfg.iMultisamples > 1;
  result.msaa = multisampled;
  result.ssaa = multisampled && cfg.bSSAA && backend.bSupportsSSAA;
  result.stereo = cfg.stereo_mode != StereoMode::Off;
  result.wireframe = cfg.bWireFrame;
  result.per_pixel_lighting = cfg.bEnablePixelLighting;
  result.vertex_rounding = cfg.UseVertexRounding();
  result.fast_depth_calc = cfg.bFastDepthCalc;
  result.bounding_box = cfg.bBBoxEnable;
  result.enable_validation_layer = cfg.bEnableValidationLayer;
  result.manual_texture_sampling = !cfg.bFastTextureSampling;
  result.manual_texture_sampling_custom_texture_sizes =
      cfg.ManualTextureSamplingWithCustomTextureSizes();
  result.backend_vs_point_line_expand = cfg.UseVSForLinePointExpand();

  // From what the backend supports.
  result.backend_dual_source_blend = backend.bSupportsDualSourceBlend;
  result.backend_geometry_shaders = backend.bSupportsGeometryShaders;
  result.backend_early_z = backend.bSupportsEarlyZ;
  result.backend_bbox = backend.bSupportsBBox;
  result.backend_gs_instancing = backend.bSupportsGSInstancing;
  result.backend_clip_control = backend.bSupportsClipControl;
  result.backend_ssaa = backend.bSupportsSSAA;
  result.backend_atomics = backend.bSupportsFragmentStoresAndAtomics;
  result.backend_depth_clamp = backend.bSupportsDepthClamp;
  result.backend_reversed_depth_range = backend.bSupportsReversedDepthRange;
  result.backend_bitfield = backend.bSupportsBitfield;
  result.backend_dynamic_sampler_indexing = backend.bSupportsDynamicSamplerIndexing;
  result.backend_shader_framebuffer_fetch = backend.bSupportsFramebufferFetch;
  result.backend_logic_op = backend.bSupportsLogicOp;
  result.backend_palette_conversion = backend.bSupportsPaletteConversion;
  result.backend_sampler_lod_bias = backend.bSupportsLodBiasInSampler;
  result.backend_dynamic_vertex_loader = backend.bSupportsDynamicVertexLoader;
  result.backend_gl_layer_in_fs = backend.bSupportsGLLayerInFS;

  return result;
}

std::string GetDiskShaderCacheFileName(APIType api_type, const char* type, bool include_gameid,
                                       bool include_host_config, bool include_api)
{
  const std::string& cache_dir = File::GetUserPath(D_SHADERCACHE_IDX);
  if (!File::Exists(cache_dir))
    File::CreateDir(cache_dir);

  std::string name = cache_dir;
  if (include_api)
    name.append(ApiFilePrefix(api_type)).push_back('-');
  name.append(type);
  if (include_gameid)
    name.append("-").append(SConfig::GetInstance().GetGameID());
  if (include_host_config)
    name += fmt::format("-{:06X}", ShaderHostConfig::GetCurrent().bits);
  name.append(".cache");
  return name;
}

void WriteIsNanHeader(ShaderCode& out, APIType api_type)
{
  if (api_type != APIType::D3D)
  {
    Append(out, "#define dolphin_isnan(f) isnan(f)\n");
    return;
  }

  // Compare the bit pattern: an exponent of all ones with a non-zero mantissa is a NaN.
  Append(out, R"(bool dolphin_isnan(float f) {
  // Workaround for the HLSL compiler deciding that isnan can never be true and
  // optimising away the call, even though the value can actually be NaN
  // Just look for the bit pattern that indicates NaN instead
  return (floatBitsToInt(f) & 0x7FFFFFFF) > 0x7F800000;
}

)");
}

void WriteBitfieldExtractHeader(ShaderCode& out, APIType api_type,
                                const ShaderHostConfig& host_config)
{
  if (host_config.backend_bitfield)
    return;

  // The comment inside the generated functions is the same for both overloads.
  constexpr std::string_view note =
      "  // This built-in function is only supported in OpenGL 4.0+ and ES 3.1+\n"
      "  // Microsoft's HLSL compiler automatically optimises this to a bitfield extract "
      "instruction.\n";
  Append(out, "uint bitfieldExtract(uint val, int off, int size) {\n");
  Append(out, note);
  Append(out, R"(  uint mask = uint((1 << size) - 1);
  return uint(val >> off) & mask;
}

int bitfieldExtract(int val, int off, int size) {
)");
  Append(out, note);
  Append(out, R"(  return ((val << (32 - size - off)) >> (32 - size));
}

)");
}

void GenerateVSOutputMembers(ShaderCode& object, APIType api_type, u32 texgens,
                             const ShaderHostConfig& host_config, std::string_view qualifier,
                             ShaderStage stage)
{
  // Only D3D geometry shaders spell out semantics: SPIRV-Cross names every varying TEXCOORDn,
  // and the geometry stage has to match both the vertex shader's outputs and the pixel shader's
  // inputs, which it cannot do with SV_ names.
  const bool with_semantics = api_type == APIType::D3D && stage == ShaderStage::Geometry;

  int semantic = 0;
  for (const InterfaceMember& m : OutputInterface(texgens, host_config))
  {
    object.Write("\t{} {} {}", qualifier, m.type, m.name);
    if (m.index >= 0)
      object.Write("{}", m.index);
    if (with_semantics)
      object.Write(" : TEXCOORD{}", semantic);
    object.Write(";\n");
    semantic++;
  }
}

void AssignVSOutputMembers(ShaderCode& object, std::string_view a, std::string_view b, u32 texgens,
                           const ShaderHostConfig& host_config)
{
  const auto copy = [&](std::string_view member) {
    object.Write("\t{0}.{2} = {1}.{2};\n", a, b, member);
  };

  copy("pos");
  copy("colors_0");
  copy("colors_1");
  for (u32 i = 0; i < texgens; ++i)
    copy(fmt::format("tex{}", i));
  if (!host_config.fast_depth_calc)
    copy("clipPos");
  if (host_config.per_pixel_lighting)
  {
    copy("Normal");
    copy("WorldPos");
  }
  if (host_config.backend_geometry_shaders)
  {
    copy("clipDist0");
    copy("clipDist1");
  }
}

void GenerateLineOffset(ShaderCode& object, std::string_view indent0, std::string_view indent1,
                        std::string_view pos_a, std::string_view pos_b, std::string_view sign)
{
  // The console caps lines horizontally or vertically depending on their slope rather than
  // perpendicular to the line: a mostly vertical line is widened along x (half the line width
  // in clip space), a mostly horizontal one along y.
  object.Write("{0}float2 offset;\n"
               "{0}float2 to = abs({1}.xy / {1}.w - {2}.xy / {2}.w);\n",
               indent0, pos_a, pos_b);
  object.Write("{}if (" I_LINEPTPARAMS ".y * to.y > " I_LINEPTPARAMS ".x * to.x) {{\n", indent0);
  object.Write("{}offset = float2({}" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".x, 0);\n", indent1,
               sign);
  object.Write("{}}} else {{\n", indent0);
  object.Write("{}offset = float2(0, {}-" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".y);\n", indent1,
               sign);
  object.Write("{}}}\n", indent0);
}

void GenerateVSLineExpansion(ShaderCode& object, std::string_view indent, u32 texgens)
{
  // The other end point goes through the projection too, then this vertex moves to its side.
  object.Write("{}other_pos = float4(", indent);
  for (int row = 0; row < 4; row++)
    object.Write("{}dot(" I_PROJECTION "[{}], other_pos)", row == 0 ? "" : ", ", row);
  object.Write(");\n\n");
  object.Write("{}float expand_sign = is_right ? 1.0f : -1.0f;\n", indent);

  const std::string inner = fmt::format("{}  ", indent);
  GenerateLineOffset(object, indent, inner, "o.pos", "other_pos", "expand_sign * ");
  object.Write("\n{}o.pos.xy += offset * o.pos.w;\n", indent);

  if (texgens == 0)
    return;

  // Texture coordinates of the right-hand vertices are shifted for the enabled texgens.
  object.Write("{}if ((" I_TEXOFFSET "[2] != 0) && is_right) {{\n", indent);
  object.Write("{}  float texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n", indent);
  for (u32 i = 0; i < texgens; i++)
  {
    object.Write("{}  if (((" I_TEXOFFSET "[0] >> {}) & 0x1) != 0)\n", indent, i);
    object.Write("{}    o.tex{}.x += texOffset;\n", indent, i);
  }
  object.Write("{}}}\n", indent);
}

void GenerateVSPointExpansion(ShaderCode& object, std::string_view indent, u32 texgens)
{
  object.Write("{}float2 expand_sign = float2(is_right ? 1.0f : -1.0f, is_bottom ? -1.0f : 1.0f);\n",
               indent);
  object.Write("{}float2 offset = expand_sign * " I_LINEPTPARAMS ".ww / " I_LINEPTPARAMS ".xy;\n",
               indent);
  object.Write("{}o.pos.xy += offset * o.pos.w;\n", indent);

  if (texgens == 0)
    return;

  object.Write("{}if (" I_TEXOFFSET "[3] != 0) {{\n", indent);
  object.Write("{}  float texOffsetMagnitude = 1.0f / float(" I_TEXOFFSET "[3]);\n", indent);
  // (No line break after this declaration in the generated code.)
  object.Write("{}  float2 texOffset = float2(is_right ? texOffsetMagnitude : 0.0f, is_bottom ? "
               "texOffsetMagnitude : 0.0f);",
               indent);
  for (u32 i = 0; i < texgens; i++)
  {
    object.Write("{}  if (((" I_TEXOFFSET "[1] >> {}) & 0x1) != 0)\n", indent, i);
    object.Write("{}    o.tex{}.xy += texOffset;\n", indent, i);
  }
  object.Write("{}}}\n", indent);
}

const char* GetInterpolationQualifier(bool msaa, bool ssaa, bool in_glsl_interface_block, bool in)
{
  if (!msaa)
    return "";

  // Inside an interface block, GLSL without GL_ARB_shading_language_420pack wants the storage
  // qualifier repeated ("centroid in"), not just "centroid".
  const bool needs_direction = in_glsl_interface_block && !g_backend_info.bSupportsBindingLayout;
  if (ssaa)
  {
    if (!needs_direction)
      return "sample";
    return in ? "sample in" : "sample out";
  }
  if (!needs_direction)
    return "centroid";
  return in ? "centroid in" : "centroid out";
}

void WriteVertexUniformMembers(ShaderCode& out)
{
  for (std::string_view decl : VERTEX_UNIFORMS)
    out.Write("\t{};\n", decl);
  // Accessors for the packed per-texgen and per-channel XF registers.
  constexpr std::array<std::pair<const char*, char>, 4> packed{
      {{"texMtxInfo", 'x'}, {"postMtxInfo", 'y'}, {"color", 'z'}, {"alpha", 'w'}}};
  for (const auto& [name, component] : packed)
    out.Write("\t#define xfmem_{}(i) (xfmem_pack1[(i)].{})\n", name, component);
}

void WriteGeometryUniformMembers(ShaderCode& out)
{
  Append(out, "\tfloat4 " I_STEREOPARAMS ";\n");
  Append(out, "\tfloat4 " I_LINEPTPARAMS ";\n");
  Append(out, "\tint4 " I_TEXOFFSET ";\n");
  Append(out, "\tuint vs_expand;\n");
}
