// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/FixedSizeQueue.h"

namespace AudioCommon
{
// Turns matrix-encoded stereo s16 frames (as games write for Dolby Pro Logic II) into 5.1 float
// frames with a passive matrix decoder. Output channel order is FL, FR, FC, LFE, BL, BR.
class SurroundDecoder
{
public:
  explicit SurroundDecoder(u32 sample_rate, u32 frame_block_size);
  ~SurroundDecoder();

  // How many stereo frames PutFrames needs to be given (a whole number of blocks) before
  // `output_frames` surround frames can be received; 0 if enough are decoded already.
  size_t QueryFramesNeededForSurroundOutput(const size_t output_frames) const;
  void PutFrames(const short* in, const size_t num_frames_in);
  void ReceiveFrames(float* out, const size_t num_frames_out);
  void Clear();

private:
  // Second-order low-pass filter (for the LFE channel).
  struct LowPass
  {
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float Step(float x);
    void Reset() { x1 = x2 = y1 = y2 = 0; }
  };

  void DecodeFrame(float left, float right);

  u32 m_block_frames;
  LowPass m_lfe_filter;

  // The surround channels lag the front by a few milliseconds, which keeps sounds in front
  // where the matrix leaks some front sound into the back.
  std::vector<float> m_delay_left;
  std::vector<float> m_delay_right;
  size_t m_delay_pos = 0;

  // Decoded 5.1 samples waiting to be received.
  Common::FixedSizeQueue<float, 32768> m_ready;
};

}  // namespace AudioCommon
