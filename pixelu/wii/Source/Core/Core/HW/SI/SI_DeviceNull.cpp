// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceNull.h"

namespace SerialInterface
{
CSIDevice_Null::CSIDevice_Null(Core::System& system, SIDevices device, int device_number)
    : ISIDevice{system, device, device_number}
{
}

int CSIDevice_Null::RunBuffer(u8*, int)
{
  return -1;
}

DataResponse CSIDevice_Null::GetData(u32&, u32&)
{
  return DataResponse::ErrorNoResponse;
}

void CSIDevice_Null::SendCommand(u32, u8)
{
}
}  // namespace SerialInterface
