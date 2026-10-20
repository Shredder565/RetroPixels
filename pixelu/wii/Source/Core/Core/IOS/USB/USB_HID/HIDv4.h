// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Host.h"

class PointerWrap;

namespace IOS::HLE
{
// /dev/usb/hid as it was before IOS57 (USB interface version 4). Only HID-class devices are
// offered, and each one gets a small integer ID that the guest uses in its requests.
class USB_HIDv4 final : public USBHost
{
public:
  USB_HIDv4(EmulationKernel& ios, const std::string& device_name);
  ~USB_HIDv4() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;

  void DoState(PointerWrap& p) override;

private:
  std::shared_ptr<USB::Device> FindByGuestId(s32 guest_id) const;

  std::optional<IPCReply> StartTransfer(const IOCtlRequest& request);
  IPCReply CancelInterrupt(const IOCtlRequest& request);
  std::optional<IPCReply> GetDeviceChange(const IOCtlRequest& request);
  IPCReply Shutdown(const IOCtlRequest& request);

  // Requires m_devicechange_hook_address_mutex to be held.
  void TriggerDeviceChangeReply();
  std::vector<u8> BuildDeviceSection(const USB::Device& device) const;
  void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device) override;
  bool ShouldAddDevice(const USB::Device& device) const override;

  bool m_has_pending_changes = true;
  // After a shutdown, the next device-change request is answered at once.
  bool m_is_shut_down = false;
  std::mutex m_devicechange_hook_address_mutex;
  std::unique_ptr<IOCtlRequest> m_devicechange_hook_request;

  mutable std::mutex m_id_map_mutex;
  // Guest device IDs and host device IDs, in both directions.
  std::map<s32, u64> m_ios_ids;
  std::map<u64, s32> m_device_ids;
};
}  // namespace IOS::HLE
