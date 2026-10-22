// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/USB/Host.h"

class PointerWrap;

namespace IOS::HLE
{
namespace USB
{
// Key of an insertion hook. Ordering (and so lookup) only looks at the vendor ID, so hooks for
// two products of one vendor share a slot. Saved raw in savestates.
struct DeviceInfo
{
  u16 vid;
  u16 pid;
  bool operator<(const DeviceInfo& other) const { return vid < other.vid; }
};
}  // namespace USB

// /dev/usb/oh0: the old (USB interface version 0) host controller. Opened devices are reached
// through OH0Device, which forwards to the Device* functions below.
class OH0 final : public USBHost
{
public:
  OH0(EmulationKernel& ios, const std::string& device_name);
  ~OH0() override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  std::pair<ReturnCode, u64> DeviceOpen(u16 vid, u16 pid);
  void DeviceClose(u64 device_id);
  std::optional<IPCReply> DeviceIOCtl(u64 device_id, const IOCtlRequest& request);
  std::optional<IPCReply> DeviceIOCtlV(u64 device_id, const IOCtlVRequest& request);

  void DoState(PointerWrap& p) override;

private:
  IPCReply RootHubDescriptorA(const IOCtlRequest& request) const;
  IPCReply CancelInsertHook(const IOCtlRequest& request);
  IPCReply ListDevices(const IOCtlVRequest& request) const;
  IPCReply RootHubPortStatus(const IOCtlVRequest& request, bool set) const;
  std::optional<IPCReply> AddInsertHook(const IOCtlVRequest& request);
  std::optional<IPCReply> AddInsertHookWithId(const IOCtlVRequest& request);
  std::optional<IPCReply> AddClassChangeHook(const IOCtlVRequest& request) const;
  std::optional<IPCReply> AddRemovalHook(u64 device_id, const IOCtlRequest& request);
  s32 StartTransfer(USB::Device& device, const IOCtlVRequest& ioctlv);

  bool IsPresent(u16 vid, u16 pid) const;
  void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device) override;

  // Answers the request waiting in `hooks` under `key`, if any, and forgets it.
  template <typename Key>
  void FireHook(std::map<Key, u32>& hooks, const Key& key, ReturnCode result);

  // Guest request addresses of the pending hooks.
  std::map<USB::DeviceInfo, u32> m_insert_hooks;
  std::map<u64, u32> m_removal_hooks;
  std::mutex m_hook_mutex;

  // Host IDs of the devices an OH0Device currently has open (guarded by m_devices_mutex).
  std::set<u64> m_open_devices;
};
}  // namespace IOS::HLE
