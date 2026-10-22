// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;

// Decoder for the 4-bit ADPCM of streamed disc audio. A 32-byte block holds
// one header byte per channel (filter in the high nibble, shift in the low
// nibble), two unused bytes and 28 bytes of samples: left in the low nibble,
// right in the high nibble.
namespace StreamADPCM
{
enum
{
  ONE_BLOCK_SIZE = 32,
  SAMPLES_PER_BLOCK = 28
};

class ADPCMDecoder
{
public:
  void ResetFilter();
  void DoState(PointerWrap& p);
  // Writes SAMPLES_PER_BLOCK interleaved stereo frames to `pcm`.
  void DecodeBlock(s16* pcm, const u8* adpcm);

private:
  // The previous two outputs of one channel's prediction filter.
  struct History
  {
    s32 last = 0;
    s32 before_last = 0;
  };

  History m_left;
  History m_right;
};
}  // namespace StreamADPCM
