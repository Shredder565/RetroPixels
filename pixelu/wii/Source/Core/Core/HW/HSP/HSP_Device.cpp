// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HSP/HSP_Device.h"

#include <memory>

#include "Core/HW/HSP/HSP_DeviceARAMExpansion.h"
#include "Core/HW/HSP/HSP_DeviceGBPlayer.h"
#include "Core/HW/HSP/HSP_DeviceNull.h"
#include "Core/System.h"

namespace HSP
{
IHSPDevice::~IHSPDevice() = default;

void IHSPDevice::DoState(PointerWrap&)
{
}

std::unique_ptr<IHSPDevice> HSPDevice_Create(const HSPDeviceType device)
{
  if (device == HSPDeviceType::ARAMExpansion)
    return std::make_unique<CHSPDevice_ARAMExpansion>();
  if (device == HSPDeviceType::GBPlayer)
    return std::make_unique<CHSPDevice_GBPlayer>(Core::System::GetInstance());

  // Unknown values fall back to an empty port.
  return std::make_unique<CHSPDevice_Null>();
}
}  // namespace HSP
