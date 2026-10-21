// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/CubebStream.h"

#include <algorithm>
#include <string>

#include <cubeb/cubeb.h>
#include <fmt/format.h>

#include "AudioCommon/CubebUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/System.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace
{
// Lower bound on the stream latency in frames (about 10 ms; 5.1 output needs at least 240).
constexpr u32 MIN_LATENCY_FRAMES = 512;
}  // namespace

CubebStream::CubebStream()
{
#ifdef _WIN32
  m_com_thread.Reset("Cubeb Worker");
  m_com_thread.PushBlocking([this] {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    // S_FALSE: COM was already initialised on this thread; it still needs a matching uninit.
    m_com_ready = result == S_OK;
    m_com_needs_uninit = result == S_OK || result == S_FALSE;
  });
#endif
}

CubebStream::~CubebStream()
{
  const auto teardown = [this] {
    cubeb_stream_stop(m_main);
    cubeb_stream_destroy(m_main);
    for (cubeb_stream*& speaker : m_speakers)
    {
      if (speaker == nullptr)
        continue;
      cubeb_stream_stop(speaker);
      cubeb_stream_destroy(speaker);
      speaker = nullptr;
    }
#ifdef _WIN32
    if (m_com_needs_uninit)
    {
      m_com_needs_uninit = false;
      CoUninitialize();
    }
    m_com_ready = false;
#endif
  };

#ifdef _WIN32
  m_com_thread.PushBlocking(teardown);
#else
  teardown();
#endif

  m_context.reset();
}

bool CubebStream::OnCubebThread(const std::function<void()>& task)
{
#ifdef _WIN32
  if (!m_com_ready)
    return false;
  m_com_thread.PushBlocking(task);
#else
  task();
#endif
  return true;
}

bool CubebStream::Init()
{
  bool opened = false;

  OnCubebThread([this, &opened] {
    m_context = CubebUtils::GetContext();
    if (!m_context)
      return;

    m_surround = Config::ShouldUseDPL2Decoder();

    cubeb_stream_params params{};
    params.rate = m_mixer->GetSampleRate();
    params.channels = m_surround ? 6 : 2;
    params.format = m_surround ? CUBEB_SAMPLE_FLOAT32NE : CUBEB_SAMPLE_S16NE;
    params.layout = m_surround ? CUBEB_LAYOUT_3F2_LFE : CUBEB_LAYOUT_STEREO;

    u32 min_latency = 0;
    if (cubeb_get_min_latency(m_context.get(), &params, &min_latency) != CUBEB_OK)
      ERROR_LOG_FMT(AUDIO, "Error getting minimum latency");
    INFO_LOG_FMT(AUDIO, "Minimum latency: {} frames", min_latency);

    opened = cubeb_stream_init(m_context.get(), &m_main, "Dolphin Audio Output", nullptr, nullptr,
                               nullptr, &params, std::max(MIN_LATENCY_FRAMES, min_latency),
                               OnMainData, OnStateChange, this) == CUBEB_OK;

    // Speaker routing only exists for Wii games.
    if (opened && Core::System::GetInstance().IsWii() &&
        Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED))
    {
      OpenSpeakerStreams();
    }
  });

  return opened;
}

void CubebStream::OpenSpeakerStreams()
{
  cubeb_stream_params params{};
  params.rate = m_mixer->GetSampleRate();
  params.channels = 2;
  params.format = CUBEB_SAMPLE_S16NE;
  params.layout = CUBEB_LAYOUT_STEREO;

  for (std::size_t i = 0; i < SPEAKER_COUNT; ++i)
  {
    if (!Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]))
      continue;

    // An empty device ID means the system default output.
    const std::string device_id = Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE[i]);
    const cubeb_devid device =
        device_id.empty() ? nullptr :
                            static_cast<cubeb_devid>(CubebUtils::GetOutputDeviceById(device_id));

    u32 min_latency = 0;
    cubeb_get_min_latency(m_context.get(), &params, &min_latency);

    m_routes[i] = {this, i};
    const std::string name = fmt::format("Dolphin Wiimote {} Audio", i + 1);
    const int result = cubeb_stream_init(
        m_context.get(), &m_speakers[i], name.c_str(), nullptr, nullptr, device, &params,
        std::max(MIN_LATENCY_FRAMES, min_latency), OnSpeakerData, OnStateChange, &m_routes[i]);
    if (result != CUBEB_OK)
    {
      ERROR_LOG_FMT(AUDIO, "Failed to create Cubeb stream for Wiimote {} audio routing", i + 1);
      m_speakers[i] = nullptr;
    }
  }
}

bool CubebStream::SetRunning(bool running)
{
  bool ok = false;

  OnCubebThread([this, running, &ok] {
    const auto apply = running ? cubeb_stream_start : cubeb_stream_stop;
    ok = apply(m_main) == CUBEB_OK;
    for (cubeb_stream* speaker : m_speakers)
    {
      if (speaker != nullptr)
        apply(speaker);
    }
  });

  return ok;
}

void CubebStream::SetVolume(int volume)
{
  OnCubebThread(
      [this, volume] { cubeb_stream_set_volume(m_main, static_cast<float>(volume) / 100.0f); });
}

long CubebStream::OnMainData(cubeb_stream*, void* user_data, const void*, void* output_buffer,
                             long num_frames)
{
  const auto* self = static_cast<const CubebStream*>(user_data);
  const auto frames = static_cast<std::size_t>(num_frames);

  if (self->m_surround)
    self->m_mixer->MixSurround(static_cast<float*>(output_buffer), frames);
  else
    self->m_mixer->Mix(static_cast<s16*>(output_buffer), frames);

  return num_frames;
}

long CubebStream::OnSpeakerData(cubeb_stream*, void* user_data, const void*, void* output_buffer,
                                long num_frames)
{
  const auto* route = static_cast<const SpeakerRoute*>(user_data);
  route->owner->m_mixer->MixWiimoteSpeaker(route->index, static_cast<s16*>(output_buffer),
                                           static_cast<std::size_t>(num_frames));
  return num_frames;
}

void CubebStream::OnStateChange(cubeb_stream*, void*, cubeb_state)
{
}
