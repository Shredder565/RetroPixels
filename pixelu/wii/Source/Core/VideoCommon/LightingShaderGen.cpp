// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/LightingShaderGen.h"

#include <array>
#include <string>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/XFMemory.h"

namespace
{
// The lit channels: 0/1 are the colour parts of channels 0/1, 2/3 their alpha parts.
constexpr u32 NUM_LIT_CHANNELS = 4;

bool Bit(u32 mask, u32 lit_channel)
{
  return ((mask >> lit_channel) & 1) != 0;
}

AttenuationFunc AttenuationOf(const LightingUidData& uid, u32 lit_channel)
{
  return static_cast<AttenuationFunc>((uid.attnfunc >> (2 * lit_channel)) & 3);
}

DiffuseFunc DiffuseOf(const LightingUidData& uid, u32 lit_channel)
{
  return static_cast<DiffuseFunc>((uid.diffusefunc >> (2 * lit_channel)) & 3);
}

bool LightEnabled(const LightingUidData& uid, u32 lit_channel, u32 light)
{
  return ((uid.light_mask >> (8 * lit_channel + light)) & 1) != 0;
}

// One light's contribution to lacc, using the shader's pos/_normal and the clights[] uniform.
void WriteLightTerm(ShaderCode& out, const LightingUidData& uid, u32 light, u32 lit_channel,
                    bool alpha)
{
  const std::string lt = fmt::format(I_LIGHTS "[{}]", light);
  const AttenuationFunc attenuation = AttenuationOf(uid, lit_channel);
  const DiffuseFunc diffuse = DiffuseOf(uid, lit_channel);

  if (attenuation == AttenuationFunc::Spot)
  {
    // Spot light: angular falloff from cosatt, distance falloff from distatt.
    out.Write("ldir = {}.pos.xyz - pos.xyz;\n", lt);
    out.Write("dist2 = dot(ldir, ldir);\n");
    out.Write("dist = sqrt(dist2);\n");
    out.Write("ldir = ldir / dist;\n");
    out.Write("attn = max(0.0, dot(ldir, {}.dir.xyz));\n", lt);
    out.Write("attn = max(0.0, {0}.cosatt.x + {0}.cosatt.y*attn + {0}.cosatt.z*attn*attn) / "
              "dot({0}.distatt.xyz, float3(1.0,dist,dist2));\n",
              lt);
  }
  else if (attenuation == AttenuationFunc::Spec)
  {
    // Specular: attenuation from the half-angle vector stored in dir.
    out.Write("ldir = normalize({}.pos.xyz - pos.xyz);\n", lt);
    out.Write("attn = (dot(_normal, ldir) >= 0.0) ? max(0.0, dot(_normal, {}.dir.xyz)) : 0.0;\n",
              lt);
    out.Write("cosAttn = {}.cosatt.xyz;\n", lt);
    out.Write("distAttn = {}({}.distatt.xyz);\n", diffuse == DiffuseFunc::None ? "" : "normalize",
              lt);
    out.Write("attn = max(0.0f, dot(cosAttn, float3(1.0, attn, attn*attn))) / dot(distAttn, "
              "float3(1.0, attn, attn*attn));\n");
  }
  else
  {
    // No attenuation (also directional lights).
    out.Write("ldir = normalize({}.pos.xyz - pos.xyz);\n", lt);
    out.Write("attn = 1.0;\n");
    out.Write("if (length(ldir) == 0.0)\n\t ldir = _normal;\n");
  }

  const char* swizzle = alpha ? "a" : "rgb";
  const char* width = alpha ? "" : "3";
  if (diffuse == DiffuseFunc::None)
  {
    out.Write("lacc.{0} += int{1}(round(attn * float{1}({2}.color.{0})));\n", swizzle, width, lt);
  }
  else if (diffuse == DiffuseFunc::Sign || diffuse == DiffuseFunc::Clamp)
  {
    const char* clamp_open = diffuse == DiffuseFunc::Clamp ? "max(0.0," : "(";
    out.Write("lacc.{0} += int{1}(round(attn * {2}dot(ldir, _normal)) * float{1}({3}.color.{0})));\n",
              swizzle, width, clamp_open, lt);
  }
  else
  {
    // Diffuse function 3 is not a valid setting.
    ASSERT(false);
  }
  out.Write("\n");
}

// Statement selecting the material or ambient colour of a channel: from the vertex colour or
// from the register (cmtrl[reg]).
void WriteColorSource(ShaderCode& out, std::string_view target, bool from_vertex, bool alpha_only,
                      u32 reg)
{
  if (from_vertex)
  {
    if (alpha_only)
      out.Write("\t{}.w = int(round(base_color.w * 255.0));\n", target);
    else
      out.Write("\t{} = int4(round(base_color * 255.0));\n", target);
  }
  else
  {
    if (alpha_only)
      out.Write("\t{}.w = {}[{}].w;\n", target, I_MATERIALS, reg);
    else
      out.Write("\t{} = {}[{}];\n", target, I_MATERIALS, reg);
  }
}

// One member of DolphinLightData with the value it is reset to.
struct LightField
{
  const char* name;
  const char* zero;
};
constexpr std::array<LightField, 6> LIGHT_FIELDS{{
    {"direction", "float3(0, 0, 0)"},
    {"position", "float3(0, 0, 0)"},
    {"color", "float3(0, 0, 0)"},
    {"cosatt", "float4(0, 0, 0, 0)"},
    {"distatt", "float4(0, 0, 0, 0)"},
    {"attenuation_type", "0"},
}};

// Copies light `light` into slot `slot` of the custom shader's light list.
void WriteCustomLight(ShaderCode& out, const LightingUidData& uid, u32 light, u32 lit_channel,
                      u32 channel, u32 slot, bool alpha)
{
  const std::string dst =
      fmt::format("frag_input.lights_chan{}_{}[{}]", channel, alpha ? "alpha" : "color", slot);
  const std::string src = fmt::format(I_LIGHTS "[{}]", light);

  out.Write("\t{{\n");
  out.Write("\t\t{}.direction = {}.dir.xyz;\n", dst, src);
  out.Write("\t\t{}.position = {}.pos.xyz;\n", dst, src);
  out.Write("\t\t{}.cosatt = {}.cosatt;\n", dst, src);
  out.Write("\t\t{}.distatt = {}.distatt;\n", dst, src);
  out.Write("\t\t{}.attenuation_type = {};\n", dst,
             static_cast<u32>(AttenuationOf(uid, lit_channel)));
  if (alpha)
    out.Write("\t\t{}.color = float3({}.color.a) / float3(255.0, 255.0, 255.0);\n", dst, src);
  else
    out.Write("\t\t{}.color = {}.color.rgb / float3(255.0, 255.0, 255.0);\n", dst, src);
  out.Write("\t}}\n");
}
}  // namespace

void WriteLightStruct(ShaderCode& out)
{
  out.Write("struct Light {{\n");
  for (const char* member : {"int4 color", "float4 cosatt", "float4 distatt", "float4 pos",
                             "float4 dir"})
  {
    out.Write("\t{};\n", member);
  }
  out.Write("}};\n");
}

void GenerateLightingShaderHeader(ShaderCode& object, const LightingUidData& uid_data)
{
  for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
  {
    const u32 color_part = chan;
    const u32 alpha_part = chan + 2;

    object.Write("vec4 dolphin_calculate_lighting_chn{}(vec4 base_color, vec3 pos, vec3 _normal)\n",
                 chan);
    object.Write("{{\n");
    object.Write("\tint4 lacc;\n");
    object.Write("\tvec3 ldir, h, cosAttn, distAttn;\n");
    object.Write("\tfloat dist, dist2, attn;\n");

    // Material colour, then the ambient colour that lacc starts from (white when unlit).
    const bool color_mat_vertex = Bit(uid_data.matsource, color_part);
    if (color_mat_vertex)
      object.Write("\tint4 mat = int4(round(base_color * 255.0));\n");
    else
      object.Write("\tint4 mat = {}[{}];\n", I_MATERIALS, chan + 2);

    const bool color_lit = Bit(uid_data.enablelighting, color_part);
    if (color_lit)
      WriteColorSource(object, "lacc", Bit(uid_data.ambsource, color_part), false, chan);
    else
      object.Write("\tlacc = int4(255, 255, 255, 255);\n");

    // The alpha half only needs its own material statement when its source differs.
    const bool alpha_mat_vertex = Bit(uid_data.matsource, alpha_part);
    if (alpha_mat_vertex != color_mat_vertex)
      WriteColorSource(object, "mat", alpha_mat_vertex, true, chan + 2);

    const bool alpha_lit = Bit(uid_data.enablelighting, alpha_part);
    if (alpha_lit)
      WriteColorSource(object, "lacc", Bit(uid_data.ambsource, alpha_part), true, chan);
    else
      object.Write("\tlacc.w = 255;\n");

    for (u32 light = 0; color_lit && light < 8; ++light)
    {
      if (LightEnabled(uid_data, color_part, light))
        WriteLightTerm(object, uid_data, light, color_part, false);
    }
    for (u32 light = 0; alpha_lit && light < 8; ++light)
    {
      if (LightEnabled(uid_data, alpha_part, light))
        WriteLightTerm(object, uid_data, light, alpha_part, true);
    }

    // lacc is scaled from 0..255 to 0..256 before modulating the material.
    object.Write("\tlacc = clamp(lacc, 0, 255);\n");
    object.Write("\treturn vec4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n");
    object.Write("}}\n\n");
  }
}

void GetLightingShaderUid(LightingUidData& uid_data)
{
  for (u32 lit = 0; lit < NUM_LIT_CHANNELS; lit++)
  {
    const u32 chan = lit % 2;
    const LitChannel& reg = lit < 2 ? xfmem.color[chan] : xfmem.alpha[chan];

    uid_data.matsource |= static_cast<u32>(reg.matsource.Value()) << lit;
    uid_data.enablelighting |= static_cast<u32>(reg.enablelighting.Value()) << lit;

    // The remaining fields only matter for lit channels.
    if (!reg.enablelighting)
      continue;
    uid_data.ambsource |= static_cast<u32>(reg.ambsource.Value()) << lit;
    uid_data.attnfunc |= static_cast<u32>(reg.attnfunc.Value()) << (2 * lit);
    uid_data.diffusefunc |= static_cast<u32>(reg.diffusefunc.Value()) << (2 * lit);
    uid_data.light_mask |= reg.GetFullLightMask() << (8 * lit);
  }
}

void GenerateCustomLighting(ShaderCode* out, const LightingUidData& uid_data)
{
  // Reset every light slot first.
  for (u32 slot = 0; slot < 8; slot++)
  {
    for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
    {
      for (const char* part : {"color", "alpha"})
      {
        for (const LightField& field : LIGHT_FIELDS)
        {
          out->Write("\tfrag_input.lights_chan{}_{}[{}].{} = {};\n", chan, part, slot, field.name,
                     field.zero);
        }
      }
    }
  }

  for (u32 chan = 0; chan < NUM_XF_COLOR_CHANNELS; chan++)
  {
    const u32 color_part = chan;
    const u32 alpha_part = chan + 2;

    const bool color_mat_vertex = Bit(uid_data.matsource, color_part);
    if (color_mat_vertex)
      out->Write("frag_input.base_material[{0}] = frag_input.color_{0};\n", chan);
    else
      out->Write("frag_input.base_material[{}] = {}[{}] / 255.0;\n", chan, I_MATERIALS, chan + 2);

    const bool color_lit = Bit(uid_data.enablelighting, color_part);
    if (!color_lit)
      out->Write("frag_input.ambient_lighting[{}] = float4(1, 1, 1, 1);\n", chan);
    else if (Bit(uid_data.ambsource, color_part))
      out->Write("frag_input.ambient_lighting[{0}] = frag_input.color_{0};\n", chan);
    else
      out->Write("frag_input.ambient_lighting[{0}] = {1}[{0}] / 255.0;\n", chan, I_MATERIALS);

    const bool alpha_mat_vertex = Bit(uid_data.matsource, alpha_part);
    if (alpha_mat_vertex != color_mat_vertex)
    {
      if (alpha_mat_vertex)
        out->Write("frag_input.base_material[{0}].w = frag_input.color_{0}.w;\n", chan);
      else
        out->Write("frag_input.base_material[{}].w = {}[{}].w / 255.0;\n", chan, I_MATERIALS,
                   chan + 2);
    }

    const bool alpha_lit = Bit(uid_data.enablelighting, alpha_part);
    if (!alpha_lit)
      out->Write("frag_input.ambient_lighting[{}].w = 1;\n", chan);
    else if (Bit(uid_data.ambsource, alpha_part))
      out->Write("frag_input.ambient_lighting[{0}].w = frag_input.color_{0}.w;\n", chan);
    else
      out->Write("frag_input.ambient_lighting[{0}].w = {1}[{0}].w / 255.0;\n", chan, I_MATERIALS);

    // The enabled lights fill the slots in order; the count tells the shader how many there are.
    for (const bool alpha : {false, true})
    {
      const u32 part = alpha ? alpha_part : color_part;
      u32 used = 0;
      if (Bit(uid_data.enablelighting, part))
      {
        for (u32 light = 0; light < 8; ++light)
        {
          if (LightEnabled(uid_data, part, light))
            WriteCustomLight(*out, uid_data, light, part, chan, used++, alpha);
        }
      }
      out->Write("\tfrag_input.light_chan{}_{}_count = {};\n", chan, alpha ? "alpha" : "color",
                 used);
    }
  }
}
