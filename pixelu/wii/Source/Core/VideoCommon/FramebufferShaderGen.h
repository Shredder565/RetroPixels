// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

enum class EFBReinterpretType;
enum class TextureFormat;

// Small utility shaders used by the framebuffer manager, the texture cache and the UI overlay.
// All of them are written in the shared GLSL-like dialect the backends translate.
namespace FramebufferShaderGen
{
// Fullscreen triangle, texture coordinate in v_tex0.
std::string GenerateScreenQuadVertexShader();
// Duplicates each triangle into both stereo layers.
std::string GeneratePassthroughGeometryShader(u32 num_tex, u32 num_colors);
std::string GenerateTextureCopyVertexShader();
std::string GenerateTextureCopyPixelShader();
std::string GenerateResolveColorPixelShader(u32 samples);
std::string GenerateResolveDepthPixelShader(u32 samples);
std::string GenerateClearVertexShader();
std::string GenerateEFBPokeVertexShader();
std::string GenerateColorPixelShader();
std::string GenerateFormatConversionShader(EFBReinterpretType convtype, u32 samples);
std::string GenerateTextureReinterpretShader(TextureFormat from_format, TextureFormat to_format);
std::string GenerateEFBRestorePixelShader();
std::string GenerateImGuiVertexShader();
std::string GenerateImGuiPixelShader(bool linear_space_output = false);

}  // namespace FramebufferShaderGen
