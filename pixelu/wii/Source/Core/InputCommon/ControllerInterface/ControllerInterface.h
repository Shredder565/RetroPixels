// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "Common/HookableEvent.h"
#include "Common/Matrix.h"
#include "Common/WindowSystemInfo.h"

#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

// Which input backends this build has: on Windows the Win32 one, on Linux XInput2 for the
// keyboard and mouse (when built with X11); SDL, DSU and the Steam Deck on both.
#ifdef _WIN32
#define CIFACE_USE_WIN32
#endif
#if defined(HAVE_X11) && HAVE_X11
#define CIFACE_USE_XLIB
#endif
#define CIFACE_USE_DUALSHOCKUDPCLIENT
#if defined(HAVE_SDL3)
#define CIFACE_USE_SDL
#endif
#if defined(HAVE_HIDAPI)
#define CIFACE_USE_STEAMDECK
#endif

namespace ciface
{
// Relative inputs (mouse movement and the like) keep a separate value for each consumer, since
// each consumer reads them at its own rate. The channel of the reading thread picks the value.
enum class InputChannel
{
  Host,
  SerialInterface,
  Bluetooth,
  FreeLook,
  Count,
};

}  // namespace ciface

// The registry of host input devices. Backends add and remove devices (also from their own
// hotplug threads); everything else reads the list and is told when it changes.
class ControllerInterface : public ciface::Core::DeviceContainer
{
public:
  enum class WindowChangeReason
  {
    // The application is exiting.
    Exit,
    Other
  };

  enum class RefreshReason
  {
    // Nothing but the render window changed.
    WindowChangeOnly,
    // Anything else (user request, initialisation...). The window may have changed too.
    Other
  };

  ControllerInterface() : m_is_init(false) {}
  void Initialize(const WindowSystemInfo& wsi);
  // Not thread safe: call from one thread at a time.
  void ChangeWindow(void* hwnd, WindowChangeReason reason = WindowChangeReason::Other);
  // Thread safe once initialised.
  void RefreshDevices(RefreshReason reason = RefreshReason::Other);
  void Shutdown();
  bool AddDevice(std::shared_ptr<ciface::Core::Device> device);
  // Removes every device `callback` returns true for. With `force_devices_release`, listeners
  // are told at once even during a population, so they drop their references immediately.
  void RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback,
                    bool force_devices_release = false);
  // Runs a backend's device population as one batch: listeners hear about the result once, and
  // populations from different threads do not interleave.
  void PlatformPopulateDevices(const std::function<void()>& callback);
  bool IsInit() const { return m_is_init; }
  void UpdateInput();

  // The ratio between the render window's aspect ratio and the drawn picture's, used to map the
  // mouse cursor onto the picture.
  void SetAspectRatioAdjustment(float);

  // Scale to apply to window-relative inputs, derived from the aspect ratio adjustment.
  Common::Vec2 GetWindowInputScale() const;

  // Asks for the mouse cursor to be moved to the centre of the render window.
  void SetMouseCenteringRequested(bool center);

  bool IsMouseCenteringRequested() const;

  [[nodiscard]] Common::EventHook
  RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback);

  static void SetCurrentInputChannel(ciface::InputChannel);
  static ciface::InputChannel GetCurrentInputChannel();

  WindowSystemInfo GetWindowSystemInfo() const;

private:
  // Drops every device (outputs reset first) and tells the listeners.
  void ClearDevices();

  void NotifyDevicesChanged();

  // Ends one population level; the outermost one tells the listeners.
  void EndPopulation();

  Common::HookableEvent<> m_devices_changed;

  // Serialises everything that adds or removes devices.
  mutable std::recursive_mutex m_population_lock;
  std::atomic<bool> m_is_init;
  // Nesting depth of populations in progress; listeners are told when it drops back to zero.
  std::atomic<int> m_population_depth;
  WindowSystemInfo m_window_info;
  std::atomic<float> m_aspect_adjustment = 1;
  std::atomic<bool> m_center_mouse = false;

  std::vector<std::unique_ptr<ciface::InputBackend>> m_backends;
};

namespace ciface
{
// A relative input's movement, kept per input channel: Move() adds to every channel, and each
// channel's Update() turns what accumulated into the value that channel reads.
template <typename T>
class RelativeInputState
{
public:
  void Update()
  {
    const auto index = static_cast<size_t>(ControllerInterface::GetCurrentInputChannel());
    m_current[index] = m_pending[index];
    m_pending[index] = {};
  }

  T GetValue() const
  {
    return m_current[static_cast<size_t>(ControllerInterface::GetCurrentInputChannel())];
  }

  void Move(T delta)
  {
    for (T& pending : m_pending)
      pending += delta;
  }

private:
  static constexpr size_t CHANNELS = static_cast<size_t>(InputChannel::Count);

  std::array<T, CHANNELS> m_current{};
  std::array<T, CHANNELS> m_pending{};
};
}  // namespace ciface

extern ControllerInterface g_controller_interface;
