// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/Debugger_SymbolMap.h"

#include <algorithm>
#include <functional>
#include <string>

#include <fmt/format.h>

#include "Common/CommonTypes.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace Dolphin_Debugger
{
namespace
{
// Frames further up than this are not reported (and a corrupt chain can't loop forever).
constexpr int MAX_STACK_FRAMES = 20;

// The PowerPC EABI back chain ends at a null pointer or anything outside RAM.
bool IsEndOfChain(const Core::CPUThreadGuard& guard, u32 address)
{
  return address == 0 || !PowerPC::MMU::HostIsRAMAddress(guard, address);
}

// Visits the saved return address (LR save word at back chain + 4) of every frame on the
// emulated stack, innermost first.
void ForEachSavedReturnAddress(const Core::CPUThreadGuard& guard,
                               const std::function<void(u32)>& visit)
{
  const u32 stack_pointer = guard.GetSystem().GetPPCState().gpr[1];
  if (IsEndOfChain(guard, stack_pointer))
    return;

  u32 frame = PowerPC::MMU::HostRead<u32>(guard, stack_pointer);
  int visited = 0;
  while (visited < MAX_STACK_FRAMES && !IsEndOfChain(guard, frame + 4))
  {
    visit(PowerPC::MMU::HostRead<u32>(guard, frame + 4));
    ++visited;
    if (IsEndOfChain(guard, frame))
      return;
    frame = PowerPC::MMU::HostRead<u32>(guard, frame);
  }
}

std::string DescribeCaller(const PPCSymbolDB& symbols, u32 address)
{
  std::string description = symbols.GetDescription(address);
  if (description.empty() || description == "Invalid")
    return "(unknown)";
  return description;
}
}  // namespace

bool GetCallstack(const Core::CPUThreadGuard& guard, std::vector<CallstackEntry>& output)
{
  Core::System& system = guard.GetSystem();
  PowerPC::PowerPCManager& cpu = system.GetPowerPC();
  const PowerPC::PowerPCState& state = cpu.GetPPCState();

  if (!Core::IsRunning(system))
    return false;
  if (!PowerPC::MMU::HostIsRAMAddress(guard, state.gpr[1]))
    return false;

  const u32 link = LR(state);
  if (link == 0)
  {
    output.push_back(CallstackEntry{.Name = "(error: LR=0)", .vAddress = 0});
    return false;
  }

  // Every entry points at the call instruction itself, i.e. one instruction before the return
  // address. The link register is the innermost caller.
  const PPCSymbolDB& symbols = cpu.GetSymbolDB();
  output.push_back(CallstackEntry{
      .Name = fmt::format(" * {} [ LR = {:08x} ]\n", symbols.GetDescription(link), link - 4),
      .vAddress = link - 4,
  });

  ForEachSavedReturnAddress(guard, [&](u32 return_address) {
    const u32 call_site = return_address - 4;
    output.push_back(CallstackEntry{
        .Name = fmt::format(" * {} [ addr = {:08x} ]\n", DescribeCaller(symbols, return_address),
                            call_site),
        .vAddress = call_site,
    });
  });
  return true;
}

void PrintCallstack(const Core::CPUThreadGuard& guard, Common::Log::LogType type,
                    Common::Log::LogLevel level)
{
  PowerPC::PowerPCManager& cpu = guard.GetSystem().GetPowerPC();
  const PowerPC::PowerPCState& state = cpu.GetPPCState();
  const PPCSymbolDB& symbols = cpu.GetSymbolDB();
  const u32 link = LR(state);

  GENERIC_LOG_FMT(type, level, "== STACK TRACE - SP = {:08x} ==", state.gpr[1]);
  if (link == 0)
    GENERIC_LOG_FMT(type, level, " LR = 0 - this is bad");

  // The link register is only worth a line if it lies outside the current function.
  const std::string link_description = symbols.GetDescription(link);
  if (link_description != symbols.GetDescription(state.pc))
    GENERIC_LOG_FMT(type, level, " * {}  [ LR = {:08x} ]", link_description, link);

  ForEachSavedReturnAddress(guard, [&](u32 return_address) {
    GENERIC_LOG_FMT(type, level, " * {} [ addr = {:08x} ]", DescribeCaller(symbols, return_address),
                    return_address);
  });
}

void PrintDataBuffer(const Core::System& system, Common::Log::LogType type, u32 address, u32 size,
                     std::string_view title)
{
  const u8* const bytes = system.GetMemory().GetPointerForRange(address, size);

  GENERIC_LOG_FMT(type, Common::Log::LogLevel::LDEBUG, "{}", title);

  // Sixteen bytes per line, each followed by a space.
  constexpr u32 BYTES_PER_LINE = 16;
  for (u32 line_start = 0; line_start < size; line_start += BYTES_PER_LINE)
  {
    const u32 line_end = std::min(size, line_start + BYTES_PER_LINE);
    std::string line;
    for (u32 offset = line_start; offset < line_end; ++offset)
      line += fmt::format("{:02x} ", bytes[offset]);
    GENERIC_LOG_FMT(type, Common::Log::LogLevel::LDEBUG, "   Data: {}", line);
  }
}

}  // namespace Dolphin_Debugger
