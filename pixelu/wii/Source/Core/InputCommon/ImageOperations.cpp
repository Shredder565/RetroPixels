// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ImageOperations.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "Common/IOFile.h"
#include "Common/Image.h"

namespace InputCommon
{
namespace
{
// Index of pixel (x, y) of a region whose top-left corner is (left, top). The arithmetic is
// deliberately 32-bit.
u32 PixelIndex(u32 image_width, u32 left, u32 top, u32 x, u32 y)
{
  return (y + top) * image_width + x + left;
}

// The source pixel nearest to the normalised coordinates (u, v).
Pixel SampleNearest(const ImagePixelData& src, double u, double v)
{
  const u32 column = std::min(static_cast<u32>(u * src.width), src.width - 1);
  const u32 row = std::min(static_cast<u32>(v * src.height), src.height - 1);
  return src.pixels[row * src.width + column];
}

// The border before a scaled image placed in `space` pixels: half the spare room, rounded up.
u32 LeadingBorder(u32 space, double used)
{
  const int spare = static_cast<int>(static_cast<double>(space) - std::trunc(used));
  int border = spare / 2;
  if (spare % 2 != 0)
    ++border;
  return static_cast<u32>(border);
}
}  // namespace

void CopyImageRegion(const ImagePixelData& src, ImagePixelData& dst, const Rect& src_region,
                     const Rect& dst_region)
{
  const u32 width = dst_region.GetWidth();
  const u32 height = dst_region.GetHeight();
  if (src_region.GetWidth() != width || src_region.GetHeight() != height)
    return;

  // Pixels that would fall outside either buffer are skipped.
  for (u32 y = 0; y < height; ++y)
  {
    for (u32 x = 0; x < width; ++x)
    {
      const u32 from = PixelIndex(src.width, src_region.left, src_region.top, x, y);
      const u32 to = PixelIndex(dst.width, dst_region.left, dst_region.top, x, y);
      if (from < src.pixels.size() && to < dst.pixels.size())
        dst.pixels[to] = src.pixels[from];
    }
  }
}

std::optional<ImagePixelData> LoadImage(const std::string& path)
{
  File::IOFile file;
  if (!file.Open(path, "rb"))
    return std::nullopt;

  const u64 size = file.GetSize();
  Common::UniqueBuffer<u8> contents(size);
  if (!file.ReadBytes(contents.data(), size))
    return std::nullopt;

  ImagePixelData image;
  Common::UniqueBuffer<u8> rgba;
  if (!Common::LoadPNG(contents, &rgba, &image.width, &image.height))
    return std::nullopt;

  // The decoder gives 4 bytes per pixel in RGBA order.
  const u32 count = image.width * image.height;
  image.pixels.resize(count);
  for (u32 i = 0; i < count; ++i)
  {
    const u8* const channels = &rgba[std::size_t{i} * 4];
    image.pixels[i] = Pixel{channels[0], channels[1], channels[2], channels[3]};
  }
  return image;
}

bool WriteImage(const std::string& path, const ImagePixelData& image)
{
  const u32 count = image.width * image.height;
  Common::UniqueBuffer<u8> rgba;
  rgba.reset(count * 4);

  u8* out = rgba.data();
  for (u32 i = 0; i < count; ++i)
  {
    const Pixel& pixel = image.pixels[i];
    *out++ = pixel.r;
    *out++ = pixel.g;
    *out++ = pixel.b;
    *out++ = pixel.a;
  }

  return Common::SavePNG(path, rgba.data(), Common::ImageByteFormat::RGBA, image.width,
                         image.height, image.width * 4);
}

ImagePixelData Resize(ResizeMode mode, const ImagePixelData& src, u32 new_width, u32 new_height)
{
  ImagePixelData result(new_width, new_height);
  if (mode != ResizeMode::Nearest)
    return result;

  // Output edges map to source edges.
  const double last_column = static_cast<double>(new_width - 1);
  const double last_row = static_cast<double>(new_height - 1);
  for (u32 y = 0; y < new_height; ++y)
  {
    const double v = y / last_row;
    for (u32 x = 0; x < new_width; ++x)
      result.pixels[y * new_width + x] = SampleNearest(src, x / last_column, v);
  }
  return result;
}

ImagePixelData ResizeKeepAspectRatio(ResizeMode mode, const ImagePixelData& src, u32 new_width,
                                     u32 new_height, const Pixel& background_color)
{
  ImagePixelData result(new_width, new_height, background_color);

  // The size the other side would need to keep src's shape.
  const double height_for_width = new_width * (src.height / static_cast<double>(src.width));
  const double width_for_height = new_height * (src.width / static_cast<double>(src.height));

  // Fill whichever side fits and centre along the other one.
  u32 left = 0;
  u32 top = 0;
  ImagePixelData scaled;
  if (height_for_width <= new_height)
  {
    top = LeadingBorder(new_height, height_for_width);
    scaled = Resize(mode, src, new_width, static_cast<u32>(height_for_width));
  }
  else
  {
    left = LeadingBorder(new_width, width_for_height);
    scaled = Resize(mode, src, static_cast<u32>(width_for_height), new_height);
  }

  CopyImageRegion(scaled, result, Rect{0, 0, scaled.width, scaled.height},
                  Rect{left, top, left + scaled.width, top + scaled.height});
  return result;
}
}  // namespace InputCommon
