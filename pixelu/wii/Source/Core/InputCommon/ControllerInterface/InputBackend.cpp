// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface
{
InputBackend::InputBackend(ControllerInterface* controller_interface)
    : m_owner(*controller_interface)
{
}

InputBackend::~InputBackend() = default;

// Most backends poll inside their devices, so the backend-wide hooks do nothing by default.
void InputBackend::UpdateInput(std::vector<std::weak_ptr<ciface::Core::Device>>&)
{
}

void InputBackend::HandleWindowChange()
{
}

ControllerInterface& InputBackend::GetControllerInterface()
{
  return m_owner;
}

}  // namespace ciface
