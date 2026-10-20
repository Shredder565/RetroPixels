// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

class PointerWrap;

namespace IOS::HLE
{
// IOCtl numbers of the state transition manager (WiiBrew, "/dev/stm").
enum
{
  // Served by /dev/stm/eventhook.
  IOCTL_STM_EVENTHOOK = 0x1000,

  // Served by /dev/stm/immediate.
  IOCTL_STM_HOTRESET = 0x2001,
  IOCTL_STM_HOTRESET_FOR_PD = 0x2002,
  IOCTL_STM_SHUTDOWN = 0x2003,
  IOCTL_STM_IDLE = 0x2004,
  IOCTL_STM_WAKEUP = 0x2005,
  IOCTL_STM_GET_IDLEMODE = 0x3001,
  IOCTL_STM_RELEASE_EH = 0x3002,
  IOCTL_STM_READDDRREG = 0x4001,
  IOCTL_STM_READDDRREG2 = 0x4002,
  IOCTL_STM_VIDIMMING = 0x5001,
  IOCTL_STM_LEDFLASH = 0x6001,
  IOCTL_STM_LEDMODE = 0x6002,
  IOCTL_STM_READVER = 0x7001,
  IOCTL_STM_WRITEDMCU = 0x8001,
};

// Event words delivered to a waiting event hook.
enum
{
  STM_EVENT_RESET = 0x00020000,
  STM_EVENT_POWER = 0x00000800
};

// /dev/stm/immediate: power state requests that are answered at once.
class STMImmediateDevice final : public EmulationDevice
{
public:
  using EmulationDevice::EmulationDevice;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
};

// /dev/stm/eventhook: holds one request open until the Reset or Power button is pressed.
class STMEventHookDevice final : public EmulationDevice
{
public:
  using EmulationDevice::EmulationDevice;
  ~STMEventHookDevice() override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  void DoState(PointerWrap& p) override;

  bool HasHookInstalled() const;
  void ResetButton() const;
  void PowerButton() const;

private:
  void TriggerEvent(u32 event) const;
};
}  // namespace IOS::HLE
