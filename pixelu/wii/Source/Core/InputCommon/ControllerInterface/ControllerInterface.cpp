// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include <algorithm>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"

#ifdef CIFACE_USE_WIN32
#include "InputCommon/ControllerInterface/Win32/Win32.h"
#endif
#ifdef CIFACE_USE_SDL
#include "InputCommon/ControllerInterface/SDL/SDL.h"
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#endif
#ifdef CIFACE_USE_STEAMDECK
#include "InputCommon/ControllerInterface/SteamDeck/SteamDeck.h"
#endif

ControllerInterface g_controller_interface;

namespace
{
// The input channel of the current thread. Every thread starts on Host: hotkeys are read on a
// worker thread and the UI on the main thread; the emulated controllers switch their own threads.
thread_local ciface::InputChannel t_channel = ciface::InputChannel::Host;

// Set while this thread runs the per-device input updates, which must not change the list.
thread_local bool t_in_input_update = false;

// Whether `device` could take `id` without clashing with an existing device of the same source
// and name.
bool IsIdFree(const std::vector<std::shared_ptr<ciface::Core::Device>>& devices,
              const ciface::Core::Device& device, int id)
{
  return std::ranges::none_of(devices, [&](const std::shared_ptr<ciface::Core::Device>& other) {
    return other->GetSource() == device.GetSource() && other->GetName() == device.GetName() &&
           other->GetId() == id;
  });
}
}  // namespace

void ControllerInterface::Initialize(const WindowSystemInfo& wsi)
{
  if (m_is_init)
    return;

  std::lock_guard population(m_population_lock);

  m_window_info = wsi;

  // Hold back change notifications until the first refresh is complete.
  m_population_depth = 1;

  // Backends are created (and later polled) in this order.
#ifdef CIFACE_USE_WIN32
  m_backends.push_back(ciface::Win32::CreateInputBackend(this));
#endif
#ifdef CIFACE_USE_SDL
  m_backends.push_back(ciface::SDL::CreateInputBackend(this));
#endif
#ifdef CIFACE_USE_DUALSHOCKUDPCLIENT
  m_backends.push_back(ciface::DualShockUDPClient::CreateInputBackend(this));
#endif
#ifdef CIFACE_USE_STEAMDECK
  m_backends.push_back(ciface::SteamDeck::CreateInputBackend(this));
#endif

  // Devices may only be added from here on; anything added earlier would be lost to the first
  // refresh (or wait on the lock from another thread).
  m_is_init = true;

  RefreshDevices();

  bool have_devices;
  {
    std::lock_guard devices(m_devices_mutex);
    have_devices = !m_devices.empty();
  }

  if (m_population_depth.fetch_sub(1) == 1 && have_devices)
    NotifyDevicesChanged();
}

void ControllerInterface::ChangeWindow(void* hwnd, WindowChangeReason reason)
{
  if (!m_is_init)
    return;

  // Only the render window is used by the backends.
  m_window_info.render_window = hwnd;

  if (reason == WindowChangeReason::Exit)
  {
    // Nothing will use the devices again.
    ClearDevices();
    return;
  }

  RefreshDevices(RefreshReason::WindowChangeOnly);
}

void ControllerInterface::EndPopulation()
{
  if (m_population_depth.fetch_sub(1) == 1)
    NotifyDevicesChanged();
}

void ControllerInterface::RefreshDevices(RefreshReason reason)
{
  if (!m_is_init)
    return;

  // Hotplug threads of several backends may be adding or removing devices at any time; holding
  // the population lock for the whole refresh keeps them from interleaving with it (which could
  // lose or duplicate devices).
  std::lock_guard population(m_population_lock);

  m_population_depth.fetch_add(1);

  if (reason == RefreshReason::WindowChangeOnly)
  {
    // Keep the devices; let the backends redo what depends on the window.
    for (const std::unique_ptr<ciface::InputBackend>& backend : m_backends)
      backend->HandleWindowChange();
    EndPopulation();
    return;
  }

  // Release every device before the backends recreate them.
  ClearDevices();

  // Some backends populate asynchronously (from their own threads, through
  // PlatformPopulateDevices), so devices are not necessarily listed in backend order. Backends
  // that provide likely default devices populate synchronously, so a default device exists by
  // the time controller profiles are loaded.
  for (const std::unique_ptr<ciface::InputBackend>& backend : m_backends)
    backend->PopulateDevices();

  WiimoteReal::PopulateDevices();

  EndPopulation();
}

void ControllerInterface::PlatformPopulateDevices(const std::function<void()>& callback)
{
  if (!m_is_init)
    return;

  std::lock_guard population(m_population_lock);

  m_population_depth.fetch_add(1);
  callback();
  EndPopulation();
}

void ControllerInterface::Shutdown()
{
  if (!m_is_init)
    return;

  // Refuse new devices from here on, and keep notifications from firing during the teardown
  // (the depth never drops back to zero).
  m_is_init = false;
  m_population_depth = 1;

  // Listeners release their references to the devices before the backends go away.
  ClearDevices();

  m_backends.clear();

  // A backend thread may have slipped a device in between its m_is_init check and the flag
  // change above (the population lock is not held for the whole shutdown, which could deadlock).
  ClearDevices();
}

void ControllerInterface::ClearDevices()
{
  std::lock_guard population(m_population_lock);

  {
    std::lock_guard devices(m_devices_mutex);

    if (m_devices.empty())
      return;

    // Leave no rumble or LED running on devices that may outlive the list.
    for (const std::shared_ptr<ciface::Core::Device>& device : m_devices)
    {
      for (ciface::Core::Device::Output* output : device->Outputs())
        output->SetState(0);
    }

    m_devices.clear();
  }

  // Listeners must drop their references now, whatever population is in progress.
  NotifyDevicesChanged();
}

bool ControllerInterface::AddDevice(std::shared_ptr<ciface::Core::Device> device)
{
  // Shut down (or shutting down): ignore.
  if (!m_is_init)
    return false;

  ASSERT_MSG(CONTROLLERINTERFACE, !t_in_input_update,
             "Devices shouldn't be added within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  std::lock_guard population(m_population_lock);

  {
    std::lock_guard devices(m_devices_mutex);

    // The device's preferred id if it is free, otherwise the lowest free id.
    int id = 0;
    const std::optional<int> preferred = device->GetPreferredId();
    if (preferred.has_value() && IsIdFree(m_devices, *device, *preferred))
    {
      id = *preferred;
    }
    else
    {
      while (!IsIdFree(m_devices, *device, id))
        ++id;
    }
    device->SetId(id);

    NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Added device: {}", device->GetQualifiedName());
    m_devices.push_back(std::move(device));

    // Keep the list in a predictable order whatever order devices arrive in: by sort priority,
    // then by arrival. The head of the list becomes the default device of new profiles.
    std::ranges::stable_sort(m_devices, std::ranges::greater{},
                             &ciface::Core::Device::GetSortPriority);
  }

  if (m_population_depth == 0)
    NotifyDevicesChanged();
  return true;
}

void ControllerInterface::RemoveDevice(std::function<bool(const ciface::Core::Device*)> callback,
                                       bool force_devices_release)
{
  // Shut down (or shutting down): ignore.
  if (!m_is_init)
    return;

  ASSERT_MSG(CONTROLLERINTERFACE, !t_in_input_update,
             "Devices shouldn't be removed within input update calls, there is a risk of deadlock "
             "if another thread was already here");

  std::lock_guard population(m_population_lock);

  size_t removed_count;
  {
    std::lock_guard devices(m_devices_mutex);
    removed_count =
        std::erase_if(m_devices, [&callback](const std::shared_ptr<ciface::Core::Device>& device) {
          if (!callback(device.get()))
            return false;
          NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Removed device: {}", device->GetQualifiedName());
          return true;
        });
  }

  if (removed_count == 0)
    return;

  if (m_population_depth == 0 || force_devices_release)
    NotifyDevicesChanged();
}

void ControllerInterface::UpdateInput()
{
  // Must not be called before initialisation.
  ASSERT(m_is_init);
  if (!m_is_init)
    return;

  // Devices asking to be removed are collected while the list is locked and removed after it is
  // unlocked: taking the population lock while holding the list lock could deadlock against a
  // population thread, and weak pointers neither keep the devices alive nor dangle if someone
  // else removes them first.
  std::vector<std::weak_ptr<ciface::Core::Device>> leaving;

  {
    std::lock_guard devices(m_devices_mutex);

    t_in_input_update = true;

    for (const std::unique_ptr<ciface::InputBackend>& backend : m_backends)
      backend->UpdateInput(leaving);

    // Every device is polled, referenced or not; some backends rely on regular polling.
    for (const std::shared_ptr<ciface::Core::Device>& device : m_devices)
    {
      if (device->UpdateInput() == ciface::Core::DeviceRemoval::Remove)
        leaving.push_back(device);
    }

    t_in_input_update = false;
  }

  if (leaving.empty())
    return;

  RemoveDevice([&leaving](const ciface::Core::Device* device) {
    return std::ranges::any_of(leaving, [device](const std::weak_ptr<ciface::Core::Device>& entry) {
      return entry.lock().get() == device;
    });
  });
}

void ControllerInterface::SetCurrentInputChannel(ciface::InputChannel input_channel)
{
  t_channel = input_channel;
}

ciface::InputChannel ControllerInterface::GetCurrentInputChannel()
{
  return t_channel;
}

WindowSystemInfo ControllerInterface::GetWindowSystemInfo() const
{
  return m_window_info;
}

void ControllerInterface::SetAspectRatioAdjustment(float value)
{
  m_aspect_adjustment = value;
}

Common::Vec2 ControllerInterface::GetWindowInputScale() const
{
  const float adjustment = m_aspect_adjustment.load();

  // Stretch the axis along which the picture is narrower than the window.
  if (adjustment > 1)
    return {1.f, adjustment};
  return {1 / adjustment, 1.f};
}

void ControllerInterface::SetMouseCenteringRequested(bool center)
{
  m_center_mouse = center;
}

bool ControllerInterface::IsMouseCenteringRequested() const
{
  return m_center_mouse.load();
}

Common::EventHook
ControllerInterface::RegisterDevicesChangedCallback(Common::HookableEvent<>::CallbackType callback)
{
  return m_devices_changed.Register(std::move(callback));
}

void ControllerInterface::NotifyDevicesChanged()
{
  m_devices_changed.Trigger();
}
