// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>

#include "VideoCommon/PixelShaderGen.h"

namespace UberShader
{
// The vertex ubershader reads all XF state from uniforms; only the texgen count is specialised.
#pragma pack(1)
struct vertex_ubershader_uid_data
{
  u32 num_texgens : 4;

  u32 NumValues() const { return sizeof(vertex_ubershader_uid_data); }
};
#pragma pack()

using VertexShaderUid = ShaderUid<vertex_ubershader_uid_data>;

VertexShaderUid GetVertexShaderUid();

ShaderCode GenVertexShader(APIType api_type, const ShaderHostConfig& host_config,
                           const vertex_ubershader_uid_data* uid_data);
// Every key the precompiler builds (0 to 8 texgens).
void EnumerateVertexShaderUids(const std::function<void(const VertexShaderUid&)>& callback);
}  // namespace UberShader

template <>
struct fmt::formatter<UberShader::vertex_ubershader_uid_data>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const UberShader::vertex_ubershader_uid_data& uid, FormatContext& ctx) const
  {
    const u32 texgens = uid.num_texgens;
    return fmt::format_to(ctx.out(), "Vertex UberShader for {} texgens", texgens);
  }
};
