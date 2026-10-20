// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Image.h"

#include <memory>
#include <string>

#include <spng.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"

namespace Common
{
namespace
{
struct SpngDeleter
{
  void operator()(spng_ctx* ctx) const { spng_ctx_free(ctx); }
};
using SpngContext = std::unique_ptr<spng_ctx, SpngDeleter>;

SpngContext NewContext(int flags)
{
  return SpngContext(spng_ctx_new(flags));
}
}  // namespace

bool LoadPNG(std::span<const u8> input, Common::UniqueBuffer<u8>* data_out, u32* width_out,
             u32* height_out)
{
  const SpngContext ctx = NewContext(0);
  if (!ctx || spng_set_png_buffer(ctx.get(), input.data(), input.size()) != 0)
    return false;

  spng_ihdr header{};
  if (spng_get_ihdr(ctx.get(), &header) != 0)
    return false;

  constexpr int OUTPUT_FORMAT = SPNG_FMT_RGBA8;
  size_t size = 0;
  if (spng_decoded_image_size(ctx.get(), OUTPUT_FORMAT, &size) != 0)
    return false;

  data_out->reset(size);
  if (spng_decode_image(ctx.get(), data_out->data(), size, OUTPUT_FORMAT, SPNG_DECODE_TRNS) != 0)
    return false;

  *width_out = header.width;
  *height_out = header.height;
  return true;
}

bool SavePNG(const std::string& path, const u8* input, ImageByteFormat format, u32 width,
             u32 height, u32 stride, int level)
{
  Common::Timer timer;
  timer.Start();

  u8 color_type;
  if (format == ImageByteFormat::RGB)
  {
    color_type = SPNG_COLOR_TYPE_TRUECOLOR;
  }
  else if (format == ImageByteFormat::RGBA)
  {
    color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
  }
  else
  {
    ASSERT_MSG(FRAMEDUMP, false, "Invalid format {}", static_cast<int>(format));
    return false;
  }

  File::IOFile file(path, "wb");
  const SpngContext ctx = NewContext(SPNG_CTX_ENCODER);
  if (!ctx)
    return false;
  if (spng_set_png_file(ctx.get(), file.GetHandle()) != 0)
    return false;
  if (spng_set_option(ctx.get(), SPNG_IMG_COMPRESSION_LEVEL, level) != 0)
    return false;

  spng_ihdr header{};
  header.width = width;
  header.height = height;
  header.color_type = color_type;
  header.bit_depth = 8;
  if (spng_set_ihdr(ctx.get(), &header) != 0)
    return false;

  // Row by row, so the caller's stride can differ from the packed row size.
  if (spng_encode_image(ctx.get(), nullptr, 0, SPNG_FMT_PNG,
                        SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE) != 0)
  {
    return false;
  }
  for (u32 y = 0; y < height; ++y)
  {
    const int error = spng_encode_row(ctx.get(), input + static_cast<size_t>(y) * stride, stride);
    if (error == SPNG_EOI)
      break;
    if (error != 0)
    {
      ERROR_LOG_FMT(FRAMEDUMP, "Failed to save {} by {} image to {} at level {}: error {}", width,
                    height, path, level, error);
      return false;
    }
  }

  size_t encoded_size = 0;
  spng_decoded_image_size(ctx.get(), SPNG_FMT_PNG, &encoded_size);
  INFO_LOG_FMT(FRAMEDUMP, "{} byte {} by {} image saved to {} at level {} in {} ms", encoded_size,
               width, height, path, level, timer.ElapsedMs());
  return true;
}

bool ConvertRGBAToRGBAndSavePNG(const std::string& path, const u8* input, u32 width, u32 height,
                                u32 stride, int level)
{
  Common::UniqueBuffer<u8> rgb(static_cast<size_t>(width) * height * 3);
  u8* out = rgb.data();
  for (u32 y = 0; y < height; ++y)
  {
    const u8* pixel = input + static_cast<size_t>(y) * stride;
    for (u32 x = 0; x < width; ++x, pixel += 4)
    {
      *out++ = pixel[0];
      *out++ = pixel[1];
      *out++ = pixel[2];
    }
  }
  return SavePNG(path, rgb.data(), ImageByteFormat::RGB, width, height, width * 3, level);
}
}  // namespace Common
