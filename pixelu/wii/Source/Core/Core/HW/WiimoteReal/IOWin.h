// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "Common/SocketContext.h"
#include "Common/WindowsDevice.h"

#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/USBUtils.h"

namespace WiimoteReal
{
class WiimoteScannerWindows;

// A remote reached through a Windows HID interface (Microsoft Bluetooth stack or DolphinBar),
// using overlapped I/O.
class WiimoteWindows final : public Wiimote
{
  friend WiimoteScannerWindows;

public:
  WiimoteWindows(std::wstring hid_iface);
  ~WiimoteWindows() override;

  std::string GetId() const override;

protected:
  bool ConnectInternal() override;
  void DisconnectInternal() override;
  bool IsConnected() const override;
  void IOWakeup() override;
  int IORead(u8* buf) override;
  int IOWrite(u8 const* buf, size_t len) override;

private:
  // The byte count, 0 on failure or -1 when nothing was transferred (timeout or wakeup).
  int OverlappedRead(u8* data, DWORD size);
  int OverlappedWrite(const u8* data, DWORD size);

  const std::wstring m_hid_iface;

  HANDLE m_dev_handle{INVALID_HANDLE_VALUE};
  // Signalled by IOWakeup to end a pending read.
  HANDLE m_wakeup_event{INVALID_HANDLE_VALUE};

  OVERLAPPED m_hid_overlap_read{};
  OVERLAPPED m_hid_overlap_write{};

  std::atomic_bool m_is_connected{};
};

class WiimoteScannerWindows final : public WiimoteScannerBackend
{
public:
  // A HID interface that looks like a Wii device. Whether it is a balance board is known up
  // front when its Bluetooth name says so; otherwise it is found out (once) on connection.
  struct EnumeratedWiimoteInterface
  {
    std::wstring hid_iface;
    std::optional<bool> is_balance_board;
  };

  WiimoteScannerWindows();

  FindResults FindNewWiimotes() override;
  FindResults FindAttachedWiimotes() override;

  void Update() override;
  void RequestStopSearching() override {}

  // Pairs remotes in sync mode (red sync button), for the controller settings dialog.
  static void FindAndAuthenticateWiimotes();
  // Makes Windows forget every paired remote.
  static void RemoveRememberedWiimotes();

private:
  FindResults FindWiimoteHIDDevices();

  // Rebuilt when Windows reports a device change.
  std::vector<EnumeratedWiimoteInterface> m_wiimote_hid_interfaces;
  std::atomic_bool m_devices_changed{true};

  Common::DeviceChangeNotification m_device_change_notification;
};
}  // namespace WiimoteReal

#else

#include "Core/HW/WiimoteReal/IODummy.h"

namespace WiimoteReal
{
using WiimoteScannerWindows = WiimoteScannerDummy;
}

#endif
