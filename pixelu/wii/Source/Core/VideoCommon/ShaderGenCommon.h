// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/BitField.h"
#include "Common/CommonTypes.h"
#include "Common/EnumMap.h"
#include "Common/StringUtil.h"
#include "Common/TypeUtils.h"

#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/VideoCommon.h"

// Building blocks shared by the shader generators.
//
// Each generator works in two steps: a small "UID" struct is filled in from the GX register state
// (only what changes the generated code goes in), and the shader source is then produced from the
// UID alone. Caches are keyed on the UID's raw bytes, so a shader is generated once per distinct
// UID and its text never depends on anything outside the UID and the host configuration.

// A shader cache key: a plain struct compared and hashed byte for byte. The struct is zeroed on
// construction so that padding and unused fields never make equal keys differ; disk caches store
// these objects directly, so they must stay trivially copyable.
template <class uid_data>
class ShaderUid
{
  static_assert(std::is_trivially_copyable_v<uid_data>, "UID structs are stored byte for byte");

public:
  ShaderUid() { std::memset(&m_key, 0, sizeof(m_key)); }

  bool operator==(const ShaderUid& other) const { return Compare(other) == 0; }
  // Byte order; only used to sort keys inside containers.
  bool operator<(const ShaderUid& other) const { return Compare(other) < 0; }

  uid_data* GetUidData() { return &m_key; }
  const uid_data* GetUidData() const { return &m_key; }
  const u8* GetUidDataRaw() const { return reinterpret_cast<const u8*>(&m_key); }
  size_t GetUidDataSize() const { return sizeof(m_key); }

private:
  int Compare(const ShaderUid& other) const
  {
    return std::memcmp(&m_key, &other.m_key, sizeof(m_key));
  }

  uid_data m_key{};
};

// Text sink for generated shader source. Write() takes fmtlib format strings, so literal braces
// in shader code are written doubled.
class ShaderCode
{
public:
  ShaderCode() { m_text.reserve(16 * 1024); }

  const std::string& GetBuffer() const { return m_text; }

  template <typename... Args>
  void Write(fmt::format_string<Args...> format, Args&&... args)
  {
    fmt::format_to(std::back_inserter(m_text), format, std::forward<Args>(args)...);
  }

private:
  std::string m_text;
};

// Host-side settings that change generated code. The bit positions are part of the shader cache
// file names and of the UID caches, so they must never move.
union ShaderHostConfig
{
  u32 bits;

  BitField<0, 1, bool, u32> msaa;
  BitField<1, 1, bool, u32> ssaa;
  BitField<2, 1, bool, u32> stereo;
  BitField<3, 1, bool, u32> wireframe;
  BitField<4, 1, bool, u32> per_pixel_lighting;
  BitField<5, 1, bool, u32> vertex_rounding;
  BitField<6, 1, bool, u32> fast_depth_calc;
  BitField<7, 1, bool, u32> bounding_box;
  BitField<8, 1, bool, u32> backend_dual_source_blend;
  BitField<9, 1, bool, u32> backend_geometry_shaders;
  BitField<10, 1, bool, u32> backend_early_z;
  BitField<11, 1, bool, u32> backend_bbox;
  BitField<12, 1, bool, u32> backend_gs_instancing;
  BitField<13, 1, bool, u32> backend_clip_control;
  BitField<14, 1, bool, u32> backend_ssaa;
  BitField<15, 1, bool, u32> backend_atomics;
  BitField<16, 1, bool, u32> backend_depth_clamp;
  BitField<17, 1, bool, u32> backend_reversed_depth_range;
  BitField<18, 1, bool, u32> backend_bitfield;
  BitField<19, 1, bool, u32> backend_dynamic_sampler_indexing;
  BitField<20, 1, bool, u32> backend_shader_framebuffer_fetch;
  BitField<21, 1, bool, u32> backend_logic_op;
  BitField<22, 1, bool, u32> backend_palette_conversion;
  BitField<23, 1, bool, u32> enable_validation_layer;
  BitField<24, 1, bool, u32> manual_texture_sampling;
  BitField<25, 1, bool, u32> manual_texture_sampling_custom_texture_sizes;
  BitField<26, 1, bool, u32> backend_sampler_lod_bias;
  BitField<27, 1, bool, u32> backend_dynamic_vertex_loader;
  BitField<28, 1, bool, u32> backend_vs_point_line_expand;
  BitField<29, 1, bool, u32> backend_gl_layer_in_fs;

  // Built from the active video config and the backend's capabilities.
  static ShaderHostConfig GetCurrent();
};

// Path of a cache file in the user's shader cache directory (created if missing), for example
// "Vulkan-vs-GALE01-1A2B3C.cache".
std::string GetDiskShaderCacheFileName(APIType api_type, const char* type, bool include_gameid,
                                       bool include_host_config, bool include_api = true);

// dolphin_isnan(): HLSL may assume isnan() is always false, so D3D gets a bit-pattern test.
void WriteIsNanHeader(ShaderCode& out, APIType api_type);
// bitfieldExtract() replacements for backends without native bitfield instructions.
void WriteBitfieldExtractHeader(ShaderCode& out, APIType api_type,
                                const ShaderHostConfig& host_config);

// The members of the vertex-to-pixel interface (VS_OUTPUT and the VertexData blocks).
void GenerateVSOutputMembers(ShaderCode& object, APIType api_type, u32 texgens,
                             const ShaderHostConfig& host_config, std::string_view qualifier,
                             ShaderStage stage);
// Member-wise copy "a = b" of that interface (GLSL cannot assign whole interface blocks).
void AssignVSOutputMembers(ShaderCode& object, std::string_view a, std::string_view b, u32 texgens,
                           const ShaderHostConfig& host_config);

// Lines are widened horizontally or vertically depending on their slope, as on console: computes
// "offset" from the two clip-space end points.
void GenerateLineOffset(ShaderCode& object, std::string_view indent0, std::string_view indent1,
                        std::string_view pos_a, std::string_view pos_b, std::string_view sign);
// Line and point expansion done in the vertex shader (four vertices per point or line segment).
void GenerateVSLineExpansion(ShaderCode& object, std::string_view indent, u32 texgens);
void GenerateVSPointExpansion(ShaderCode& object, std::string_view indent, u32 texgens);

// Interpolation qualifier for varyings: with MSAA the varyings are interpolated at the centroid
// (or per sample with SSAA), so that pixels only partly covered by a primitive never sample
// texture coordinates from outside it. Without MSAA there is no qualifier.
const char* GetInterpolationQualifier(bool msaa, bool ssaa, bool in_glsl_interface_block = false,
                                      bool in = false);

// GLSL expression extracting the given BitField member from `source` (an int or uint value).
template <auto ptr_to_bitfield_member>
std::string BitfieldExtract(std::string_view source)
{
  using Field = Common::MemberType<ptr_to_bitfield_member>;
  const std::string_view cast = Field::IsSigned() ? "int" : "uint";
  return fmt::format("bitfieldExtract({}({}), {}, {})", cast, source,
                     static_cast<u32>(Field::StartBit()), static_cast<u32>(Field::NumBits()));
}

namespace ShaderGenDetail
{
// Emits the cases [first, end) of WriteSwitch as nested ifs, halving the range each level.
template <auto last_member>
void WriteSwitchRange(ShaderCode& out, std::string_view variable,
                      const Common::EnumMap<std::string_view, last_member>& values, u32 indent,
                      u32 first, u32 end)
{
  if (end - first == 1)
  {
    const auto key = static_cast<decltype(last_member)>(first);
    out.Write("{:{}}{}  // {}\n", "", indent, values[key], key);
    return;
  }
  const u32 split = first + (end - first) / 2;
  out.Write("{:{}}if ({} < {}u) {{\n", "", indent, variable, split);
  WriteSwitchRange(out, variable, values, indent + 2, first, split);
  out.Write("{:{}}}} else {{\n", "", indent);
  WriteSwitchRange(out, variable, values, indent + 2, split, end);
  out.Write("{:{}}}}\n", "", indent);
}
}  // namespace ShaderGenDetail

// A "switch" over an enum-valued shader variable, written as a binary tree of if statements
// (some drivers handle switch statements badly). values[k] is the statement for case k.
template <auto last_member>
void WriteSwitch(ShaderCode& out, [[maybe_unused]] APIType ApiType, std::string_view variable,
                 const Common::EnumMap<std::string_view, last_member>& values, int indent,
                 [[maybe_unused]] bool break_)
{
  ShaderGenDetail::WriteSwitchRange(out, variable, values, static_cast<u32>(indent), 0,
                                    static_cast<u32>(last_member) + 1);
}

// Names of the shader uniforms. Pixel stage (PSBlock):
#define I_COLORS "color"
#define I_KCOLORS "k"
#define I_ALPHA "alphaRef"
#define I_TEXDIMS "texdim"
#define I_ZBIAS "czbias"
#define I_INDTEXSCALE "cindscale"
#define I_INDTEXMTX "cindmtx"

// Fog and depth:
#define I_FOGCOLOR "cfogcolor"
#define I_FOGI "cfogi"
#define I_FOGF "cfogf"
#define I_FOGRANGE "cfogrange"
#define I_ZSLOPE "czslope"
#define I_EFBSCALE "cefbscale"

// Vertex stage (VSBlock): transforms,
#define I_POSNORMALMATRIX "cpnmtx"
#define I_PROJECTION "cproj"
#define I_TEXMATRICES "ctexmtx"
#define I_TRANSFORMMATRICES "ctrmtx"
#define I_NORMALMATRICES "cnmtx"
#define I_POSTTRANSFORMMATRICES "cpostmtx"

// lighting,
#define I_MATERIALS "cmtrl"
#define I_LIGHTS "clights"

// viewport and cached vertex attributes.
#define I_PIXELCENTERCORRECTION "cpixelcenter"
#define I_VIEWPORT_SIZE "cviewport"
#define I_CACHED_NORMAL "cnormal"
#define I_CACHED_TANGENT "ctangent"
#define I_CACHED_BINORMAL "cbinormal"

// Geometry stage (GSBlock), also used for vertex-shader line/point expansion:
#define I_STEREOPARAMS "cstereo"
#define I_LINEPTPARAMS "clinept"
#define I_TEXOFFSET "ctexoffset"

// The members of the VSBlock and GSBlock uniform blocks (the layouts of VertexShaderConstants and
// GeometryShaderConstants in ConstantManager.h).
void WriteVertexUniformMembers(ShaderCode& out);
void WriteGeometryUniformMembers(ShaderCode& out);

// Custom (graphics mod) pixel shaders call this function for the emulated colour.
constexpr std::string_view CUSTOM_PIXELSHADER_COLOR_FUNC = "customShaderColor";

struct CustomPixelShader
{
  std::string custom_shader;
  std::string material_uniform_block;

  bool operator==(const CustomPixelShader& other) const = default;
};

struct CustomPixelShaderContents
{
  std::vector<CustomPixelShader> shaders;

  bool operator==(const CustomPixelShaderContents& other) const = default;
};
