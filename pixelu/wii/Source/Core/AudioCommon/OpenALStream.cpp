// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef _WIN32

#include "AudioCommon/OpenALStream.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"

// 5.1 formats from the AL_EXT_MCFORMATS extension.
#ifndef AL_FORMAT_51CHN16
#define AL_FORMAT_51CHN16 0x120B
#endif
#ifndef AL_FORMAT_51CHN32
#define AL_FORMAT_51CHN32 0x120C
#endif

namespace
{
constexpr u32 STEREO = 2;
constexpr u32 SURROUND = 6;

// Entry points resolved from openal32.dll.
struct OpenALEntryPoints
{
  decltype(&alBufferData) BufferData;
  decltype(&alcCloseDevice) CloseDevice;
  decltype(&alcCreateContext) CreateContext;
  decltype(&alcDestroyContext) DestroyContext;
  decltype(&alcGetContextsDevice) GetContextsDevice;
  decltype(&alcGetCurrentContext) GetCurrentContext;
  decltype(&alcGetString) GetDeviceString;
  decltype(&alcIsExtensionPresent) IsDeviceExtensionPresent;
  decltype(&alcMakeContextCurrent) MakeContextCurrent;
  decltype(&alcOpenDevice) OpenDevice;
  decltype(&alDeleteBuffers) DeleteBuffers;
  decltype(&alDeleteSources) DeleteSources;
  decltype(&alGenBuffers) GenBuffers;
  decltype(&alGenSources) GenSources;
  decltype(&alGetError) GetError;
  decltype(&alGetSourcei) GetSourcei;
  decltype(&alGetString) GetString;
  decltype(&alIsExtensionPresent) IsExtensionPresent;
  decltype(&alSourcef) Sourcef;
  decltype(&alSourcei) Sourcei;
  decltype(&alSourcePlay) SourcePlay;
  decltype(&alSourceQueueBuffers) SourceQueueBuffers;
  decltype(&alSourceStop) SourceStop;
  decltype(&alSourceUnqueueBuffers) SourceUnqueueBuffers;
};

HMODULE s_library = nullptr;
OpenALEntryPoints al{};

template <typename Fn>
bool Resolve(Fn& slot, const char* name)
{
  slot = reinterpret_cast<Fn>(::GetProcAddress(s_library, name));
  return slot != nullptr;
}

bool ResolveEntryPoints()
{
  return Resolve(al.BufferData, "alBufferData") && Resolve(al.CloseDevice, "alcCloseDevice") &&
         Resolve(al.CreateContext, "alcCreateContext") &&
         Resolve(al.DestroyContext, "alcDestroyContext") &&
         Resolve(al.GetContextsDevice, "alcGetContextsDevice") &&
         Resolve(al.GetCurrentContext, "alcGetCurrentContext") &&
         Resolve(al.GetDeviceString, "alcGetString") &&
         Resolve(al.IsDeviceExtensionPresent, "alcIsExtensionPresent") &&
         Resolve(al.MakeContextCurrent, "alcMakeContextCurrent") &&
         Resolve(al.OpenDevice, "alcOpenDevice") && Resolve(al.DeleteBuffers, "alDeleteBuffers") &&
         Resolve(al.DeleteSources, "alDeleteSources") && Resolve(al.GenBuffers, "alGenBuffers") &&
         Resolve(al.GenSources, "alGenSources") && Resolve(al.GetError, "alGetError") &&
         Resolve(al.GetSourcei, "alGetSourcei") && Resolve(al.GetString, "alGetString") &&
         Resolve(al.IsExtensionPresent, "alIsExtensionPresent") &&
         Resolve(al.Sourcef, "alSourcef") && Resolve(al.Sourcei, "alSourcei") &&
         Resolve(al.SourcePlay, "alSourcePlay") &&
         Resolve(al.SourceQueueBuffers, "alSourceQueueBuffers") &&
         Resolve(al.SourceStop, "alSourceStop") &&
         Resolve(al.SourceUnqueueBuffers, "alSourceUnqueueBuffers");
}

bool LoadOpenAL()
{
  if (s_library != nullptr)
    return true;

  s_library = ::LoadLibrary(TEXT("openal32.dll"));
  if (s_library == nullptr)
    return false;

  if (ResolveEntryPoints())
    return true;

  FreeLibrary(s_library);
  s_library = nullptr;
  return false;
}

const char* ErrorName(ALenum error)
{
  switch (error)
  {
  case AL_INVALID_NAME:
    return "AL_INVALID_NAME";
  case AL_INVALID_ENUM:
    return "AL_INVALID_ENUM";
  case AL_INVALID_VALUE:
    return "AL_INVALID_VALUE";
  case AL_INVALID_OPERATION:
    return "AL_INVALID_OPERATION";
  case AL_OUT_OF_MEMORY:
    return "AL_OUT_OF_MEMORY";
  default:
    return "UNKNOWN_ERROR";
  }
}

// Reads (and so clears) the AL error state, logging it with what was being done.
ALenum LogALError(const char* action)
{
  const ALenum error = al.GetError();
  if (error != AL_NO_ERROR)
    ERROR_LOG_FMT(AUDIO, "Error {}: {:08x} {}", action, error, ErrorName(error));
  return error;
}

// Creative X-Fi cards with hardware OpenAL also take 32-bit integer samples, though no extension
// says so.
bool IsXFiRenderer()
{
  return std::strstr(al.GetString(AL_RENDERER), "X-Fi") != nullptr;
}

// Scales [-1, 1] floats to a full-range integer type, saturating.
template <typename T>
void ToFixedPoint(float* samples, T* out, u32 count)
{
  constexpr float top = static_cast<float>(std::numeric_limits<T>::max());
  constexpr float bottom = static_cast<float>(std::numeric_limits<T>::min());
  for (u32 i = 0; i < count; ++i)
  {
    samples[i] = samples[i] * top;
    if (samples[i] > top)
      out[i] = std::numeric_limits<T>::max();
    else if (samples[i] < bottom)
      out[i] = std::numeric_limits<T>::min();
    else
      out[i] = static_cast<T>(samples[i]);
  }
}
}  // namespace

bool OpenALStream::IsValid()
{
  return LoadOpenAL();
}

// OpenAL 1.1 is thread safe, so the feeding thread may use the context made current here.
bool OpenALStream::Init()
{
  if (!al.IsDeviceExtensionPresent(nullptr, "ALC_ENUMERATION_EXT"))
  {
    PanicAlertFmtT("OpenAL: can't find sound devices");
    return false;
  }

  const char* device_name = al.GetDeviceString(nullptr, ALC_DEFAULT_DEVICE_SPECIFIER);
  INFO_LOG_FMT(AUDIO, "Found OpenAL device {}", device_name);

  ALCdevice* device = al.OpenDevice(device_name);
  if (device == nullptr)
  {
    PanicAlertFmtT("OpenAL: can't open device {0}", device_name);
    return false;
  }

  ALCcontext* context = al.CreateContext(device, nullptr);
  if (context == nullptr)
  {
    al.CloseDevice(device);
    PanicAlertFmtT("OpenAL: can't create context for device {0}", device_name);
    return false;
  }

  al.MakeContextCurrent(context);
  m_keep_feeding.Set();
  m_feeder = std::thread(&OpenALStream::FeedLoop, this);
  return true;
}

OpenALStream::~OpenALStream()
{
  // Nothing was set up if Init failed.
  if (!m_feeder.joinable())
    return;

  m_keep_feeding.Clear();
  m_feeder.join();

  al.SourceStop(m_source);
  al.Sourcei(m_source, AL_BUFFER, 0);
  al.DeleteSources(1, &m_source);
  m_source = 0;
  al.DeleteBuffers(QUEUE_LENGTH, m_queue.data());

  ALCcontext* context = al.GetCurrentContext();
  ALCdevice* device = al.GetContextsDevice(context);
  al.MakeContextCurrent(nullptr);
  al.DestroyContext(context);
  al.CloseDevice(device);
}

void OpenALStream::SetVolume(int volume)
{
  m_gain = static_cast<float>(volume) / 100.0f;
  if (m_source != 0)
    al.Sourcef(m_source, AL_GAIN, m_gain);
}

bool OpenALStream::SetRunning(bool running)
{
  if (running)
    al.SourcePlay(m_source);
  else
    al.SourceStop(m_source);
  return true;
}

void OpenALStream::FeedLoop()
{
  Common::SetCurrentThreadName("Audio thread - openal");

  const bool has_float32 = al.IsExtensionPresent("AL_EXT_float32") != 0;
  const bool has_surround = al.IsExtensionPresent("AL_EXT_MCFORMATS") || IsXFiRenderer();
  bool surround = Config::ShouldUseDPL2Decoder() && has_surround;
  const bool has_int32 = IsXFiRenderer();

  const u32 rate = m_mixer->GetSampleRate();

  // Split the configured latency (at least 1 ms) over the queued buffers.
  const int latency_ms = Config::Get(Config::MAIN_AUDIO_LATENCY);
  const u32 per_buffer =
      std::min(rate / 1000 * static_cast<u32>(latency_ms > 0 ? latency_ms : 1) / QUEUE_LENGTH,
               MAX_FRAMES_PER_BUFFER);

  INFO_LOG_FMT(AUDIO, "Using {} buffers, each with {} audio frames for a total of {}.",
               QUEUE_LENGTH, per_buffer, per_buffer * QUEUE_LENGTH);

  m_stereo_frames.resize(std::size_t{per_buffer} * STEREO);
  m_source = 0;

  // Drop any stale error so the checks below only see our own calls.
  al.GetError();

  al.GenBuffers(QUEUE_LENGTH, m_queue.data());
  LogALError("generating buffers");
  al.GenSources(1, &m_source);
  LogALError("generating sources");
  al.Sourcef(m_source, AL_GAIN, m_gain);

  u32 next = 0;
  u32 queued = 0;
  ALint state = 0;

  while (m_keep_feeding.IsSet())
  {
    int finished = 0;
    al.GetSourcei(m_source, AL_BUFFERS_PROCESSED, &finished);
    if (queued == QUEUE_LENGTH && finished == 0)
    {
      // Every buffer is still queued; wait for one to play out.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    if (finished != 0)
    {
      std::array<ALuint, QUEUE_LENGTH> done_ids;
      al.SourceUnqueueBuffers(m_source, finished, done_ids.data());
      LogALError("unqueuing buffers");
      queued -= static_cast<u32>(finished);
    }

    if (surround)
    {
      std::array<float, MAX_FRAMES_PER_BUFFER * SURROUND> decoded;
      const auto frames = static_cast<u32>(m_mixer->MixSurround(decoded.data(), per_buffer));
      if (frames < per_buffer)
        continue;

      const u32 count = frames * SURROUND;
      if (has_float32)
      {
        al.BufferData(m_queue[next], AL_FORMAT_51CHN32, decoded.data(),
                      static_cast<ALsizei>(count * sizeof(float)), static_cast<ALsizei>(rate));
      }
      else if (has_int32)
      {
        std::array<s32, MAX_FRAMES_PER_BUFFER * SURROUND> converted;
        ToFixedPoint(decoded.data(), converted.data(), count);
        al.BufferData(m_queue[next], AL_FORMAT_51CHN32, converted.data(),
                      static_cast<ALsizei>(count * sizeof(s32)), static_cast<ALsizei>(rate));
      }
      else
      {
        std::array<s16, MAX_FRAMES_PER_BUFFER * SURROUND> converted;
        ToFixedPoint(decoded.data(), converted.data(), count);
        al.BufferData(m_queue[next], AL_FORMAT_51CHN16, converted.data(),
                      static_cast<ALsizei>(count * sizeof(s16)), static_cast<ALsizei>(rate));
      }

      if (LogALError("buffering data") == AL_INVALID_ENUM)
      {
        // The device has no 5.1 support after all; continue in stereo.
        WARN_LOG_FMT(
            AUDIO, "Unable to set 5.1 surround mode.  Updating OpenAL Soft might fix this issue.");
        surround = false;
      }
    }
    else
    {
      const auto frames = static_cast<u32>(m_mixer->Mix(m_stereo_frames.data(), per_buffer));
      if (frames == 0)
        continue;

      al.BufferData(m_queue[next], AL_FORMAT_STEREO16, m_stereo_frames.data(),
                    static_cast<ALsizei>(frames * STEREO * sizeof(s16)),
                    static_cast<ALsizei>(rate));
    }

    al.SourceQueueBuffers(m_source, 1, &m_queue[next]);
    LogALError("queuing buffers");
    ++queued;
    next = (next + 1) % QUEUE_LENGTH;

    // An underrun stops the source; restart it.
    al.GetSourcei(m_source, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
    {
      al.SourcePlay(m_source);
      LogALError("occurred resuming playback");
    }
  }
}

#endif  // _WIN32
