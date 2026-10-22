// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/StreamADPCM.h"

#include <algorithm>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"

namespace StreamADPCM
{
namespace
{
// Prediction weights (in 1/64ths) for the last and second-to-last output.
// Filter numbers above 3 predict nothing.
struct Weights
{
  s32 last;
  s32 before_last;
};
constexpr Weights FILTERS[4] = {{0, 0}, {0x3c, 0}, {0x73, -0x34}, {0x62, -0x37}};

// The history holds outputs with six fractional bits.
s16 DecodeNibble(u32 nibble, u8 header, s32& last, s32& before_last)
{
  const u32 filter = header >> 4;
  const u32 shift = header & 0xf;

  s32 prediction = 0;
  if (filter < 4)
    prediction = last * FILTERS[filter].last + before_last * FILTERS[filter].before_last;
  prediction = std::clamp((prediction + 0x20) >> 6, -0x200000, 0x1fffff);

  // The nibble is a signed value in the top four bits of a 16-bit word.
  const s32 sample = static_cast<s16>(nibble << 12);
  const s32 value = ((sample >> shift) << 6) + prediction;

  before_last = last;
  last = value;

  return static_cast<s16>(std::clamp(value >> 6, -0x8000, 0x7fff));
}
}  // namespace

void ADPCMDecoder::ResetFilter()
{
  m_left = {};
  m_right = {};
}

void ADPCMDecoder::DoState(PointerWrap& p)
{
  p.Do(m_left.last);
  p.Do(m_left.before_last);
  p.Do(m_right.last);
  p.Do(m_right.before_last);
}

void ADPCMDecoder::DecodeBlock(s16* pcm, const u8* adpcm)
{
  const u8 left_header = adpcm[0];
  const u8 right_header = adpcm[1];
  const u8* samples = adpcm + (ONE_BLOCK_SIZE - SAMPLES_PER_BLOCK);

  for (int frame = 0; frame < SAMPLES_PER_BLOCK; ++frame)
  {
    const u8 byte = samples[frame];
    *pcm++ = DecodeNibble(byte & 0xf, left_header, m_left.last, m_left.before_last);
    *pcm++ = DecodeNibble(byte >> 4, right_header, m_right.last, m_right.before_last);
  }
}
}  // namespace StreamADPCM
