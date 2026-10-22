// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/GeckoCode.h"

#include <mutex>
#include <string>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Gecko
{
namespace
{
// One GCT entry is two words.
constexpr u32 kLineBytes = 8;

// Saved in savestates (as a 32-bit value), so the numbers must not change.
enum class HandlerState
{
  NotInstalled = 0,
  Installed = 1,
  InstallFailed = 2,
};

std::mutex s_mutex;
HandlerState s_handler = HandlerState::NotInstalled;
std::vector<GeckoCode> s_active;
std::vector<GeckoCode> s_netplay_synced;

void Write32(const Core::CPUThreadGuard& guard, u32 value, u32 address)
{
  PowerPC::MMU::HostWrite<u32>(guard, value, address);
}

// Copies codehandler.bin to game RAM and adapts it to the console: the handler addresses the
// hardware registers through 0xCC00xxxx on a GameCube and 0xCD00xxxx on a Wii, written as the
// upper half of a "lis" instruction (0x3F00CC00 / 0x3F00CD00).
bool CopyHandler(const Core::CPUThreadGuard& guard, const std::string& handler)
{
  for (u32 offset = 0; offset < handler.size(); ++offset)
  {
    PowerPC::MMU::HostWrite<u8>(guard, static_cast<u8>(handler[offset]),
                                INSTALLER_BASE_ADDRESS + offset);
  }

  const u32 hardware_page = guard.GetSystem().IsWii() ? 0xCD : 0xCC;
  const u32 wrong_lis = 0x3F000000u | ((hardware_page ^ 1) << 8);
  const u32 right_lis = 0x3F000000u | (hardware_page << 8);
  for (u32 offset = 0; offset < handler.size(); offset += 4)
  {
    const u32 address = INSTALLER_BASE_ADDRESS + offset;
    if (PowerPC::MMU::HostRead<u32>(guard, address) != wrong_lis)
      continue;
    NOTICE_LOG_FMT(ACTIONREPLAY, "Patching MMIO access at {:08x}", address);
    Write32(guard, right_lis, address);
  }
  return true;
}

// Builds the GCT right after the handler: a start marker, the code lines of every active code
// that still fits, and an end code. Codes that don't fit are left out.
void WriteCodeTable(const Core::CPUThreadGuard& guard, u32 table_address)
{
  Write32(guard, 0x00D0C0DE, table_address);
  Write32(guard, 0x00D0C0DE, table_address + 4);

  const u32 first_line = table_address + kLineBytes;
  // Room for the end code is kept free.
  const u32 limit = INSTALLER_END_ADDRESS - kLineBytes;
  u32 cursor = first_line;

  for (const GeckoCode& code : s_active)
  {
    const std::size_t bytes = code.codes.size() * kLineBytes;
    if (cursor + bytes > limit)
    {
      NOTICE_LOG_FMT(ACTIONREPLAY,
                     "Too many GeckoCodes! Ran out of storage space in Game RAM. Could "
                     "not write: \"{}\". Need {} bytes, only {} remain.",
                     code.name, bytes, limit - cursor);
      continue;
    }
    for (const GeckoCode::Code& line : code.codes)
    {
      Write32(guard, line.address, cursor);
      Write32(guard, line.data, cursor + 4);
      cursor += kLineBytes;
    }
  }

  WARN_LOG_FMT(ACTIONREPLAY, "GeckoCodes: Using {} of {} bytes", cursor - first_line,
               limit - first_line);

  Write32(guard, 0xF0000000, cursor);
  Write32(guard, 0x00000000, cursor + 4);
}

// Caller holds s_mutex.
HandlerState Install(const Core::CPUThreadGuard& guard)
{
  std::string handler;
  if (!File::ReadFileToString(File::GetSysDirectory() + GECKO_CODE_HANDLER, handler))
  {
    ERROR_LOG_FMT(ACTIONREPLAY,
                  "Could not enable cheats because " GECKO_CODE_HANDLER " was missing.");
    return HandlerState::InstallFailed;
  }

  // The handler, plus at least the table's start marker, must fit in front of the end address.
  if (handler.size() > INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS - kLineBytes)
  {
    ERROR_LOG_FMT(ACTIONREPLAY, GECKO_CODE_HANDLER " is too big. The file may be corrupt.");
    return HandlerState::InstallFailed;
  }

  CopyHandler(guard, handler);

  // The handler file ends with an 8-byte placeholder that the table's start marker replaces.
  const u32 table_address = INSTALLER_BASE_ADDRESS + static_cast<u32>(handler.size()) - kLineBytes;

  Write32(guard, MAGIC_GAMEID, INSTALLER_BASE_ADDRESS);
  WriteCodeTable(guard, table_address);
  Write32(guard, 0, HLE_TRAMPOLINE_ADDRESS);

  // The handler's "codes enabled" flag.
  PowerPC::MMU::HostWrite<u8>(guard, 1, INSTALLER_BASE_ADDRESS + 7);

  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  for (u32 offset = 0; offset < INSTALLER_END_ADDRESS - INSTALLER_BASE_ADDRESS; offset += 32)
  {
    ppc_state.iCache.Invalidate(system.GetMemory(), system.GetJitInterface(),
                                INSTALLER_BASE_ADDRESS + offset);
  }
  Host_JitCacheInvalidation();
  return HandlerState::Installed;
}

// Makes the CPU "call" the handler at the next instruction. The handler is entered from an
// arbitrary point in the game, so a stack frame is built by hand that the HLE return trampoline
// (HLE_Misc::GeckoReturnTrampoline) uses to restore the game's state:
//   SP+0   back chain (SP + 8)
//   SP+4   free, the handler saves LR here
//   SP+8   back chain to the game's own frame
//   SP+12  the interrupted PC
//   SP+16  LR
//   SP+20  CR
//   SP+24  FPR0-13 (volatile), each as PS0 then PS1
// It sits below the game's stack red zone and is 16-byte aligned.
void EnterHandler(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  constexpr u32 kRedZone = 256;
  constexpr u32 kSavedFrame = 16 + 14 * 2 * sizeof(u64);
  constexpr u32 kHandlerFrame = 8;

  const u32 game_sp = ppc_state.gpr[1];
  const u32 sp = (game_sp - kRedZone - kSavedFrame - kHandlerFrame) & ~u32{0xF};
  ppc_state.gpr[1] = sp;

  Write32(guard, sp + 8, sp);
  Write32(guard, game_sp, sp + 8);
  Write32(guard, ppc_state.pc, sp + 12);
  Write32(guard, LR(ppc_state), sp + 16);
  Write32(guard, ppc_state.cr.Get(), sp + 20);

  u32 slot = sp + 24;
  for (u32 reg = 0; reg < 14; ++reg)
  {
    PowerPC::MMU::HostWrite<u64>(guard, ppc_state.ps[reg].PS0AsU64(), slot);
    PowerPC::MMU::HostWrite<u64>(guard, ppc_state.ps[reg].PS1AsU64(), slot + 8);
    slot += 16;
  }

  DEBUG_LOG_FMT(ACTIONREPLAY,
                "GeckoCodes: Initiating phantom branch-and-link. "
                "PC = {:#010x}, SP = {:#010x}, SFP = {:#010x}",
                ppc_state.pc, sp, game_sp);

  LR(ppc_state) = HLE_TRAMPOLINE_ADDRESS;
  ppc_state.pc = ENTRY_POINT;
  ppc_state.npc = ENTRY_POINT;
}

template <typename Predicate>
void ReplaceActive(std::span<const GeckoCode> codes, Predicate&& keep)
{
  s_active.clear();
  if (Config::AreCheatsEnabled())
  {
    s_active.reserve(codes.size());
    for (const GeckoCode& code : codes)
    {
      if (keep(code))
        s_active.push_back(code);
    }
  }
  s_active.shrink_to_fit();
  // The handler has to be installed again with the new table.
  s_handler = HandlerState::NotInstalled;
}
}  // namespace

bool operator==(const GeckoCode& lhs, const GeckoCode& rhs)
{
  return lhs.codes == rhs.codes;
}

bool operator==(const GeckoCode::Code& lhs, const GeckoCode::Code& rhs)
{
  return lhs.address == rhs.address && lhs.data == rhs.data;
}

size_t CountEnabledCodes()
{
  if (!Config::AreCheatsEnabled())
    return 0;

  std::lock_guard lock(s_mutex);
  return s_active.size();
}

void SetActiveCodes(std::span<const GeckoCode> gcodes, const std::string& game_id, u16 revision)
{
  std::lock_guard lock(s_mutex);
  ReplaceActive(gcodes, [&](const GeckoCode& code) {
    return AchievementManager::GetInstance().ShouldGeckoCodeBeActivated(code, game_id, revision);
  });
}

void SetSyncedCodesAsActive()
{
  s_active = s_netplay_synced;
}

void UpdateSyncedCodes(std::span<const GeckoCode> gcodes)
{
  s_netplay_synced.clear();
  for (const GeckoCode& code : gcodes)
  {
    if (code.enabled)
      s_netplay_synced.push_back(code);
  }
  s_netplay_synced.shrink_to_fit();
}

std::vector<GeckoCode> SetAndReturnActiveCodes(std::span<const GeckoCode> gcodes)
{
  std::lock_guard lock(s_mutex);
  ReplaceActive(gcodes, [](const GeckoCode& code) { return code.enabled; });
  return s_active;
}

// The handler is part of the game's RAM, so whether it was installed is part of the state: the PC
// may be inside it, and reinstalling would undo self-modifying codes.
void DoState(PointerWrap& p)
{
  std::lock_guard lock(s_mutex);
  p.Do(s_handler);
}

void Shutdown()
{
  std::lock_guard lock(s_mutex);
  s_active.clear();
  s_handler = HandlerState::NotInstalled;
}

void RunCodeHandler(const Core::CPUThreadGuard& guard)
{
  if (!Config::AreCheatsEnabled())
    return;

  {
    // Released before running the handler: memory writes can raise alerts that wait on the GUI.
    std::lock_guard lock(s_mutex);
    if (s_handler != HandlerState::Installed)
    {
      // A failed install (missing or broken file) is not retried every frame.
      if (s_active.empty() || s_handler == HandlerState::InstallFailed)
        return;
      s_handler = Install(guard);
      if (s_handler != HandlerState::Installed)
        return;
    }
  }

  EnterHandler(guard);
}

}  // namespace Gecko
