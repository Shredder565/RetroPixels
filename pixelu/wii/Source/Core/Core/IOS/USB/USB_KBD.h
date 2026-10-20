// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <queue>
#include <string>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

namespace IOS::HLE
{
// /dev/usb/kbd: a USB keyboard fed from the host keyboard (Windows only for now). Each ioctl
// hands the guest the oldest queued report, if there is one.
class USB_KBD : public EmulationDevice
{
public:
  USB_KBD(EmulationKernel& ios, const std::string& device_name);

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Write(const ReadWriteRequest& request) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  void Update() override;

private:
  enum class ReportKind : u32
  {
    Connected = 0,
    Disconnected = 1,
    KeyChange = 2,
  };

  using NewKeys = std::array<u8, 6>;

  // What the guest receives: 16 bytes, the kind stored big endian.
#pragma pack(push, 1)
  struct Report
  {
    u32 kind_be = 0;
    u32 reserved = 0;
    u8 modifiers = 0;
    u8 reserved2 = 0;
    NewKeys keys{};
  };
#pragma pack(pop)
  static_assert(sizeof(Report) == 16);
  static_assert(std::is_trivially_copyable_v<Report>);

  static Report MakeReport(ReportKind kind, u8 modifiers, const NewKeys& keys);
  static bool IsHostKeyDown(int virtual_key);
  static u8 ReadHostModifiers();

  std::queue<Report> m_reports;

  // Host key state as of the last update, indexed by virtual key code.
  std::array<bool, 256> m_was_down{};
  u8 m_last_modifiers = 0;

  // 0 = QWERTY, 1 = AZERTY ("USB Keyboard" / "Layout" in the main ini file).
  int m_layout = 0;
};
}  // namespace IOS::HLE
