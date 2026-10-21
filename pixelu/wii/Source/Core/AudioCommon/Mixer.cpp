// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/Mixer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "AudioCommon/Enums.h"
#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/System.h"

namespace
{
// The read position of a source is a 32-bit number whose top 8 bits pick a frame of a granule
// and whose low 24 bits are the fraction between two frames.
constexpr u32 FRACTION_BITS = 24;
constexpr u32 FRACTION_ONE = u32{1} << FRACTION_BITS;
constexpr u32 HALF_GRANULE_PHASE = 0x80000000;

constexpr std::size_t SURROUND_OUT_CHANNELS = 6;
constexpr std::size_t MAX_SURROUND_INPUT_FRAMES = 0x8000;

// First half of the granule window: a Slepian (DPSS, NW = 4) taper of 129 points convolved with
// a 128-point box, scaled so that two windows offset by half a granule add up to one at most.
// The window is symmetric; the second half is this table reversed.
constexpr std::array<float, 128> WINDOW_FIRST_HALF = {
    0.0000016272f, 0.0000050749f, 0.0000113187f, 0.0000216492f, 0.0000377350f,
    0.0000616906f, 0.0000961509f, 0.0001443499f, 0.0002102045f, 0.0002984010f,
    0.0004144844f, 0.0005649486f, 0.0007573262f, 0.0010002765f, 0.0013036694f,
    0.0016786636f, 0.0021377783f, 0.0026949534f, 0.0033656000f, 0.0041666352f,
    0.0051165029f, 0.0062351752f, 0.0075441359f, 0.0090663409f, 0.0108261579f,
    0.0128492811f, 0.0151626215f, 0.0177941726f, 0.0207728499f, 0.0241283062f,
    0.0278907219f, 0.0320905724f, 0.0367583739f, 0.0419244083f, 0.0476184323f,
    0.0538693708f, 0.0607049996f, 0.0681516192f, 0.0762337261f, 0.0849736833f,
    0.0943913952f, 0.1045039915f, 0.1153255250f, 0.1268666867f, 0.1391345431f,
    0.1521323012f, 0.1658591025f, 0.1803098534f, 0.1954750915f, 0.2113408944f,
    0.2278888303f, 0.2450959552f, 0.2629348550f, 0.2813737361f, 0.3003765625f,
    0.3199032396f, 0.3399098438f, 0.3603488941f, 0.3811696664f, 0.4023185434f,
    0.4237393998f, 0.4453740162f, 0.4671625177f, 0.4890438330f, 0.5109561670f,
    0.5328374823f, 0.5546259838f, 0.5762606002f, 0.5976814566f, 0.6188303336f,
    0.6396511059f, 0.6600901562f, 0.6800967604f, 0.6996234375f, 0.7186262639f,
    0.7370651450f, 0.7549040448f, 0.7721111697f, 0.7886591056f, 0.8045249085f,
    0.8196901466f, 0.8341408975f, 0.8478676988f, 0.8608654569f, 0.8731333133f,
    0.8846744750f, 0.8954960085f, 0.9056086048f, 0.9150263167f, 0.9237662739f,
    0.9318483808f, 0.9392950004f, 0.9461306292f, 0.9523815677f, 0.9580755917f,
    0.9632416261f, 0.9679094276f, 0.9721092781f, 0.9758716938f, 0.9792271501f,
    0.9822058274f, 0.9848373785f, 0.9871507189f, 0.9891738421f, 0.9909336591f,
    0.9924558641f, 0.9937648248f, 0.9948834971f, 0.9958333648f, 0.9966344000f,
    0.9973050466f, 0.9978622217f, 0.9983213364f, 0.9986963306f, 0.9989997235f,
    0.9992426738f, 0.9994350514f, 0.9995855156f, 0.9997015990f, 0.9997897955f,
    0.9998556501f, 0.9999038491f, 0.9999383094f, 0.9999622650f, 0.9999783508f,
    0.9999886813f, 0.9999949251f, 0.9999983728f,
};

float WindowWeight(std::size_t frame)
{
  const std::size_t half = WINDOW_FIRST_HALF.size();
  return WINDOW_FIRST_HALF[frame < half ? frame : 2 * half - 1 - frame];
}

u32 SurroundBlockFrames(AudioCommon::DPL2Quality quality)
{
  switch (quality)
  {
  case AudioCommon::DPL2Quality::Lowest:
    return 512;
  case AudioCommon::DPL2Quality::Low:
    return 1024;
  case AudioCommon::DPL2Quality::Highest:
    return 4096;
  case AudioCommon::DPL2Quality::High:
  default:
    return 2048;
  }
}

// Rounds one channel of a mixed frame into the s16 output, carrying the rounding error (at most
// one step either way) into the next frame of that channel.
void StoreRounded(s16& out, float value, float& carry)
{
  value += static_cast<float>(out) - carry;
  out = MathUtil::SaturatingCast<s16>(std::lround(value));
  carry = std::clamp(static_cast<float>(out) - value, -1.0f, 1.0f);
}

void StartDump(WaveFileWriter& writer, bool& active, const std::string& path, u32 divisor,
               const char* kind)
{
  if (active)
  {
    WARN_LOG_FMT(AUDIO, "{} Audio logging has already been started", kind);
    return;
  }

  if (!writer.Start(path, divisor))
  {
    writer.Stop();
    NOTICE_LOG_FMT(AUDIO, "Unable to start {} Audio logging", kind);
    return;
  }

  active = true;
  writer.SetSkipSilence(false);
  NOTICE_LOG_FMT(AUDIO, "Starting {} Audio logging", kind);
}

void StopDump(WaveFileWriter& writer, bool& active, const char* kind)
{
  if (!active)
  {
    WARN_LOG_FMT(AUDIO, "{} Audio logging has already been stopped", kind);
    return;
  }

  active = false;
  writer.Stop();
  NOTICE_LOG_FMT(AUDIO, "Stopping {} Audio logging", kind);
}
}  // namespace

Mixer::Mixer(u32 BackendSampleRate)
    : m_output_sample_rate(BackendSampleRate),
      m_surround(BackendSampleRate, SurroundBlockFrames(Config::Get(Config::MAIN_DPL2_QUALITY)))
{
  m_config_callback = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  INFO_LOG_FMT(AUDIO_INTERFACE, "Mixer is initialized");
}

Mixer::~Mixer()
{
  Config::RemoveConfigChangedCallback(m_config_callback);
}

void Mixer::RefreshConfig()
{
  Settings settings;
  settings.emulation_speed = Config::Get(Config::MAIN_EMULATION_SPEED);
  settings.preserve_pitch = Config::Get(Config::MAIN_AUDIO_PRESERVE_PITCH);
  settings.fill_gaps = Config::Get(Config::MAIN_AUDIO_FILL_GAPS);
  settings.buffer_ms = Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE);
  settings.wiimote_routing = Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED);
  for (std::size_t i = 0; i < settings.wiimote_output.size(); ++i)
    settings.wiimote_output[i] = Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]);
  m_settings = settings;
}

void Mixer::DoState(PointerWrap& p)
{
  // Order: DMA, streaming, the four speakers, the portal, the four GBAs.
  m_dma.DoState(p);
  m_streaming.DoState(p);
  for (Source& speaker : m_wiimote_speakers)
    speaker.DoState(p);
  m_skylander_portal.DoState(p);
  for (Source& gba : m_gbas)
    gba.DoState(p);
}

std::size_t Mixer::Mix(s16* samples, std::size_t numSamples)
{
  if (samples == nullptr)
    return 0;

  std::fill_n(samples, numSamples * 2, s16{0});

  m_dma.AddTo(samples, numSamples);
  m_streaming.AddTo(samples, numSamples);
  for (std::size_t i = 0; i < m_wiimote_speakers.size(); ++i)
  {
    // A speaker routed to a device of its own is played there (MixWiimoteSpeaker) instead.
    const bool routed_elsewhere = m_settings.wiimote_routing && m_settings.wiimote_output[i];
    if (!routed_elsewhere)
      m_wiimote_speakers[i].AddTo(samples, numSamples);
  }
  m_skylander_portal.AddTo(samples, numSamples);
  for (Source& gba : m_gbas)
    gba.AddTo(samples, numSamples);

  return numSamples;
}

std::size_t Mixer::MixSurround(float* samples, std::size_t num_samples)
{
  if (num_samples == 0)
    return 0;

  std::fill_n(samples, num_samples * SURROUND_OUT_CHANNELS, 0.0f);

  const std::size_t stereo_frames = m_surround.QueryFramesNeededForSurroundOutput(num_samples);
  ASSERT_MSG(AUDIO, stereo_frames <= MAX_SURROUND_INPUT_FRAMES,
             "needed_frames would overflow m_scratch_buffer: {} -> {} > {}", num_samples,
             stereo_frames, MAX_SURROUND_INPUT_FRAMES);

  // Only the audio thread calls this, so one buffer can be reused across calls.
  static thread_local std::vector<s16> stereo;
  stereo.resize(std::max(stereo_frames, MAX_SURROUND_INPUT_FRAMES) * 2);

  const std::size_t mixed = Mix(stereo.data(), stereo_frames);
  if (mixed != stereo_frames)
  {
    ERROR_LOG_FMT(AUDIO,
                  "Error decoding surround frames: needed {} frames for {} samples but got {}",
                  stereo_frames, num_samples, mixed);
    return 0;
  }

  m_surround.PutFrames(stereo.data(), stereo_frames);
  m_surround.ReceiveFrames(samples, num_samples);
  return num_samples;
}

void Mixer::PushSamples(const s16* samples, std::size_t num_samples)
{
  // The DSP produces big-endian frames with the right channel first.
  if (IsOutputSampleRateValid())
  {
    for (std::size_t i = 0; i < num_samples; ++i)
    {
      const u16 right = Common::swap16(static_cast<u16>(samples[2 * i]));
      const u16 left = Common::swap16(static_cast<u16>(samples[2 * i + 1]));
      m_dma.Push(static_cast<s16>(left), static_cast<s16>(right));
    }
  }

  if (m_dsp_dumping)
  {
    const auto [left_volume, right_volume] = m_dma.GetVolume();
    m_dsp_dump.AddStereoSamplesBE(samples, static_cast<u32>(num_samples), m_dma.GetDivisor(),
                                  left_volume, right_volume);
  }
}

void Mixer::PushStreamingSamples(const s16* samples, std::size_t num_samples)
{
  // Same layout as the DSP: big-endian, right channel first.
  if (IsOutputSampleRateValid())
  {
    for (std::size_t i = 0; i < num_samples; ++i)
    {
      const u16 right = Common::swap16(static_cast<u16>(samples[2 * i]));
      const u16 left = Common::swap16(static_cast<u16>(samples[2 * i + 1]));
      m_streaming.Push(static_cast<s16>(left), static_cast<s16>(right));
    }
  }

  if (m_dtk_dumping)
  {
    const auto [left_volume, right_volume] = m_streaming.GetVolume();
    m_dtk_dump.AddStereoSamplesBE(samples, static_cast<u32>(num_samples),
                                  m_streaming.GetDivisor(), left_volume, right_volume);
  }
}

void Mixer::PushWiimoteSpeakerSamples(std::size_t wiimote_index, const s16* samples,
                                      std::size_t num_samples, u32 sample_rate_divisor)
{
  if (!IsOutputSampleRateValid() || wiimote_index >= m_wiimote_speakers.size())
    return;

  // Mono samples in host byte order, played on both channels.
  Source& speaker = m_wiimote_speakers[wiimote_index];
  speaker.SetDivisor(sample_rate_divisor);
  for (std::size_t i = 0; i < num_samples; ++i)
    speaker.Push(samples[i], samples[i]);
}

std::size_t Mixer::MixWiimoteSpeaker(std::size_t wiimote_index, s16* samples,
                                     std::size_t num_samples)
{
  if (samples == nullptr || wiimote_index >= m_wiimote_speakers.size())
    return 0;

  std::fill_n(samples, num_samples * 2, s16{0});
  m_wiimote_speakers[wiimote_index].AddTo(samples, num_samples);
  return num_samples;
}

void Mixer::PushSkylanderPortalSamples(const u8* samples, std::size_t num_samples)
{
  if (!IsOutputSampleRateValid())
    return;

  // The portal speaker sends mono 16-bit little-endian samples.
  for (std::size_t i = 0; i < num_samples; ++i)
  {
    const u16 raw = static_cast<u16>(samples[2 * i] | (samples[2 * i + 1] << 8));
    m_skylander_portal.Push(static_cast<s16>(raw), static_cast<s16>(raw));
  }
}

void Mixer::PushGBASamples(std::size_t device_number, const s16* samples, std::size_t num_samples)
{
  if (!IsOutputSampleRateValid())
    return;

  // Host byte order, left channel first.
  Source& gba = m_gbas[device_number];
  for (std::size_t i = 0; i < num_samples; ++i)
    gba.Push(samples[2 * i], samples[2 * i + 1]);
}

void Mixer::SetDMAInputSampleRateDivisor(u32 rate_divisor)
{
  m_dma.SetDivisor(rate_divisor);
}

void Mixer::SetStreamInputSampleRateDivisor(u32 rate_divisor)
{
  m_streaming.SetDivisor(rate_divisor);
}

void Mixer::SetGBAInputSampleRate(std::size_t device_number, u32 sample_rate)
{
  m_gbas[device_number].SetDivisor(GBA_DIVIDEND / sample_rate);
}

void Mixer::SetStreamingVolume(u32 lvolume, u32 rvolume)
{
  m_streaming.SetVolume(std::min<u32>(lvolume, 0xff), std::min<u32>(rvolume, 0xff));
}

void Mixer::SetWiimoteSpeakerVolume(std::size_t wiimote_index, u32 lvolume, u32 rvolume)
{
  if (wiimote_index < m_wiimote_speakers.size())
    m_wiimote_speakers[wiimote_index].SetVolume(lvolume, rvolume);
}

void Mixer::SetGBAVolume(std::size_t device_number, u32 lvolume, u32 rvolume)
{
  m_gbas[device_number].SetVolume(lvolume, rvolume);
}

void Mixer::StartLogDTKAudio(const std::string& filename)
{
  StartDump(m_dtk_dump, m_dtk_dumping, filename, m_streaming.GetDivisor(), "DTK");
}

void Mixer::StopLogDTKAudio()
{
  StopDump(m_dtk_dump, m_dtk_dumping, "DTK");
}

void Mixer::StartLogDSPAudio(const std::string& filename)
{
  StartDump(m_dsp_dump, m_dsp_dumping, filename, m_dma.GetDivisor(), "DSP");
}

void Mixer::StopLogDSPAudio()
{
  StopDump(m_dsp_dump, m_dsp_dumping, "DSP");
}

void Mixer::Source::DoState(PointerWrap& p)
{
  p.Do(m_divisor);
  p.Do(m_volume_left);
  p.Do(m_volume_right);
}

void Mixer::Source::SetVolume(u32 left, u32 right)
{
  // Map 0..255 onto 0..256 so that full volume is exact unity gain.
  m_volume_left.store(static_cast<s32>(left + (left >> 7)));
  m_volume_right.store(static_cast<s32>(right + (right >> 7)));
}

std::pair<s32, s32> Mixer::Source::GetVolume() const
{
  return {m_volume_left.load(), m_volume_right.load()};
}

void Mixer::Source::PublishGranule()
{
  const std::size_t slot = m_ring_write.load(std::memory_order_acquire);
  const std::size_t next_slot = (slot + 1) % RING_SLOTS;
  if (next_slot == m_ring_read.load(std::memory_order_acquire))
  {
    WARN_LOG_FMT(AUDIO,
                 "Granule Queue has completely filled and audio samples are being dropped. "
                 "This should not happen unless the audio backend has stopped requesting audio.");
    return;
  }

  // The write position now points at the oldest frame of the pending granule.
  Granule& granule = m_ring[slot];
  for (std::size_t i = 0; i < GRANULE_FRAMES; ++i)
  {
    const Frame& frame = m_pending[(m_pending_pos + i) % GRANULE_FRAMES];
    const float weight = WindowWeight(i);
    granule[i] = {frame.left * weight, frame.right * weight};
  }

  m_ring_write.store(next_slot, std::memory_order_release);
  m_fading.store(false, std::memory_order_relaxed);
  m_repeating.store(false, std::memory_order_relaxed);
}

bool Mixer::Source::TakeGranule(Granule& out)
{
  const std::size_t depth = m_target_depth.load(std::memory_order_relaxed);
  const std::size_t write = m_ring_write.load(std::memory_order_acquire);
  std::size_t read = m_ring_read.load(std::memory_order_acquire);

  // Fallen too far behind the producer: drop granules until half the target depth is left.
  if ((write - read) % RING_SLOTS > depth)
    read = (write - (depth / 2 + 1)) % RING_SLOTS;

  std::size_t next_read = (read + 1) % RING_SLOTS;
  if (next_read == write)
  {
    // Out of new granules. While the game runs, replay recent ones (fading out if the gap goes
    // on) rather than cutting to silence; when paused, play silence so nothing stutters.
    const bool running = Core::GetState(Core::System::GetInstance()) == Core::State::Running;
    if (!m_owner->m_settings.fill_gaps || !running)
    {
      out.fill(Frame{});
      m_fading.store(false, std::memory_order_relaxed);
      m_repeating.store(false, std::memory_order_relaxed);
      return false;
    }

    next_read = (write - (std::max<std::size_t>(2, depth / 2) - 1)) % RING_SLOTS;
    m_fading.store(m_repeating.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_repeating.store(true, std::memory_order_relaxed);
  }

  out = m_ring[read];
  m_ring_read.store(next_read, std::memory_order_release);
  return m_fading.load(std::memory_order_relaxed);
}

void Mixer::Source::AddTo(s16* frames, std::size_t count)
{
  const Settings& settings = m_owner->m_settings;

  // Double precision keeps the 24 fraction bits of the step exact.
  const double out_rate = m_owner->m_output_sample_rate;
  double in_rate = static_cast<double>(m_dividend) / m_divisor;
  const double speed = settings.emulation_speed;
  if (!settings.preserve_pitch && speed > 0 && speed != 1.0)
    in_rate *= speed;

  const u32 step =
      static_cast<u32>(std::lround(static_cast<double>(FRACTION_ONE) * in_rate / out_rate));

  // Per-frame smoothing factors for time constants of 8 ms (fade in) and 64 ms (fade out), so
  // the fade speed does not depend on the output rate.
  const float fade_in_rate = static_cast<float>(-std::expm1(-1.0 / (out_rate * 0.008)));
  const float fade_out_rate = static_cast<float>(-std::expm1(-1.0 / (out_rate * 0.064)));

  const float left_gain = static_cast<float>(m_volume_left.load()) / 256.0f;
  const float right_gain = static_cast<float>(m_volume_right.load()) / 256.0f;

  // How many granules to keep queued for the configured latency, between 4 and the ring size.
  const auto latency_ms = static_cast<std::size_t>(settings.buffer_ms);
  const auto latency_frames =
      static_cast<std::size_t>(std::llround(static_cast<double>(latency_ms) * in_rate / 1000.0));
  m_target_depth.store(std::clamp<std::size_t>(latency_frames / (GRANULE_FRAMES / 2), 4,
                                               RING_SLOTS),
                       std::memory_order_relaxed);

  bool fading = m_fading.load(std::memory_order_relaxed);

  for (std::size_t n = 0; n < count; ++n, frames += 2)
  {
    // The two granules are read half a granule apart; whichever wraps past its end is replaced
    // by the next queued granule.
    m_phase += step;
    const u32 lead_phase = m_phase;
    const u32 trail_phase = m_phase + HALF_GRANULE_PHASE;
    if (lead_phase < step)
      fading = TakeGranule(m_lead);
    else if (trail_phase < step)
      fading = TakeGranule(m_trail);

    // Six consecutive frames of the reconstructed signal around the read position, from two
    // frames before it to three after.
    const std::size_t lead_frame = lead_phase >> FRACTION_BITS;
    const std::size_t trail_frame = trail_phase >> FRACTION_BITS;
    std::array<Frame, 6> taps;
    for (std::size_t k = 0; k < taps.size(); ++k)
    {
      const Frame& a = m_lead[(lead_frame + k - 2) % GRANULE_FRAMES];
      const Frame& b = m_trail[(trail_frame + k - 2) % GRANULE_FRAMES];
      taps[k] = {a.left + b.left, a.right + b.right};
    }

    // 6-point, 3rd-order Hermite weights (Niemitalo, "Polynomial Interpolators for High-Quality
    // Resampling of Oversampled Audio", 2001).
    const float t = static_cast<float>(m_phase & (FRACTION_ONE - 1)) /
                    static_cast<float>(FRACTION_ONE);
    const float tt = t * t;
    const float ttt = tt * t;
    const std::array<float, 6> weights = {
        (t - 2.0f * tt + ttt) / 12.0f,          (-8.0f * t + 15.0f * tt - 7.0f * ttt) / 12.0f,
        (3.0f - 7.0f * tt + 4.0f * ttt) / 3.0f, (2.0f * t + 5.0f * tt - 4.0f * ttt) / 3.0f,
        (-t - 6.0f * tt + 7.0f * ttt) / 12.0f,  (tt - ttt) / 12.0f,
    };

    float left = taps[0].left * weights[0];
    float right = taps[0].right * weights[0];
    for (std::size_t k = 1; k < taps.size(); ++k)
    {
      left = left + taps[k].left * weights[k];
      right = right + taps[k].right * weights[k];
    }

    // Fade towards silence while replaying old granules, back to full level otherwise.
    if (fading)
      m_fade_gain += fade_out_rate * (0.0f - m_fade_gain);
    else
      m_fade_gain += fade_in_rate * (1.0f - m_fade_gain);

    left = left * left_gain * m_fade_gain;
    right = right * right_gain * m_fade_gain;

    StoreRounded(frames[0], left, m_carry.left);
    StoreRounded(frames[1], right, m_carry.right);
  }
}
