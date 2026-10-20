// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/UberShaderCommon.h"

#include <string_view>
#include <utility>

#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

namespace UberShader
{
namespace
{
// Light register fields as read in the shader.
constexpr std::string_view LIGHT = I_LIGHTS "[index]";

void WriteAttenuationCases(ShaderCode& out)
{
  // Directional lights (and no attenuation) only need the direction to the light.
  out.Write("  case {:s}:\n  case {:s}:\n", AttenuationFunc::None, AttenuationFunc::Dir);
  out.Write(R"(    ldir = normalize({0}.pos.xyz - pos.xyz);
    attn = 1.0;
    if (length(ldir) == 0.0)
      ldir = normal;
    break;

)",
            LIGHT);

  // Specular: the attenuation factor comes from the angle to the light's direction (half angle).
  out.Write("  case {:s}:\n", AttenuationFunc::Spec);
  out.Write(R"(    ldir = normalize({0}.pos.xyz - pos.xyz);
    attn = (dot(normal, ldir) >= 0.0) ? max(0.0, dot(normal, {0}.dir.xyz)) : 0.0;
    cosAttn = {0}.cosatt.xyz;
)",
            LIGHT);
  out.Write("    if (diffusefunc == {:s})\n", DiffuseFunc::None);
  out.Write(R"(      distAttn = {0}.distatt.xyz;
    else
      distAttn = normalize({0}.distatt.xyz);
    attn = max(0.0, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, float3(1.0, attn, attn*attn));
    break;

)",
            LIGHT);

  // Spot lights: angular attenuation over distance attenuation.
  out.Write("  case {:s}:\n", AttenuationFunc::Spot);
  out.Write(R"(    ldir = {0}.pos.xyz - pos.xyz;
    dist2 = dot(ldir, ldir);
    dist = sqrt(dist2);
    ldir = ldir / dist;
    attn = max(0.0, dot(ldir, {0}.dir.xyz));
    attn = max(0.0, {0}.cosatt.x + {0}.cosatt.y * attn + {0}.cosatt.z * attn * attn) / dot({0}.distatt.xyz, float3(1.0, dist, dist2));
    break;

)",
            LIGHT);
}

void WriteDiffuseCases(ShaderCode& out)
{
  // Each case scales the light colour; the sign and clamp variants use N.L.
  const std::pair<DiffuseFunc, std::string_view> cases[] = {
      {DiffuseFunc::None, "attn"},
      {DiffuseFunc::Sign, "attn * dot(ldir, normal)"},
      {DiffuseFunc::Clamp, "attn * max(0.0, dot(ldir, normal))"},
  };
  for (const auto& [func, scale] : cases)
  {
    out.Write("  case {:s}:\n    return int4(round({} * float4({}.color)));\n\n", func, scale,
              LIGHT);
  }
}

// Ambient (or alpha) source of a lit channel. Only the alpha path checks which vertex colours exist.
void WriteMaterialAndAmbient(ShaderCode& out, std::string_view in0, std::string_view in1)
{
  out.Write("  if ({} != 0u)\n", BitfieldExtract<&LitChannel::matsource>("colorreg"));
  out.Write("    mat.xyz = int3(round(((chan == 0u) ? {}.xyz : {}.xyz) * 255.0));\n", in0, in1);
  out.Write("  if ({} != 0u)\n", BitfieldExtract<&LitChannel::matsource>("alphareg"));
  out.Write("    mat.w = int(round(((chan == 0u) ? {}.w : {}.w) * 255.0));\n", in0, in1);
  out.Write("  else\n    mat.w = {} [chan + 2u].w;\n\n", I_MATERIALS);
}

// light_mask/attnfunc/diffusefunc of one register ("colorreg" or "alphareg").
void WriteLightSelection(ShaderCode& out, std::string_view reg)
{
  out.Write("    uint light_mask = {} | ({} << 4u);\n",
            BitfieldExtract<&LitChannel::lightMask0_3>(reg),
            BitfieldExtract<&LitChannel::lightMask4_7>(reg));
  out.Write("    uint attnfunc = {};\n", BitfieldExtract<&LitChannel::attnfunc>(reg));
  out.Write("    uint diffusefunc = {};\n", BitfieldExtract<&LitChannel::diffusefunc>(reg));
}
}  // namespace

void WriteLightingFunction(ShaderCode& out)
{
  out.Write(R"(int4 CalculateLighting(uint index, uint attnfunc, uint diffusefunc, float3 pos, float3 normal) {{
  float3 ldir, h, cosAttn, distAttn;
  float dist, dist2, attn;

  switch (attnfunc) {{
)");
  WriteAttenuationCases(out);
  out.Write(R"(  default:
    attn = 1.0;
    ldir = normal;
    break;
  }}

  switch (diffusefunc) {{
)");
  WriteDiffuseCases(out);
  out.Write(R"(  default:
    return int4(0, 0, 0, 0);
  }}
}}

)");
}

void WriteVertexLighting(ShaderCode& out, [[maybe_unused]] APIType api_type,
                         std::string_view world_pos_var, std::string_view normal_var,
                         std::string_view in_color_0_var,
                         std::string_view in_color_1_var, std::string_view out_color_0_var,
                         std::string_view out_color_1_var)
{
  out.Write("// Lighting\nfor (uint chan = 0u; chan < {}u; chan++) {{\n", NUM_XF_COLOR_CHANNELS);
  // Materials 2 and 3 are the material colours; 0 and 1 the ambient ones.
  // (The material line keeps its trailing space.)
  out.Write("  uint colorreg = xfmem_color(chan);\n  uint alphareg = xfmem_alpha(chan);\n"
            "  int4 mat = {}[chan + 2u]; \n  int4 lacc = int4(255, 255, 255, 255);\n\n",
            I_MATERIALS);
  WriteMaterialAndAmbient(out, in_color_0_var, in_color_1_var);

  // Colour lighting.
  out.Write("  if ({} != 0u) {{\n", BitfieldExtract<&LitChannel::enablelighting>("colorreg"));
  out.Write("    if ({} != 0u)\n", BitfieldExtract<&LitChannel::ambsource>("colorreg"));
  out.Write(R"(      lacc.xyz = int3(round(((chan == 0u) ? {}.xyz : {}.xyz) * 255.0));
    else
      lacc.xyz = {} [chan].xyz;

)",
            in_color_0_var, in_color_1_var, I_MATERIALS);
  WriteLightSelection(out, "colorreg");
  out.Write(R"(    for (uint light_index = 0u; light_index < 8u; light_index++) {{
      if ((light_mask & (1u << light_index)) != 0u)
        lacc.xyz += CalculateLighting(light_index, attnfunc, diffusefunc, {}, {}).xyz;
    }}
  }}

)",
            world_pos_var, normal_var);

  // Alpha lighting. A missing vertex colour falls back to colour 0, then to opaque.
  const u32 col0 = std::to_underlying(VB_HAS_COL0);
  out.Write("  if ({} != 0u) {{\n", BitfieldExtract<&LitChannel::enablelighting>("alphareg"));
  out.Write("    if ({} != 0u) {{\n", BitfieldExtract<&LitChannel::ambsource>("alphareg"));
  out.Write(R"(      if ((components & ({0}u << chan)) != 0u) // VB_HAS_COL0
        lacc.w = int(round(((chan == 0u) ? {1}.w : {2}.w) * 255.0));
      else if ((components & {0}u) != 0u) // VB_HAS_COLO0
        lacc.w = int(round({1}.w * 255.0));
      else
        lacc.w = 255;
    }} else {{
      lacc.w = {3} [chan].w;
    }}

)",
            col0, in_color_0_var, in_color_1_var, I_MATERIALS);
  WriteLightSelection(out, "alphareg");
  out.Write(R"(    for (uint light_index = 0u; light_index < 8u; light_index++) {{

      if ((light_mask & (1u << light_index)) != 0u)

        lacc.w += CalculateLighting(light_index, attnfunc, diffusefunc, {}, {}).w;
    }}
  }}

)",
            world_pos_var, normal_var);

  // Material times light, with the light scaled from 0..255 to 0..256.
  out.Write(R"(  lacc = clamp(lacc, 0, 255);

  // Hopefully GPUs that can support dynamic indexing will optimize this.
  float4 lit_color = float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;
  switch (chan) {{
  case 0u: {} = lit_color; break;
  case 1u: {} = lit_color; break;
  }}
}}

)",
            out_color_0_var, out_color_1_var);
}
}  // namespace UberShader
