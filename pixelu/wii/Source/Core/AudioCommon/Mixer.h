// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <string>
#include <utility>

#include "AudioCommon/SurroundDecoder.h"
#include "AudioCommon/WaveFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Inline.h"

class PointerWrap;

// Combines every emulated audio source (DSP DMA, disc streaming, the Wii Remote speakers, the
// Skylander portal and GBAs) into the host's output stream. Producers push samples from the
// emulation thread; the sound backend pulls mixed frames from its own thread.
class Mixer final
{
public:
  explicit Mixer(u32 BackendSampleRate);
  ~Mixer();

  void DoState(PointerWrap& p);

  // Audio thread side.
  std::size_t Mix(s16* samples, std::size_t numSamples);
  std::size_t MixSurround(float* samples, std::size_t num_samples);

  // Emulation thread side.
  void PushSamples(const s16* samples, std::size_t num_samples);
  void PushStreamingSamples(const s16* samples, std::size_t num_samples);
  void PushWiimoteSpeakerSamples(std::size_t wiimote_index, const s16* samples,
                                 std::size_t num_samples, u32 sample_rate_divisor);
  void PushSkylanderPortalSamples(const u8* samples, std::size_t num_samples);
  void PushGBASamples(std::size_t device_number, const s16* samples, std::size_t num_samples);

  u32 GetSampleRate() const { return m_output_sample_rate; }
  void SetSampleRate(u32 output_sample_rate) { m_output_sample_rate = output_sample_rate; }

  // A rate of zero means "no output" (the null backend sets it).
  bool IsOutputSampleRateValid() const { return m_output_sample_rate != 0; }

  void SetDMAInputSampleRateDivisor(u32 rate_divisor);
  void SetStreamInputSampleRateDivisor(u32 rate_divisor);
  void SetGBAInputSampleRate(std::size_t device_number, u32 sample_rate);

  void SetStreamingVolume(u32 lvolume, u32 rvolume);
  void SetWiimoteSpeakerVolume(std::size_t wiimote_index, u32 lvolume, u32 rvolume);
  std::size_t MixWiimoteSpeaker(std::size_t wiimote_index, s16* samples, std::size_t num_samples);
  void SetGBAVolume(std::size_t device_number, u32 lvolume, u32 rvolume);

  void StartLogDTKAudio(const std::string& filename);
  void StopLogDTKAudio();

  void StartLogDSPAudio(const std::string& filename);
  void StopLogDSPAudio();

  // Input rates are expressed as this dividend over a per-source divisor. 108 MHz (twice the
  // 54 MHz bus clock) divides evenly by 32000 and 48000.
  static constexpr u64 FIXED_SAMPLE_RATE_DIVIDEND = 108000000;

private:
  // One input source. Incoming samples are cut into 256-frame granules that overlap by half and
  // are windowed when queued; the reader keeps two granules in flight half a granule apart, so
  // summing them reconstructs the signal, and resamples the sum to the output rate.
  class Source final
  {
  public:
    static constexpr std::size_t GRANULE_FRAMES = 256;
    static constexpr std::size_t RING_SLOTS = 256;

    Source(Mixer* owner, u32 divisor, u32 dividend = FIXED_SAMPLE_RATE_DIVIDEND)
        : m_owner(owner), m_dividend(dividend), m_divisor(divisor)
    {
    }

    void DoState(PointerWrap& p);

    DOLPHIN_FORCE_INLINE void Push(s16 left, s16 right)
    {
      m_pending[m_pending_pos] = {static_cast<float>(left), static_cast<float>(right)};
      m_pending_pos = (m_pending_pos + 1) % GRANULE_FRAMES;
      // A new granule starts every half granule.
      if (m_pending_pos % (GRANULE_FRAMES / 2) == 0)
        PublishGranule();
    }

    // Adds this source's resampled output to interleaved stereo frames.
    void AddTo(s16* frames, std::size_t count);

    u32 GetDivisor() const { return m_divisor; }
    void SetDivisor(u32 divisor) { m_divisor = divisor; }

    void SetVolume(u32 left, u32 right);
    std::pair<s32, s32> GetVolume() const;

  private:
    struct Frame
    {
      float left = 0.0f;
      float right = 0.0f;
    };
    using Granule = std::array<Frame, GRANULE_FRAMES>;

    void PublishGranule();
    bool TakeGranule(Granule& out);

    Mixer* m_owner;

    // Only GBAs use a dividend other than FIXED_SAMPLE_RATE_DIVIDEND.
    u32 m_dividend;
    u32 m_divisor;

    // Producer side: the frames of the granule being collected.
    Granule m_pending{};
    std::size_t m_pending_pos = 0;

    // Consumer side: the 8.24 fixed point read position and the two granules being played.
    u32 m_phase = 0;
    Granule m_lead{};
    Granule m_trail{};

    std::array<Granule, RING_SLOTS> m_ring{};
    std::atomic<std::size_t> m_ring_write{0};
    std::atomic<std::size_t> m_ring_read{0};
    std::atomic<std::size_t> m_target_depth{20};
    std::atomic<bool> m_fading{false};
    std::atomic<bool> m_repeating{false};
    float m_fade_gain = 1.0f;

    // 256 is unity gain.
    std::atomic<s32> m_volume_left{256};
    std::atomic<s32> m_volume_right{256};

    // Rounding error carried over to the next output frame.
    Frame m_carry{};
  };

  struct Settings
  {
    float emulation_speed = 1.0f;
    bool preserve_pitch = false;
    bool fill_gaps = false;
    int buffer_ms = 0;
    bool wiimote_routing = false;
    std::array<bool, 4> wiimote_output{};
  };

  void RefreshConfig();

  // GBAs mostly play at 65536 Hz, which FIXED_SAMPLE_RATE_DIVIDEND does not divide.
  static constexpr u32 GBA_DIVIDEND = 0x1000000;

  Source m_dma{this, FIXED_SAMPLE_RATE_DIVIDEND / 32000};
  Source m_streaming{this, FIXED_SAMPLE_RATE_DIVIDEND / 48000};
  std::array<Source, 4> m_wiimote_speakers{
      Source{this, FIXED_SAMPLE_RATE_DIVIDEND / 3000},
      Source{this, FIXED_SAMPLE_RATE_DIVIDEND / 3000},
      Source{this, FIXED_SAMPLE_RATE_DIVIDEND / 3000},
      Source{this, FIXED_SAMPLE_RATE_DIVIDEND / 3000},
  };
  Source m_skylander_portal{this, FIXED_SAMPLE_RATE_DIVIDEND / 8000};
  std::array<Source, 4> m_gbas{
      Source{this, GBA_DIVIDEND / 65536, GBA_DIVIDEND},
      Source{this, GBA_DIVIDEND / 65536, GBA_DIVIDEND},
      Source{this, GBA_DIVIDEND / 65536, GBA_DIVIDEND},
      Source{this, GBA_DIVIDEND / 65536, GBA_DIVIDEND},
  };

  u32 m_output_sample_rate;
  AudioCommon::SurroundDecoder m_surround;

  WaveFileWriter m_dtk_dump;
  WaveFileWriter m_dsp_dump;
  bool m_dtk_dumping = false;
  bool m_dsp_dumping = false;

  Settings m_settings;
  Config::ConfigChangedCallbackID m_config_callback;
};
