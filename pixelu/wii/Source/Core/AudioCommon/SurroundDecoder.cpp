// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Passive matrix decoding. A Pro Logic II encoder folds five channels into two:
//   Lt = L + 0.707 C - a Ls - b Rs
//   Rt = R + 0.707 C + b Ls + a Rs        (a = 0.8718, b = 0.4899)
// so the centre is what the two channels share, and each surround channel is recovered by
// projecting (Lt, Rt) onto its own encoding direction. The subwoofer gets the low end of the
// sum. Nothing here adapts to the signal; it is the plain matrix inverse.

#include "AudioCommon/SurroundDecoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace AudioCommon
{
namespace
{
constexpr size_t IN_CHANNELS = 2;
constexpr size_t OUT_CHANNELS = 6;

constexpr float kSurroundMain = 0.8718f;
constexpr float kSurroundCross = 0.4899f;
constexpr float kCentre = std::numbers::sqrt2_v<float> / 2;
// Keeps the loudest decoded channel within range for full-scale input.
constexpr float kOutputGain = 0.5f;

constexpr float kLfeCutoffHz = 120.0f;
constexpr float kSurroundDelaySeconds = 0.010f;
}  // namespace

float SurroundDecoder::LowPass::Step(float x)
{
  const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
  x2 = x1;
  x1 = x;
  y2 = y1;
  y1 = y;
  return y;
}

SurroundDecoder::SurroundDecoder(u32 sample_rate, u32 frame_block_size)
    : m_block_frames(frame_block_size)
{
  // Butterworth low-pass by the bilinear transform.
  const float k = std::tan(std::numbers::pi_v<float> * kLfeCutoffHz / sample_rate);
  const float q = std::numbers::sqrt2_v<float> / 2;
  const float norm = 1.0f / (1.0f + k / q + k * k);
  m_lfe_filter.b0 = k * k * norm;
  m_lfe_filter.b1 = 2 * m_lfe_filter.b0;
  m_lfe_filter.b2 = m_lfe_filter.b0;
  m_lfe_filter.a1 = 2 * (k * k - 1) * norm;
  m_lfe_filter.a2 = (1 - k / q + k * k) * norm;

  const size_t delay = std::max<size_t>(1, static_cast<size_t>(sample_rate * kSurroundDelaySeconds));
  m_delay_left.assign(delay, 0.0f);
  m_delay_right.assign(delay, 0.0f);
}

SurroundDecoder::~SurroundDecoder() = default;

void SurroundDecoder::Clear()
{
  m_lfe_filter.Reset();
  std::fill(m_delay_left.begin(), m_delay_left.end(), 0.0f);
  std::fill(m_delay_right.begin(), m_delay_right.end(), 0.0f);
  m_delay_pos = 0;
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

void SurroundDecoder::DecodeFrame(float left, float right)
{
  const float centre = kCentre * (left + right);
  const float lfe = m_lfe_filter.Step(0.5f * (left + right));

  const float back_left = kSurroundMain * left - kSurroundCross * right;
  const float back_right = kSurroundMain * right - kSurroundCross * left;
  const float delayed_left = m_delay_left[m_delay_pos];
  const float delayed_right = m_delay_right[m_delay_pos];
  m_delay_left[m_delay_pos] = back_left;
  m_delay_right[m_delay_pos] = back_right;
  m_delay_pos = (m_delay_pos + 1) % m_delay_left.size();

  // FL, FR, FC, LFE, BL, BR. The encoder gave the left surround a negative sign in Lt.
  m_ready.push(kOutputGain * left);
  m_ready.push(kOutputGain * right);
  m_ready.push(kOutputGain * centre);
  m_ready.push(kOutputGain * lfe);
  m_ready.push(kOutputGain * -delayed_left);
  m_ready.push(kOutputGain * delayed_right);
}

void SurroundDecoder::PutFrames(const short* in, const size_t num_frames_in)
{
  constexpr float scale = std::numeric_limits<short>::max();

  // Callers pass whole blocks; the decoder itself works frame by frame.
  for (size_t frame = 0; frame < num_frames_in; ++frame)
    DecodeFrame(in[frame * IN_CHANNELS] / scale, in[frame * IN_CHANNELS + 1] / scale);
}

void SurroundDecoder::ReceiveFrames(float* out, const size_t num_frames_out)
{
  for (size_t i = 0; i < num_frames_out * OUT_CHANNELS; ++i)
    out[i] = m_ready.pop_front();
}

}  // namespace AudioCommon
