// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBScanner.h"

class PointerWrap;

namespace IOS::HLE
{
// Base of the IOS resource managers that hand out USB devices (/dev/usb/oh0, /dev/usb/ven,
// /dev/usb/hid). It keeps the list of attached devices in sync with the USB scanner and tells
// subclasses about every insertion and removal.
class USBHost : public EmulationDevice
{
public:
  USBHost(EmulationKernel& ios, const std::string& device_name);
  ~USBHost() override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;

  void DoState(PointerWrap& p) override;

  // Called by the scanner thread whenever the set of host devices changes.
  void OnDevicesChanged(const USBScanner::DeviceMap& new_devices);

protected:
  enum class ChangeEvent
  {
    Inserted,
    Removed,
  };

  std::shared_ptr<USB::Device> GetDeviceById(u64 device_id) const;
  virtual void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> changed_device);
  // Called once after a batch of OnDeviceChange calls.
  virtual void OnDeviceChangeEnd();
  virtual bool ShouldAddDevice(const USB::Device& device) const;

  // Runs `submit` for a transfer on `device`. No reply is returned when the transfer was
  // queued (the backend answers later); otherwise the reply carries the error.
  std::optional<IPCReply> HandleTransfer(const std::shared_ptr<USB::Device>& device, u32 request,
                                         const std::function<s32()>& submit) const;

  std::map<u64, std::shared_ptr<USB::Device>> m_devices;
  mutable std::recursive_mutex m_devices_mutex;

private:
  void Update() override;
  void SyncDeviceList(const USBScanner::DeviceMap& new_devices);

  bool m_scanner_attached = false;
};
}  // namespace IOS::HLE
