// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_Device.h"

namespace ExpansionInterface
{
// A placeholder for hardware that is not emulated. Unlike an empty slot it reports itself as
// present, so software sees an unknown device; every access is only logged.
class CEXIDummy final : public IEXIDevice
{
public:
  CEXIDummy(Core::System& system, std::string name);

  void ImmWrite(u32 data, u32 size) override;
  u32 ImmRead(u32 size) override;

  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;

  bool IsPresent() const override;

private:
  void TransferByte(u8& byte) override;

  std::string m_label;
};
}  // namespace ExpansionInterface
