// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Writing a surface to a 32-bit Windows bitmap (BITMAPV4HEADER, BI_BITFIELDS,
// BGRA byte order), for screenshots and for dumping texture RAM in each of
// the Real3D texel formats. A pixel format is a type with bytes_per_pixel and
// static GetRed/GetGreen/GetBlue/GetAlpha(const uint8_t *pixel).

#ifndef INCLUDED_BMPFILE_HPP
#define INCLUDED_BMPFILE_HPP

#include "OSD/Logger.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Util
{
  namespace bmp
  {
    // Texture RAM words are read in the host's byte order.
    inline uint16_t Texel16(const uint8_t *pixel)
    {
      uint16_t v;
      std::memcpy(&v, pixel, 2);
      return v;
    }

    // An n-bit channel value spread over 0..255 (truncating, as the float
    // product 255/max * v).
    template <unsigned Bits>
    inline uint8_t Expand(unsigned v)
    {
      constexpr float scale = 255.0f / float((1u << Bits) - 1);
      return uint8_t(scale * float(v & ((1u << Bits) - 1)));
    }

    inline void Put16(std::vector<uint8_t> &out, uint32_t v)
    {
      out.push_back(uint8_t(v));
      out.push_back(uint8_t(v >> 8));
    }

    inline void Put32(std::vector<uint8_t> &out, uint32_t v)
    {
      for (int i = 0; i < 4; i++)
        out.push_back(uint8_t(v >> (8 * i)));
    }

    constexpr uint32_t kFileHeaderSize = 14;
    constexpr uint32_t kInfoHeaderSize = 108; // BITMAPV4HEADER
    constexpr uint32_t kPixelOffset = kFileHeaderSize + kInfoHeaderSize;

    inline void PutHeaders(std::vector<uint8_t> &out, int32_t width, int32_t height)
    {
      uint32_t pixelBytes = uint32_t(width) * uint32_t(height) * 4;
      // BITMAPFILEHEADER
      out.push_back('B');
      out.push_back('M');
      Put32(out, kPixelOffset + pixelBytes);  // file size
      Put32(out, 0);                          // two reserved words
      Put32(out, kPixelOffset);
      // BITMAPV4HEADER
      Put32(out, kInfoHeaderSize);
      Put32(out, uint32_t(width));
      Put32(out, uint32_t(height));           // positive: bottom row first
      Put16(out, 1);                          // planes
      Put16(out, 32);                         // bits per pixel
      Put32(out, 3);                          // BI_BITFIELDS
      Put32(out, pixelBytes);
      Put32(out, 2835);                       // 72 dpi, in pixels per metre
      Put32(out, 2835);
      Put32(out, 0);                          // palette colours
      Put32(out, 0);                          // important colours
      Put32(out, 0x00ff0000);                 // red mask
      Put32(out, 0x0000ff00);                 // green mask
      Put32(out, 0x000000ff);                 // blue mask
      Put32(out, 0xff000000);                 // alpha mask
      Put32(out, 1);                          // LCS_DEVICE_RGB
      for (int i = 0; i < 12; i++)            // endpoints and gamma: unused
        Put32(out, 0);
    }
  } // bmp

  // 8-bit R, G, B, A bytes.
  struct RGBA8
  {
    static constexpr unsigned bytes_per_pixel = 4;
    static uint8_t GetRed(const uint8_t *p)   { return p[0]; }
    static uint8_t GetGreen(const uint8_t *p) { return p[1]; }
    static uint8_t GetBlue(const uint8_t *p)  { return p[2]; }
    static uint8_t GetAlpha(const uint8_t *p) { return p[3]; }
  };

  // Texel format 0: T RRRRR GGGGG BBBBB. With the contour bit honoured, T set
  // means transparent; otherwise every texel is opaque.
  template <bool EnableContour>
  struct T1RGB5
  {
    static constexpr unsigned bytes_per_pixel = 2;
    static uint8_t GetRed(const uint8_t *p)   { return bmp::Expand<5>(bmp::Texel16(p) >> 10); }
    static uint8_t GetGreen(const uint8_t *p) { return bmp::Expand<5>(bmp::Texel16(p) >> 5); }
    static uint8_t GetBlue(const uint8_t *p)  { return bmp::Expand<5>(bmp::Texel16(p)); }
    static uint8_t GetAlpha(const uint8_t *p)
    {
      if (!EnableContour)
        return 0xff;
      return (bmp::Texel16(p) & 0x8000) ? 0x00 : 0xff;
    }
  };

  using T1RGB5ContourEnabled = T1RGB5<true>;
  using T1RGB5ContourIgnored = T1RGB5<false>;

  // Grey-scale texels: a 4-bit luminance and a 4-bit alpha at the given bit
  // positions of the 16-bit word (formats 1-4).
  template <unsigned LumShift, unsigned AlphaShift>
  struct LumAlpha4
  {
    static constexpr unsigned bytes_per_pixel = 2;
    static uint8_t Lum(const uint8_t *p)      { return bmp::Expand<4>(bmp::Texel16(p) >> LumShift); }
    static uint8_t GetRed(const uint8_t *p)   { return Lum(p); }
    static uint8_t GetGreen(const uint8_t *p) { return Lum(p); }
    static uint8_t GetBlue(const uint8_t *p)  { return Lum(p); }
    static uint8_t GetAlpha(const uint8_t *p) { return bmp::Expand<4>(bmp::Texel16(p) >> AlphaShift); }
  };

  using A4L4Low = LumAlpha4<0, 4>;    // format 1: .... .... AAAA LLLL
  using L4A4Low = LumAlpha4<4, 0>;    // format 2: .... .... LLLL AAAA
  using A4L4High = LumAlpha4<8, 12>;  // format 3: AAAA LLLL .... ....
  using L4A4High = LumAlpha4<12, 8>;  // format 4: LLLL AAAA .... ....

  // 8-bit luminance in one byte of the word; 0xff is transparent (formats 5
  // and 6).
  template <unsigned Shift>
  struct Lum8
  {
    static constexpr unsigned bytes_per_pixel = 2;
    static uint8_t Lum(const uint8_t *p)      { return uint8_t(bmp::Texel16(p) >> Shift); }
    static uint8_t GetRed(const uint8_t *p)   { return Lum(p); }
    static uint8_t GetGreen(const uint8_t *p) { return Lum(p); }
    static uint8_t GetBlue(const uint8_t *p)  { return Lum(p); }
    static uint8_t GetAlpha(const uint8_t *p) { return Lum(p) == 0xff ? 0x00 : 0xff; }
  };

  using L8Low = Lum8<0>;
  using L8High = Lum8<8>;

  // Texel format 7: RRRR GGGG BBBB AAAA.
  struct RGBA4
  {
    static constexpr unsigned bytes_per_pixel = 2;
    static uint8_t GetRed(const uint8_t *p)   { return bmp::Expand<4>(bmp::Texel16(p) >> 12); }
    static uint8_t GetGreen(const uint8_t *p) { return bmp::Expand<4>(bmp::Texel16(p) >> 8); }
    static uint8_t GetBlue(const uint8_t *p)  { return bmp::Expand<4>(bmp::Texel16(p) >> 4); }
    static uint8_t GetAlpha(const uint8_t *p) { return bmp::Expand<4>(bmp::Texel16(p)); }
  };

  // Texel formats 8-11: a 4-bit luminance in nibble Channel (0 = lowest);
  // 0xf is transparent.
  template <int Channel>
  struct L4
  {
    static constexpr unsigned bytes_per_pixel = 2;
    static unsigned Nibble(const uint8_t *p)  { return (bmp::Texel16(p) >> (4 * Channel)) & 0xf; }
    static uint8_t GetRed(const uint8_t *p)   { return bmp::Expand<4>(Nibble(p)); }
    static uint8_t GetGreen(const uint8_t *p) { return bmp::Expand<4>(Nibble(p)); }
    static uint8_t GetBlue(const uint8_t *p)  { return bmp::Expand<4>(Nibble(p)); }
    static uint8_t GetAlpha(const uint8_t *p) { return Nibble(p) == 0xf ? 0x00 : 0xff; }
  };

  using L4Channel0 = L4<0>;
  using L4Channel1 = L4<1>;
  using L4Channel2 = L4<2>;
  using L4Channel3 = L4<3>;

  // Writes a width x height surface (rows top to bottom in 'pixels') to
  // file_name. A bitmap stores the bottom row first, so the rows are written
  // in reverse unless flip_vertical is set (for a surface that is already
  // bottom-up, such as an OpenGL read-back). Returns true on failure.
  template <class SurfaceFormat>
  static bool WriteSurfaceToBMP(const std::string &file_name, const uint8_t *pixels, int32_t width, int32_t height, bool flip_vertical)
  {
    std::vector<uint8_t> out;
    out.reserve(bmp::kPixelOffset + size_t(width) * size_t(height) * 4);
    bmp::PutHeaders(out, width, height);
    const size_t rowBytes = size_t(width) * SurfaceFormat::bytes_per_pixel;
    for (int32_t i = 0; i < height; i++)
    {
      int32_t y = flip_vertical ? i : height - 1 - i;
      const uint8_t *src = pixels + size_t(y) * rowBytes;
      for (int32_t x = 0; x < width; x++, src += SurfaceFormat::bytes_per_pixel)
      {
        out.push_back(SurfaceFormat::GetBlue(src));
        out.push_back(SurfaceFormat::GetGreen(src));
        out.push_back(SurfaceFormat::GetRed(src));
        out.push_back(SurfaceFormat::GetAlpha(src));
      }
    }
    FILE *fp = fopen(file_name.c_str(), "wb");
    if (!fp)
    {
      ErrorLog("Unable to open '%s' for writing.", file_name.c_str());
      return true;
    }
    fwrite(out.data(), 1, out.size(), fp);
    fclose(fp);
    return false;
  }
} // Util

#endif  // INCLUDED_BMPFILE_HPP
