// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "AudioCommon/WASAPIStream.h"

#ifdef _WIN32

// clang-format off
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wil/resource.h>
// clang-format on

#include <functional>
#include <string>
#include <thread>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "VideoCommon/OnScreenDisplay.h"

using Microsoft::WRL::ComPtr;

namespace
{
constexpr DWORD STREAM_FLAGS = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;

// Logs a failed HRESULT with what was being attempted. Returns true on success.
bool Check(std::string_view action, HRESULT result)
{
  if (SUCCEEDED(result))
    return true;

  const std::string reason = result == AUDCLNT_E_DEVICE_IN_USE ? "Audio endpoint already in use!" :
                                                                 Common::GetHResultMessage(result);
  ERROR_LOG_FMT(AUDIO, "WASAPI: {}: {} ({:08x})", action, reason, result);
  return false;
}

// Reads the endpoint's display name; false if its property store cannot be opened.
bool ReadFriendlyName(IMMDevice* device, std::string* name)
{
  ComPtr<IPropertyStore> properties;
  if (!Check("Failed to initialize IPropertyStore",
             device->OpenPropertyStore(STGM_READ, &properties)))
  {
    return false;
  }

  wil::unique_prop_variant value;
  properties->GetValue(PKEY_Device_FriendlyName, value.addressof());
  *name = TStrToUTF8(value.pwszVal);
  return true;
}

// Calls `visit` for each active render endpoint until it returns false.
void VisitRenderEndpoints(const std::function<bool(ComPtr<IMMDevice>, std::string)>& visit)
{
  // This may run on any thread. RPC_E_CHANGED_MODE (COM already set up in another apartment
  // model on this thread) is fine for what follows.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (com != RPC_E_CHANGED_MODE && !Check("Failed to call CoInitialize", com))
    return;

  // Balances a successful CoInitializeEx once the COM objects below are released.
  wil::unique_couninitialize_call com_scope;
  if (FAILED(com))
    com_scope.release();

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (!Check("Failed to create MMDeviceEnumerator",
             CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&enumerator))))
  {
    return;
  }

  ComPtr<IMMDeviceCollection> endpoints;
  if (!Check("Failed to get available devices",
             enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)))
  {
    return;
  }

  UINT count = 0;
  endpoints->GetCount(&count);

  for (UINT i = 0; i < count; ++i)
  {
    ComPtr<IMMDevice> device;
    if (!Check(fmt::format("Failed to get device {}", i), endpoints->Item(i, &device)))
      continue;

    std::string name;
    if (!ReadFriendlyName(device.Get(), &name))
      continue;

    if (!visit(std::move(device), std::move(name)))
      break;
  }
}

// Initialises the client for exclusive mode with the given buffer period (in 100 ns units).
HRESULT InitializeExclusive(IAudioClient* client, REFERENCE_TIME period,
                            WAVEFORMATEXTENSIBLE* format)
{
  return client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, STREAM_FLAGS, period, period,
                            reinterpret_cast<WAVEFORMATEX*>(format), nullptr);
}
}  // namespace

WASAPIStream::WASAPIStream()
{
  if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    m_com_scope.activate();

  // 16-bit stereo PCM at the mixer's rate.
  WAVEFORMATEX& wave = m_format.Format;
  wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wave.nChannels = 2;
  wave.nSamplesPerSec = GetMixer()->GetSampleRate();
  wave.nAvgBytesPerSec = wave.nSamplesPerSec * 4;
  wave.nBlockAlign = 4;
  wave.wBitsPerSample = 16;
  wave.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  m_format.Samples.wValidBitsPerSample = 16;
  m_format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  m_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

WASAPIStream::~WASAPIStream()
{
  m_rendering.store(false, std::memory_order_relaxed);
  if (m_render_thread.joinable())
    m_render_thread.join();
}

bool WASAPIStream::IsValid()
{
  return true;
}

std::vector<std::string> WASAPIStream::GetAvailableDevices()
{
  std::vector<std::string> names;
  VisitRenderEndpoints([&names](ComPtr<IMMDevice>, std::string name) {
    names.push_back(std::move(name));
    return true;
  });
  return names;
}

ComPtr<IMMDevice> WASAPIStream::GetDeviceByName(std::string_view name)
{
  ComPtr<IMMDevice> match;
  VisitRenderEndpoints([&match, name](ComPtr<IMMDevice> device, std::string device_name) {
    if (device_name != name)
      return true;
    match = std::move(device);
    return false;
  });
  return match;
}

bool WASAPIStream::Init()
{
  ASSERT(m_enumerator == nullptr);
  return Check("Failed to create MMDeviceEnumerator",
               CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&m_enumerator)));
}

bool WASAPIStream::SetRunning(bool running)
{
  if (!running)
  {
    m_rendering.store(false, std::memory_order_relaxed);
    if (m_render_thread.joinable())
      m_render_thread.join();

    m_buffer_ready.reset();
    m_renderer.Reset();
    m_client.Reset();
    return true;
  }

  // Pick the configured endpoint, or the default console endpoint if none is set or it is gone.
  ComPtr<IMMDevice> device;
  HRESULT result = S_OK;
  if (Config::IsDefaultValue(Config::MAIN_WASAPI_DEVICE))
  {
    result = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  }
  else
  {
    device = GetDeviceByName(Config::Get(Config::MAIN_WASAPI_DEVICE));
    if (!device)
    {
      ERROR_LOG_FMT(AUDIO, "Can't find device '{}', falling back to default",
                    Config::Get(Config::MAIN_WASAPI_DEVICE));
      result = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
  }
  if (!Check("Failed to obtain default endpoint", result))
    return false;

  std::string endpoint_name;
  if (!ReadFriendlyName(device.Get(), &endpoint_name))
    return false;
  INFO_LOG_FMT(AUDIO, "Using audio endpoint '{}'", endpoint_name);

  ComPtr<IAudioClient> client;
  if (!Check("Failed to activate IAudioClient",
             device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client)))
  {
    return false;
  }

  // The device's default period plus the configured extra latency.
  REFERENCE_TIME period = 0;
  result = client->GetDevicePeriod(nullptr, &period);
  period += Config::Get(Config::MAIN_AUDIO_LATENCY) * (10000 / m_format.Format.nChannels);
  INFO_LOG_FMT(AUDIO, "Audio period set to {}", period);
  if (!Check("Failed to obtain device period", result))
    return false;

  result = InitializeExclusive(client.Get(), period, &m_format);

  if (result == AUDCLNT_E_UNSUPPORTED_FORMAT)
  {
    OSD::AddMessage("Your current audio device doesn't support 16-bit 48000 hz PCM audio. WASAPI "
                    "exclusive mode won't work.",
                    6000U);
    return false;
  }

  if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
  {
    // Retry with a period that matches the buffer size the device proposes. A client whose
    // Initialize failed cannot be reused, so activate a new one.
    if (!Check("Failed to get aligned buffer size", client->GetBufferSize(&m_buffer_frames)))
      return false;
    if (!Check("Failed to reactivate IAudioClient",
               device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &client)))
    {
      return false;
    }

    const double aligned = 10000.0 * 1000 * m_buffer_frames / m_format.Format.nSamplesPerSec;
    period = static_cast<REFERENCE_TIME>(aligned + 0.5) +
             Config::Get(Config::MAIN_AUDIO_LATENCY) * 10000;
    result = InitializeExclusive(client.Get(), period, &m_format);
  }

  if (!Check("Failed to initialize IAudioClient", result))
    return false;
  if (!Check("Failed to get buffer size from IAudioClient",
             client->GetBufferSize(&m_buffer_frames)))
    return false;

  ComPtr<IAudioRenderClient> renderer;
  if (!Check("Failed to get IAudioRenderClient from IAudioClient",
             client->GetService(IID_PPV_ARGS(&renderer))))
  {
    return false;
  }

  wil::unique_event_nothrow buffer_ready;
  buffer_ready.create();
  client->SetEventHandle(buffer_ready.get());

  if (!Check("Failed to get IAudioRenderClient from IAudioClient", client->Start()))
    return false;

  INFO_LOG_FMT(AUDIO, "WASAPI: Successfully initialized!");

  m_client = std::move(client);
  m_renderer = std::move(renderer);
  m_buffer_ready = std::move(buffer_ready);

  m_rendering.store(true, std::memory_order_relaxed);
  m_render_thread = std::thread(&WASAPIStream::RenderLoop, this);
  return true;
}

void WASAPIStream::RenderLoop()
{
  Common::SetCurrentThreadName("WASAPI Handler");

  // Start with one buffer of silence.
  BYTE* buffer = nullptr;
  if (m_renderer->GetBuffer(m_buffer_frames, &buffer) != S_OK)
  {
    m_rendering.store(false, std::memory_order_relaxed);
    return;
  }
  m_renderer->ReleaseBuffer(m_buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);

  while (m_rendering.load(std::memory_order_relaxed))
  {
    WaitForSingleObject(m_buffer_ready.get(), 1000);

    if (m_renderer->GetBuffer(m_buffer_frames, &buffer) != S_OK)
    {
      m_rendering.store(false, std::memory_order_relaxed);
      return;
    }

    s16* const frames = reinterpret_cast<s16*>(buffer);
    GetMixer()->Mix(frames, m_buffer_frames);

    // Exclusive mode has no endpoint volume of its own, so scale the samples here.
    const int volume = Config::Get(Config::MAIN_AUDIO_VOLUME);
    const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED) || volume == 0;
    if (!muted && volume != 100)
    {
      const float gain = static_cast<float>(volume) / 100.0f;
      for (u32 i = 0; i < m_buffer_frames * 2; ++i)
        frames[i] = static_cast<s16>(frames[i] * gain);
    }

    m_renderer->ReleaseBuffer(m_buffer_frames, muted ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
  }
}

#endif  // _WIN32
