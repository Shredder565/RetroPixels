// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef HAVE_HIDAPI

#include <string>

#include <hidapi.h>

#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
// A remote reached through a hidapi device path.
class WiimoteHidapi final : public Wiimote
{
public:
  explicit WiimoteHidapi(std::string device_path);
  ~WiimoteHidapi() override;

  std::string GetId() const override { return m_device_path; }

protected:
  bool ConnectInternal() override;
  void DisconnectInternal() override;
  bool IsConnected() const override;
  // Reads time out on their own, so there is nothing to wake.
  void IOWakeup() override {}
  int IORead(u8* buf) override;
  int IOWrite(const u8* buf, size_t len) override;

private:
  const std::string m_device_path;
  hid_device* m_handle = nullptr;
};

// Enumerates HID devices; there is no Bluetooth inquiry through hidapi.
class WiimoteScannerHidapi final : public WiimoteScannerBackend
{
public:
  WiimoteScannerHidapi();
  ~WiimoteScannerHidapi() override;

  FindResults FindAttachedWiimotes() override;

  void Update() override {}
  void RequestStopSearching() override {}
};
}  // namespace WiimoteReal

#else

#include "Core/HW/WiimoteReal/IODummy.h"

namespace WiimoteReal
{
using WiimoteScannerHidapi = WiimoteScannerDummy;
}

#endif
