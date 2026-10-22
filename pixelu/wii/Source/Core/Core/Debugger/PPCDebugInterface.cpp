// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/PPCDebugInterface.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <regex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/Contains.h"
#include "Common/GekkoDisassembler.h"
#include "Common/StringUtil.h"

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Debugger/OSThread.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

void ApplyMemoryPatch(const Core::CPUThreadGuard& guard, std::span<u8> value, const u32 address,
                      bool store_existing_value)
{
  if (value.empty() || !PowerPC::MMU::HostIsRAMAddress(guard, address))
    return;

  PowerPC::PowerPCManager& cpu = guard.GetSystem().GetPowerPC();

  // Bytes are written one at a time. Each 32-bit word that actually changed has its instruction
  // cache line invalidated once, when the last byte of the word (or of the patch) is reached.
  // With store_existing_value the replaced bytes are handed back in `value`, so that the same
  // call can undo the patch later.
  bool word_changed = false;
  const u32 length = static_cast<u32>(value.size());
  for (u32 i = 0; i < length; ++i)
  {
    const u32 target = address + i;
    const u8 previous = PowerPC::MMU::HostRead<u8>(guard, target);
    if (previous != value[i])
    {
      PowerPC::MMU::HostWrite<u8>(guard, value[i], target);
      if (store_existing_value)
        value[i] = previous;
      word_changed = true;
    }

    const bool last_byte_of_word = (target & 3) == 3;
    if (last_byte_of_word)
    {
      if (word_changed)
        cpu.ScheduleInvalidateCacheThreadSafe(Common::AlignDown(target, 4));
      word_changed = false;
    }
  }

  if (word_changed)
    cpu.ScheduleInvalidateCacheThreadSafe(Common::AlignDown(address + length - 1, 4));
}

// PPCPatches ---------------------------------------------------------------------------------

void PPCPatches::Patch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  // One-shot patches are written now; the others are re-applied by the patch engine every frame.
  Common::Debug::MemoryPatch& entry = m_patches[index];
  if (entry.type != Common::Debug::MemoryPatch::ApplyType::Once)
  {
    PatchEngine::AddMemoryPatch(index);
    return;
  }
  ApplyMemoryPatch(guard, entry.value, entry.address);
}

void PPCPatches::UnPatch(std::size_t index)
{
  // One-shot patches have nothing registered (and the memory is restored by the caller).
  if (m_patches[index].type != Common::Debug::MemoryPatch::ApplyType::Once)
    PatchEngine::RemoveMemoryPatch(index);
}

void PPCPatches::ApplyExistingPatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  Common::Debug::MemoryPatch& entry = m_patches[index];
  ApplyMemoryPatch(guard, entry.value, entry.address, false);
}

// PPCDebugInterface --------------------------------------------------------------------------

PPCDebugInterface::PPCDebugInterface(Core::System& system, PPCSymbolDB& ppc_symbol_db)
    : m_system(system), m_ppc_symbol_db(ppc_symbol_db)
{
}

PPCDebugInterface::~PPCDebugInterface() = default;

void PPCDebugInterface::Clear(const Core::CPUThreadGuard& guard)
{
  ClearAllBreakpoints();
  ClearAllMemChecks();
  ClearPatches(guard);
  ClearWatches();
  m_network_logger.reset();
}

// Watches are kept in a Common::Debug::Watches list.

std::size_t PPCDebugInterface::SetWatch(u32 address, std::string name)
{
  return m_watches.SetWatch(address, std::move(name));
}

void PPCDebugInterface::UpdateWatch(std::size_t index, u32 address, std::string name)
{
  m_watches.UpdateWatch(index, address, std::move(name));
}

void PPCDebugInterface::UpdateWatchName(std::size_t index, std::string name)
{
  m_watches.UpdateWatchName(index, std::move(name));
}

void PPCDebugInterface::UpdateWatchAddress(std::size_t index, u32 address)
{
  m_watches.UpdateWatchAddress(index, address);
}

void PPCDebugInterface::UpdateWatchLockedState(std::size_t index, bool locked)
{
  m_watches.UpdateWatchLockedState(index, locked);
}

const Common::Debug::Watch& PPCDebugInterface::GetWatch(std::size_t index) const
{
  return m_watches.GetWatch(index);
}

const std::vector<Common::Debug::Watch>& PPCDebugInterface::GetWatches() const
{
  return m_watches.GetWatches();
}

bool PPCDebugInterface::HasEnabledWatch(u32 address) const
{
  return m_watches.HasEnabledWatch(address);
}

void PPCDebugInterface::EnableWatch(std::size_t index)
{
  m_watches.EnableWatch(index);
}

void PPCDebugInterface::DisableWatch(std::size_t index)
{
  m_watches.DisableWatch(index);
}

void PPCDebugInterface::UnsetWatch(u32 address)
{
  m_watches.UnsetWatch(address);
}

void PPCDebugInterface::RemoveWatch(std::size_t index)
{
  m_watches.RemoveWatch(index);
}

void PPCDebugInterface::ClearWatches()
{
  m_watches.Clear();
}

std::vector<std::string> PPCDebugInterface::SaveWatchesToStrings() const
{
  return m_watches.SaveToStrings();
}

void PPCDebugInterface::LoadWatchesFromStrings(std::span<const std::string> watches)
{
  m_watches.LoadFromStrings(watches);
}

// Memory patches are kept in the PPCPatches list.

const std::vector<Common::Debug::MemoryPatch>& PPCDebugInterface::GetPatches() const
{
  return m_patches.GetPatches();
}

bool PPCDebugInterface::HasEnabledPatch(u32 address) const
{
  return m_patches.HasEnabledPatch(address);
}

void PPCDebugInterface::SetPatch(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  m_patches.SetPatch(guard, address, value);
}

void PPCDebugInterface::SetPatch(const Core::CPUThreadGuard& guard, u32 address,
                                 std::vector<u8> value)
{
  m_patches.SetPatch(guard, address, std::move(value));
}

void PPCDebugInterface::SetFramePatch(const Core::CPUThreadGuard& guard, u32 address,
                                      std::vector<u8> value)
{
  m_patches.SetFramePatch(guard, address, std::move(value));
}

void PPCDebugInterface::SetFramePatch(const Core::CPUThreadGuard& guard, u32 address, u32 value)
{
  m_patches.SetFramePatch(guard, address, value);
}

void PPCDebugInterface::EnablePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  m_patches.EnablePatch(guard, index);
}

void PPCDebugInterface::DisablePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  m_patches.DisablePatch(guard, index);
}

void PPCDebugInterface::ApplyExistingPatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  m_patches.ApplyExistingPatch(guard, index);
}

void PPCDebugInterface::UnsetPatch(const Core::CPUThreadGuard& guard, u32 address)
{
  m_patches.UnsetPatch(guard, address);
}

void PPCDebugInterface::RemovePatch(const Core::CPUThreadGuard& guard, std::size_t index)
{
  m_patches.RemovePatch(guard, index);
}

void PPCDebugInterface::ClearPatches(const Core::CPUThreadGuard& guard)
{
  m_patches.ClearPatches(guard);
}

// Threads ------------------------------------------------------------------------------------

Common::Debug::Threads PPCDebugInterface::GetThreads(const Core::CPUThreadGuard& guard) const
{
  // OS globals: the address of the running thread's OSThread structure.
  constexpr u32 OS_CURRENT_THREAD_POINTER = 0x800000dc;

  Common::Debug::Threads result;
  const auto in_ram = [&guard](u32 address) {
    return PowerPC::MMU::HostIsRAMAddress(guard, address);
  };
  if (!in_ram(OS_CURRENT_THREAD_POINTER))
    return result;
  const u32 current_address = PowerPC::MMU::HostRead<u32>(guard, OS_CURRENT_THREAD_POINTER);
  if (!in_ram(current_address))
    return result;

  auto current = std::make_unique<Core::Debug::OSThreadView>(guard, current_address);
  if (!current->IsValid(guard))
    return result;

  // The threads form a doubly linked list around the current one. Walk backwards first (and
  // flip that part so it ends up in list order), then forwards. A thread is never listed twice,
  // which also stops corrupt links that form a cycle.
  std::vector<u32> seen{current->GetAddress()};
  const auto follow = [&](u32 address, bool forwards) {
    while (address != 0 && in_ram(address) && !Common::Contains(seen, address))
    {
      seen.push_back(address);
      auto view = std::make_unique<Core::Debug::OSThreadView>(guard, address);
      if (!view->IsValid(guard))
        return;
      const auto& link = view->Data().thread_link;
      address = forwards ? link.next : link.prev;
      result.emplace_back(std::move(view));
    }
  };

  follow(current->Data().thread_link.prev, false);
  std::ranges::reverse(result);

  const u32 after_current = current->Data().thread_link.next;
  result.emplace_back(std::move(current));
  follow(after_current, true);
  return result;
}

// Code and memory views ----------------------------------------------------------------------

std::string PPCDebugInterface::Disassemble(const Core::CPUThreadGuard* guard, u32 address) const
{
  if (guard == nullptr)
    return "<unknown>";
  if (!PowerPC::MMU::HostIsRAMAddress(*guard, address))
    return "(No RAM here)";

  const u32 opcode = PowerPC::MMU::HostRead_Instruction(*guard, address);
  std::string text = Common::GekkoDisassembler::Disassemble(opcode, address);
  // Primary opcode 1 is unused by the CPU and marks an HLE hook.
  if (UGeckoInstruction{opcode}.OPCD == 1)
    text += " (hle)";
  return text;
}

std::string PPCDebugInterface::GetRawMemoryString(const Core::CPUThreadGuard& guard, int memory,
                                                  u32 address) const
{
  // Every result is eight characters wide (plus the ARAM marker).
  if (!IsAlive())
    return "<unknwn>";

  const bool aram = memory != 0;
  if (!aram && !PowerPC::MMU::HostIsRAMAddress(guard, address))
    return "--------";
  return fmt::format("{:08X}{}", ReadExtraMemory(guard, memory, address), aram ? " (ARAM)" : "");
}

u32 PPCDebugInterface::ReadMemory(const Core::CPUThreadGuard& guard, u32 address) const
{
  return PowerPC::MMU::HostRead<u32>(guard, address);
}

u32 PPCDebugInterface::ReadInstruction(const Core::CPUThreadGuard& guard, u32 address) const
{
  return PowerPC::MMU::HostRead_Instruction(guard, address);
}

u32 PPCDebugInterface::ReadExtraMemory(const Core::CPUThreadGuard& guard, int memory,
                                       u32 address) const
{
  if (memory == 0)
    return PowerPC::MMU::HostRead<u32>(guard, address);
  if (memory != EXTRAMEM_ARAM)
    return 0;

  // ARAM is byte addressed; assemble a big-endian word.
  const auto& dsp = guard.GetSystem().GetDSP();
  u32 word = 0;
  for (u32 i = 0; i < 4; ++i)
    word = (word << 8) | dsp.ReadARAM(address + i);
  return word;
}

u32 PPCDebugInterface::GetColor(const Core::CPUThreadGuard* guard, u32 address) const
{
  // Background colours for the code view: each function gets one of a few pastel shades so that
  // neighbouring functions are told apart; data symbols are lavender, unmapped memory grey.
  constexpr u32 WHITE = 0xFFFFFF;
  constexpr u32 NO_MEMORY = 0xEEEEEE;
  constexpr u32 DATA_SYMBOL = 0xEEEEFF;
  static constexpr std::array<u32, 6> FUNCTION_SHADES{
      0xD0FFFF,  // cyan
      0xFFD0D0,  // red
      0xD8D8FF,  // blue
      0xFFD0FF,  // purple
      0xD0FFD0,  // green
      0xFFFFD0,  // yellow
  };

  if (guard == nullptr || !IsAlive())
    return WHITE;
  if (!PowerPC::MMU::HostIsRAMAddress(*guard, address))
    return NO_MEMORY;

  const Common::Symbol* const symbol = m_ppc_symbol_db.GetSymbolFromAddr(address);
  if (symbol == nullptr)
    return WHITE;
  if (symbol->type == Common::Symbol::Type::Function)
    return FUNCTION_SHADES[static_cast<size_t>(symbol->index) % FUNCTION_SHADES.size()];
  return DATA_SYMBOL;
}

std::string PPCDebugInterface::GetDescription(u32 address) const
{
  return m_ppc_symbol_db.GetDescription(address);
}

std::optional<u32>
PPCDebugInterface::GetMemoryAddressFromInstruction(const std::string& instruction) const
{
  // Matches the operand part of a disassembled load/store such as "lwz r3, -0x10 (r31)" or
  // "stw r0, 0x4 (sp)". Captures: 1 = optional minus sign, 2 = immediate offset (hex) or an
  // index register "rN" (indexed forms), 3 = base register number, "p" (sp) or "toc" (rtoc).
  static const std::regex operand_pattern(
      ",[^r0-]*(-?)(?:0[xX])?([0-9a-fA-F]+|r\\d+)[^r^s]*.(p|toc|\\d+)");

  std::smatch found;
  if (!std::regex_search(instruction, found, operand_pattern))
    return std::nullopt;

  const auto group = [&found](size_t n) {
    return std::string_view{&*found[n].first, static_cast<size_t>(found[n].length())};
  };
  const std::string_view negative = group(1);
  const std::string_view displacement = group(2);
  const std::string_view base = group(3);

  const PowerPC::PowerPCState& state = m_system.GetPPCState();

  // The displacement is either an immediate or (for indexed forms) a register, where r0 reads
  // as zero.
  u32 displacement_value = 0;
  if (displacement.front() == 'r')
  {
    unsigned index_register = 0;
    Common::FromChars(displacement.substr(1), index_register, 10);
    if (index_register != 0)
      displacement_value = state.gpr[index_register];
  }
  else
  {
    Common::FromChars(displacement, displacement_value, 16);
  }

  u32 base_register = 0;
  switch (base.front())
  {
  case 'p':  // sp
    base_register = 1;
    break;
  case 't':  // rtoc
    base_register = 2;
    break;
  default:
    Common::FromChars(base, base_register, 10);
    break;
  }

  const u32 base_value = state.gpr[base_register];
  return negative.empty() ? base_value + displacement_value : base_value - displacement_value;
}

// Execution control --------------------------------------------------------------------------

bool PPCDebugInterface::IsAlive() const
{
  return Core::IsRunning(m_system);
}

u32 PPCDebugInterface::GetPC() const
{
  return m_system.GetPPCState().pc;
}

void PPCDebugInterface::SetPC(u32 address)
{
  m_system.GetPPCState().pc = address;
}

void PPCDebugInterface::RunTo(u32 address)
{
  // A temporary breakpoint is removed again once it is hit.
  m_system.GetPowerPC().GetBreakPoints().SetTemporary(address);
  m_system.GetCPU().SetStepping(false);
}

bool PPCDebugInterface::IsBreakpoint(u32 address) const
{
  return m_system.GetPowerPC().GetBreakPoints().IsAddressBreakPoint(address);
}

void PPCDebugInterface::ToggleBreakpoint(u32 address)
{
  m_system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(address);
}

void PPCDebugInterface::AddBreakpoint(u32 address)
{
  m_system.GetPowerPC().GetBreakPoints().Add(address);
}

void PPCDebugInterface::RemoveBreakpoint(u32 address)
{
  m_system.GetPowerPC().GetBreakPoints().Remove(address);
}

void PPCDebugInterface::ClearAllBreakpoints()
{
  m_system.GetPowerPC().GetBreakPoints().Clear();
}

bool PPCDebugInterface::IsMemCheck(u32 address, size_t size) const
{
  return m_system.GetPowerPC().GetMemChecks().GetMemCheck(address, size) != nullptr;
}

void PPCDebugInterface::ToggleMemCheck(u32 address, bool read, bool write, bool log)
{
  MemChecks& checks = m_system.GetPowerPC().GetMemChecks();
  if (IsMemCheck(address))
  {
    checks.Remove(address);
    return;
  }

  // A new check covers the single address and always breaks.
  TMemCheck check;
  check.start_address = address;
  check.end_address = address;
  check.is_break_on_read = read;
  check.is_break_on_write = write;
  check.log_on_hit = log;
  check.break_on_hit = true;
  checks.Add(std::move(check));
}

void PPCDebugInterface::ClearAllMemChecks()
{
  m_system.GetPowerPC().GetMemChecks().Clear();
}

// Network capture ----------------------------------------------------------------------------

std::shared_ptr<Core::NetworkCaptureLogger> PPCDebugInterface::NetworkLogger()
{
  // PCAP output takes precedence over the raw SSL dump.
  Core::NetworkCaptureType wanted = Core::NetworkCaptureType::None;
  if (Config::Get(Config::MAIN_NETWORK_DUMP_AS_PCAP))
    wanted = Core::NetworkCaptureType::PCAP;
  else if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ) ||
           Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
    wanted = Core::NetworkCaptureType::Raw;

  // Keep the current logger (and its open files) as long as the setting doesn't change.
  if (m_network_logger != nullptr && m_network_logger->GetCaptureType() == wanted)
    return m_network_logger;

  if (wanted == Core::NetworkCaptureType::PCAP)
    m_network_logger = std::make_shared<Core::PCAPSSLCaptureLogger>();
  else if (wanted == Core::NetworkCaptureType::Raw)
    m_network_logger = std::make_shared<Core::BinarySSLCaptureLogger>();
  else
    m_network_logger = std::make_shared<Core::DummyNetworkCaptureLogger>();
  return m_network_logger;
}
