// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/SI/SI_Device.h"

namespace SerialInterface
{
// An empty port: it never answers, so the game sees "no response".
class CSIDevice_Null final : public ISIDevice
{
public:
  CSIDevice_Null(Core::System& system, SIDevices device, int device_number);

  int RunBuffer(u8* buffer, int request_length) override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;
};
}  // namespace SerialInterface
