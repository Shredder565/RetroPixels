// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/CodeTrace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <regex>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include "Common/Contains.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

// Code trace follows a value through the program while single-stepping: it starts from the
// target register of the current instruction (and the memory a store writes it to) and keeps
// sets of registers and memory bytes that hold the value, updating them as instructions copy,
// load, store or overwrite it. Everything works on the disassembler's text output.

namespace
{
bool StartsWithAny(std::string_view text, std::span<const std::string_view> prefixes)
{
  return std::ranges::any_of(prefixes,
                             [text](std::string_view prefix) { return text.starts_with(prefix); });
}

bool IsLoadOrStore(std::string_view mnemonic)
{
  if (mnemonic.starts_with("psq_l") || mnemonic.starts_with("psq_s"))
    return true;
  if (mnemonic.starts_with("st"))
    return true;
  // Every "l..." except the load-immediate forms (li, lis) accesses memory.
  return mnemonic.starts_with('l') && !mnemonic.starts_with("li");
}

// The access width, from the size letter in the mnemonic (lbz, sth, lfd, psq_l, ...). Only the
// first four characters are looked at so that operands can't match.
u32 AccessSize(std::string_view instruction)
{
  const std::string_view head = instruction.substr(0, 4);
  if (head.contains('b'))
    return 1;
  if (head.contains('h'))
    return 2;
  if (head.contains('d') || head.contains('p'))
    return 8;
  return 4;
}

// Whether an access at `address` (of the instruction's width) touches any tracked byte.
bool TouchesTrackedMemory(const std::string& instruction, u32 address, const std::set<u32>& bytes)
{
  const auto first_at_or_after = bytes.lower_bound(address);
  if (first_at_or_after == bytes.end())
    return false;
  return *first_at_or_after == address || *first_at_or_after < address + AccessSize(instruction);
}

void TrackRange(std::set<u32>& bytes, u32 start, u32 size)
{
  for (u32 offset = 0; offset < size; ++offset)
    bytes.insert(start + offset);
}

void UntrackRange(std::set<u32>& bytes, u32 start, u32 size)
{
  for (u32 offset = 0; offset < size; ++offset)
    bytes.erase(start + offset);
}

// Instruction groups that need special treatment.
constexpr std::array<std::string_view, 3> IGNORED_PREFIXES{"dc", "ic", "mt"};  // cache ops, mt*
constexpr std::array<std::string_view, 2> COMPARE_PREFIXES{"c", "fc"};
constexpr std::array<std::string_view, 1> PARTIAL_WRITE_PREFIXES{"rlwimi"};  // keeps some bits
constexpr std::array<std::string_view, 2> MOVE_PREFIXES{"mr", "fmr"};
}  // namespace

void CodeTrace::SetRegTracked(const std::string& reg)
{
  m_reg_autotrack.push_back(reg);
}

InstructionAttributes CodeTrace::GetInstructionAttributes(const TraceOutput& line) const
{
  InstructionAttributes attributes;
  attributes.address = line.address;
  attributes.instruction = line.instruction;

  // Normalise register names: sp -> r1, rtoc -> r2, and paired-single registers pN -> fN (they
  // are treated like floating point registers).
  static const std::regex sp_name("(\\W)sp");
  static const std::regex rtoc_name("rtoc");
  static const std::regex paired_name("(\\W)p(\\d+)");
  std::string text = std::regex_replace(line.instruction, sp_name, "$1r1");
  text = std::regex_replace(text, rtoc_name, "r2");
  text = std::regex_replace(text, paired_name, "$1f$2");

  // Up to four register operands, the first being the destination ("add r4, r5, r6" gives r4,
  // r5, r6). Paired-single operations usually only yield the first one.
  static const std::regex operands(
      "\\W([rfp]\\d+)[^r^f]*(?:([rf]\\d+))?[^r^f\\d]*(?:([rf]\\d+))?[^r^f\\d]*(?:([rf]\\d+))?",
      std::regex::optimize);

  std::smatch found;
  if (!std::regex_search(text, found, operands))
    return attributes;

  attributes.reg0 = found.str(1);
  std::string* const sources[] = {&attributes.reg1, &attributes.reg2, &attributes.reg3};
  for (size_t group = 2; group <= 4; ++group)
  {
    if (found[group].matched)
      *sources[group - 2] = found.str(group);
  }

  if (line.memory_target.has_value())
  {
    attributes.memory_target = line.memory_target;
    attributes.memory_target_size = AccessSize(text);
    const bool store = text.starts_with("st") || text.starts_with("psq_s");
    attributes.is_store = store;
    attributes.is_load = !store;
  }
  return attributes;
}

TraceOutput CodeTrace::SaveCurrentInstruction(const Core::CPUThreadGuard& guard) const
{
  PowerPC::PowerPCManager& cpu = guard.GetSystem().GetPowerPC();
  Core::DebugInterface& debugger = cpu.GetDebugInterface();
  const u32 pc = cpu.GetPPCState().pc;

  // Only the cheap parts (text and effective address) are captured per step.
  TraceOutput step;
  step.address = pc;
  step.instruction = debugger.Disassemble(&guard, pc);
  if (IsLoadOrStore(step.instruction))
    step.memory_target = debugger.GetMemoryAddressFromInstruction(step.instruction);
  return step;
}

AutoStepResults CodeTrace::AutoStepping(const Core::CPUThreadGuard& guard, bool continue_previous,
                                        AutoStop stop_on)
{
  AutoStepResults results;
  if (m_recording)
    return results;

  const InstructionAttributes start = GetInstructionAttributes(SaveCurrentInstruction(guard));
  // Without a target register (branches and the like) there is nothing to follow, unless an
  // earlier trace is being continued.
  if (!continue_previous && start.reg0.empty())
    return results;

  m_recording = true;

  if (!continue_previous)
  {
    // Fresh trace: follow the destination register, and for a store also the bytes written.
    // (The source memory of a load could be followed too, but isn't.)
    m_reg_autotrack.assign(1, start.reg0);
    m_mem_autotrack.clear();
    if (start.is_store)
      TrackRange(m_mem_autotrack, start.memory_target.value(), AccessSize(start.instruction));
  }

  // The hit types are ordered by how interesting they are; stepping stops at the first hit at
  // or above the requested level.
  HitType stop_level = HitType::SAVELOAD;
  switch (stop_on)
  {
  case AutoStop::Always:
    stop_level = HitType::SAVELOAD;
    break;
  case AutoStop::Used:
    stop_level = HitType::PASSIVE;
    break;
  case AutoStop::Changed:
    stop_level = HitType::ACTIVE;
    break;
  }

  PowerPC::PowerPCManager& cpu = guard.GetSystem().GetPowerPC();
  const PowerPC::CoreMode previous_mode = cpu.GetMode();
  cpu.SetMode(PowerPC::CoreMode::Interpreter);

  using SteadyClock = std::chrono::steady_clock;
  const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::seconds(4);

  bool keep_going = true;
  while (keep_going)
  {
    cpu.SingleStep();
    const HitType hit = TraceLogic(SaveCurrentInstruction(guard));
    ++results.count;

    const bool nothing_tracked = m_reg_autotrack.empty() && m_mem_autotrack.empty();
    keep_going = SteadyClock::now() < deadline && hit < stop_level && !nothing_tracked;
  }
  results.timed_out = SteadyClock::now() >= deadline;

  cpu.SetMode(previous_mode);
  m_recording = false;

  // An empty result means the value was overwritten everywhere; the caller tells that apart
  // from a timeout.
  results.reg_tracked = m_reg_autotrack;
  results.mem_tracked = m_mem_autotrack;
  return results;
}

HitType CodeTrace::TraceLogic(const TraceOutput& current_instr, bool first_hit)
{
  // Must not be called with nothing tracked. Paired-single operations are only partly handled,
  // and update-indexed (-ux) accesses are not special-cased.

  const bool memory_hit = current_instr.memory_target.has_value() && !m_mem_autotrack.empty() &&
                          TouchesTrackedMemory(current_instr.instruction,
                                               *current_instr.memory_target, m_mem_autotrack);

  // Fast path while only memory is followed.
  if (!memory_hit && m_reg_autotrack.empty())
    return HitType::SKIP;

  const InstructionAttributes op = GetInstructionAttributes(current_instr);
  if (op.reg0.empty())
    return HitType::SKIP;

  const auto is_tracked = [this](const std::string& reg) {
    return !reg.empty() && Common::Contains(m_reg_autotrack, reg);
  };
  const auto destination = std::ranges::find(m_reg_autotrack, op.reg0);
  const bool writes_tracked = destination != m_reg_autotrack.end();
  const bool reads_tracked = is_tracked(op.reg1) || is_tracked(op.reg2) || is_tracked(op.reg3);

  if (!writes_tracked && !reads_tracked && !memory_hit)
    return HitType::SKIP;

  const std::string_view mnemonic = op.instruction;

  // Moves between a tracked r0 and the link register.
  if (writes_tracked && mnemonic.starts_with("mflr"))
  {
    m_reg_autotrack.erase(destination);
    return HitType::OVERWRITE;
  }
  if (writes_tracked && mnemonic.starts_with("mtlr"))
    return HitType::MOVED;  // the link register itself isn't followed

  if (StartsWithAny(mnemonic, IGNORED_PREFIXES))
    return HitType::SKIP;
  if (StartsWithAny(mnemonic, COMPARE_PREFIXES))
    return HitType::PASSIVE;
  if (!writes_tracked && reads_tracked && (op.is_load || op.is_store))
    return HitType::POINTER;  // the value is used as an address or index

  if (op.memory_target.has_value())
  {
    const u32 address = *op.memory_target;
    if (memory_hit)
    {
      if (writes_tracked)
        return HitType::SAVELOAD;  // both sides already followed
      if (op.is_load)
      {
        // Loaded from tracked memory into a new register.
        m_reg_autotrack.push_back(op.reg0);
        return HitType::SAVELOAD;
      }
      if (op.is_store)
      {
        // Tracked memory overwritten by something else (kept on the first hit).
        if (first_hit)
          return HitType::SAVELOAD;
        UntrackRange(m_mem_autotrack, address, op.memory_target_size);
        return HitType::OVERWRITE;
      }
      return HitType::SAVELOAD;
    }
    if (writes_tracked && op.is_store)
    {
      // The value is copied to memory: follow it there too.
      TrackRange(m_mem_autotrack, address, op.memory_target_size);
      return HitType::SAVELOAD;
    }
    if (writes_tracked && op.is_load)
    {
      // The register is replaced by unrelated memory (kept on the first hit).
      if (first_hit)
        return HitType::SAVELOAD;
      m_reg_autotrack.erase(destination);
      return HitType::OVERWRITE;
    }
    return HitType::SKIP;
  }

  if (reads_tracked && !writes_tracked)
  {
    // The value flows into a new register.
    m_reg_autotrack.push_back(op.reg0);
    return StartsWithAny(mnemonic, MOVE_PREFIXES) ? HitType::MOVED : HitType::ACTIVE;
  }
  if (writes_tracked && !reads_tracked)
  {
    // The tracked register gets a new value, unless the instruction keeps part of it.
    if (first_hit || StartsWithAny(mnemonic, PARTIAL_WRITE_PREFIXES))
      return HitType::UPDATED;
    m_reg_autotrack.erase(destination);
    return HitType::OVERWRITE;
  }
  if (writes_tracked && reads_tracked)
    return HitType::UPDATED;
  return HitType::SKIP;
}
