// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PatchEngine.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/Debug/MemoryPatches.h"
#include "Common/IniFile.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/ActionReplay.h"
#include "Core/CheatCodes.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace PatchEngine
{
namespace
{
// Indexed by PatchType.
constexpr std::array<const char*, 3> kTypeNames{"byte", "word", "dword"};

std::vector<Patch> s_ini_patches;

std::mutex s_debugger_patches_mutex;
std::vector<std::size_t> s_debugger_patches;

std::optional<PatchType> TypeFromName(std::string_view name)
{
  for (std::size_t i = 0; i < kTypeNames.size(); ++i)
  {
    if (name == kTypeNames[i])
      return static_cast<PatchType>(i);
  }
  return std::nullopt;
}

// Writes `entry` if it is unconditional or memory currently holds its comparand (compared at the
// entry's width).
template <typename T>
void ApplyEntry(const Core::CPUThreadGuard& guard, const PatchEntry& entry)
{
  const T value = static_cast<T>(entry.value);
  if (entry.conditional &&
      PowerPC::MMU::HostRead<T>(guard, entry.address) != static_cast<T>(entry.comparand))
  {
    return;
  }
  ApplyMemoryPatch<T>(guard, value, entry.address);
}

void ApplyIniPatches(const Core::CPUThreadGuard& guard, std::span<const Patch> patches)
{
  for (const Patch& patch : patches)
  {
    if (!patch.enabled)
      continue;
    for (const PatchEntry& entry : patch.entries)
    {
      if (entry.type == PatchType::Patch8Bit)
        ApplyEntry<u8>(guard, entry);
      else if (entry.type == PatchType::Patch16Bit)
        ApplyEntry<u16>(guard, entry);
      else if (entry.type == PatchType::Patch32Bit)
        ApplyEntry<u32>(guard, entry);
    }
  }
}

void ApplyDebuggerPatches(const Core::CPUThreadGuard& guard)
{
  std::lock_guard lock(s_debugger_patches_mutex);
  auto& debug_interface = guard.GetSystem().GetPowerPC().GetDebugInterface();
  for (const std::size_t index : s_debugger_patches)
    debug_interface.ApplyExistingPatch(guard, index);
}

// Patches are applied from the VI interrupt, which can land while the game is in an exception
// handler or otherwise not running normal code. As a guess at "normal code", the stack must have
// at least two sane frames and the saved return address must point at an instruction.
// Expects address translation to be on.
bool LooksLikeNormalCode(const Core::CPUThreadGuard& guard)
{
  const auto& ppc_state = guard.GetSystem().GetPPCState();
  DEBUG_ASSERT(ppc_state.msr.DR && ppc_state.msr.IR);

  const u32 stack = ppc_state.gpr[1];
  if (!PowerPC::MMU::HostIsRAMAddress(guard, stack))
    return false;

  // The back chain must point further up the stack, at readable memory.
  const u32 caller_frame = PowerPC::MMU::HostRead<u32>(guard, stack);
  if (caller_frame <= stack)
    return false;
  if (!PowerPC::MMU::HostIsRAMAddress(guard, caller_frame) ||
      !PowerPC::MMU::HostIsRAMAddress(guard, caller_frame + 4))
  {
    return false;
  }

  // The caller frame's LR save word.
  const u32 return_address = PowerPC::MMU::HostRead<u32>(guard, caller_frame + 4);
  if (!PowerPC::MMU::HostIsInstructionRAMAddress(guard, return_address))
    return false;
  return PowerPC::MMU::HostRead_Instruction(guard, return_address) != 0;
}

void LogRetryLater(const PowerPC::PowerPCState& ppc_state)
{
  DEBUG_LOG_FMT(ACTIONREPLAY,
                "Need to retry later. CPU configuration is currently incorrect. PC = {:#010x}, "
                "MSR = {:#010x}",
                ppc_state.pc, ppc_state.msr.Hex);
}

bool TranslationEnabled(const PowerPC::PowerPCState& ppc_state)
{
  return ppc_state.msr.DR && ppc_state.msr.IR;
}

// Applies the ini patches right after (re)loading them, so they don't wait for the next frame.
void ApplyAfterReload(Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);

  const auto& ppc_state = system.GetPPCState();
  if (!TranslationEnabled(ppc_state))
  {
    LogRetryLater(ppc_state);
    return;
  }
  ApplyIniPatches(guard, s_ini_patches);
}
}  // namespace

const char* PatchTypeAsString(PatchType type)
{
  return kTypeNames.at(static_cast<std::size_t>(type));
}

std::optional<PatchEntry> DeserializeLine(std::string line)
{
  // "address=type:value" is accepted too.
  if (const std::size_t equals = line.find('='); equals != std::string::npos)
    line[equals] = ':';

  const std::vector<std::string> fields = SplitString(line, ':');
  if (fields.size() < 3)
    return std::nullopt;

  PatchEntry entry;
  if (!TryParse(fields[0], &entry.address) || !TryParse(fields[2], &entry.value))
    return std::nullopt;
  if (fields.size() > 3)
  {
    if (!TryParse(fields[3], &entry.comparand))
      return std::nullopt;
    entry.conditional = true;
  }

  const std::optional<PatchType> type = TypeFromName(fields[1]);
  if (!type)
    return std::nullopt;
  entry.type = *type;
  return entry;
}

std::string SerializeLine(const PatchEntry& entry)
{
  std::string line =
      fmt::format("0x{:08X}:{}:0x{:08X}", entry.address, PatchTypeAsString(entry.type), entry.value);
  if (entry.conditional)
    line += fmt::format(":0x{:08X}", entry.comparand);
  return line;
}

void LoadPatchSection(const std::string& section, std::vector<Patch>* patches,
                      const Common::IniFile& globalIni, const Common::IniFile& localIni)
{
  // A section is "$Name" lines, each followed by its entry lines. Entry lines that don't parse are
  // skipped, as are entries in front of the first name. A patch is kept even without entries,
  // except the section's last one.
  const auto read_ini = [&](const Common::IniFile& ini, bool user_defined) {
    std::vector<std::string> lines;
    ini.GetLines(section, &lines);

    Patch pending;
    for (const std::string& line : lines)
    {
      if (line.empty())
        continue;

      if (line[0] != '$')
      {
        if (std::optional<PatchEntry> entry = DeserializeLine(line))
          pending.entries.push_back(*entry);
        continue;
      }

      if (!pending.name.empty())
        patches->push_back(pending);
      pending.entries.clear();
      pending.name = line.substr(1);
      pending.user_defined = user_defined;
    }
    if (!pending.name.empty() && !pending.entries.empty())
      patches->push_back(std::move(pending));

    ReadEnabledAndDisabled(ini, section, patches);
  };

  read_ini(globalIni, false);
  // The global ini decides the defaults; the local ini's lists are saved against them.
  for (Patch& patch : *patches)
    patch.default_enabled = patch.enabled;
  read_ini(localIni, true);
}

void SavePatchSection(Common::IniFile* local_ini, std::span<const Patch> patches)
{
  std::vector<std::string> turned_on;
  std::vector<std::string> turned_off;
  std::vector<std::string> user_patches;

  for (const Patch& patch : patches)
  {
    if (patch.enabled != patch.default_enabled)
      (patch.enabled ? turned_on : turned_off).push_back("$" + patch.name);

    // Shipped patches stay in the global ini.
    if (!patch.user_defined)
      continue;
    user_patches.push_back("$" + patch.name);
    for (const PatchEntry& entry : patch.entries)
      user_patches.push_back(SerializeLine(entry));
  }

  local_ini->SetLines("OnFrame_Enabled", std::move(turned_on));
  local_ini->SetLines("OnFrame_Disabled", std::move(turned_off));
  local_ini->SetLines("OnFrame", std::move(user_patches));
}

void LoadPatches()
{
  const SConfig& config = SConfig::GetInstance();
  const Common::IniFile global_ini = config.LoadDefaultGameIni();
  const Common::IniFile local_ini = config.LoadLocalGameIni();

  LoadPatchSection("OnFrame", &s_ini_patches, global_ini, local_ini);

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().FilterApprovedPatches(s_ini_patches, config.GetGameID(),
                                                          config.GetRevision());
#endif  // USE_RETRO_ACHIEVEMENTS

  // During NetPlay with code syncing, the host's codes are used instead of this ini's.
  if (Config::Get(Config::SESSION_CODE_SYNC_OVERRIDE))
  {
    Gecko::SetSyncedCodesAsActive();
    ActionReplay::SetSyncedCodesAsActive();
  }
  else
  {
    Gecko::SetActiveCodes(Gecko::LoadCodes(global_ini, local_ini), config.GetGameID(),
                          config.GetRevision());
    ActionReplay::LoadAndApplyCodes(global_ini, local_ini, config.GetGameID(),
                                    config.GetRevision());
  }

  const auto patch_count =
      std::ranges::count_if(s_ini_patches, [](const Patch& patch) { return patch.enabled; });
  if (patch_count != 0)
  {
    OSD::AddMessage(fmt::format("{} game patch(es) enabled", patch_count),
                    OSD::Duration::NORMAL);
  }

  const std::size_t cheat_count = ActionReplay::CountEnabledCodes() + Gecko::CountEnabledCodes();
  if (cheat_count != 0)
    OSD::AddMessage(fmt::format("{} cheat(s) enabled", cheat_count), OSD::Duration::NORMAL);
}

void AddMemoryPatch(std::size_t index)
{
  std::lock_guard lock(s_debugger_patches_mutex);
  s_debugger_patches.push_back(index);
}

void RemoveMemoryPatch(std::size_t index)
{
  std::lock_guard lock(s_debugger_patches_mutex);
  std::erase(s_debugger_patches, index);
}

bool ApplyFramePatches(Core::System& system)
{
  const auto& ppc_state = system.GetPPCState();

  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);

  // SystemTimers calls again a little later when this returns false.
  if (!TranslationEnabled(ppc_state) || !LooksLikeNormalCode(guard))
  {
    LogRetryLater(ppc_state);
    return false;
  }

  ApplyIniPatches(guard, s_ini_patches);
  ApplyDebuggerPatches(guard);
  Gecko::RunCodeHandler(guard);
  ActionReplay::RunAllActive(guard);
  return true;
}

void Shutdown()
{
  s_ini_patches.clear();
  ActionReplay::ApplyCodes({}, "", 0);
  Gecko::Shutdown();
}

void Reload(Core::System& system)
{
  Shutdown();
  LoadPatches();
  ApplyAfterReload(system);
}

}  // namespace PatchEngine
