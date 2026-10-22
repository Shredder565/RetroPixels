// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <span>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace HSP
{

// Every access over the high speed port moves one 32-byte block.
static constexpr std::size_t TRANSFER_SIZE = 32;

// Stored in configs and savestates; keep the values.
enum class HSPDeviceType : int
{
  None,
  ARAMExpansion,
  GBPlayer,
};

// Something plugged into the GameCube's high speed port.
class IHSPDevice
{
public:
  virtual ~IHSPDevice();

  virtual HSPDeviceType GetDeviceType() const = 0;

  // The DSP MMIO handlers only pass block-aligned (32 byte) addresses.
  virtual void Read(u32 address, std::span<u8, TRANSFER_SIZE> data) = 0;
  virtual void Write(u32 address, std::span<const u8, TRANSFER_SIZE> data) = 0;

  // Default: the device has no state to save.
  virtual void DoState(PointerWrap& p);
};

std::unique_ptr<IHSPDevice> HSPDevice_Create(HSPDeviceType device);

}  // namespace HSP
