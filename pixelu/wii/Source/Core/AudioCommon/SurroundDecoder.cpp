// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/SurroundDecoder.h"

#include <limits>

#include <FreeSurround/FreeSurroundDecoder.h>

namespace AudioCommon
{
namespace
{
constexpr size_t IN_CHANNELS = 2;
constexpr size_t OUT_CHANNELS = 6;

// FreeSurround writes FL, FC, FR, BL, BR, LFE; the backends expect FL, FR, FC, LFE, BL, BR.
// Entry i is the FreeSurround channel that goes to output position i.
constexpr std::array<size_t, OUT_CHANNELS> CHANNEL_ORDER = {0, 2, 1, 5, 3, 4};
}  // namespace

SurroundDecoder::SurroundDecoder(u32 sample_rate, u32 frame_block_size)
    : m_block_frames(frame_block_size), m_decoder(std::make_unique<DPL2FSDecoder>())
{
  m_decoder->Init(cs_5point1, m_block_frames, sample_rate);
}

SurroundDecoder::~SurroundDecoder() = default;

void SurroundDecoder::Clear()
{
  m_decoder->flush();
  m_ready.clear();
}

size_t SurroundDecoder::QueryFramesNeededForSurroundOutput(const size_t output_frames) const
{
  const size_t ready_frames = m_ready.size() / OUT_CHANNELS;
  if (m_ready.size() >= output_frames * OUT_CHANNELS)
    return 0;

  // Round the shortfall up to whole blocks. A shortfall that is already a whole number of blocks
  // still gets one more block.
  const size_t missing = output_frames - ready_frames;
  return missing - missing % m_block_frames + m_block_frames;
}

void SurroundDecoder::PutFrames(const short* in, const size_t num_frames_in)
{
  constexpr float scale = std::numeric_limits<short>::max();

  // Decoding goes a whole block at a time; callers pass whole blocks.
  for (size_t done = 0; done < num_frames_in; done += m_block_frames)
  {
    const short* block = in + done * IN_CHANNELS;
    for (size_t i = 0; i < m_block_frames * IN_CHANNELS; ++i)
      m_block_input[i] = block[i] / scale;

    const float* decoded = m_decoder->decode(m_block_input.data());
    for (size_t frame = 0; frame < m_block_frames; ++frame)
    {
      for (const size_t channel : CHANNEL_ORDER)
        m_ready.push(decoded[frame * OUT_CHANNELS + channel]);
    }
  }
}

void SurroundDecoder::ReceiveFrames(float* out, const size_t num_frames_out)
{
  for (size_t i = 0; i < num_frames_out * OUT_CHANNELS; ++i)
    out[i] = m_ready.pop_front();
}

}  // namespace AudioCommon
