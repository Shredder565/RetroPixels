// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "AudioCommon/SoundStream.h"

#ifdef _WIN32
#include <array>
#include <thread>
#include <vector>

#include <al.h>
#include <alc.h>
#include <alext.h>

#include "Common/Flag.h"
#endif

// Sound output through OpenAL (openal32.dll, loaded at run time). A thread keeps a small queue of
// buffers filled from the mixer, in stereo or, when the host supports it, decoded 5.1.
class OpenALStream final : public SoundStream
{
#ifdef _WIN32
public:
  OpenALStream() = default;
  ~OpenALStream() override;

  bool Init() override;
  void SetVolume(int volume) override;
  bool SetRunning(bool running) override;

  // Loads openal32.dll and its entry points on first use.
  static bool IsValid();

  // OpenAL needs at least two queued buffers; three give some slack.
  static constexpr u32 QUEUE_LENGTH = 3;
  static constexpr u32 MAX_FRAMES_PER_BUFFER = 4096;

private:
  void FeedLoop();

  std::thread m_feeder;
  Common::Flag m_keep_feeding;

  std::vector<s16> m_stereo_frames;
  std::array<ALuint, QUEUE_LENGTH> m_queue{};
  ALuint m_source = 0;
  ALfloat m_gain = 1.0f;
#endif  // _WIN32
};
