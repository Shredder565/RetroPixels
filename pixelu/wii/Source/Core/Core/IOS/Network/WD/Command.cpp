// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/WD/Command.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/Swap.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/MACUtils.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Bit n set = channel n may be used. Channel 0 never exists; DS play is limited to 1..13.
constexpr u16 USABLE_CHANNELS = 0x7ffe;
constexpr u16 USABLE_NITRO_CHANNELS = 0x3ffe;

// WD's channel pick: starting after `current`, step through the channels three at a time
// (wrapping at 16) and take the first enabled one.
u16 PickChannel(u16 enabled, u16 current)
{
  const u16 usable = enabled & USABLE_CHANNELS;
  u16 channel = current;
  for (int tries = 0; tries < 16; ++tries)
  {
    channel = static_cast<u16>((channel + 3) % 16);
    if ((usable >> channel) & 1)
      return channel;
  }
  // Nothing enabled: WD then answers with the (clear) bit of the last channel it looked at.
  return static_cast<u16>((usable >> channel) & 1);
}

// Fake scan result: one station with a counting BSSID and SSID "dolphin-emu" on channel 2.
// Layout of a station entry (60 bytes, big endian):
constexpr u32 BSS_ENTRY_SIZE = 60;
constexpr u32 BSS_LENGTH = 0;
constexpr u32 BSS_RSSI = 2;
constexpr u32 BSS_BSSID = 4;
constexpr u32 BSS_SSID_LENGTH = 10;
constexpr u32 BSS_SSID = 12;
constexpr u32 BSS_CHANNEL = 50;
constexpr std::string_view FAKE_SSID = "dolphin-emu";

IPCReply Fail(NetWDCommandDevice::ResultCode code)
{
  return IPCReply(static_cast<s32>(code));
}
}  // namespace

NetWDCommandDevice::Status NetWDCommandDevice::LinkUpStatus(WD::Mode mode)
{
  if (mode == WD::Mode::DSCommunications)
    return Status::ScanningForDS;
  if (mode == WD::Mode::AOSSAccessPointScan)
    return Status::ScanningForAOSSAccessPoint;
  return Status::Idle;
}

NetWDCommandDevice::NetWDCommandDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  // A console would take the DS channels from the MPCH entry of setting.txt.
  m_nitro_enabled_channels = USABLE_NITRO_CHANNELS;

  m_info.mac = Net::GetMACAddress();
  m_info.enabled_channels = 0xfffe;
  m_info.channel = static_cast<u8>(PickChannel(m_info.enabled_channels, 0));
  // NUL-terminated: WD prints it as a C string. The rest of the field stays zero.
  m_info.country_code[0] = 'U';
  m_info.country_code[1] = 'S';
  m_info.nitro_allowed_channels =
      static_cast<u16>(m_info.enabled_channels & m_nitro_enabled_channels & USABLE_NITRO_CHANNELS);
  m_info.initialised = true;
  // wl_version is left empty.
}

void NetWDCommandDevice::Update()
{
  Device::Update();
  AnswerPendingReceives();
  StepTowardsTargetStatus();
}

void NetWDCommandDevice::AnswerPendingReceives()
{
  // Nothing is ever received, so receive requests normally just wait. Once the owner has closed
  // its handle they are all released.
  if (!m_clear_all_requests.TestAndClear())
    return;

  const s32 result = m_ipc_owner_fd < 0 ? static_cast<s32>(ResultCode::InvalidFd) : 0;
  for (std::deque<u32>* queue : {&m_recv_notification_requests, &m_recv_frame_requests})
  {
    for (const u32 address : *queue)
    {
      INFO_LOG_FMT(IOS_NET, "Processed request {:08x} (result {:08x})", address, result);
      GetEmulationKernel().EnqueueIPCReply(Request{GetSystem(), address}, result);
    }
    queue->clear();
  }
}

void NetWDCommandDevice::StepTowardsTargetStatus()
{
  if (m_status == m_target_status)
    return;

  const Status before = m_status;
  if (before == Status::Idle)
  {
    // Going up: the scan the target stands for starts straight away.
    m_status = m_target_status;
  }
  else if (before == Status::ScanningForDS)
  {
    m_status = Status::Idle;
  }
  // Leaving the AOSS scan would need a driver reset, which is not emulated: it stays.

  INFO_LOG_FMT(IOS_NET, "WD status change: {} -> {} (target {})", before, m_status,
               m_target_status);
}

void NetWDCommandDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  // The order is the savestate layout.
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };
  save(m_ipc_owner_fd, m_mode, m_buffer_flags, m_status, m_target_status,
       m_nitro_enabled_channels, m_info, m_recv_frame_requests, m_recv_notification_requests);
}

std::optional<IPCReply> NetWDCommandDevice::Open(const OpenRequest& request)
{
  // Only the first handle chooses the mode; later ones just open the device.
  if (m_ipc_owner_fd < 0)
  {
    const u32 flags = static_cast<u32>(request.flags);
    const WD::Mode mode = static_cast<WD::Mode>(flags & 0xffff);
    const u32 buffer_flags = flags & 0x7fff0000;
    INFO_LOG_FMT(IOS_NET, "Opening with mode={} buffer_flags={:08x}", mode, buffer_flags);

    if (mode != WD::Mode::DSCommunications && mode != WD::Mode::AOSSAccessPointScan)
    {
      ERROR_LOG_FMT(IOS_NET, "Unsupported WD operating mode: {}", mode);
      DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesUncommonWDMode);
      return Fail(ResultCode::UnavailableCommand);
    }

    if (m_target_status == Status::Idle)
    {
      m_mode = mode;
      m_ipc_owner_fd = static_cast<s32>(request.fd);
      m_buffer_flags = buffer_flags;
    }
  }

  return Device::Open(request);
}

std::optional<IPCReply> NetWDCommandDevice::Close(u32 fd)
{
  if (m_ipc_owner_fd < 0 || fd != static_cast<u32>(m_ipc_owner_fd))
  {
    ERROR_LOG_FMT(IOS_NET, "Invalid close attempt.");
    return Fail(ResultCode::InvalidFd);
  }

  // Closing the owner handle drops the link and releases every waiting receive.
  m_status = Status::Idle;
  m_target_status = Status::Idle;
  m_ipc_owner_fd = -1;
  m_clear_all_requests.Set();
  return Device::Close(fd);
}

IPCReply NetWDCommandDevice::SetLinkState(const IOCtlVRequest& request)
{
  const IOCtlVRequest::IOVector* const vector = request.GetVector(0);
  if (vector == nullptr || vector->address == 0)
    return Fail(ResultCode::IllegalParameter);

  const u32 up = GetSystem().GetMemory().Read_U32(vector->address);
  INFO_LOG_FMT(IOS_NET, "WD_SetLinkState(state={}, mode={})", up, m_mode);
  if (up > 1)
    return Fail(ResultCode::IllegalParameter);
  if (!WD::IsValidMode(m_mode))
    return Fail(ResultCode::UnavailableCommand);

  if (up == 0)
  {
    m_target_status = Status::Idle;
    return IPCReply(IPC_SUCCESS);
  }

  const Status wanted = LinkUpStatus(m_mode);
  // A link cannot come up without any channel to use.
  if (m_status != wanted && m_info.enabled_channels == 0)
    return Fail(ResultCode::UnavailableCommand);
  m_target_status = wanted;
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetWDCommandDevice::GetLinkState() const
{
  if (!WD::IsValidMode(m_mode))
    return Fail(ResultCode::UnavailableCommand);
  // Despite the name, the answer is only whether the link is fully up (1) or not (0).
  return IPCReply(m_status == LinkUpStatus(m_mode) ? 1 : 0);
}

IPCReply NetWDCommandDevice::Disassociate(const IOCtlVRequest& request) const
{
  const IOCtlVRequest::IOVector* const vector = request.GetVector(0);
  if (vector == nullptr || vector->address == 0)
    return Fail(ResultCode::IllegalParameter);

  Common::MACAddress station;
  GetSystem().GetMemory().CopyFromEmu(station.data(), vector->address, station.size());
  INFO_LOG_FMT(IOS_NET, "WD_Disassociate({})", Common::MacAddressToString(station));

  const bool mode_allows =
      m_mode == WD::Mode::DSCommunications || m_mode == WD::Mode::Unknown5 ||
      m_mode == WD::Mode::Unknown6;
  if (!mode_allows || m_status != LinkUpStatus(m_mode))
  {
    ERROR_LOG_FMT(IOS_NET, "WD_Disassociate not possible (mode {}, status {})", m_mode, m_status);
    return Fail(ResultCode::UnavailableCommand);
  }

  // No station is ever associated, so every address is unknown.
  return Fail(ResultCode::IllegalParameter);
}

IPCReply NetWDCommandDevice::GetInfo(const IOCtlVRequest& request) const
{
  const IOCtlVRequest::IOVector* const vector = request.GetVector(0);
  if (vector == nullptr || vector->address == 0)
    return Fail(ResultCode::IllegalParameter);

  GetSystem().GetMemory().CopyToEmu(vector->address, &m_info, sizeof(m_info));
  return IPCReply(IPC_SUCCESS);
}

void NetWDCommandDevice::WriteScanResult(const IOCtlVRequest& request) const
{
  // The scan parameters (in vector 0) are ignored. The reply is a station count followed by
  // the stations.
  std::array<u8, 2 + BSS_ENTRY_SIZE> reply{};
  const auto put16 = [&reply](u32 offset, u16 value) {
    reply[offset] = static_cast<u8>(value >> 8);
    reply[offset + 1] = static_cast<u8>(value);
  };

  put16(0, 1);
  constexpr u32 entry = 2;
  put16(entry + BSS_LENGTH, static_cast<u16>(BSS_ENTRY_SIZE));
  put16(entry + BSS_RSSI, 0xffff);
  for (u32 i = 0; i < 6; ++i)
    reply[entry + BSS_BSSID + i] = static_cast<u8>(i);
  put16(entry + BSS_SSID_LENGTH, static_cast<u16>(FAKE_SSID.size()));
  std::ranges::copy(FAKE_SSID, reply.begin() + entry + BSS_SSID);
  put16(entry + BSS_CHANNEL, 2);

  GetSystem().GetMemory().CopyToEmu(request.io_vectors.at(0).address, reply.data(), reply.size());
}

std::optional<IPCReply> NetWDCommandDevice::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case IOCTLV_WD_INVALID:
    return Fail(ResultCode::UnavailableCommand);
  case IOCTLV_WD_GET_MODE:
    return IPCReply(static_cast<s32>(m_mode));
  case IOCTLV_WD_SET_LINKSTATE:
    return SetLinkState(request);
  case IOCTLV_WD_GET_LINKSTATE:
    INFO_LOG_FMT(IOS_NET, "WD_GetLinkState(status={}, mode={})", m_status, m_mode);
    return GetLinkState();
  case IOCTLV_WD_DISASSOC:
    return Disassociate(request);
  case IOCTLV_WD_GET_INFO:
    return GetInfo(request);

  case IOCTLV_WD_SCAN:
    WriteScanResult(request);
    return IPCReply(IPC_SUCCESS);

  case IOCTLV_WD_RECV_FRAME:
    m_recv_frame_requests.push_back(request.address);
    return std::nullopt;
  case IOCTLV_WD_RECV_NOTIFICATION:
    m_recv_notification_requests.push_back(request.address);
    return std::nullopt;

  default:
    // Configuration, beacons, sending frames, WL calls and so on are accepted and ignored.
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesWDUnimplementedIOCtl);
    request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET,
                 Common::Log::LogLevel::LWARNING);
    return IPCReply(IPC_SUCCESS);
  }
}
}  // namespace IOS::HLE
