// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Emulated/Microphone.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef HAVE_CUBEB
#include <cubeb/cubeb.h>

#include "AudioCommon/CubebUtils.h"
#endif

#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/System.h"

#ifdef ANDROID
#include "jni/AndroidCommon/IDCache.h"
#endif

namespace IOS::HLE::USB
{
namespace
{
using Float = Microphone::FloatType;

// Full scale of a 16-bit sample, and the decibel range the loudness level covers: from one step
// above silence (DB_FLOOR) up to full scale (DB_CEILING, 0 dB).
constexpr Float FULL_SCALE = Microphone::UnsignedSampleType{
    std::numeric_limits<Microphone::UnsignedSampleType>::max() / 2};
constexpr u32 LEVEL_RANGE = 0x4000;  // Monster Hunter 3 draws levels up to about 0x3a00.

Float ToDecibels(Float ratio)
{
  return 20 * std::log10(ratio);
}

const Float DB_FLOOR = ToDecibels(Float(1) / FULL_SCALE);
const Float DB_CEILING = ToDecibels(Float(1));

// Maps the peak sample of a window to a loudness level.
u16 LevelFromPeak(Microphone::SampleType peak)
{
  static const Float db_per_step = (DB_CEILING - DB_FLOOR) / LEVEL_RANGE;
  const Float steps = (ToDecibels(peak / FULL_SCALE) - DB_FLOOR) / db_per_step;
  // A silent window (or the -32768 peak below) gives -inf or NaN: report it as level 0.
  if (!(steps >= 0))
    return 0;
  return static_cast<u16>(steps);
}
}  // namespace

Microphone::Microphone(const MicrophoneState& sampler, std::string worker_name)
    : m_sampler(sampler)
#ifdef HAVE_CUBEB
      ,
      m_worker(std::move(worker_name))
#endif
{
#ifndef HAVE_CUBEB
  static_cast<void>(worker_name);
#endif
}

Microphone::~Microphone()
{
  CloseContext();
}

void Microphone::Initialize()
{
  OpenContext();
}

bool Microphone::HasData(u32 sample_count) const
{
  std::lock_guard guard(m_ring_lock);
  return m_buffered >= sample_count;
}

u16 Microphone::ReadIntoBuffer(u8* ptr, u32 size)
{
  constexpr u32 BLOCK_BYTES = BUFF_SIZE_SAMPLES * sizeof(SampleType);

  std::lock_guard guard(m_ring_lock);
  const u32 stream_size = GetStreamSize();
  if (stream_size == 0 || m_ring.empty())
    return 0;

  u32 copied = 0;
  while (size - copied >= BLOCK_BYTES && m_buffered >= BUFF_SIZE_SAMPLES)
  {
    u8* const out = ptr + copied;
    if (m_read_index + BUFF_SIZE_SAMPLES <= m_ring.size())
    {
      std::memcpy(out, &m_ring[m_read_index], BLOCK_BYTES);
    }
    else
    {
      // The block runs past the end of the ring: take the rest from its start.
      for (u32 i = 0; i < BUFF_SIZE_SAMPLES; ++i)
      {
        const SampleType sample = m_ring[(m_read_index + i) % m_ring.size()];
        std::memcpy(out + i * sizeof(SampleType), &sample, sizeof(SampleType));
      }
    }
    copied += BLOCK_BYTES;
    m_buffered -= BUFF_SIZE_SAMPLES;
    m_read_index = (m_read_index + BUFF_SIZE_SAMPLES) % stream_size;
  }
  return static_cast<u16>(copied);
}

u16 Microphone::GetLoudnessLevel() const
{
  if (m_sampler.IsMuted() || IsMicrophoneMuted())
    return 0;
  return m_loudness_level;
}

Microphone::FloatType Microphone::ComputeGain(FloatType relative_db) const
{
  return std::pow(FloatType(10), relative_db / 20);
}

void Microphone::SetSamplingRate(u32 sampling_rate)
{
  CloseStream();
  OpenStream(sampling_rate);
}

void Microphone::FeedLevelMeter(SampleType sample)
{
  m_window_low = std::min(m_window_low, sample);
  m_window_high = std::max(m_window_high, sample);
  if (++m_window_samples < LEVEL_WINDOW_SAMPLES)
    return;

  // The larger magnitude, kept as a 16-bit sample (so a -32768 peak stays negative).
  const int widest = std::max(std::abs(int{m_window_low}), std::abs(int{m_window_high}));
  m_loudness_level = LevelFromPeak(static_cast<SampleType>(widest));

  m_window_samples = 0;
  m_window_low = 0;
  m_window_high = 0;
}

#ifndef HAVE_CUBEB
void Microphone::OpenContext()
{
}

void Microphone::CloseContext()
{
}

void Microphone::OpenStream(u32)
{
}

void Microphone::CloseStream()
{
}
#else
void Microphone::OpenContext()
{
  const bool ran = m_worker.Execute([this] { m_cubeb_ctx = CubebUtils::GetContext(); });
  if (!ran)
  {
    ERROR_LOG_FMT(IOS_USB, "Failed to init microphone stream");
    return;
  }
  OpenStream(m_sampler.GetDefaultSamplingRate());
}

void Microphone::CloseContext()
{
  CloseStream();
  if (m_cubeb_ctx)
    m_worker.Execute([this] { m_cubeb_ctx.reset(); });
}

void Microphone::OpenStream(u32 sampling_rate)
{
  if (!m_cubeb_ctx)
    return;

  m_worker.Execute([this, sampling_rate] {
#ifdef ANDROID
    // Recording needs the RECORD_AUDIO permission; ask for it if the app doesn't have it yet.
    JNIEnv* env = IDCache::GetEnvForThread();
    const jclass handler = IDCache::GetPermissionHandlerClass();
    if (env->CallStaticBooleanMethod(handler,
                                     IDCache::GetPermissionHandlerHasRecordAudioPermission(),
                                     nullptr) == JNI_FALSE)
    {
      env->CallStaticVoidMethod(handler,
                                IDCache::GetPermissionHandlerRequestRecordAudioPermission(),
                                nullptr);
    }
#endif

    cubeb_stream_params format{};
    format.format = CUBEB_SAMPLE_S16LE;
    format.rate = sampling_rate;
    format.channels = 1;
    format.layout = CUBEB_LAYOUT_MONO;

    std::lock_guard guard(m_ring_lock);

    u32 latency_frames = 0;
    if (cubeb_get_min_latency(m_cubeb_ctx.get(), &format, &latency_frames) != CUBEB_OK)
    {
      WARN_LOG_FMT(IOS_USB, "Error getting minimum latency");
      latency_frames = 16;
    }
    latency_frames = std::max<u32>(latency_frames, 16);

    const cubeb_devid device = CubebUtils::GetInputDeviceById(GetInputDeviceId());
    const std::string name = GetCubebStreamName();
    const auto on_state = [](cubeb_stream*, void*, cubeb_state) {};
    if (cubeb_stream_init(m_cubeb_ctx.get(), &m_cubeb_stream, name.c_str(), device, &format,
                          nullptr, nullptr, latency_frames, &Microphone::OnCubebData, on_state,
                          this) != CUBEB_OK)
    {
      ERROR_LOG_FMT(IOS_USB, "Error initializing cubeb stream");
      return;
    }
    if (cubeb_stream_start(m_cubeb_stream) != CUBEB_OK)
    {
      ERROR_LOG_FMT(IOS_USB, "Error starting cubeb stream");
      return;
    }

    m_ring.resize(GetStreamSize());
    m_write_index = 0;
    INFO_LOG_FMT(IOS_USB, "started cubeb stream");
  });
}

void Microphone::CloseStream()
{
  if (!m_cubeb_stream)
    return;

  m_worker.Execute([this] {
    if (cubeb_stream_stop(m_cubeb_stream) != CUBEB_OK)
      ERROR_LOG_FMT(IOS_USB, "Error stopping cubeb stream");
    cubeb_stream_destroy(m_cubeb_stream);
    m_cubeb_stream = nullptr;
  });
}

long Microphone::OnCubebData(cubeb_stream*, void* user_data, const void* input_buffer, void*,
                             long nframes)
{
  // Drop audio while emulation is paused or stopped, and while the user muted the device.
  if (Core::GetState(Core::System::GetInstance()) != Core::State::Running)
    return nframes;

  auto* const self = static_cast<Microphone*>(user_data);
  if (self->IsMicrophoneMuted())
    return nframes;

  return self->StoreCapturedSamples(static_cast<const SampleType*>(input_buffer), nframes);
}

long Microphone::StoreCapturedSamples(const SampleType* samples, long count)
{
  std::lock_guard guard(m_ring_lock);

  // The game turned sampling off, or muted the device.
  if (!m_sampler.IsSampleOn() || m_sampler.IsMuted())
    return count;

  const FloatType gain = ComputeGain(GetVolumeModifier());
  const u32 stream_size = GetStreamSize();
  const bool swap = AreSamplesByteSwapped();
  for (long i = 0; i < count; ++i)
  {
    const auto sample = MathUtil::SaturatingCast<SampleType>(samples[i] * gain);
    FeedLevelMeter(sample);
    const auto raw = static_cast<u16>(sample);
    m_ring[m_write_index] = static_cast<SampleType>(swap ? Common::swap16(raw) : raw);
    m_write_index = (m_write_index + 1) % stream_size;
  }

  m_buffered = std::min(static_cast<u32>(m_buffered + static_cast<u32>(count)), stream_size);
  return count;
}
#endif
}  // namespace IOS::HLE::USB
