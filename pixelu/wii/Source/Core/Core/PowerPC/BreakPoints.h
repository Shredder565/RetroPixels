// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Debugger breakpoints: instruction breakpoints (BreakPoints) and memory
// watchpoints (MemChecks). Both can be saved to and loaded from the list of
// strings kept in a game's debugger settings.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Core/PowerPC/Expression.h"

namespace Core
{
class System;
}

struct TBreakPoint
{
  u32 address = 0;
  bool is_enabled = false;
  bool log_on_hit = false;
  bool break_on_hit = false;
  std::optional<Expression> condition;
};

struct TMemCheck
{
  // Inclusive range; equal for a single address.
  u32 start_address = 0;
  u32 end_address = 0;

  bool is_enabled = true;
  bool is_ranged = false;

  bool is_break_on_read = false;
  bool is_break_on_write = false;

  bool log_on_hit = false;
  bool break_on_hit = false;

  u32 num_hits = 0;

  std::optional<Expression> condition;

  // Called for an access that hits the range. Logs if asked to and returns
  // whether emulation should stop.
  bool Action(Core::System& system, u64 value, u32 addr, bool write, size_t size, u32 pc);
};

class BreakPoints
{
public:
  explicit BreakPoints(Core::System& system);
  BreakPoints(const BreakPoints& other) = delete;
  BreakPoints(BreakPoints&& other) = delete;
  BreakPoints& operator=(const BreakPoints& other) = delete;
  BreakPoints& operator=(BreakPoints&& other) = delete;
  ~BreakPoints();

  using TBreakPoints = std::vector<TBreakPoint>;
  using TBreakPointsStr = std::vector<std::string>;

  const TBreakPoints& GetBreakPoints() const { return m_breakpoints; }
  TBreakPointsStr GetStrings() const;
  void AddFromStrings(const TBreakPointsStr& bp_strings);

  bool IsAddressBreakPoint(u32 address) const;
  bool IsBreakPointEnable(u32 address) const;
  // The breakpoint that applies at an address; the temporary one wins.
  const TBreakPoint* GetBreakpoint(u32 address) const;
  // Only the regular (saved) breakpoints, for editing.
  const TBreakPoint* GetRegularBreakpoint(u32 address) const;

  // Adds a breakpoint; an existing one at the same address is replaced but
  // keeps its enabled state.
  void Add(u32 address, bool break_on_hit, bool log_on_hit, std::optional<Expression> condition);
  void Add(u32 address);
  // Adds a breakpoint unless the address already has one.
  void Add(TBreakPoint bp);
  // The one-shot breakpoint used by "step over" and "run to here". It may
  // share an address with a regular breakpoint, and it is cleared whenever
  // emulation pauses.
  void SetTemporary(u32 address);

  bool ToggleBreakPoint(u32 address);
  bool ToggleEnable(u32 address);

  void EnableBreaking(bool enable);
  bool IsBreakingEnabled() const { return m_breaking_enabled; }

  // Returns whether a breakpoint was removed.
  bool Remove(u32 address);
  void Clear();
  void ClearTemporary();

private:
  TBreakPoints m_breakpoints;
  std::optional<TBreakPoint> m_temp_breakpoint;
  Core::System& m_system;
  bool m_breaking_enabled = true;
};

class DelayedMemCheckUpdate;

class MemChecks
{
public:
  explicit MemChecks(Core::System& system);
  MemChecks(const MemChecks& other) = delete;
  MemChecks(MemChecks&& other) = delete;
  MemChecks& operator=(const MemChecks& other) = delete;
  MemChecks& operator=(MemChecks&& other) = delete;
  ~MemChecks();

  using TMemChecks = std::vector<TMemCheck>;
  using TMemChecksStr = std::vector<std::string>;

  const TMemChecks& GetMemChecks() const { return m_mem_checks; }
  TMemChecksStr GetStrings() const;
  void AddFromStrings(const TMemChecksStr& mc_strings);

  // Adding and removing only mark the JIT/MMU refresh as needed; it happens
  // when the returned object goes away, so several changes cost one refresh.
  DelayedMemCheckUpdate Add(TMemCheck memory_check);

  bool ToggleEnable(u32 address);

  TMemCheck* GetMemCheck(u32 address, size_t size = 1);
  // Whether any watchpoint touches the length-aligned block holding address.
  bool OverlapsMemcheck(u32 address, u32 length) const;
  DelayedMemCheckUpdate Remove(u32 address);

  void EnableBreaking(bool enable);
  bool IsBreakingEnabled() const { return m_breaking_enabled; }

  void Update();
  void Clear();
  bool HasAny() const { return !m_mem_checks.empty() && m_breaking_enabled; }

  BitSet32 GetGPRsUsedInConditions() { return m_gprs_used_in_conditions; }
  BitSet32 GetFPRsUsedInConditions() { return m_fprs_used_in_conditions; }

private:
  // Returns whether the register sets changed.
  bool UpdateRegistersUsedInConditions();

  TMemChecks m_mem_checks;
  Core::System& m_system;
  BitSet32 m_gprs_used_in_conditions;
  BitSet32 m_fprs_used_in_conditions;
  bool m_mem_breakpoints_set = false;
  bool m_breaking_enabled = true;
};

// Runs MemChecks::Update() on destruction if any change it collected asked
// for one.
class DelayedMemCheckUpdate final
{
public:
  DelayedMemCheckUpdate(MemChecks* memchecks, bool update_needed = false)
      : m_memchecks(memchecks), m_update_needed(update_needed)
  {
  }

  DelayedMemCheckUpdate(const DelayedMemCheckUpdate&) = delete;
  DelayedMemCheckUpdate(DelayedMemCheckUpdate&& other) = delete;
  DelayedMemCheckUpdate& operator=(const DelayedMemCheckUpdate&) = delete;
  DelayedMemCheckUpdate& operator=(DelayedMemCheckUpdate&& other) = delete;

  ~DelayedMemCheckUpdate()
  {
    if (m_update_needed)
      m_memchecks->Update();
  }

  // Takes over the other object's pending update (same MemChecks only).
  DelayedMemCheckUpdate& operator|=(DelayedMemCheckUpdate&& other)
  {
    if (other.m_memchecks != m_memchecks)
      return *this;
    if (other.m_update_needed)
      m_update_needed = true;
    other.m_update_needed = false;
    return *this;
  }

  operator bool() const { return m_update_needed; }

private:
  MemChecks* m_memchecks;
  bool m_update_needed;
};
