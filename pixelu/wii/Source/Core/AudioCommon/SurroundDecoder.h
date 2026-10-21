// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/FixedSizeQueue.h"

class DPL2FSDecoder;

namespace AudioCommon
{
// Turns stereo s16 frames into 5.1 float frames with FreeSurround's Dolby Pro Logic II decoder.
// Output channel order is FL, FR, FC, LFE, BL, BR.
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
  u32 m_block_frames;
  std::unique_ptr<DPL2FSDecoder> m_decoder;

  // One block of stereo input converted to float.
  std::array<float, 32768> m_block_input{};

  // Decoded 5.1 samples waiting to be received.
  Common::FixedSizeQueue<float, 32768> m_ready;
};

}  // namespace AudioCommon
