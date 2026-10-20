// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/HSP/HSP_Device.h"

namespace HSP
{
// Extra auxiliary RAM on the high speed port. The configured size is rounded
// up to a power of two and addresses wrap around it.
class CHSPDevice_ARAMExpansion final : public IHSPDevice
{
public:
  explicit CHSPDevice_ARAMExpansion();
  ~CHSPDevice_ARAMExpansion() override;

  HSPDeviceType GetDeviceType() const override { return HSPDeviceType::ARAMExpansion; }

  void Read(u32 address, std::span<u8, TRANSFER_SIZE> data) override;
  void Write(u32 address, std::span<const u8, TRANSFER_SIZE> data) override;

  void DoState(PointerWrap&) override;

private:
  u8* Block(u32 address) const;

  const u32 m_bytes;
  const u32 m_wrap;
  u8* const m_storage;
};
}  // namespace HSP
