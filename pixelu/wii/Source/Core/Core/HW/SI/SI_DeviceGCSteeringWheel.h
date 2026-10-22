// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/SI/SI_DeviceGCController.h"

namespace SerialInterface
{
// The GameCube steering wheel with pedals and force feedback.
class CSIDevice_GCSteeringWheel final : public CSIDevice_GCController
{
public:
  CSIDevice_GCSteeringWheel(Core::System& system, SIDevices device, int device_number);

  int RunBuffer(u8* buffer, int request_length) override;
  DataResponse GetData(u32& hi, u32& low) override;
  void SendCommand(u32 command, u8 poll) override;
};
}  // namespace SerialInterface
