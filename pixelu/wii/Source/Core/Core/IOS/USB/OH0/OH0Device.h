// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"

class PointerWrap;

namespace IOS::HLE
{
class OH0;

// One /dev/usb/oh0/<vid>/<pid> node. Everything is handed on to /dev/usb/oh0.
class OH0Device final : public EmulationDevice
{
public:
  OH0Device(EmulationKernel& ios, const std::string& device_name);

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;
  void DoState(PointerWrap& p) override;

private:
  void FindController();

  std::shared_ptr<OH0> m_controller;
  u16 m_vid = 0;
  u16 m_pid = 0;
  u64 m_device_id = 0;
};
}  // namespace IOS::HLE
