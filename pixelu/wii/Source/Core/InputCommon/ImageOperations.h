// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Matrix.h"

// Small RGBA image helpers for dynamic input textures.
namespace InputCommon
{
// One 8-bit-per-channel RGBA pixel.
struct Pixel
{
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 0;

  bool operator==(const Pixel& other) const
  {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
};

using Point = Common::TVec2<u32>;
using Rect = MathUtil::Rectangle<u32>;

// Row-major pixels, top row first.
struct ImagePixelData
{
  ImagePixelData() = default;

  explicit ImagePixelData(std::vector<Pixel> image_pixels, u32 width_, u32 height_)
      : pixels(std::move(image_pixels)), width(width_), height(height_)
  {
  }

  // An image filled with one colour.
  explicit ImagePixelData(u32 width_, u32 height_, const Pixel& default_color = Pixel{0, 0, 0, 0})
      : pixels(width_ * height_, default_color), width(width_), height(height_)
  {
  }

  std::vector<Pixel> pixels;
  u32 width = 0;
  u32 height = 0;
};

// Copies src_region of src over dst_region of dst. Does nothing unless both regions are the
// same size.
void CopyImageRegion(const ImagePixelData& src, ImagePixelData& dst, const Rect& src_region,
                     const Rect& dst_region);

// Reads a PNG file.
std::optional<ImagePixelData> LoadImage(const std::string& path);

// Writes a PNG file.
bool WriteImage(const std::string& path, const ImagePixelData& image);

enum class ResizeMode
{
  Nearest,
};

// Stretches src to the new size.
ImagePixelData Resize(ResizeMode mode, const ImagePixelData& src, u32 new_width, u32 new_height);

// Scales src to fit inside the new size without distorting it, centred on background_color.
ImagePixelData ResizeKeepAspectRatio(ResizeMode mode, const ImagePixelData& src, u32 new_width,
                                     u32 new_height, const Pixel& background_color);
}  // namespace InputCommon
