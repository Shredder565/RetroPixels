// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/Network/NCD/WiiNetConfig.h"

namespace IOS::HLE
{
// /dev/net/ncd/manage: the network settings, the link state and the wireless driver lock.
class NetNCDManageDevice : public EmulationDevice
{
public:
  NetNCDManageDevice(EmulationKernel& ios, const std::string& device_name);

  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  void DoState(PointerWrap& p) override;

private:
  enum
  {
    IOCTLV_NCD_LOCKWIRELESSDRIVER = 0x1,
    IOCTLV_NCD_UNLOCKWIRELESSDRIVER = 0x2,
    IOCTLV_NCD_GETCONFIG = 0x3,
    IOCTLV_NCD_SETCONFIG = 0x4,
    IOCTLV_NCD_READCONFIG = 0x5,
    IOCTLV_NCD_WRITECONFIG = 0x6,
    IOCTLV_NCD_GETLINKSTATUS = 0x7,
    IOCTLV_NCD_GETWIRELESSMACADDRESS = 0x8,
  };

  Net::WiiNetConfig m_config;
  // The fd that holds the wireless driver lock, or 0.
  u32 m_ipc_fd = 0;
};
}  // namespace IOS::HLE
