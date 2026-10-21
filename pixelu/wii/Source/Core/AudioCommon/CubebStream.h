// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>

#include "AudioCommon/SoundStream.h"
#ifdef _WIN32
#include "Common/WorkQueueThread.h"
#endif

#ifdef HAVE_CUBEB
#include <cubeb/cubeb.h>
#endif

// Sound output through libcubeb: one main stream (stereo s16, or 5.1 float when the Dolby Pro
// Logic II decoder is on) plus, for Wii games with speaker routing enabled, one stereo stream per
// routed Wii Remote speaker on the device chosen for it.
class CubebStream final : public SoundStream
{
#ifdef HAVE_CUBEB
public:
  CubebStream();
  CubebStream(const CubebStream& other) = delete;
  CubebStream(CubebStream&& other) = delete;
  CubebStream& operator=(const CubebStream& other) = delete;
  CubebStream& operator=(CubebStream&& other) = delete;
  ~CubebStream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int) override;
  static bool IsValid() { return true; }

private:
  static constexpr std::size_t SPEAKER_COUNT = 4;

  // What a speaker stream's callback needs to find its mixer input.
  struct SpeakerRoute
  {
    CubebStream* owner;
    std::size_t index;
  };

  // On Windows, cubeb's WASAPI backend must be driven from a thread that initialised COM in the
  // multithreaded apartment, so every cubeb call is made on a worker thread that owns COM.
  // Returns false (without running `task`) if that thread could not initialise COM.
  bool OnCubebThread(const std::function<void()>& task);

  void OpenSpeakerStreams();

  static long OnMainData(cubeb_stream* stream, void* user_data, const void* input_buffer,
                         void* output_buffer, long num_frames);
  static long OnSpeakerData(cubeb_stream* stream, void* user_data, const void* input_buffer,
                            void* output_buffer, long num_frames);
  static void OnStateChange(cubeb_stream* stream, void* user_data, cubeb_state state);

  std::shared_ptr<cubeb> m_context;
  cubeb_stream* m_main = nullptr;
  bool m_surround = false;

  std::array<SpeakerRoute, SPEAKER_COUNT> m_routes{};
  std::array<cubeb_stream*, SPEAKER_COUNT> m_speakers{};

#ifdef _WIN32
  Common::AsyncWorkThread m_com_thread;
  bool m_com_ready = false;
  bool m_com_needs_uninit = false;
#endif
#endif
};
