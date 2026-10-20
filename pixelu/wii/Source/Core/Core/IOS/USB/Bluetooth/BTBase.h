// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

class SysConf;

namespace IOS::HLE
{
// Saves a pending USB message by the address of its IPC request (0 when there is none). On load
// the message is decoded again from that request in emulated memory.
template <typename T>
static void DoStateForMessage(EmulationKernel& ios, PointerWrap& p, std::unique_ptr<T>& message)
{
  u32 address = message ? message->ios_request.address : 0;
  p.Do(address);
  if (address == 0)
    return;
  const IOCtlVRequest request{ios.GetSystem(), address};
  message = std::make_unique<T>(ios, request);
}

// The emulated Bluetooth module rewrites the SYSCONF list of paired remotes (BT.DINF). These keep
// the user's original list in a backup file and put it back afterwards.
void BackUpBTInfoSection(const SysConf* sysconf);
void RestoreBTInfoSection(SysConf* sysconf);

// Common base of /dev/usb/oh1/57e/305, the Wii's internal Bluetooth module.
class BluetoothBaseDevice : public EmulationDevice
{
public:
  using EmulationDevice::EmulationDevice;

  // The red sync button. Only Bluetooth passthrough reacts to it.
  virtual void UpdateSyncButtonState(bool /*is_held*/) {}
  virtual void TriggerSyncButtonPressedEvent() {}
  virtual void TriggerSyncButtonHeldEvent() {}

protected:
  // Buffer sizes the Wii's module (a BCM2045) reports. Nintendo's stack must never be told about
  // more than 10 ACL buffers.
  static constexpr int ACL_PKT_SIZE = 339;
  static constexpr int ACL_PKT_NUM = 10;
  static constexpr int SCO_PKT_SIZE = 64;
  static constexpr int SCO_PKT_NUM = 0;

  // USB endpoints of the module.
  enum USBEndpoint
  {
    HCI_CTRL = 0x00,
    HCI_EVENT = 0x81,
    ACL_DATA_IN = 0x82,
    ACL_DATA_OUT = 0x02
  };
};
}  // namespace IOS::HLE
