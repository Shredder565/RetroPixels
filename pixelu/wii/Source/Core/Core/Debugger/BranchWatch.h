// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Branch Watch records every branch the CPU takes (or doesn't take) together with the
// instruction, so that the debugger can narrow them down to the few that react to some input.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/System.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Core
{
// Origin and destination packed into one 64-bit value, so the JITs can pass both in one register.
struct FakeBranchWatchCollectionKey
{
  u32 origin_addr;
  u32 destin_addr;

  constexpr operator u64() const { return std::bit_cast<u64>(*this); }
};
struct BranchWatchCollectionKey : FakeBranchWatchCollectionKey
{
  UGeckoInstruction original_inst;
};
struct BranchWatchCollectionValue
{
  std::size_t total_hits = 0;
  std::size_t hits_snapshot = 0;
};
}  // namespace Core

template <> struct std::hash<Core::BranchWatchCollectionKey>
{
  std::size_t operator()(const Core::BranchWatchCollectionKey& key) const noexcept
  {
    // The instruction is left out: the same branch site practically never changes instruction.
    const u64 addresses = static_cast<const Core::FakeBranchWatchCollectionKey&>(key);
    return std::hash<u64>{}(addresses);
  }
};

namespace Core
{
inline bool operator==(const BranchWatchCollectionKey& lhs,
                       const BranchWatchCollectionKey& rhs) noexcept
{
  if (lhs.original_inst.hex != rhs.original_inst.hex)
    return false;
  const std::hash<BranchWatchCollectionKey> key_hash{};
  return key_hash(lhs) == key_hash(rhs);
}

enum class BranchWatchSelectionInspection : u8
{
  SetOriginNOP = 1u << 0,
  SetDestinBLR = 1u << 1,
  SetOriginSymbolBLR = 1u << 2,
  SetDestinSymbolBLR = 1u << 3,
  InvertBranchOption = 1u << 4,  // flips the condition and/or the CTR test
  MakeUnconditional = 1u << 5,
  EndOfEnumeration,
};

constexpr BranchWatchSelectionInspection operator|(BranchWatchSelectionInspection lhs,
                                                   BranchWatchSelectionInspection rhs)
{
  return static_cast<BranchWatchSelectionInspection>(std::to_underlying(lhs) |
                                                     std::to_underlying(rhs));
}

constexpr BranchWatchSelectionInspection operator&(BranchWatchSelectionInspection lhs,
                                                   BranchWatchSelectionInspection rhs)
{
  return static_cast<BranchWatchSelectionInspection>(std::to_underlying(lhs) &
                                                     std::to_underlying(rhs));
}

constexpr BranchWatchSelectionInspection& operator|=(BranchWatchSelectionInspection& self,
                                                     BranchWatchSelectionInspection other)
{
  return self = self | other;
}

using BranchWatchCollection =
    std::unordered_map<BranchWatchCollectionKey, BranchWatchCollectionValue>;

struct BranchWatchSelectionValueType
{
  using Inspection = BranchWatchSelectionInspection;

  BranchWatchCollection::value_type* collection_ptr;
  bool is_virtual;
  bool condition;
  // What the user did to this branch from the GUI; kept here so it is saved with the snapshot.
  Inspection inspection;
};

using BranchWatchSelection = std::vector<BranchWatchSelectionValueType>;

enum class BranchWatchPhase : bool
{
  Blacklist,
  Reduction,
};

class BranchWatch final
{
public:
  using Collection = BranchWatchCollection;
  using Selection = BranchWatchSelection;
  using Phase = BranchWatchPhase;
  using SelectionInspection = BranchWatchSelectionInspection;

  bool GetRecordingActive() const { return m_recording_active; }
  void SetRecordingActive(const CPUThreadGuard& guard, bool active)
  {
    m_recording_active = active;
    // JIT blocks are compiled with or without the recording calls, so they must be rebuilt.
    guard.GetSystem().GetJitInterface().ClearCache(guard);
  }
  void Clear(const CPUThreadGuard& guard);

  void Save(const CPUThreadGuard& guard, std::FILE* file) const;
  void Load(const CPUThreadGuard& guard, std::FILE* file);

  void IsolateHasExecuted(const CPUThreadGuard& guard);
  void IsolateNotExecuted(const CPUThreadGuard& guard);
  void IsolateWasOverwritten(const CPUThreadGuard& guard);
  void IsolateNotOverwritten(const CPUThreadGuard& guard);
  void UpdateHitsSnapshot();
  void ClearSelectionInspection();
  void SetSelectedInspected(std::size_t idx, SelectionInspection inspection);

  Selection& GetSelection() { return m_selection; }
  const Selection& GetSelection() const { return m_selection; }

  std::size_t GetCollectionSize() const
  {
    std::size_t total = 0;
    for (const Collection* part :
         {&m_collection_vt, &m_collection_vf, &m_collection_pt, &m_collection_pf})
      total += part->size();
    return total;
  }
  std::size_t GetBlacklistSize() const { return m_blacklist_size; }
  Phase GetRecordingPhase() const { return m_recording_phase; }

  // A snapshot only stores selected entries in the reduction phase, so a reduction phase with
  // nothing selected would load back as a blacklist phase.
  bool CanSave() const { return m_recording_phase != Phase::Reduction || !m_selection.empty(); }

  // The Hit functions run on the CPU thread. The JITs call the static ones directly (they can't
  // call member functions), and the _fk variants take origin and destination as one packed
  // FakeBranchWatchCollectionKey. The _n variants count n hits at once (used by the dcbx code).
  // There are no false-path _n variants because nothing needs them.
  static void HitVirtualTrue_fk(BranchWatch* branch_watch, u64 fake_key, u32 inst)
  {
    Count(branch_watch->m_collection_vt, fake_key, inst, 1);
  }
  static void HitVirtualFalse_fk(BranchWatch* branch_watch, u64 fake_key, u32 inst)
  {
    Count(branch_watch->m_collection_vf, fake_key, inst, 1);
  }
  static void HitPhysicalTrue_fk(BranchWatch* branch_watch, u64 fake_key, u32 inst)
  {
    Count(branch_watch->m_collection_pt, fake_key, inst, 1);
  }
  static void HitPhysicalFalse_fk(BranchWatch* branch_watch, u64 fake_key, u32 inst)
  {
    Count(branch_watch->m_collection_pf, fake_key, inst, 1);
  }
  static void HitVirtualTrue_fk_n(BranchWatch* branch_watch, u64 fake_key, u32 inst, u32 n)
  {
    Count(branch_watch->m_collection_vt, fake_key, inst, n);
  }
  static void HitPhysicalTrue_fk_n(BranchWatch* branch_watch, u64 fake_key, u32 inst, u32 n)
  {
    Count(branch_watch->m_collection_pt, fake_key, inst, n);
  }

  static void HitVirtualTrue(BranchWatch* branch_watch, u32 origin, u32 destination, u32 inst)
  {
    Count(branch_watch->m_collection_vt, Pack(origin, destination), inst, 1);
  }
  static void HitVirtualFalse(BranchWatch* branch_watch, u32 origin, u32 destination, u32 inst)
  {
    Count(branch_watch->m_collection_vf, Pack(origin, destination), inst, 1);
  }
  static void HitPhysicalTrue(BranchWatch* branch_watch, u32 origin, u32 destination, u32 inst)
  {
    Count(branch_watch->m_collection_pt, Pack(origin, destination), inst, 1);
  }
  static void HitPhysicalFalse(BranchWatch* branch_watch, u32 origin, u32 destination, u32 inst)
  {
    Count(branch_watch->m_collection_pf, Pack(origin, destination), inst, 1);
  }

  // `translate` tells whether the addresses are effective (virtual) or physical ones.
  void HitTrue(u32 origin, u32 destination, UGeckoInstruction inst, bool translate)
  {
    Count(translate ? m_collection_vt : m_collection_pt, Pack(origin, destination), inst.hex, 1);
  }
  void HitFalse(u32 origin, u32 destination, UGeckoInstruction inst, bool translate)
  {
    Count(translate ? m_collection_vf : m_collection_pf, Pack(origin, destination), inst.hex, 1);
  }

private:
  static constexpr u64 Pack(u32 origin, u32 destination)
  {
    return FakeBranchWatchCollectionKey{origin, destination};
  }
  static void Count(Collection& collection, u64 fake_key, u32 inst, u32 hits)
  {
    const BranchWatchCollectionKey key{std::bit_cast<FakeBranchWatchCollectionKey>(fake_key), inst};
    collection[key].total_hits += hits;
  }

  Collection& GetCollectionV(bool condition)
  {
    return condition ? m_collection_vt : m_collection_vf;
  }
  Collection& GetCollectionP(bool condition)
  {
    return condition ? m_collection_pt : m_collection_pf;
  }
  Collection& GetCollection(bool is_virtual, bool condition)
  {
    return is_virtual ? GetCollectionV(condition) : GetCollectionP(condition);
  }

  void IsolateOverwrittenShared(const CPUThreadGuard& guard,
                                const std::function<bool(u32, u32)>& compare_func);

  std::size_t m_blacklist_size = 0;
  Phase m_recording_phase = Phase::Blacklist;
  bool m_recording_active = false;
  // One collection per address space (virtual/physical) and outcome (taken/not taken).
  Collection m_collection_vt;
  Collection m_collection_vf;
  Collection m_collection_pt;
  Collection m_collection_pf;
  Selection m_selection;
};
}  // namespace Core
