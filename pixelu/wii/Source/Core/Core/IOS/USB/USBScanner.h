// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/WaitableFlag.h"
#include "Core/IOS/USB/Common.h"
#include "Core/LibusbUtils.h"

class PointerWrap;

namespace IOS::HLE
{
class USBHost;

// Keeps the list of USB devices that the emulated USB host interfaces can see: the emulated
// accessories enabled in the settings plus whitelisted host devices (through libusb). A
// background thread refreshes the list while at least one USBHost is registered and pushes
// every change to the registered hosts.
class USBScanner final
{
public:
  using DeviceMap = std::map<u64, std::shared_ptr<USB::Device>>;

  ~USBScanner();

  // Both return whether the set of clients changed. The first client starts the scan thread
  // and the last one to go stops it.
  bool AddClient(USBHost* client);
  bool RemoveClient(USBHost* client);

  // Blocks until the scan thread has produced its first list (if it is running).
  void WaitForFirstScan();

  DeviceMap GetDevices() const;

private:
  void StartThread();
  void StopThread();
  void ThreadLoop();

  bool Rescan();
  bool CollectDevices(DeviceMap* devices) const;

  DeviceMap m_devices;
  mutable std::mutex m_devices_mutex;

  std::set<USBHost*> m_clients;
  std::mutex m_clients_mutex;

  Common::Flag m_thread_running;
  std::thread m_thread;
  std::mutex m_thread_start_stop_mutex;
  Common::WaitableFlag m_first_scan_complete_flag;

  LibusbUtils::Context m_context;
};

}  // namespace IOS::HLE
