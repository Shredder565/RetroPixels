// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "InputCommon/GCPadStatus.h"

namespace SerialInterface
{
// A port fed by a real controller on the USB GameCube adapter.
class CSIDevice_GCAdapter final : public CSIDevice_GCController
{
public:
  CSIDevice_GCAdapter(Core::System& system, SIDevices device, int device_number);

  GCPadStatus GetPadStatus() override;
  int RunBuffer(u8* buffer, int request_length) override;

  DataResponse GetData(u32& hi, u32& low) override;

private:
  // Report only the bongo buttons (for DK Bongos plugged into the adapter).
  bool m_simulate_konga{};
};
}  // namespace SerialInterface
