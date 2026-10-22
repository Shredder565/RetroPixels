// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

// Writes 16-bit stereo PCM to a .wav file (used for the DSP and DTK audio dumps). Samples come
// in as the emulated hardware produces them: big-endian, right channel first. When the sample
// rate changes mid-dump, the current file is finished and a numbered new one is started.
// The destructor finishes the file if Stop() was not called.
class WaveFileWriter
{
public:
  WaveFileWriter();
  ~WaveFileWriter();

  WaveFileWriter(const WaveFileWriter&) = delete;
  WaveFileWriter& operator=(const WaveFileWriter&) = delete;
  WaveFileWriter(WaveFileWriter&&) = delete;
  WaveFileWriter& operator=(WaveFileWriter&&) = delete;

  bool Start(const std::string& filename, u32 sample_rate_divisor);
  void Stop();

  void SetSkipSilence(bool skip) { m_skip_silence = skip; }
  void AddStereoSamplesBE(const short* sample_data, u32 count, u32 sample_rate_divisor,
                          int l_volume, int r_volume);
  u32 GetAudioSize() const { return m_audio_size; }

private:
  // Most frames accepted by one AddStereoSamplesBE call (two samples each).
  static constexpr u32 MAX_FRAMES_PER_CALL = 16 * 1024;

  File::IOFile m_file;

  // Name of the first file, without directory or extension; numbered follow-up files use it.
  std::string m_stem;
  u32 m_sequence = 0;

  u32 m_audio_size = 0;
  u32 m_divisor = 0;
  bool m_skip_silence = false;

  std::array<s16, MAX_FRAMES_PER_CALL * 2> m_scratch{};
};
