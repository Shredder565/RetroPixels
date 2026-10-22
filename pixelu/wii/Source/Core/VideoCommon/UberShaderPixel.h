// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>

#include "Common/CommonTypes.h"
#include "VideoCommon/ShaderGenCommon.h"

enum class APIType;

namespace UberShader
{
// The pixel ubershader interprets the TEV registers at run time; the key only holds what cannot
// be decided in the shader (outputs, depth handling) plus the texgen count.
#pragma pack(1)
struct pixel_ubershader_uid_data
{
  u32 num_texgens : 4;
  u32 early_depth : 1;
  u32 per_pixel_depth : 1;
  u32 uint_output : 1;
  u32 no_dual_src : 1;

  u32 NumValues() const { return sizeof(pixel_ubershader_uid_data); }
};
#pragma pack()

using PixelShaderUid = ShaderUid<pixel_ubershader_uid_data>;

PixelShaderUid GetPixelShaderUid();

ShaderCode GenPixelShader(APIType api_type, const ShaderHostConfig& host_config,
                          const pixel_ubershader_uid_data* uid_data);

// Every key the precompiler builds.
void EnumeratePixelShaderUids(const std::function<void(const PixelShaderUid&)>& callback);
// Normalises the key bits the backend decides (dual-source use, integer output).
void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid);
}  // namespace UberShader

template <>
struct fmt::formatter<UberShader::pixel_ubershader_uid_data>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  template <typename FormatContext>
  auto format(const UberShader::pixel_ubershader_uid_data& uid, FormatContext& ctx) const
  {
    const u32 texgens = uid.num_texgens;
    const char* const early = uid.early_depth ? ", early-depth" : "";
    const char* const depth = uid.per_pixel_depth ? ", per-pixel depth" : "";
    const char* const uint_out = uid.uint_output ? ", uint output" : "";
    const char* const single_src = uid.no_dual_src ? ", no dual-source blending" : "";
    return fmt::format_to(ctx.out(), "Pixel UberShader for {} texgens{}{}{}{}", texgens, early,
                          depth, uint_out, single_src);
  }
};
