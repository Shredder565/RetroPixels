// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "AudioCommon/SoundStream.h"

#ifdef _WIN32

// clang-format off
#include <windows.h>
#include <mmreg.h>
#include <objbase.h>
#include <wil/resource.h>
// clang-format on

#include <atomic>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <wrl/client.h>

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

#endif

// Sound output through WASAPI in exclusive mode: 16-bit stereo PCM at the mixer's rate, fed from
// an event-driven thread.
class WASAPIStream final : public SoundStream
{
#ifdef _WIN32
public:
  explicit WASAPIStream();
  ~WASAPIStream() override;
  bool Init() override;
  bool SetRunning(bool running) override;

  static bool IsValid();
  // Friendly names of the active render endpoints.
  static std::vector<std::string> GetAvailableDevices();
  static Microsoft::WRL::ComPtr<IMMDevice> GetDeviceByName(std::string_view name);

private:
  void RenderLoop();

  u32 m_buffer_frames = 0;
  std::atomic<bool> m_rendering = false;
  std::thread m_render_thread;

  // Members are destroyed in reverse order, so this one (which balances the constructor's
  // CoInitializeEx) must come before every COM pointer below.
  wil::unique_couninitialize_call m_com_scope{false};

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
  Microsoft::WRL::ComPtr<IAudioClient> m_client;
  Microsoft::WRL::ComPtr<IAudioRenderClient> m_renderer;
  wil::unique_event_nothrow m_buffer_ready;
  WAVEFORMATEXTENSIBLE m_format{};
#endif  // _WIN32
};
