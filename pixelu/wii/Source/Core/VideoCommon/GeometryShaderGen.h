// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

#include "VideoCommon/RenderState.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;

// Geometry shaders expand points and lines into quads and handle stereoscopy and wireframe.
#pragma pack(1)
struct geometry_shader_uid_data
{
  u32 NumValues() const { return sizeof(geometry_shader_uid_data); }
  // True when the geometry stage has nothing to do (triangles without stereo or wireframe).
  bool IsPassthrough() const;

  u32 numTexGens : 4;
  u32 primitive_type : 2;  // PrimitiveType
};
#pragma pack()

using GeometryShaderUid = ShaderUid<geometry_shader_uid_data>;

ShaderCode GenerateGeometryShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data);
GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type);
// Calls `callback` for every UID the precompiler should build.
void EnumerateGeometryShaderUids(const std::function<void(const GeometryShaderUid&)>& callback);

template <>
struct fmt::formatter<geometry_shader_uid_data>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const geometry_shader_uid_data& uid, FormatContext& ctx) const
  {
    const bool passthrough = uid.IsPassthrough();
    const u32 texgens = uid.numTexGens;
    const u32 primitive = uid.primitive_type;
    return fmt::format_to(ctx.out(), "passthrough: {}, {} tex gens, primitive type {}",
                          passthrough, texgens, primitive);
  }
};
