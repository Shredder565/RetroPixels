// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/DolphinDevice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/SettingsHandler.h"
#include "Common/Timer.h"
#include "Common/Version.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// IOCtlV numbers understood by /dev/dolphin.
constexpr u32 CMD_GET_ELAPSED_TIME = 0x01;
constexpr u32 CMD_GET_VERSION = 0x02;
constexpr u32 CMD_GET_SPEED_LIMIT = 0x03;
constexpr u32 CMD_SET_SPEED_LIMIT = 0x04;
constexpr u32 CMD_GET_CPU_SPEED = 0x05;
constexpr u32 CMD_GET_REAL_PRODUCTCODE = 0x06;
constexpr u32 CMD_DISCORD_SET_CLIENT = 0x07;
constexpr u32 CMD_DISCORD_SET_PRESENCE = 0x08;
constexpr u32 CMD_DISCORD_RESET = 0x09;
constexpr u32 CMD_GET_SYSTEM_TIME = 0x0A;

IPCReply Invalid()
{
  return IPCReply(IPC_EINVAL);
}

// True when the request has exactly one io vector of exactly `size` bytes (and no in vectors).
bool HasOutputWord(const IOCtlVRequest& request, u32 size)
{
  return request.HasNumberOfValidVectors(0, 1) && request.io_vectors[0].size == size;
}

// Clears the whole output vector and copies as much of `text` into it as fits.
void WriteTruncatedString(Memory::MemoryManager& memory, const IOCtlVRequest::IOVector& out,
                          std::string_view text, size_t length)
{
  memory.Memset(out.address, 0, out.size);
  memory.CopyToEmu(out.address, text.data(), length);
}

IPCReply GetVersion(Core::System& system, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return Invalid();

  const std::string& version = Common::GetScmDescStr();
  const auto& out = request.io_vectors[0];
  WriteTruncatedString(system.GetMemory(), out, version,
                       std::min<size_t>(out.size, version.size()));
  return IPCReply(IPC_SUCCESS);
}

IPCReply GetCPUSpeed(Core::System& system, const IOCtlVRequest& request)
{
  if (!HasOutputWord(request, 4))
    return Invalid();

  // The overclock factor is only looked at as on/off: the base clock is reported whenever it is
  // non-zero.
  const bool overclock_nonzero = system.GetCoreTiming().GetOverclock() != 0.0f;
  const float base = static_cast<float>(system.GetSystemTimers().GetTicksPerSecond());
  const u32 clock = static_cast<u32>(overclock_nonzero ? base * 1.0f : base * 0.0f);

  system.GetMemory().Write_U32(clock, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply GetSpeedLimit(Core::System& system, const IOCtlVRequest& request)
{
  if (!HasOutputWord(request, 4))
    return Invalid();

  const float percent = Config::Get(Config::MAIN_EMULATION_SPEED) * 100;
  system.GetMemory().Write_U32(static_cast<u32>(percent), request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply SetSpeedLimit(Core::System& system, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(1, 0) || request.in_vectors[0].size != 4)
    return Invalid();

  const u32 percent = system.GetMemory().Read_U32(request.in_vectors[0].address);
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, static_cast<float>(percent) / 100.0f);
  return IPCReply(IPC_SUCCESS);
}

// Reports the product code stored in the NAND backup's setting.txt (the real console's model).
IPCReply GetRealProductCode(Core::System& system, const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(0, 1))
    return Invalid();

  const std::string path = File::GetUserPath(D_BACKUP_IDX) + DIR_SEP + WII_SETTING;
  File::IOFile settings_file(path, "rb");
  if (!settings_file)
    return IPCReply(IPC_ENOENT);

  Common::SettingsBuffer raw;
  if (!settings_file.ReadBytes(raw.data(), raw.size()))
    return IPCReply(IPC_ENOENT);

  const std::string code = Common::SettingsReader(raw).GetValue("CODE");
  const auto& out = request.io_vectors[0];
  const size_t length = std::min<size_t>(out.size, code.length());
  if (length == 0)
    return IPCReply(IPC_ENOENT);

  WriteTruncatedString(system.GetMemory(), out, code, length);
  return IPCReply(IPC_SUCCESS);
}

std::string ReadInString(Memory::MemoryManager& memory, const IOCtlVRequest& request, size_t index)
{
  const auto& v = request.in_vectors[index];
  return memory.GetString(v.address, v.size);
}

IPCReply SetDiscordClient(Core::System& system, const IOCtlVRequest& request)
{
  if (!Config::Get(Config::MAIN_USE_DISCORD_PRESENCE))
    return IPCReply(IPC_EACCES);
  if (!request.HasNumberOfValidVectors(1, 0))
    return Invalid();

  Host_UpdateDiscordClientID(ReadInString(system.GetMemory(), request, 0));
  return IPCReply(IPC_SUCCESS);
}

// In vectors: details, state, large image key/text, small image key/text (strings), start and
// end timestamps (u64), party size and party maximum (u32).
IPCReply SetDiscordPresence(Core::System& system, const IOCtlVRequest& request)
{
  if (!Config::Get(Config::MAIN_USE_DISCORD_PRESENCE))
    return IPCReply(IPC_EACCES);
  if (!request.HasNumberOfValidVectors(10, 0))
    return Invalid();

  auto& memory = system.GetMemory();
  std::array<std::string, 6> text;
  for (size_t i = 0; i < text.size(); ++i)
    text[i] = ReadInString(memory, request, i);

  const auto start = static_cast<int64_t>(memory.Read_U64(request.in_vectors[6].address));
  const auto end = static_cast<int64_t>(memory.Read_U64(request.in_vectors[7].address));
  const auto party_size = static_cast<int>(memory.Read_U32(request.in_vectors[8].address));
  const auto party_max = static_cast<int>(memory.Read_U32(request.in_vectors[9].address));

  const bool accepted = Host_UpdateDiscordPresenceRaw(text[0], text[1], text[2], text[3], text[4],
                                                      text[5], start, end, party_size, party_max);
  return IPCReply(accepted ? IPC_SUCCESS : IPC_EACCES);
}

IPCReply ResetDiscord()
{
  if (!Config::Get(Config::MAIN_USE_DISCORD_PRESENCE))
    return IPCReply(IPC_EACCES);

  Host_UpdateDiscordClientID();
  return IPCReply(IPC_SUCCESS);
}
}  // namespace

// Milliseconds since the device was created, measured on the host clock. Homebrew uses it to
// tell whether emulation runs at full speed; a relative value is less likely to trip up code
// that cannot cope with large timestamps.
IPCReply DolphinDevice::GetElapsedTime(const IOCtlVRequest& request) const
{
  if (!HasOutputWord(request, 4))
    return Invalid();

  const auto elapsed = static_cast<u32>(m_timer.ElapsedMs());
  GetSystem().GetMemory().Write_U32(elapsed, request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

// The host's wall clock as milliseconds since the Unix epoch.
IPCReply DolphinDevice::GetSystemTime(const IOCtlVRequest& request) const
{
  if (!HasOutputWord(request, 8))
    return Invalid();

  using namespace std::chrono;
  const auto now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
  GetSystem().GetMemory().Write_U64(static_cast<u64>(now_ms.count()),
                                    request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

DolphinDevice::DolphinDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  m_timer.Start();
}

std::optional<IPCReply> DolphinDevice::IOCtlV(const IOCtlVRequest& request)
{
  // Everything here depends on the host, so it is off-limits during netplay and movies.
  if (Core::WantsDeterminism())
    return IPCReply(IPC_EACCES);

  auto& system = GetSystem();
  switch (request.request)
  {
  case CMD_GET_ELAPSED_TIME:
    return GetElapsedTime(request);
  case CMD_GET_VERSION:
    return GetVersion(system, request);
  case CMD_GET_SPEED_LIMIT:
    return GetSpeedLimit(system, request);
  case CMD_SET_SPEED_LIMIT:
    return SetSpeedLimit(system, request);
  case CMD_GET_CPU_SPEED:
    return GetCPUSpeed(system, request);
  case CMD_GET_REAL_PRODUCTCODE:
    return GetRealProductCode(system, request);
  case CMD_DISCORD_SET_CLIENT:
    return SetDiscordClient(system, request);
  case CMD_DISCORD_SET_PRESENCE:
    return SetDiscordPresence(system, request);
  case CMD_DISCORD_RESET:
    return ResetDiscord();
  case CMD_GET_SYSTEM_TIME:
    return GetSystemTime(request);
  default:
    return Invalid();
  }
}
}  // namespace IOS::HLE
