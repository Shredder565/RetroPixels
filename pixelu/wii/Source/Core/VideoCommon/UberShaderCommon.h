// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

class ShaderCode;
enum class APIType;
union ShaderHostConfig;

namespace UberShader
{
// CalculateLighting(): the contribution of one hardware light, with the attenuation and diffuse
// functions chosen at run time.
void WriteLightingFunction(ShaderCode& out);
// The per-channel lighting loop of the ubershaders, reading the XF registers from uniforms. The
// variable names say where the position, normal and vertex colours are and where the lit colours
// go.
void WriteVertexLighting(ShaderCode& out, APIType api_type, std::string_view world_pos_var,
                         std::string_view normal_var, std::string_view in_color_0_var,
                         std::string_view in_color_1_var, std::string_view out_color_0_var,
                         std::string_view out_color_1_var);
}  // namespace UberShader
