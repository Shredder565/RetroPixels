// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Host.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBScanner.h"
#include "Core/System.h"

namespace IOS::HLE
{
USBHost::USBHost(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

USBHost::~USBHost()
{
  GetSystem().GetUSBScanner().RemoveClient(this);
}

std::optional<IPCReply> USBHost::Open(const OpenRequest&)
{
  if (!m_scanner_attached)
  {
    USBScanner& scanner = GetSystem().GetUSBScanner();
    scanner.AddClient(this);
    // Some titles (Your Shape, for one) only look at the device list of the very first
    // device-change reply, so the first scan has to be finished before anything is answered.
    scanner.WaitForFirstScan();
    SyncDeviceList(scanner.GetDevices());
    m_scanner_attached = true;
  }
  return IPCReply(IPC_SUCCESS);
}

void USBHost::DoState(PointerWrap& p)
{
  Device::DoState(p);
  if (!IsOpened() || !p.IsReadMode())
    return;

  // The loaded state knows nothing about which host devices are plugged in now. Start from an
  // empty list so that every present device gets its insertion hooks fired again.
  std::lock_guard lock(m_devices_mutex);
  m_devices.clear();
  OnDevicesChanged(GetSystem().GetUSBScanner().GetDevices());
}

std::shared_ptr<USB::Device> USBHost::GetDeviceById(const u64 device_id) const
{
  std::lock_guard lock(m_devices_mutex);
  if (const auto found = m_devices.find(device_id); found != m_devices.end())
    return found->second;
  return nullptr;
}

void USBHost::OnDeviceChange(ChangeEvent, std::shared_ptr<USB::Device>)
{
}

void USBHost::OnDeviceChangeEnd()
{
}

bool USBHost::ShouldAddDevice(const USB::Device&) const
{
  return true;
}

void USBHost::Update()
{
  // With determinism wanted the scanner's callbacks are ignored; the list is pulled here, on
  // the emulation thread, instead.
  if (Core::WantsDeterminism())
    SyncDeviceList(GetSystem().GetUSBScanner().GetDevices());
}

void USBHost::OnDevicesChanged(const USBScanner::DeviceMap& new_devices)
{
  if (Core::WantsDeterminism())
    return;
  SyncDeviceList(new_devices);
}

void USBHost::SyncDeviceList(const USBScanner::DeviceMap& new_devices)
{
  std::lock_guard lock(m_devices_mutex);

  std::vector<u64> gone;
  for (const auto& entry : m_devices)
  {
    if (!new_devices.contains(entry.first))
      gone.push_back(entry.first);
  }

  // Each device is taken out of the list before its removal is announced, so a subclass that
  // looks at m_devices in the callback no longer sees it.
  for (const u64 id : gone)
  {
    auto node = m_devices.extract(id);
    std::shared_ptr<USB::Device> removed = std::move(node.mapped());
    INFO_LOG_FMT(IOS_USB, "{} - Removed device: {:04x}:{:04x}", GetDeviceName(), removed->GetVid(),
                 removed->GetPid());
    OnDeviceChange(ChangeEvent::Removed, std::move(removed));
  }

  bool added_any = false;
  for (const auto& [id, candidate] : new_devices)
  {
    if (m_devices.contains(id) || !ShouldAddDevice(*candidate))
      continue;

    INFO_LOG_FMT(IOS_USB, "{} - New device: {:04x}:{:04x}", GetDeviceName(), candidate->GetVid(),
                 candidate->GetPid());
    m_devices.emplace(id, candidate);
    added_any = true;
    OnDeviceChange(ChangeEvent::Inserted, candidate);
  }

  if (added_any || !gone.empty())
    OnDeviceChangeEnd();
}

std::optional<IPCReply> USBHost::HandleTransfer(const std::shared_ptr<USB::Device>& device,
                                                u32 request,
                                                const std::function<s32()>& submit) const
{
  if (!device)
    return IPCReply(IPC_ENOENT);

  const s32 result = submit();
  if (result == IPC_SUCCESS)
    return std::nullopt;

  ERROR_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Transfer (request {}) was not submitted: {}",
                device->GetVid(), device->GetPid(), request, device->GetErrorName(result));
  // Positive backend codes are not meaningful to the guest.
  return IPCReply(result > 0 ? IPC_EINVAL : result);
}
}  // namespace IOS::HLE
