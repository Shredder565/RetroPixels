// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USBScanner.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <thread>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Infinity.h"
#include "Core/IOS/USB/Emulated/LogitechMic.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/WiiSpeak.h"
#include "Core/IOS/USB/Host.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Time between two scans of the host's USB devices.
constexpr int RESCAN_INTERVAL_MS = 50;

void Insert(USBScanner::DeviceMap* devices, std::unique_ptr<USB::Device> device)
{
  const u64 id = device->GetId();
  (*devices)[id] = std::move(device);
}

// Emulated accessories are never offered during netplay, since their state is not synced.
bool WantsEmulated(const Config::Info<bool>& setting)
{
  return Config::Get(setting) && !NetPlay::IsNetPlayRunning();
}

void AddEmulatedAccessories(USBScanner::DeviceMap* devices)
{
  if (WantsEmulated(Config::MAIN_EMULATE_SKYLANDER_PORTAL))
    Insert(devices, std::make_unique<USB::SkylanderUSB>());
  if (WantsEmulated(Config::MAIN_EMULATE_INFINITY_BASE))
    Insert(devices, std::make_unique<USB::InfinityUSB>());
  if (WantsEmulated(Config::MAIN_EMULATE_WII_SPEAK))
    Insert(devices, std::make_unique<USB::WiiSpeak>());

  for (u8 slot = 0; slot < Config::EMULATED_LOGITECH_MIC_COUNT; ++slot)
  {
    if (WantsEmulated(Config::MAIN_EMULATE_LOGITECH_MIC[slot]))
      Insert(devices, std::make_unique<USB::LogitechMic>(slot));
  }
}

}  // namespace

USBScanner::~USBScanner()
{
  StopThread();
}

bool USBScanner::AddClient(USBHost* client)
{
  std::lock_guard start_stop_lock(m_thread_start_stop_mutex);

  bool was_first;
  bool inserted;
  {
    std::lock_guard clients_lock(m_clients_mutex);
    was_first = m_clients.empty();
    inserted = m_clients.insert(client).second;
  }

  if (was_first)
    StartThread();
  return inserted;
}

bool USBScanner::RemoveClient(USBHost* client)
{
  std::lock_guard start_stop_lock(m_thread_start_stop_mutex);

  bool erased;
  bool was_last;
  {
    std::lock_guard clients_lock(m_clients_mutex);
    const bool had_clients = !m_clients.empty();
    erased = m_clients.erase(client) != 0;
    was_last = had_clients && m_clients.empty();
  }

  // The scan thread takes m_clients_mutex itself, so it must not be held while joining.
  if (was_last)
    StopThread();
  return erased;
}

void USBScanner::WaitForFirstScan()
{
  if (!m_thread_running.IsSet())
    return;
  m_first_scan_complete_flag.Wait(true);
}

USBScanner::DeviceMap USBScanner::GetDevices() const
{
  std::lock_guard lock(m_devices_mutex);
  return m_devices;
}

void USBScanner::StartThread()
{
  if (!m_thread_running.TestAndSet())
    return;
  m_thread = std::thread(&USBScanner::ThreadLoop, this);
}

void USBScanner::StopThread()
{
  if (m_thread_running.TestAndClear())
    m_thread.join();
}

void USBScanner::ThreadLoop()
{
  Common::SetCurrentThreadName("USB Scan Thread");
  while (m_thread_running.IsSet())
  {
    if (Rescan())
      m_first_scan_complete_flag.Set(true);
    Common::SleepCurrentThread(RESCAN_INTERVAL_MS);
  }

  {
    std::lock_guard lock(m_devices_mutex);
    m_devices.clear();
  }
  m_first_scan_complete_flag.Set(false);
}

// Runs on the scan thread. Returns false when no device list could be built.
bool USBScanner::Rescan()
{
  DeviceMap fresh;
  if (!CollectDevices(&fresh))
    return false;

  std::lock_guard lock(m_devices_mutex);
  // Only the set of IDs matters; the hosts are only told when a device came or went.
  const bool same_ids = std::ranges::equal(std::views::keys(m_devices), std::views::keys(fresh));
  if (same_ids)
    return true;

  m_devices = std::move(fresh);
  std::lock_guard clients_lock(m_clients_mutex);
  for (USBHost* client : m_clients)
    client->OnDevicesChanged(m_devices);
  return true;
}

bool USBScanner::CollectDevices(DeviceMap* devices) const
{
  AddEmulatedAccessories(devices);
  return true;
}

}  // namespace IOS::HLE
