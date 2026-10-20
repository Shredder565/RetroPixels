// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace PowerPC
{
enum class CoreMode;
}

class PPCSymbolDB;

namespace HLE
{
using HookFunction = void (*)(const Core::CPUThreadGuard&);

enum class HookType
{
  None,     // No hook
  Start,    // Run the HLE handler on entry, then let the guest function run
  Replace,  // Run the HLE handler instead of the guest function
};

enum class HookFlag
{
  Generic,  // Nothing special
  Debug,    // Logging hook; only active when its output can be seen
  Fixed,    // Installed at a hard-coded address rather than found by symbol name
};

struct Hook
{
  char name[128];
  HookFunction function;
  HookType type;
  HookFlag flags;
};

struct TryReplaceFunctionResult
{
  HookType type = HookType::None;
  u32 hook_index = 0;

  explicit operator bool() const { return type != HookType::None; }
};

void PatchFixedFunctions(Core::System& system);
void PatchFunctions(Core::System& system);
void Clear();
void Reload(Core::System& system);

void Patch(Core::System& system, u32 pc, std::string_view func_name);
u32 UnPatch(Core::System& system, std::string_view patch_name);
u32 UnpatchRange(Core::System& system, u32 start_addr, u32 end_addr);
void Execute(const Core::CPUThreadGuard& guard, u32 current_pc, u32 hook_index);
void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system);

// Hook table index installed at `address`, or 0 for none.
u32 GetHookByAddress(u32 address);
// Like GetHookByAddress, but symbol hooks only count at their function's first instruction.
u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address);
const char* GetHookNameByIndex(u32 index);
HookType GetHookTypeByIndex(u32 index);
HookFlag GetHookFlagsByIndex(u32 index);

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode);

// Shared check used by every CPU backend before compiling or interpreting a
// function entry: reports whether (and how) the code at `address` is hooked.
TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode);

}  // namespace HLE
