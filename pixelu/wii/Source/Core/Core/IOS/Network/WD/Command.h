// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <deque>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/Flag.h"
#include "Common/Network.h"
#include "Common/Swap.h"
#include "Core/IOS/Device.h"

namespace IOS::HLE::WD
{
// Operating mode, chosen with the open flags. Only 1 and 3 are known to be used (by games for
// DS communication, and by the system menu for AOSS set-up).
enum class Mode
{
  NotInitialized = 0,
  DSCommunications = 1,
  Unknown2 = 2,
  AOSSAccessPointScan = 3,
  Unknown4 = 4,
  Unknown5 = 5,
  Unknown6 = 6,
};

constexpr bool IsValidMode(Mode mode)
{
  return mode >= Mode::DSCommunications && mode <= Mode::Unknown6;
}
}  // namespace IOS::HLE::WD

namespace IOS::HLE
{
// /dev/net/wd/command: the wireless driver for local (DS and AOSS) communication. No radio is
// emulated: links come up at once, scans find one fake station and no frames ever arrive.
class NetWDCommandDevice : public EmulationDevice
{
public:
  enum class ResultCode : u32
  {
    InvalidFd = 0x80008000,
    IllegalParameter = 0x80008001,
    UnavailableCommand = 0x80008002,
    DriverError = 0x80008003,
  };

  NetWDCommandDevice(EmulationKernel& ios, const std::string& device_name);

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;
  void Update() override;
  bool IsOpened() const override { return true; }
  void DoState(PointerWrap& p) override;

private:
  enum
  {
    IOCTLV_WD_INVALID = 0x1000,
    IOCTLV_WD_GET_MODE = 0x1001,
    IOCTLV_WD_SET_LINKSTATE = 0x1002,
    IOCTLV_WD_GET_LINKSTATE = 0x1003,
    IOCTLV_WD_SET_CONFIG = 0x1004,
    IOCTLV_WD_GET_CONFIG = 0x1005,
    IOCTLV_WD_CHANGE_BEACON = 0x1006,
    IOCTLV_WD_DISASSOC = 0x1007,
    IOCTLV_WD_MP_SEND_FRAME = 0x1008,
    IOCTLV_WD_SEND_FRAME = 0x1009,
    IOCTLV_WD_SCAN = 0x100a,
    IOCTLV_WD_MEASURE_CHANNEL = 0x100b,
    IOCTLV_WD_CALL_WL = 0x100c,
    IOCTLV_WD_GET_LASTERROR = 0x100d,
    IOCTLV_WD_GET_INFO = 0x100e,
    IOCTLV_WD_CHANGE_GAMEINFO = 0x100f,
    IOCTLV_WD_CHANGE_VTSF = 0x1010,
    IOCTLV_WD_RECV_FRAME = 0x8000,
    IOCTLV_WD_RECV_NOTIFICATION = 0x8001,
  };

  // Driver information returned by GET_INFO (and saved raw in savestates).
#pragma pack(push, 1)
  struct Info
  {
    Common::MACAddress mac{};
    Common::BigEndianValue<u16> enabled_channels{};
    Common::BigEndianValue<u16> nitro_allowed_channels{};
    std::array<char, 4> country_code{};
    u8 channel{};
    bool initialised{};
    std::array<char, 0x80> wl_version{};
  };
#pragma pack(pop)
  static_assert(sizeof(Info) == 0x90);

  enum class Status
  {
    Idle,
    ScanningForAOSSAccessPoint,
    ScanningForDS,
  };
  friend struct fmt::formatter<IOS::HLE::NetWDCommandDevice::Status>;

  // The status a link in the given mode is brought up to.
  static Status LinkUpStatus(WD::Mode mode);

  void AnswerPendingReceives();
  void StepTowardsTargetStatus();

  IPCReply SetLinkState(const IOCtlVRequest& request);
  IPCReply GetLinkState() const;
  IPCReply Disassociate(const IOCtlVRequest& request) const;
  IPCReply GetInfo(const IOCtlVRequest& request) const;
  void WriteScanResult(const IOCtlVRequest& request) const;

  // fd of the handle that set the mode, or -1.
  s32 m_ipc_owner_fd = -1;
  WD::Mode m_mode = WD::Mode::NotInitialized;
  u32 m_buffer_flags{};

  Status m_status = Status::Idle;
  Status m_target_status = Status::Idle;

  u16 m_nitro_enabled_channels{};
  Info m_info;

  // Set on close: every pending receive request is then answered on the next update.
  Common::Flag m_clear_all_requests;
  std::deque<u32> m_recv_frame_requests;
  std::deque<u32> m_recv_notification_requests;
};
}  // namespace IOS::HLE

template <>
struct fmt::formatter<IOS::HLE::WD::Mode> : EnumFormatter<IOS::HLE::WD::Mode::Unknown6>
{
  static constexpr array_type names{
      "Not initialized", "DS Communications", "Unknown 2", "AOSS Access Point Scan",
      "Unknown 4",       "Unknown 5",         "Unknown 6",
  };
  constexpr formatter() : EnumFormatter(names) {}
};
template <>
struct fmt::formatter<IOS::HLE::NetWDCommandDevice::Status>
    : EnumFormatter<IOS::HLE::NetWDCommandDevice::Status::ScanningForDS>
{
  static constexpr array_type names{"Idle", "Scanning for AOSS Access Point", "Scanning for DS"};
  constexpr formatter() : EnumFormatter(names) {}
};
