// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/AudioCommon.h"

#include <algorithm>
#include <ctime>
#include <memory>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "AudioCommon/CubebStream.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/NullSoundStream.h"
#include "AudioCommon/OpenALStream.h"
#include "AudioCommon/WASAPIStream.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/TimeUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/System.h"

namespace AudioCommon
{
namespace
{
constexpr int MIN_VOLUME = 0;
constexpr int MAX_VOLUME = 100;

// Creates the named backend if it exists in this build and can run here; null otherwise.
std::unique_ptr<SoundStream> MakeBackend(std::string_view name)
{
  if (name == BACKEND_CUBEB)
    return CubebStream::IsValid() ? std::make_unique<CubebStream>() : nullptr;
  if (name == BACKEND_OPENAL)
    return OpenALStream::IsValid() ? std::make_unique<OpenALStream>() : nullptr;
  if (name == BACKEND_NULLSOUND)
    return std::make_unique<NullSound>();
  if (name == BACKEND_WASAPI)
    return WASAPIStream::IsValid() ? std::make_unique<WASAPIStream>() : nullptr;
  return nullptr;
}

void SetVolumeSetting(Core::System& system, int volume)
{
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, volume);
  UpdateSoundStream(system);
}
}  // namespace

void InitSoundStream(Core::System& system)
{
  std::string backend = Config::Get(Config::MAIN_AUDIO_BACKEND);
  std::unique_ptr<SoundStream> stream = MakeBackend(backend);

  if (!stream)
  {
    const std::string fallback = GetDefaultSoundBackend();
    WARN_LOG_FMT(AUDIO, "Unknown backend {}, using {} instead.", backend, fallback);
    backend = fallback;
    stream = MakeBackend(backend);
  }

  if (!stream || !stream->Init())
  {
    WARN_LOG_FMT(AUDIO, "Could not initialize backend {}, using {} instead.", backend,
                 BACKEND_NULLSOUND);
    stream = std::make_unique<NullSound>();
    stream->Init();
  }

  system.SetSoundStream(std::move(stream));
}

void PostInitSoundStream(Core::System& system)
{
  // The input sample rates are known only once AudioInterface and SerialInterface (GBAs) are
  // initialised, which is why this runs separately from InitSoundStream.
  UpdateSoundStream(system);
  SetSoundStreamRunning(system, true);

  if (Config::Get(Config::MAIN_DUMP_AUDIO) && !system.IsAudioDumpStarted())
    StartAudioDump(system);
}

void ShutdownSoundStream(Core::System& system)
{
  INFO_LOG_FMT(AUDIO, "Shutting down sound stream");

  if (Config::Get(Config::MAIN_DUMP_AUDIO) && system.IsAudioDumpStarted())
    StopAudioDump(system);

  SetSoundStreamRunning(system, false);
  system.SetSoundStream(nullptr);

  INFO_LOG_FMT(AUDIO, "Done shutting down sound stream");
}

std::string GetDefaultSoundBackend()
{
  return CubebStream::IsValid() ? BACKEND_CUBEB : BACKEND_NULLSOUND;
}

DPL2Quality GetDefaultDPL2Quality()
{
  return DPL2Quality::High;
}

std::vector<std::string> GetSoundBackends()
{
  std::vector<std::string> names{BACKEND_NULLSOUND};
  if (CubebStream::IsValid())
    names.emplace_back(BACKEND_CUBEB);
  if (OpenALStream::IsValid())
    names.emplace_back(BACKEND_OPENAL);
  if (WASAPIStream::IsValid())
    names.emplace_back(BACKEND_WASAPI);
  return names;
}

bool SupportsDPL2Decoder(std::string_view backend)
{
  return backend == BACKEND_OPENAL || backend == BACKEND_CUBEB || backend == BACKEND_PULSEAUDIO;
}

bool SupportsLatencyControl(std::string_view backend)
{
  return backend == BACKEND_OPENAL || backend == BACKEND_WASAPI;
}

bool SupportsVolumeChanges(std::string_view backend)
{
  // Only these backends implement SetVolume.
  return backend == BACKEND_CUBEB || backend == BACKEND_OPENAL || backend == BACKEND_WASAPI;
}

void UpdateSoundStream(Core::System& system)
{
  SoundStream* stream = system.GetSoundStream();
  if (!stream)
    return;

  const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED);
  stream->SetVolume(muted ? 0 : Config::Get(Config::MAIN_AUDIO_VOLUME));
}

void SetSoundStreamRunning(Core::System& system, bool running)
{
  SoundStream* stream = system.GetSoundStream();
  if (!stream || system.IsSoundStreamRunning() == running)
    return;

  system.SetSoundStreamRunning(running);
  if (!stream->SetRunning(running))
  {
    if (running)
      ERROR_LOG_FMT(AUDIO, "Error starting stream.");
    else
      ERROR_LOG_FMT(AUDIO, "Error stopping stream.");
  }
}

void SendAIBuffer(Core::System& system, const short* samples, unsigned int num_samples)
{
  const SoundStream* stream = system.GetSoundStream();
  if (!stream)
    return;

  // Follow the "dump audio" setting while the game runs.
  const bool want_dump = Config::Get(Config::MAIN_DUMP_AUDIO);
  if (want_dump != system.IsAudioDumpStarted())
  {
    if (want_dump)
      StartAudioDump(system);
    else
      StopAudioDump(system);
  }

  Mixer* mixer = stream->GetMixer();
  if (mixer != nullptr && samples != nullptr)
    mixer->PushSamples(samples, num_samples);
}

void StartAudioDump(Core::System& system)
{
  const SoundStream* stream = system.GetSoundStream();

  const std::time_t now = std::time(nullptr);
  const std::string prefix =
      File::GetUserPath(D_DUMPAUDIO_IDX) + SConfig::GetInstance().GetGameID();

  const auto local_time = Common::LocalTime(now);
  if (!local_time)
    return;

  // <dump folder>/<game id>_<date>_<time>_{dtk,dsp}dump.wav
  const std::string stem = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", prefix, *local_time);
  const std::string dtk_path = stem + "_dtkdump.wav";
  const std::string dsp_path = stem + "_dspdump.wav";
  File::CreateFullPath(dtk_path);
  File::CreateFullPath(dsp_path);

  Mixer* mixer = stream->GetMixer();
  mixer->StartLogDTKAudio(dtk_path);
  mixer->StartLogDSPAudio(dsp_path);
  system.SetAudioDumpStarted(true);
}

void StopAudioDump(Core::System& system)
{
  const SoundStream* stream = system.GetSoundStream();
  if (!stream)
    return;

  Mixer* mixer = stream->GetMixer();
  mixer->StopLogDTKAudio();
  mixer->StopLogDSPAudio();
  system.SetAudioDumpStarted(false);
}

void IncreaseVolume(Core::System& system, unsigned short offset)
{
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, false);
  SetVolumeSetting(system, std::min(Config::Get(Config::MAIN_AUDIO_VOLUME) + offset, MAX_VOLUME));
}

void DecreaseVolume(Core::System& system, unsigned short offset)
{
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, false);
  SetVolumeSetting(system, std::max(Config::Get(Config::MAIN_AUDIO_VOLUME) - offset, MIN_VOLUME));
}

void ToggleMuteVolume(Core::System& system)
{
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MUTED, !Config::Get(Config::MAIN_AUDIO_MUTED));
  UpdateSoundStream(system);
}
}  // namespace AudioCommon
