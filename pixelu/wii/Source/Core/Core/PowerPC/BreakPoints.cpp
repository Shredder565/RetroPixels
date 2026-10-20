// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/BreakPoints.h"

#include <algorithm>
#include <cstddef>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/DebugInterface.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

// Saved form of both kinds (one string each):
//   breakpoint:  "$<address> <flags>[c <condition>]"
//   watchpoint:  "$<start> <end> <flags>[c <condition>]"
// Addresses are eight hex digits. Flags: n = enabled, r/w = break on
// read/write (watchpoints only), l = log, b = break, c = a condition follows
// after a space and runs to the end of the string.

namespace
{
// A string stream that reads and writes numbers the same way in every
// locale.
std::istringstream ClassicReader(const std::string& text)
{
  std::istringstream stream(text);
  stream.imbue(std::locale::classic());
  if (stream.peek() == '$')
    stream.get();
  return stream;
}

std::optional<Expression> ReadCondition(std::istringstream& stream, const std::string& flags)
{
  if (!flags.contains('c'))
    return std::nullopt;
  std::string text;
  stream >> std::ws;
  std::getline(stream, text);
  return Expression::TryParse(text);
}

void AppendCondition(std::string& out, const std::optional<Expression>& condition)
{
  if (condition)
    out += "c " + condition->GetText();
}

template <typename Container, typename Member>
auto FindBy(Container& items, u32 address, Member member)
{
  return std::ranges::find(items, address, member);
}
}  // namespace

// ---------------------------------------------------------------------------
// Instruction breakpoints
// ---------------------------------------------------------------------------

BreakPoints::BreakPoints(Core::System& system) : m_system(system)
{
}

BreakPoints::~BreakPoints() = default;

bool BreakPoints::IsAddressBreakPoint(u32 address) const
{
  return GetBreakpoint(address) != nullptr;
}

bool BreakPoints::IsBreakPointEnable(u32 address) const
{
  if (!m_breaking_enabled)
    return false;
  const TBreakPoint* bp = GetBreakpoint(address);
  return bp != nullptr && bp->is_enabled;
}

const TBreakPoint* BreakPoints::GetBreakpoint(u32 address) const
{
  if (m_temp_breakpoint.has_value() && m_temp_breakpoint->address == address)
    return &m_temp_breakpoint.value();
  return GetRegularBreakpoint(address);
}

const TBreakPoint* BreakPoints::GetRegularBreakpoint(u32 address) const
{
  const auto it = FindBy(m_breakpoints, address, &TBreakPoint::address);
  return it != m_breakpoints.end() ? &*it : nullptr;
}

BreakPoints::TBreakPointsStr BreakPoints::GetStrings() const
{
  TBreakPointsStr result;
  result.reserve(m_breakpoints.size());
  for (const TBreakPoint& bp : m_breakpoints)
  {
    std::string line = fmt::format("${:08x} ", bp.address);
    if (bp.is_enabled)
      line += 'n';
    if (bp.log_on_hit)
      line += 'l';
    if (bp.break_on_hit)
      line += 'b';
    AppendCondition(line, bp.condition);
    result.push_back(std::move(line));
  }
  return result;
}

void BreakPoints::AddFromStrings(const TBreakPointsStr& bp_strings)
{
  for (const std::string& line : bp_strings)
  {
    std::istringstream stream = ClassicReader(line);
    TBreakPoint bp;
    std::string flags;
    stream >> std::hex >> bp.address;
    stream >> flags;
    bp.is_enabled = flags.contains('n');
    bp.log_on_hit = flags.contains('l');
    bp.break_on_hit = flags.contains('b');
    bp.condition = ReadCondition(stream, flags);
    Add(std::move(bp));
  }
}

void BreakPoints::Add(TBreakPoint bp)
{
  if (IsAddressBreakPoint(bp.address))
    return;

  m_system.GetJitInterface().InvalidateICache(bp.address, 4, true);
  m_breakpoints.push_back(std::move(bp));
}

void BreakPoints::Add(u32 address)
{
  Add(address, true, false, std::nullopt);
}

void BreakPoints::Add(u32 address, bool break_on_hit, bool log_on_hit,
                      std::optional<Expression> condition)
{
  TBreakPoint bp;
  bp.address = address;
  bp.is_enabled = true;
  bp.log_on_hit = log_on_hit;
  bp.break_on_hit = break_on_hit;
  bp.condition = std::move(condition);

  const auto existing = FindBy(m_breakpoints, address, &TBreakPoint::address);
  if (existing == m_breakpoints.end())
  {
    m_breakpoints.push_back(std::move(bp));
  }
  else
  {
    // Replacing keeps whether the old one was enabled.
    bp.is_enabled = existing->is_enabled;
    *existing = std::move(bp);
  }

  m_system.GetJitInterface().InvalidateICache(address, 4, true);
}

void BreakPoints::SetTemporary(u32 address)
{
  TBreakPoint bp;
  bp.address = address;
  bp.is_enabled = true;
  bp.break_on_hit = true;
  m_temp_breakpoint = std::move(bp);

  m_system.GetJitInterface().InvalidateICache(address, 4, true);
}

bool BreakPoints::ToggleBreakPoint(u32 address)
{
  if (Remove(address))
    return false;
  Add(address);
  return true;
}

bool BreakPoints::ToggleEnable(u32 address)
{
  const auto it = FindBy(m_breakpoints, address, &TBreakPoint::address);
  if (it == m_breakpoints.end())
    return false;
  it->is_enabled = !it->is_enabled;
  return true;
}

void BreakPoints::EnableBreaking(bool enable)
{
  m_breaking_enabled = enable;
}

bool BreakPoints::Remove(u32 address)
{
  const auto it = FindBy(m_breakpoints, address, &TBreakPoint::address);
  if (it == m_breakpoints.end())
    return false;

  m_breakpoints.erase(it);
  m_system.GetJitInterface().InvalidateICache(address, 4, true);
  return true;
}

void BreakPoints::Clear()
{
  auto& jit = m_system.GetJitInterface();
  for (const TBreakPoint& bp : m_breakpoints)
    jit.InvalidateICache(bp.address, 4, true);
  m_breakpoints.clear();

  ClearTemporary();
}

void BreakPoints::ClearTemporary()
{
  if (!m_temp_breakpoint)
    return;
  m_system.GetJitInterface().InvalidateICache(m_temp_breakpoint->address, 4, true);
  m_temp_breakpoint.reset();
}

// ---------------------------------------------------------------------------
// Memory watchpoints
// ---------------------------------------------------------------------------

MemChecks::MemChecks(Core::System& system) : m_system(system)
{
}

MemChecks::~MemChecks() = default;

MemChecks::TMemChecksStr MemChecks::GetStrings() const
{
  TMemChecksStr result;
  result.reserve(m_mem_checks.size());
  for (const TMemCheck& mc : m_mem_checks)
  {
    std::string line = fmt::format("${:08x} {:08x} ", mc.start_address, mc.end_address);
    if (mc.is_enabled)
      line += 'n';
    if (mc.is_break_on_read)
      line += 'r';
    if (mc.is_break_on_write)
      line += 'w';
    if (mc.log_on_hit)
      line += 'l';
    if (mc.break_on_hit)
      line += 'b';
    AppendCondition(line, mc.condition);
    result.push_back(std::move(line));
  }
  return result;
}

void MemChecks::AddFromStrings(const TMemChecksStr& mc_strings)
{
  const Core::CPUThreadGuard guard(m_system);
  DelayedMemCheckUpdate pending(this);

  for (const std::string& line : mc_strings)
  {
    std::istringstream stream = ClassicReader(line);
    TMemCheck mc;
    std::string flags;
    stream >> std::hex >> mc.start_address >> mc.end_address >> flags;

    mc.is_ranged = mc.start_address != mc.end_address;
    mc.is_enabled = flags.contains('n');
    mc.is_break_on_read = flags.contains('r');
    mc.is_break_on_write = flags.contains('w');
    mc.log_on_hit = flags.contains('l');
    mc.break_on_hit = flags.contains('b');
    mc.condition = ReadCondition(stream, flags);

    pending |= Add(std::move(mc));
  }
}

DelayedMemCheckUpdate MemChecks::Add(TMemCheck memory_check)
{
  const Core::CPUThreadGuard guard(m_system);

  const auto existing =
      FindBy(m_mem_checks, memory_check.start_address, &TMemCheck::start_address);
  if (existing == m_mem_checks.end())
  {
    m_mem_checks.push_back(std::move(memory_check));
  }
  else
  {
    // Replacing keeps the enabled state and restarts the hit count.
    memory_check.is_enabled = existing->is_enabled;
    memory_check.num_hits = 0;
    *existing = std::move(memory_check);
  }

  return DelayedMemCheckUpdate(this, true);
}

bool MemChecks::ToggleEnable(u32 address)
{
  const auto it = FindBy(m_mem_checks, address, &TMemCheck::start_address);
  if (it == m_mem_checks.end())
    return false;
  it->is_enabled = !it->is_enabled;
  return true;
}

void MemChecks::EnableBreaking(bool enabled)
{
  m_breaking_enabled = enabled;
  Update();
}

DelayedMemCheckUpdate MemChecks::Remove(u32 address)
{
  const auto it = FindBy(m_mem_checks, address, &TMemCheck::start_address);
  if (it == m_mem_checks.end())
    return DelayedMemCheckUpdate(this, false);

  const Core::CPUThreadGuard guard(m_system);
  m_mem_checks.erase(it);
  return DelayedMemCheckUpdate(this, true);
}

void MemChecks::Clear()
{
  const Core::CPUThreadGuard guard(m_system);
  m_mem_checks.clear();
  Update();
}

void MemChecks::Update()
{
  const Core::CPUThreadGuard guard(m_system);

  // The JIT compiles memory accesses differently while watchpoints exist
  // (and flushes the registers conditions read), so its cache has to go
  // whenever either of those changes.
  const bool registers_changed = UpdateRegistersUsedInConditions();
  const bool any = HasAny();
  if (registers_changed || any != m_mem_breakpoints_set)
  {
    m_system.GetJitInterface().ClearCache(guard);
    m_mem_breakpoints_set = any;
  }

  // Watched pages are kept out of the fast BAT lookup.
  m_system.GetMMU().DBATUpdated();
}

bool MemChecks::UpdateRegistersUsedInConditions()
{
  BitSet32 gprs;
  BitSet32 fprs;
  for (TMemCheck& mc : m_mem_checks)
  {
    if (!mc.condition)
      continue;
    gprs |= mc.condition->GetGPRsUsed();
    fprs |= mc.condition->GetFPRsUsed();
  }

  const bool changed = gprs != m_gprs_used_in_conditions || fprs != m_fprs_used_in_conditions;
  m_gprs_used_in_conditions = gprs;
  m_fprs_used_in_conditions = fprs;
  return changed;
}

TMemCheck* MemChecks::GetMemCheck(u32 address, size_t size)
{
  // The access covers [address, address + size - 1], computed without
  // 32-bit wraparound.
  const size_t last = address + size - 1;
  for (TMemCheck& mc : m_mem_checks)
  {
    if (mc.end_address >= address && last >= mc.start_address)
      return &mc;
  }
  return nullptr;
}

bool MemChecks::OverlapsMemcheck(u32 address, u32 length) const
{
  if (!HasAny())
    return false;

  // Compare the length-aligned blocks (length is a power of two) that hold
  // the address and each end of every watched range: a match if either end
  // falls in the block, or the range spans it.
  const u32 low_bits = length - 1;
  const u32 block = address | low_bits;
  for (const TMemCheck& mc : m_mem_checks)
  {
    const u32 start_block = mc.start_address | low_bits;
    const u32 end_block = mc.end_address | low_bits;
    if (start_block == block || end_block == block)
      return true;
    if (start_block < block && end_block > block)
      return true;
  }
  return false;
}

bool TMemCheck::Action(Core::System& system, u64 value, u32 addr, bool write, size_t size, u32 pc)
{
  if (!is_enabled)
    return false;

  const bool watched = write ? is_break_on_write : is_break_on_read;
  if (!watched || !EvaluateCondition(system, condition))
    return false;

  if (log_on_hit)
  {
    auto& symbols = system.GetPPCSymbolDB();
    NOTICE_LOG_FMT(MEMMAP, "MBP {:08x} ({}) {}{} {:x} at {:08x} ({})", pc,
                   symbols.GetDescription(pc), write ? "Write" : "Read", size * 8, value, addr,
                   symbols.GetDescription(addr));
  }
  return break_on_hit;
}
