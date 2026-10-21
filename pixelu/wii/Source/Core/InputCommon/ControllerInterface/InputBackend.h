// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <vector>

class ControllerInterface;

namespace ciface
{
namespace Core
{
class Device;
}

// A source of devices (one host input API). ControllerInterface owns one instance per enabled
// API and asks it to (re)populate its devices, to poll, and to react to render window changes.
class InputBackend
{
public:
  InputBackend(ControllerInterface* controller_interface);
  virtual ~InputBackend();

  // Adds this backend's devices (directly or later, from the backend's own thread).
  virtual void PopulateDevices() = 0;

  // Called once per input update, with the device list locked. A backend must not add or remove
  // devices here; it may only append the devices that should go away to `devices_to_remove`.
  virtual void UpdateInput(std::vector<std::weak_ptr<ciface::Core::Device>>& devices_to_remove);

  // The render window changed; refresh only what depends on the window handle.
  virtual void HandleWindowChange();

  ControllerInterface& GetControllerInterface();

private:
  ControllerInterface& m_owner;
};

}  // namespace ciface
