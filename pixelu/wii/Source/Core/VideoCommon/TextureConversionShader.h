// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"

enum class APIType;
enum class TextureFormat;
enum class EFBCopyFormat;
enum class TLUTFormat;
enum TexelBufferFormat : u32;
struct EFBCopyParams;

// GPU shaders that convert between the console's tiled texture formats and RGBA8.
namespace TextureConversionShaderTiled
{
// How many EFB texels one RGBA8 pixel of an EFB-to-RAM encoder output holds.
u16 GetEncodedSampleCount(EFBCopyFormat format);

// Fragment shader encoding an EFB copy into the tiled memory layout of its copy format.
std::string GenerateEncodingShader(const EFBCopyParams& params, APIType api_type);

// What is needed to compile and dispatch the compute shader that decodes one texture format.
struct DecodingShaderInfo
{
  TexelBufferFormat buffer_format;
  u32 palette_size;
  u32 group_size_x;
  u32 group_size_y;
  bool group_flatten;
  const char* shader_body;
};

// The decoder for a format, or nullptr for formats that have no GPU decoder.
const DecodingShaderInfo* GetDecodingShaderInfo(TextureFormat format);

// Thread groups to dispatch for a width x height image, as (x, y); z is always 1.
std::pair<u32, u32> GetDispatchCount(const DecodingShaderInfo* info, u32 width, u32 height);

// Full GLSL source of the decoder for `format` (empty if there is none), set up for the given
// palette format when the texture is paletted.
std::string GenerateDecodingShader(TextureFormat format, std::optional<TLUTFormat> palette_format,
                                   APIType api_type);

// GLSL source of the fragment shader that applies a palette of `palette_format` to an index
// texture.
std::string GeneratePaletteConversionShader(TLUTFormat palette_format, APIType api_type);

}  // namespace TextureConversionShaderTiled
