// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <fmt/format.h>
#include <string>

#include "Common/CommonTypes.h"

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureDecoder.h"

enum class APIType;

// Shaders for EFB copies that are kept on the GPU as textures.
namespace TextureConversionShaderGen
{
// Everything the pixel shader depends on. Stored byte for byte in shader caches: the layout
// (packed, field order and bit widths) must not change.
#pragma pack(1)
struct UidData
{
  u32 NumValues() const { return sizeof(UidData); }
  EFBCopyFormat dst_format;

  u32 efb_has_alpha : 1;
  u32 is_depth_copy : 1;
  u32 is_intensity : 1;
  u32 scale_by_half : 1;
  u32 all_copy_filter_coefs_needed : 1;
  u32 copy_filter_can_overflow : 1;
  u32 apply_gamma : 1;
};
#pragma pack()

using TCShaderUid = ShaderUid<UidData>;

ShaderCode GenerateVertexShader(APIType api_type);
ShaderCode GeneratePixelShader(APIType api_type, const UidData* uid_data);

// Builds the uid for a copy, reading the EFB pixel format from BP memory and the true-colour
// setting from the active config.
TCShaderUid GetShaderUid(EFBCopyFormat dst_format, bool is_depth_copy, bool is_intensity,
                         bool scale_by_half, float gamma_rcp,
                         const std::array<u32, 3>& filter_coefficients);

}  // namespace TextureConversionShaderGen

template <>
struct fmt::formatter<TextureConversionShaderGen::UidData>
{
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const TextureConversionShaderGen::UidData& uid, FormatContext& ctx) const
  {
    // XFB is not one of the named EFB copy formats, so it is spelled out here.
    const std::string dst_format =
        uid.dst_format == EFBCopyFormat::XFB ? std::string("XFB") : fmt::to_string(uid.dst_format);
    return fmt::format_to(ctx.out(),
                          "dst_format: {}, efb_has_alpha: {}, is_depth_copy: {}, is_intensity: {}, "
                          "scale_by_half: {}, all_copy_filter_coefs_needed: {}, "
                          "copy_filter_can_overflow: {}, apply_gamma: {}",
                          dst_format, uid.efb_has_alpha, uid.is_depth_copy, uid.is_intensity,
                          uid.scale_by_half, uid.all_copy_filter_coefs_needed,
                          uid.copy_filter_can_overflow, uid.apply_gamma);
  }
};
