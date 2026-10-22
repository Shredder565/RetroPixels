// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>

#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTBase.h"

class PointerWrap;

namespace IOS::HLE
{
// Stands in for Bluetooth passthrough in builds without libusb: opening it fails.
class BluetoothStubDevice final : public BluetoothBaseDevice
{
public:
  using BluetoothBaseDevice::BluetoothBaseDevice;
  std::optional<IPCReply> Open(const OpenRequest& request) override;
  void DoState(PointerWrap& p) override;
};
}  // namespace IOS::HLE
