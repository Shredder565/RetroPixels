// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureConverterShaderGen.h"

#include <array>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

// Shaders for EFB copies that stay on the GPU (EFB-to-texture): a full-screen triangle vertex
// shader and a pixel shader that samples the EFB, applies the copy filter, gamma and intensity
// conversion, and reduces the result to the precision of the destination format. The GLSL text
// is part of the shader cache key and is kept exactly as before.

namespace TextureConversionShaderGen
{
namespace
{
constexpr std::string_view kUniforms = R"glsl(UBO_BINDING(std140, 1) uniform PSBlock {
  float2 src_offset, src_size;
  uint3 filter_coefficients;
  float gamma_rcp;
  float2 clamp_tb;
  float pixel_height;
};
)glsl";

constexpr std::string_view kVertexOutBlock = R"glsl(VARYING_LOCATION(0) out VertexData {
  float3 v_tex0;
};
)glsl";

// Vertices 0..2 cover the viewport; v_tex0 maps them onto the source rectangle.
constexpr std::string_view kVertexMain = R"glsl(#define id gl_VertexID
#define opos gl_Position
void main() {
  v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);
  opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  v_tex0 = float3(src_offset + (src_size * v_tex0.xy), 0.0f);
)glsl";

constexpr std::string_view kPixelInBlock = R"glsl(VARYING_LOCATION(0) in VertexData {
  float3 v_tex0;
};
)glsl";

constexpr std::string_view kSampleOpen = R"glsl(SAMPLER_BINDING(0) uniform sampler2DArray samp0;
uint4 SampleEFB(float3 uv, float y_offset) {
  float4 tex_sample = texture(samp0, float3(uv.x, clamp(uv.y + (y_offset * pixel_height), clamp_tb.x, clamp_tb.y), )glsl";

constexpr std::string_view kDepthBytes = R"glsl(  uint depth = uint(tex_sample.x * 16777216.0);
  return uint4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);
}
)glsl";

constexpr std::string_view kColorBytes = R"glsl(  return uint4(tex_sample * 255.0);
}
)glsl";

constexpr std::string_view kPixelMainOpen = R"glsl(FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;
void main()
{
)glsl";

constexpr std::string_view kCombineThreeRows =
    R"glsl(  uint3 combined_rows = prev_row.rgb * filter_coefficients[0] +
                        current_row.rgb * filter_coefficients[1] +
                        next_row.rgb * filter_coefficients[2];
)glsl";

constexpr std::string_view kNormalizeComment = R"glsl(  // Shift right by 6 to divide by 64, as filter coefficients
  // that sum to 64 result in no change in brightness
)glsl";

constexpr std::string_view kGamma = R"glsl(  texcol_raw = uint4(round(pow(abs(float4(texcol_raw) / 255.0),
                     float4(gamma_rcp, gamma_rcp, gamma_rcp, 1.0)) * 255.0));
)glsl";

constexpr std::string_view kIntensity = R"glsl(  // Intensity/YUV format conversion constants determined by hardware testing
  const float4 y_const = float4( 66, 129,  25,  16);
  const float4 u_const = float4(-38, -74, 112, 128);
  const float4 v_const = float4(112, -94, -18, 128);
  // Intensity/YUV format conversion
  texcol_raw.rgb = uint3(dot(y_const, float4(texcol_raw.rgb, 256)),
                         dot(u_const, float4(texcol_raw.rgb, 256)),
                         dot(v_const, float4(texcol_raw.rgb, 256)));
  // Divide by 256 and round .5 and higher up
  texcol_raw.rgb = (texcol_raw.rgb >> 8) + ((texcol_raw.rgb >> 7) & 1u);
)glsl";

constexpr std::string_view kStoreRGBA8 = "  ocol0 = float4(texcol_raw.rgba) / 255.0;\n";

void Put(ShaderCode& out, std::string_view glsl)
{
  out.Write("{}", glsl);
}

// The output statement(s) for a destination format, or nothing for an unknown one. The
// rounding matches the bit depth the console would keep, while the data stays RGBA8 (or
// single-channel) on the host.
std::string_view StoreFor(EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
    return "  float red = float(texcol_raw.r & 0xF0u) / 240.0;\n"
           "  ocol0 = float4(red, red, red, red);\n";
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
    return "  ocol0 = float4(texcol_raw).rrrr / 255.0;\n";
  case EFBCopyFormat::RA4:
    return "  float2 red_alpha = float2(texcol_raw.ra & 0xF0u) / 240.0;\n"
           "  ocol0 = red_alpha.rrrg;\n";
  case EFBCopyFormat::RA8:
    return "  ocol0 = float4(texcol_raw).rrra / 255.0;\n";
  case EFBCopyFormat::A8:
    return "  ocol0 = float4(texcol_raw).aaaa / 255.0;\n";
  case EFBCopyFormat::G8:
    return "  ocol0 = float4(texcol_raw).gggg / 255.0;\n";
  case EFBCopyFormat::B8:
    return "  ocol0 = float4(texcol_raw).bbbb / 255.0;\n";
  case EFBCopyFormat::RG8:
    return "  ocol0 = float4(texcol_raw).rrrg / 255.0;\n";
  case EFBCopyFormat::GB8:
    return "  ocol0 = float4(texcol_raw).gggb / 255.0;\n";
  case EFBCopyFormat::RGB565:
    return "  float2 red_blue = float2(texcol_raw.rb & 0xF8u) / 248.0;\n"
           "  float green = float(texcol_raw.g & 0xFCu) / 252.0;\n"
           "  ocol0 = float4(red_blue.r, green, red_blue.g, 1.0);\n";
  case EFBCopyFormat::RGB5A3:
    // Always the 5-bit-colour/3-bit-alpha reduction: the per-texel RGB555 vs ARGB3444 choice
    // of the real format cannot be expressed in a float render target.
    return "  float3 color = float3(texcol_raw.rgb & 0xF8u) / 248.0;\n"
           "  float alpha = float(texcol_raw.a & 0xE0u) / 224.0;\n"
           "  ocol0 = float4(color, alpha);\n";
  case EFBCopyFormat::RGBA8:
    return kStoreRGBA8;
  case EFBCopyFormat::XFB:
    return "  ocol0 = float4(float3(texcol_raw.rgb) / 255.0, 1.0);\n";
  default:
    return {};
  }
}
}  // namespace

TCShaderUid GetShaderUid(EFBCopyFormat dst_format, bool is_depth_copy, bool is_intensity,
                         bool scale_by_half, float gamma_rcp,
                         const std::array<u32, 3>& filter_coefficients)
{
  // With "force true color", the 16-bit colour formats are copied at full precision. RGB565
  // becomes the XFB format here (an RGB8 copy with alpha forced to 1); the XFB value still
  // identifies real XFB copies elsewhere, so this substitution is limited to the shader.
  if (g_ActiveConfig.bForceTrueColor)
  {
    if (dst_format == EFBCopyFormat::RGB565)
      dst_format = EFBCopyFormat::XFB;
    else if (dst_format == EFBCopyFormat::RGB5A3)
      dst_format = EFBCopyFormat::RGBA8;
  }

  TCShaderUid uid;
  UidData& data = *uid.GetUidData();
  data.dst_format = dst_format;
  data.efb_has_alpha = bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;
  data.is_depth_copy = is_depth_copy;
  data.is_intensity = is_intensity;
  data.scale_by_half = scale_by_half;
  data.all_copy_filter_coefs_needed =
      TextureCacheBase::AllCopyFilterCoefsNeeded(filter_coefficients);
  data.copy_filter_can_overflow = TextureCacheBase::CopyFilterCanOverflow(filter_coefficients);
  data.apply_gamma = gamma_rcp != 1.0f;
  return uid;
}

ShaderCode GenerateVertexShader(APIType api_type)
{
  ShaderCode out;
  Put(out, kUniforms);
  Put(out, g_backend_info.bSupportsGeometryShaders ? kVertexOutBlock :
                                                     "VARYING_LOCATION(0) out float3 v_tex0;\n");
  Put(out, kVertexMain);
  // Vulkan's clip space has y pointing down.
  if (api_type == APIType::Vulkan)
    Put(out, "  opos.y = -opos.y;\n");
  Put(out, "}\n");
  return out;
}

ShaderCode GeneratePixelShader(APIType /*api_type*/, const UidData* uid_data)
{
  const UidData& uid = *uid_data;
  // With mono depth, stereo depth copies read only the first layer.
  const bool single_layer = uid.is_depth_copy && g_ActiveConfig.bStereoEFBMonoDepth;

  ShaderCode out;
  Put(out, kUniforms);

  Put(out, kSampleOpen);
  Put(out, single_layer ? "0.0" : "uv.z");
  Put(out, "));\n");
  if (uid.is_depth_copy)
  {
    if (!g_backend_info.bSupportsReversedDepthRange)
      Put(out, "  tex_sample.x = 1.0 - tex_sample.x;\n");
    Put(out, kDepthBytes);
  }
  else
  {
    Put(out, kColorBytes);
  }

  Put(out, g_backend_info.bSupportsGeometryShaders ? kPixelInBlock :
                                                     "VARYING_LOCATION(0) in vec3 v_tex0;\n");
  Put(out, kPixelMainOpen);

  // Vertical copy filter over three rows (colour and depth alike, RGB only; alpha passes
  // through). When the outer taps are zero only the centre row is sampled.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 3> taps = {{
      {"prev", "-1.0f"},
      {"current", "0.0f"},
      {"next", "1.0f"},
  }};
  for (const auto& [row, y_offset] : taps)
  {
    if (uid.all_copy_filter_coefs_needed || row == "current")
      out.Write("  uint4 {}_row = SampleEFB(v_tex0, {});\n", row, y_offset);
  }
  if (uid.all_copy_filter_coefs_needed)
    Put(out, kCombineThreeRows);
  else
    Put(out, "  uint3 combined_rows = current_row.rgb * filter_coefficients[1];\n");

  Put(out, kNormalizeComment);
  out.Write("  uint4 texcol_raw = uint4(combined_rows.rgb >> 6, {});\n",
            uid.efb_has_alpha ? "current_row.a" : "255");
  if (uid.copy_filter_can_overflow)
    Put(out, "  texcol_raw &= 0x1ffu;\n");
  // Values can exceed 255 even without wrapping, so the clamp is unconditional.
  Put(out, "  texcol_raw = min(texcol_raw, uint4(255, 255, 255, 255));\n");
  if (uid.apply_gamma)
    Put(out, kGamma);
  if (uid.is_intensity)
    Put(out, kIntensity);

  std::string_view store = StoreFor(uid.dst_format);
  if (store.empty())
  {
    ERROR_LOG_FMT(VIDEO, "Unknown copy/intensity color format: {} {}", uid_data->dst_format,
                  uid_data->is_intensity);
    store = kStoreRGBA8;
  }
  Put(out, store);
  Put(out, "}\n");
  return out;
}

}  // namespace TextureConversionShaderGen
