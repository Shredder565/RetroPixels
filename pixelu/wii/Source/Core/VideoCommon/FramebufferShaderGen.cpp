// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/FramebufferShaderGen.h"

#include <string_view>

#include "Common/Logging/Log.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace FramebufferShaderGen
{
namespace
{
// Writes text that is not a format string (single braces are literal).
void Put(ShaderCode& code, std::string_view text)
{
  code.Write("{}", text);
}

// Every real backend consumes the same dialect; the null backend gets no declarations at all.
bool HasShaderDialect()
{
  return g_backend_info.api_type != APIType::Nothing;
}

// True where clip space has y pointing down relative to the D3D convention.
bool FlipsClipY(bool include_opengl)
{
  const APIType api = g_backend_info.api_type;
  return api == APIType::Vulkan || (include_opengl && api == APIType::OpenGL);
}

// The fullscreen-triangle position derived from gl_VertexID.
constexpr std::string_view QUAD_TEXCOORD =
    "  v_tex0 = float3(float((id << 1) & 2), float(id & 2), 0.0f);\n"
    "  opos = float4(v_tex0.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);\n";

void FinishVertexMain(ShaderCode& code, bool flip_on_opengl)
{
  if (FlipsClipY(flip_on_opengl))
    Put(code, "  opos.y = -opos.y;\n");
  Put(code, "}\n");
}

void DeclareUniformBlock(ShaderCode& code, std::string_view members)
{
  Put(code, "UBO_BINDING(std140, 1) uniform PSBlock\n");
  Put(code, members);
}

void DeclareSamplers(ShaderCode& code, u32 count, bool multisampled)
{
  if (!HasShaderDialect())
    return;
  const std::string_view type = multisampled ? "sampler2DMSArray" : "sampler2DArray";
  for (u32 slot = 0; slot < count; ++slot)
    code.Write("SAMPLER_BINDING({0}) uniform {1} samp{0};\n", slot, type);
}

void Sample(ShaderCode& code, u32 slot, std::string_view coords)
{
  if (HasShaderDialect())
    code.Write("texture(samp{}, {})", slot, coords);
}

// coords is an int4 of (x, y, layer, level).
void Load(ShaderCode& code, u32 slot, std::string_view coords)
{
  if (HasShaderDialect())
    code.Write("texelFetch(samp{0}, ({1}).xyz, ({1}).w)", slot, coords);
}

// The members of the VertexData interface block (or the loose varyings without one).
void WriteVaryings(ShaderCode& code, std::string_view direction, u32 num_tex, u32 num_colors,
                   u32 color_location_base)
{
  if (g_backend_info.bSupportsGeometryShaders)
  {
    code.Write("VARYING_LOCATION(0) {} VertexData {{\n", direction);
    for (u32 n = 0; n < num_tex; ++n)
      code.Write("  float3 v_tex{};\n", n);
    for (u32 n = 0; n < num_colors; ++n)
      code.Write("  float4 v_col{};\n", n);
    Put(code, "};\n");
    return;
  }
  for (u32 n = 0; n < num_tex; ++n)
    code.Write("VARYING_LOCATION({0}) {1} float3 v_tex{0};\n", n, direction);
  for (u32 n = 0; n < num_colors; ++n)
    code.Write("VARYING_LOCATION({}) {} float4 v_col{};\n", color_location_base + n, direction, n);
}

struct VertexLayout
{
  u32 tex_inputs = 0;
  u32 color_inputs = 0;
  bool position_input = false;
  u32 tex_outputs = 0;
  u32 color_outputs = 0;
};

void BeginVertexMain(ShaderCode& code, const VertexLayout& layout, std::string_view preamble = {})
{
  if (!HasShaderDialect())
    return;
  for (u32 n = 0; n < layout.tex_inputs; ++n)
  {
    code.Write("ATTRIBUTE_LOCATION({:s}) in float3 rawtex{};\n",
               ShaderAttrib::TexCoord0 + static_cast<int>(n), n);
  }
  for (u32 n = 0; n < layout.color_inputs; ++n)
  {
    code.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawcolor{};\n",
               ShaderAttrib::Color0 + static_cast<int>(n), n);
  }
  if (layout.position_input)
    code.Write("ATTRIBUTE_LOCATION({:s}) in float4 rawpos;\n", ShaderAttrib::Position);

  // The loose colour varyings are placed after the *input* texcoord count.
  WriteVaryings(code, "out", layout.tex_outputs, layout.color_outputs, layout.tex_inputs);
  Put(code, "#define opos gl_Position\n");
  code.Write("{}\nvoid main()\n", preamble);
}

void BeginPixelMain(ShaderCode& code, u32 num_tex, u32 num_colors,
                    std::string_view output_type = "float4", bool frag_coord_alias = false)
{
  if (!HasShaderDialect())
    return;
  WriteVaryings(code, "in", num_tex, num_colors, num_tex);
  code.Write("FRAGMENT_OUTPUT_LOCATION(0) out {} ocol0;\n\n", output_type);
  if (frag_coord_alias)
    Put(code, "#define frag_coord gl_FragCoord\n");
  Put(code, "void main()\n");
}

// Opens main() of a single-sampled or multisampled fetch shader.
void BeginFetch(ShaderCode& code)
{
  Put(code, "{\n  int layer = int(v_tex0.z);\n  int3 coords = int3(int2(gl_FragCoord.xy), layer);\n");
}

// --- Texture reinterpretation ---

// Packs the source texel into raw_value. Returns false for formats that cannot be read.
bool WriteRawDecode(ShaderCode& code, TextureFormat format)
{
  std::string_view decode;
  switch (format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
    decode = "  raw_value = uint(temp_value.r * 255.0);\n";
    break;
  case TextureFormat::IA8:
    decode = "  raw_value = uint(temp_value.r * 255.0) | (uint(temp_value.a * 255.0) << 8);\n";
    break;
  case TextureFormat::I4:
    decode = "  raw_value = uint(temp_value.r * 15.0);\n";
    break;
  case TextureFormat::IA4:
    decode = "  raw_value = uint(temp_value.r * 15.0) | (uint(temp_value.a * 15.0) << 4);\n";
    break;
  case TextureFormat::RGB565:
    decode = "  raw_value = uint(temp_value.b * 31.0) | (uint(temp_value.g * 63.0) << 5) |\n"
             "              (uint(temp_value.r * 31.0) << 11);\n";
    break;
  case TextureFormat::RGB5A3:
    // Alpha above 224/255 cannot be stored in three bits, so it selects the opaque encoding.
    decode = R"(  if (temp_value.a > 0.878f) {
    raw_value = (uint(temp_value.b * 31.0)) | (uint(temp_value.g * 31.0) << 5) |
                (uint(temp_value.r * 31.0) << 10) | 0x8000u;
  } else {
     raw_value = (uint(temp_value.b * 15.0)) | (uint(temp_value.g * 15.0) << 4) |
                 (uint(temp_value.r * 15.0) << 8) | (uint(temp_value.a * 7.0) << 12);
  }
)";
    break;
  default:
    return false;
  }
  Put(code, "  float4 temp_value = ");
  Load(code, 0, "coords");
  Put(code, ";\n");
  Put(code, decode);
  return true;
}

// Unpacks raw_value as the destination format. Returns false for unsupported formats.
bool WriteRawEncode(ShaderCode& code, TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::I8:
  case TextureFormat::C8:
    Put(code, "  float orgba = float(raw_value & 0xFFu) / 255.0;\n"
              "  ocol0 = float4(orgba, orgba, orgba, orgba);\n");
    return true;
  case TextureFormat::IA8:
  case TextureFormat::IA4:
  {
    const bool wide = format == TextureFormat::IA8;
    const std::string_view mask = wide ? "0xFFu" : "0xFu";
    const std::string_view scale = wide ? "255.0" : "15.0";
    code.Write("  float orgb = float(raw_value & {0}) / {1};\n"
               "  ocol0 = float4(orgb, orgb, orgb, float((raw_value >> {2}) & {0}) / {1});\n",
               mask, scale, wide ? 8 : 4);
    return true;
  }
  case TextureFormat::RGB565:
    Put(code, R"(  ocol0 = float4(float((raw_value >> 10) & 0x1Fu) / 31.0,
                 float((raw_value >> 5) & 0x1Fu) / 31.0,
                 float(raw_value & 0x1Fu) / 31.0, 1.0);
)");
    return true;
  case TextureFormat::RGB5A3:
    Put(code, R"(  if ((raw_value & 0x8000u) != 0u) {
    ocol0 = float4(float((raw_value >> 10) & 0x1Fu) / 31.0,
                   float((raw_value >> 5) & 0x1Fu) / 31.0,
                   float(raw_value & 0x1Fu) / 31.0, 1.0);
  } else {
    ocol0 = float4(float((raw_value >> 8) & 0x0Fu) / 15.0,
                   float((raw_value >> 4) & 0x0Fu) / 15.0,
                   float(raw_value & 0x0Fu) / 15.0,
                   float((raw_value >> 12) & 0x07u) / 7.0);
  }
)");
    return true;
  default:
    return false;
  }
}
}  // namespace

std::string GenerateScreenQuadVertexShader()
{
  ShaderCode code;
  BeginVertexMain(code, {.tex_outputs = 1}, "#define id gl_VertexID\n");
  Put(code, "{\n");
  Put(code, QUAD_TEXCOORD);
  // Flipped on Vulkan for its NDC, and on GL so that (0,0) ends up bottom-left.
  FinishVertexMain(code, true);
  return code.GetBuffer();
}

std::string GeneratePassthroughGeometryShader(u32 num_tex, u32 num_colors)
{
  ShaderCode code;
  const APIType api = g_backend_info.api_type;
  if (api == APIType::D3D)
  {
    // The two structs differ only in the render-target index (and the missing newline after the
    // second opening brace, which is kept as it always was).
    const auto members = [&] {
      for (u32 n = 0; n < num_tex; ++n)
        code.Write("  float3 tex{0} : TEXCOORD{0};\n", n);
      for (u32 n = 0; n < num_colors; ++n)
        code.Write("  float4 color{} : TEXCOORD{};\n", n, num_tex + n);
      Put(code, "  float4 position : SV_Position;\n");
    };
    Put(code, "struct VS_OUTPUT\n{\n");
    members();
    Put(code, "};\nstruct GS_OUTPUT\n{");
    members();
    Put(code, R"(  uint slice : SV_RenderTargetArrayIndex;
};

[maxvertexcount(6)]
void main(triangle VS_OUTPUT vso[3], inout TriangleStream<GS_OUTPUT> output)
{
  for (uint slice = 0; slice < 2u; slice++)
  {
    for (int i = 0; i < 3; i++)
    {
      GS_OUTPUT gso;
      gso.position = vso[i].position;
)");
    for (u32 n = 0; n < num_tex; ++n)
      code.Write("      gso.tex{0} = float3(vso[i].tex{0}.xy, float(slice));\n", n);
    for (u32 n = 0; n < num_colors; ++n)
      code.Write("      gso.color{0} = vso[i].color{0};\n", n);
    Put(code, R"(      gso.slice = slice;
      output.Append(gso);
    }
    output.RestartStrip();
  }
}
)");
    return code.GetBuffer();
  }

  if (api != APIType::OpenGL && api != APIType::Vulkan)
    return code.GetBuffer();

  Put(code, "layout(triangles) in;\nlayout(triangle_strip, max_vertices = 6) out;\n");
  if (num_tex != 0 || num_colors != 0)
  {
    for (const std::string_view side : {"in", "out"})
    {
      code.Write("VARYING_LOCATION(0) {} VertexData {{\n", side);
      for (u32 n = 0; n < num_tex; ++n)
        code.Write("  float3 v_tex{};\n", n);
      for (u32 n = 0; n < num_colors; ++n)
        code.Write("  float4 v_col{};\n", n);
      Put(code, side == "in" ? "} v_in[];\n" : "} v_out;\n");
    }
  }
  Put(code, "\nvoid main()\n{\n  for (int j = 0; j < 2; j++)\n  {\n    gl_Layer = j;\n");
  // Some GL drivers reject a loop over the input vertices, so it is unrolled.
  for (u32 vertex = 0; vertex < 3; ++vertex)
  {
    code.Write("    gl_Position = gl_in[{}].gl_Position;\n", vertex);
    for (u32 n = 0; n < num_tex; ++n)
      code.Write("    v_out.v_tex{0} = float3(v_in[{1}].v_tex{0}.xy, float(j));\n", n, vertex);
    for (u32 n = 0; n < num_colors; ++n)
      code.Write("    v_out.v_col{0} = v_in[{1}].v_col{0};\n", n, vertex);
    Put(code, "    EmitVertex();\n\n");
  }
  Put(code, "    EndPrimitive();\n  }\n}\n");
  return code.GetBuffer();
}

std::string GenerateTextureCopyVertexShader()
{
  ShaderCode code;
  DeclareUniformBlock(code, "{  float2 src_offset;\n  float2 src_size;\n};\n\n");
  BeginVertexMain(code, {.tex_outputs = 1}, "#define id gl_VertexID");
  Put(code, "{\n");
  Put(code, QUAD_TEXCOORD);
  Put(code, "  v_tex0 = float3(src_offset + (src_size * v_tex0.xy), 0.0f);\n");
  FinishVertexMain(code, true);
  return code.GetBuffer();
}

std::string GenerateTextureCopyPixelShader()
{
  ShaderCode code;
  DeclareSamplers(code, 1, false);
  BeginPixelMain(code, 1, 0);
  Put(code, "{\n  ocol0 = ");
  Sample(code, 0, "v_tex0");
  Put(code, ";\n}\n");
  return code.GetBuffer();
}

std::string GenerateColorPixelShader()
{
  ShaderCode code;
  BeginPixelMain(code, 0, 1);
  Put(code, "{\n  ocol0 = v_col0;\n}\n");
  return code.GetBuffer();
}

std::string GenerateResolveColorPixelShader(u32 samples)
{
  ShaderCode code;
  DeclareSamplers(code, 1, true);
  BeginPixelMain(code, 1, 0);
  BeginFetch(code);
  // Box filter over every sample.
  code.Write("  ocol0 = float4(0.0f);\n"
             "  for (int i = 0; i < {0}; i++)\n"
             "    ocol0 += texelFetch(samp0, coords, i);\n"
             "  ocol0 /= {0}.0f;\n}}\n",
             samples);
  return code.GetBuffer();
}

std::string GenerateResolveDepthPixelShader(u32 samples)
{
  ShaderCode code;
  DeclareSamplers(code, 1, true);
  BeginPixelMain(code, 1, 0, "float");
  BeginFetch(code);
  // The nearest sample wins.
  code.Write("  ocol0 = texelFetch(samp0, coords, 0).r;\n"
             "  for (int i = 1; i < {}; i++)\n"
             "    ocol0 = min(ocol0, texelFetch(samp0, coords, i).r);\n}}\n",
             samples);
  return code.GetBuffer();
}

std::string GenerateClearVertexShader()
{
  ShaderCode code;
  DeclareUniformBlock(code, "{\n  float4 clear_color;\n  float clear_depth;\n};\n");
  BeginVertexMain(code, {.color_outputs = 1}, "#define id gl_VertexID\n");
  Put(code, R"({
  float2 coord = float2(float((id << 1) & 2), float(id & 2));
  opos = float4(coord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), clear_depth, 1.0f);
  v_col0 = clear_color;
)");
  FinishVertexMain(code, false);
  return code.GetBuffer();
}

std::string GenerateEFBPokeVertexShader()
{
  ShaderCode code;
  BeginVertexMain(code, {.color_inputs = 1, .position_input = true, .color_outputs = 1});
  Put(code, "{\n  v_col0 = rawcolor0;\n  opos = float4(rawpos.xyz, 1.0f);\n");
  // Point pokes carry their size in w.
  if (g_backend_info.bSupportsLargePoints)
    Put(code, "  gl_PointSize = rawpos.w;\n");
  FinishVertexMain(code, false);
  return code.GetBuffer();
}

std::string GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples)
{
  ShaderCode code;
  DeclareSamplers(code, 1, samples > 1);
  BeginPixelMain(code, 1, 0);
  BeginFetch(code);

  if (samples == 1)
  {
    Put(code, "  float4 val = texelFetch(samp0, coords, 0);\n");
  }
  else if (g_ActiveConfig.bSSAA)
  {
    // Per-sample shading reads just its own sample (this line has never ended in a newline).
    Put(code, "  float4 val = texelFetch(samp0, coords, gl_SampleID);");
  }
  else
  {
    code.Write("  float4 val = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
               "  for (int i = 0; i < {0}; i++)\n"
               "    val += texelFetch(samp0, coords, i);\n"
               "  val /= float({0});\n",
               samples);
  }

  if (convtype == EFBReinterpretType::RGB8ToRGBA6)
  {
    // Regroup the 24 colour bits as four 6-bit channels.
    Put(code, R"(  int4 src8 = int4(round(val * 255.f));
  int4 dst6;
  dst6.r = src8.r >> 2;
  dst6.g = ((src8.r & 0x3) << 4) | (src8.g >> 4);
  dst6.b = ((src8.g & 0xF) << 2) | (src8.b >> 6);
  dst6.a = src8.b & 0x3F;
  ocol0 = float4(dst6) / 63.f;
)");
  }
  else if (convtype == EFBReinterpretType::RGBA6ToRGB8)
  {
    // And the other way round, alpha forced opaque.
    Put(code, R"(  int4 src6 = int4(round(val * 63.f));
  int4 dst8;
  dst8.r = (src6.r << 2) | (src6.g >> 4);
  dst8.g = ((src6.g & 0xF) << 4) | (src6.b >> 2);
  dst8.b = ((src6.b & 0x3) << 6) | src6.a;
  dst8.a = 255;
  ocol0 = float4(dst8) / 255.f;
)");
  }
  else if (convtype == EFBReinterpretType::RGB8ToRGB565 ||
           convtype == EFBReinterpretType::RGBA6ToRGB565 ||
           convtype == EFBReinterpretType::RGB565ToRGB8 ||
           convtype == EFBReinterpretType::RGB565ToRGBA6)
  {
    // The RGB565 conversions are passed through unchanged.
    Put(code, "  ocol0 = val;\n");
  }

  Put(code, "}\n");
  return code.GetBuffer();
}

std::string GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format)
{
  ShaderCode code;
  DeclareSamplers(code, 1, false);
  BeginPixelMain(code, 1, 0, "float4", true);
  Put(code, R"({
  int layer = int(v_tex0.z);
  int4 coords = int4(int2(frag_coord.xy), layer, 0);
  uint raw_value;
)");

  // raw_value holds every channel of the source texel, zero-extended to 32 bits.
  if (!WriteRawDecode(code, from_format))
  {
    WARN_LOG_FMT(VIDEO, "From format {} is not supported", from_format);
    return "{}\n";
  }
  if (!WriteRawEncode(code, to_format))
  {
    WARN_LOG_FMT(VIDEO, "To format {} is not supported", to_format);
    return "{}\n";
  }

  Put(code, "}\n");
  return code.GetBuffer();
}

std::string GenerateEFBRestorePixelShader()
{
  ShaderCode code;
  DeclareSamplers(code, 2, false);
  BeginPixelMain(code, 1, 0);
  // Colour from the first texture, depth from the red channel of the second.
  Put(code, "{\n  ocol0 = ");
  Sample(code, 0, "v_tex0");
  Put(code, ";\n  gl_FragDepth = ");
  Sample(code, 1, "v_tex0");
  Put(code, ".r;\n}\n");
  return code.GetBuffer();
}

std::string GenerateImGuiVertexShader()
{
  ShaderCode code;
  // Vertices arrive in pixels; the block holds 2 / viewport size.
  DeclareUniformBlock(code, "{\nfloat2 u_rcp_viewport_size_mul2;\n};\n\n");
  BeginVertexMain(code, {.tex_inputs = 1,
                         .color_inputs = 1,
                         .position_input = true,
                         .tex_outputs = 1,
                         .color_outputs = 1});
  Put(code, "{\n  v_tex0 = float3(rawtex0.xy, 0.0);\n  v_col0 = rawcolor0;\n");
  Put(code, "  opos = float4(rawpos.x * u_rcp_viewport_size_mul2.x - 1.0,"
            "                1.0 - rawpos.y * u_rcp_viewport_size_mul2.y, 0.0, 1.0);\n");
  FinishVertexMain(code, false);
  return code.GetBuffer();
}

std::string GenerateImGuiPixelShader(bool linear_space_output)
{
  ShaderCode code;
  DeclareSamplers(code, 1, false);
  BeginPixelMain(code, 1, 1);
  Put(code, "{\n  ocol0 = ");
  Sample(code, 0, "float3(v_tex0.xy, 0.0)");
  // A linear target gets the vertex colour converted with a plain 2.2 gamma (close enough to
  // sRGB for UI). With HDR this would ideally also be scaled by the paper-white level.
  Put(code, linear_space_output ? " * pow(v_col0, float4(2.2f, 2.2f, 2.2f, 1.0f));\n}\n" :
                                  " * v_col0;\n}\n");
  return code.GetBuffer();
}

}  // namespace FramebufferShaderGen
