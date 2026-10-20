// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"

namespace Common
{
// Decodes a PNG to tightly packed RGBA8 (transparency chunks applied).
bool LoadPNG(std::span<const u8> input, Common::UniqueBuffer<u8>* data_out, u32* width_out,
             u32* height_out);

enum class ImageByteFormat
{
  RGB,
  RGBA,
};

// Writes 8-bit RGB or RGBA pixels (rows `stride` bytes apart) as a PNG at zlib level `level`.
bool SavePNG(const std::string& path, const u8* input, ImageByteFormat format, u32 width,
             u32 height, u32 stride, int level = 6);
// Drops the alpha channel of RGBA pixels and saves the result as an RGB PNG.
bool ConvertRGBAToRGBAndSavePNG(const std::string& path, const u8* input, u32 width, u32 height,
                                u32 stride, int level);
}  // namespace Common
