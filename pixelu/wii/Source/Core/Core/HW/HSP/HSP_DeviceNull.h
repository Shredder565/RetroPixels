// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/HSP/HSP_Device.h"

namespace HSP
{
// Empty port: writes vanish and reads leave the caller's buffer as it was.
class CHSPDevice_Null final : public IHSPDevice
{
public:
  HSPDeviceType GetDeviceType() const override { return HSPDeviceType::None; }

  void Read(u32 address, std::span<u8, TRANSFER_SIZE> data) override;
  void Write(u32 address, std::span<const u8, TRANSFER_SIZE> data) override;
};
}  // namespace HSP
