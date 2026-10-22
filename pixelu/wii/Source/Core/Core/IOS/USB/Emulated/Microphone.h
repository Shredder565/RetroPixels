// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "Common/CommonTypes.h"

#ifdef HAVE_CUBEB
#include "AudioCommon/CubebUtils.h"

struct cubeb;
struct cubeb_stream;
#endif

namespace IOS::HLE::USB
{
// What an emulated USB microphone tells the capture code: whether the game wants samples.
class MicrophoneState
{
public:
  virtual ~MicrophoneState() = default;

  virtual bool IsSampleOn() const = 0;
  virtual bool IsMuted() const = 0;
  virtual u32 GetDefaultSamplingRate() const = 0;
};

// Captures mono 16-bit audio from a host input device (through cubeb) into a ring buffer that an
// emulated device drains in blocks of BUFF_SIZE_SAMPLES samples. It also keeps a rough loudness
// level of the captured audio for devices that report one.
class Microphone
{
public:
  using FloatType = float;
  using SampleType = s16;
  using UnsignedSampleType = std::make_unsigned_t<SampleType>;

  Microphone(const MicrophoneState& sampler, std::string worker_name);
  virtual ~Microphone();

  void Initialize();
  bool HasData(u32 sample_count) const;
  u16 ReadIntoBuffer(u8* ptr, u32 size);
  u16 GetLoudnessLevel() const;
  FloatType ComputeGain(FloatType relative_db) const;
  void SetSamplingRate(u32 sampling_rate);

protected:
  static constexpr u32 BUFF_SIZE_SAMPLES = 32;

private:
  // Per-device settings, provided by the emulated device.
#ifdef HAVE_CUBEB
  virtual std::string GetInputDeviceId() const = 0;
  virtual std::string GetCubebStreamName() const = 0;
  virtual s16 GetVolumeModifier() const = 0;
  virtual bool AreSamplesByteSwapped() const = 0;
#endif
  virtual bool IsMicrophoneMuted() const = 0;
  virtual u32 GetStreamSize() const = 0;

#ifdef HAVE_CUBEB
  static long OnCubebData(cubeb_stream* stream, void* user_data, const void* input_buffer,
                          void* output_buffer, long nframes);
#endif
  long StoreCapturedSamples(const SampleType* samples, long count);
  void FeedLevelMeter(SampleType sample);

  void OpenContext();
  void CloseContext();
  void OpenStream(u32 sampling_rate);
  void CloseStream();

  // Ring buffer of captured samples (already in the device's byte order).
  std::vector<SampleType> m_ring;
  u32 m_write_index = 0;
  u32 m_read_index = 0;
  u32 m_buffered = 0;
  mutable std::mutex m_ring_lock;

  // Loudness: the peak of each window of LEVEL_WINDOW_SAMPLES samples, mapped onto
  // 0..LEVEL_RANGE on a decibel scale. How real hardware measures it is unknown.
  static constexpr u16 LEVEL_WINDOW_SAMPLES = 128;
  static_assert(LEVEL_WINDOW_SAMPLES % BUFF_SIZE_SAMPLES == 0);
  u16 m_window_samples = 0;
  SampleType m_window_low = 0;
  SampleType m_window_high = 0;
  std::atomic<u16> m_loudness_level = 0;

  const MicrophoneState& m_sampler;

#ifdef HAVE_CUBEB
  std::shared_ptr<cubeb> m_cubeb_ctx = nullptr;
  cubeb_stream* m_cubeb_stream = nullptr;
  CubebUtils::CoInitSyncWorker m_worker;
#endif
};
}  // namespace IOS::HLE::USB
