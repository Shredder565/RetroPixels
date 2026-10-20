// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <array>
#include <map>

#include "Common/CommonTypes.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE
{
namespace
{
// Every guest address that currently traps into HLE, with the index of its
// entry in the hook table below.
std::map<u32, u32> s_hook_at;

using Flag = HookFlag;
constexpr HookType kStart = HookType::Start;
constexpr HookType kReplace = HookType::Replace;

// The hook table. A hook is referred to by its position here (the JITs bake
// the position into compiled code), so entries must never be reordered.
// Slot 0 is reserved: an index of zero means "no hook".
// clang-format off
constexpr std::array<Hook, 23> s_hooks{{
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       kReplace, Flag::Generic},
    // Placed at a fixed address by the boot code for homebrew reloads.
    {"HBReload",                     HLE_Misc::HBReload,                    kReplace, Flag::Fixed},
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   kReplace, Flag::Debug},
    // Must stay ahead of vprintf, which this function ends up calling.
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        kStart,   Flag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                kStart,   Flag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 kStart,   Flag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                kStart,   Flag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 kStart,   Flag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    // Compilers sometimes lower a plain printf to puts.
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    // Early-initialisation logger.
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Debug},
    // Console writer seen in the system menu.
    {"__write_console",              HLE_OS::HLE_write_console,             kStart,   Flag::Debug},
    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, kStart,   Flag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       kReplace, Flag::Fixed},
    // The apploader is handed an OSReport-style logging callback.
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         kStart,   Flag::Fixed},
}};
// clang-format on

constexpr u32 HOOK_COUNT = static_cast<u32>(s_hooks.size());

// Throws away translated code for single addresses.
class CacheFlusher
{
public:
  explicit CacheFlusher(Core::System& system) : m_system(system) {}

  void Drop(u32 address)
  {
    m_system.GetPPCState().iCache.Invalidate(m_system.GetMemory(), m_system.GetJitInterface(),
                                             address);
  }

private:
  Core::System& m_system;
};

bool IsFixed(u32 index)
{
  return s_hooks[index].flags == HookFlag::Fixed;
}

// Hooks (or unhooks) every word of a symbol's body. The end is computed in
// 32-bit arithmetic, matching how symbol extents are stored.
template <typename Fn> void ForEachWord(const Common::Symbol& symbol, Fn&& fn)
{
  const u32 end = symbol.address + symbol.size;
  for (u32 word = symbol.address; word < end; word += 4)
    fn(word);
}

// Drops every map entry for which `pred` is true, flushing each address and
// handing it to `on_erase`.
template <typename Pred, typename OnErase>
void EraseHooksIf(CacheFlusher& flusher, Pred&& pred, OnErase&& on_erase)
{
  auto it = s_hook_at.begin();
  while (it != s_hook_at.end())
  {
    if (!pred(*it))
    {
      ++it;
      continue;
    }
    on_erase(it->first);
    flusher.Drop(it->first);
    it = s_hook_at.erase(it);
  }
}
}  // namespace

void Patch(Core::System& system, u32 pc, std::string_view func_name)
{
  // Slot 0 is the "no hook" marker and can never be installed by name.
  const auto first = s_hooks.begin() + 1;
  const auto match = std::find_if(first, s_hooks.end(),
                                  [func_name](const Hook& hook) { return func_name == hook.name; });
  if (match == s_hooks.end())
    return;

  s_hook_at[pc] = static_cast<u32>(match - s_hooks.begin());
  CacheFlusher(system).Drop(pc);
  Host_JitCacheInvalidation();
}

void PatchFixedFunctions(Core::System& system)
{
  // Under MIOS the range 0x1800-0x3000 of MEM1 holds MIOS' own patch data,
  // and writing there breaks the IPL for patched games. Gecko codes therefore
  // can't work under MIOS; that is the lesser problem.
  if (system.IsMIOS())
    return;

  // The homebrew reload stub would get in the way of the Gecko code handler,
  // so it is only installed while cheats are off.
  if (!Config::AreCheatsEnabled())
  {
    Patch(system, 0x80001800, "HBReload");
    system.GetMemory().CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // The code handler (ours or Gecko OS's) doesn't flush the icache itself.
  Patch(system, Gecko::ENTRY_POINT, "GeckoCodehandler");
  // Installed unconditionally: a savestate may have been made with the PC
  // inside the code handler even though cheats are now disabled.
  Patch(system, Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");
}

void PatchFunctions(Core::System& system)
{
  CacheFlusher flusher(system);

  // Symbol-based hooks are rebuilt from scratch; fixed hooks stay.
  EraseHooksIf(flusher, [](const auto& entry) { return !IsFixed(entry.second); }, [](u32) {});

  PPCSymbolDB& symbols = system.GetPowerPC().GetSymbolDB();
  for (u32 index = 1; index < HOOK_COUNT; ++index)
  {
    if (IsFixed(index))
      continue;

    const Hook& hook = s_hooks[index];
    for (const Common::Symbol* symbol : symbols.GetSymbolsFromName(hook.name))
    {
      ForEachWord(*symbol, [&](u32 word) {
        s_hook_at[word] = index;
        flusher.Drop(word);
      });
      INFO_LOG_FMT(OSHLE, "Patching {} {:08x}", hook.name, symbol->address);
    }
  }

  Host_JitCacheInvalidation();
}

void Clear()
{
  s_hook_at.clear();
}

void Reload(Core::System& system)
{
  Clear();
  PatchFixedFunctions(system);
  PatchFunctions(system);
}

void Execute(const Core::CPUThreadGuard& guard, u32, u32 hook_index)
{
  // The upper bits may carry extra information from the caller.
  const u32 index = hook_index & 0xFFFFF;
  if (index == 0 || index >= HOOK_COUNT)
  {
    PanicAlertFmt("HLE system tried to call an undefined HLE function {}.", index);
    return;
  }
  s_hooks[index].function(guard);
}

void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  const Core::CPUThreadGuard guard(system);
  Execute(guard, current_pc, hook_index);
}

u32 GetHookByAddress(u32 address)
{
  const auto found = s_hook_at.find(address);
  if (found == s_hook_at.end())
    return 0;
  return found->second;
}

u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address)
{
  const u32 index = GetHookByAddress(address);

  // A fixed hook sits on exactly one address, so there's no symbol to check.
  if (index != 0 && !IsFixed(index))
  {
    const Common::Symbol* symbol = ppc_symbol_db.GetSymbolFromAddr(address);
    if (symbol == nullptr || symbol->address != address)
      return 0;
  }
  return index;
}

const char* GetHookNameByIndex(u32 index)
{
  return s_hooks[index].name;
}

HookType GetHookTypeByIndex(u32 index)
{
  return s_hooks[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return s_hooks[index].flags;
}

TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode)
{
  TryReplaceFunctionResult outcome;

  const u32 index = GetHookByFunctionAddress(ppc_symbol_db, address);
  if (index == 0)
    return outcome;

  const HookType kind = GetHookTypeByIndex(index);
  const bool replaceable = kind == HookType::Start || kind == HookType::Replace;
  if (replaceable && IsEnabled(GetHookFlagsByIndex(index), mode))
  {
    outcome.type = kind;
    outcome.hook_index = index;
  }
  return outcome;
}

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode)
{
  // Logging hooks are only honoured when someone can see the output, or when
  // the interpreter (which pays nothing extra for them) is running.
  if (flag != HookFlag::Debug)
    return true;
  if (Config::IsDebuggingEnabled())
    return true;
  return mode == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(Core::System& system, std::string_view patch_name)
{
  u32 index = 0;
  while (index < HOOK_COUNT && patch_name != s_hooks[index].name)
    ++index;
  if (index == HOOK_COUNT)
    return 0;

  CacheFlusher flusher(system);

  if (IsFixed(index))
  {
    // Fixed hooks have no symbol; find their addresses by table index.
    u32 last_removed = 0;
    EraseHooksIf(
        flusher, [index](const auto& entry) { return entry.second == index; },
        [&last_removed](u32 address) { last_removed = address; });
    Host_JitCacheInvalidation();
    return last_removed;
  }

  const auto matches = system.GetPowerPC().GetSymbolDB().GetSymbolsFromName(patch_name);
  if (matches.empty())
    return 0;

  // Only the first symbol of that name is unhooked.
  const Common::Symbol& symbol = *matches.front();
  ForEachWord(symbol, [&](u32 word) {
    s_hook_at.erase(word);
    flusher.Drop(word);
  });
  Host_JitCacheInvalidation();
  return symbol.address;
}

u32 UnpatchRange(Core::System& system, u32 start_addr, u32 end_addr)
{
  CacheFlusher flusher(system);
  u32 removed = 0;

  for (auto it = s_hook_at.lower_bound(start_addr); it != s_hook_at.end() && it->first < end_addr;
       ++removed)
  {
    INFO_LOG_FMT(OSHLE, "Unpatch HLE hooks [{:08x};{:08x}): {} at {:08x}", start_addr, end_addr,
                 s_hooks[it->second].name, it->first);
    flusher.Drop(it->first);
    it = s_hook_at.erase(it);
  }
  Host_JitCacheInvalidation();

  return removed;
}
}  // namespace HLE
