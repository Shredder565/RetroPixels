// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/NetKDTime.h"

#include <ctime>
#include <string>

#include <fmt/chrono.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
enum : u32
{
  IOCTL_NW24_GET_UNIVERSAL_TIME = 0x14,
  IOCTL_NW24_SET_UNIVERSAL_TIME = 0x15,
  IOCTL_NW24_UNIMPLEMENTED = 0x16,
  IOCTL_NW24_SET_RTC_COUNTER = 0x17,
  IOCTL_NW24_GET_TIME_DIFF = 0x18,
};

constexpr s32 RESULT_UNIMPLEMENTED = -9;
}  // namespace

NetKDTimeDevice::NetKDTimeDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

NetKDTimeDevice::~NetKDTimeDevice() = default;

std::optional<IPCReply> NetKDTimeDevice::IOCtl(const IOCtlRequest& request)
{
  Memory::MemoryManager& memory = GetSystem().GetMemory();
  s32 result = 0;

  // Layouts: in = value (u64 or u32) followed by a "write misc.bin" flag, which is ignored;
  // out = a result word followed by the u64 answer.
  switch (request.request)
  {
  case IOCTL_NW24_GET_UNIVERSAL_TIME:
  {
    const u64 now = GetAdjustedUTC();
    memory.Write_U64(now, request.buffer_out + 4);
    INFO_LOG_FMT(IOS_WC24, "GET_UNIVERSAL_TIME -> {}", now);
    break;
  }
  case IOCTL_NW24_SET_UNIVERSAL_TIME:
  {
    const u64 wanted = memory.Read_U64(request.buffer_in);
    SetAdjustedUTC(wanted);
    [[maybe_unused]] const u32 misc = memory.Read_U32(request.buffer_in + 8);
    INFO_LOG_FMT(IOS_WC24, "SET_UNIVERSAL_TIME({}, misc={})", wanted, misc);
    break;
  }
  case IOCTL_NW24_SET_RTC_COUNTER:
  {
    m_rtc_counter = memory.Read_U32(request.buffer_in);
    [[maybe_unused]] const u32 misc = memory.Read_U32(request.buffer_in + 4);
    INFO_LOG_FMT(IOS_WC24, "SET_RTC_COUNTER({}, misc={})", m_rtc_counter, misc);
    break;
  }
  case IOCTL_NW24_GET_TIME_DIFF:
  {
    const u64 difference = GetAdjustedUTC() - m_rtc_counter;
    memory.Write_U64(difference, request.buffer_out + 4);
    INFO_LOG_FMT(IOS_WC24, "GET_TIME_DIFF -> {}", difference);
    break;
  }
  case IOCTL_NW24_UNIMPLEMENTED:
    result = RESULT_UNIMPLEMENTED;
    break;
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_WC24);
    break;
  }

  // The result word is always 0, whatever the command.
  memory.Write_U32(0, request.buffer_out);
  return IPCReply(result);
}

NetKDTimeDevice::ClockReading NetKDTimeDevice::ReadClock() const
{
  using ExpansionInterface::CEXIIPL;
  const time_t rtc = CEXIIPL::GetEmulatedTime(GetSystem(), CEXIIPL::UNIX_EPOCH);

  // The UTC calendar time is turned back into seconds as if it were local time; mktime then
  // also says whether that local time falls in daylight saving time.
  std::tm calendar = fmt::gmtime(rtc);
  const u32 seconds = static_cast<u32>(std::mktime(&calendar));
  return {seconds, calendar.tm_isdst == 1 ? 3600 : 0};
}

u64 NetKDTimeDevice::GetAdjustedUTC() const
{
  const ClockReading clock = ReadClock();
  return static_cast<u64>(static_cast<s64>(clock.seconds) + m_offset - clock.dst_correction);
}

void NetKDTimeDevice::SetAdjustedUTC(u64 wii_utc)
{
  const ClockReading clock = ReadClock();
  // Unsigned arithmetic on purpose: the offset may be negative and wraps through u64.
  const u64 offset = u64{clock.seconds} - wii_utc - static_cast<u64>(clock.dst_correction);
  m_offset = static_cast<s64>(offset);
}
}  // namespace IOS::HLE
