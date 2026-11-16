// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// A baseline JPEG encoder (ITU-T T.81): YCbCr with 4:2:0 chroma, the example Huffman tables of
// the standard's Annex K, and a restart marker after every row of 16-pixel blocks so the rows
// can be encoded on several threads. Used for the frames of video dumps (Motion JPEG).

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

namespace VideoCommon
{
class JpegEncoder
{
public:
  // quality: 1 (smallest) to 100 (best), scaling the standard's example quantization tables.
  explicit JpegEncoder(int quality);

  // Encodes an RGBA8 image (rows `stride` bytes apart) into `out` (replacing its contents).
  void Encode(const u8* rgba, int width, int height, int stride, std::vector<u8>* out) const;

private:
  void EncodeRow(const u8* rgba, int width, int height, int stride, int row,
                 std::vector<u8>* out) const;

  u8 m_luma_quant[64];
  u8 m_chroma_quant[64];
  // Reciprocals of the quantizers with the DCT's scale folded in, in natural order.
  float m_luma_scale[64];
  float m_chroma_scale[64];
};
}  // namespace VideoCommon
