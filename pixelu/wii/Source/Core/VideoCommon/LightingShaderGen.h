// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

#include "Common/CommonTypes.h"

class ShaderCode;

// XF lighting state in UID form. Bit c of the 4-bit masks (and bits 2c..2c+1 of the function
// fields, bits 8c..8c+7 of light_mask) describe "lit channel" c: 0 and 1 are the colour halves of
// channels 0 and 1, 2 and 3 their alpha halves.
struct LightingUidData
{
  u32 matsource : 4;       // material from vertex colour (1) or register (0)
  u32 enablelighting : 4;  // lighting enabled
  u32 ambsource : 4;       // ambient from vertex colour (1) or register (0)
  u32 diffusefunc : 8;     // DiffuseFunc per lit channel
  u32 attnfunc : 8;        // AttenuationFunc per lit channel
  u32 light_mask : 32;     // lights 0-7 per lit channel
};

// The "Light" struct of the clights uniform array (VertexShaderConstants::Light).
void WriteLightStruct(ShaderCode& out);

// dolphin_calculate_lighting_chn0/1(): the fixed-function lighting of both channels.
void GenerateLightingShaderHeader(ShaderCode& object, const LightingUidData& uid_data);
// Fills uid_data from the current XF lighting registers.
void GetLightingShaderUid(LightingUidData& uid_data);
// Copies the light parameters into the DolphinFragmentInput of custom (graphics mod) shaders.
void GenerateCustomLighting(ShaderCode* out, const LightingUidData& uid_data);
