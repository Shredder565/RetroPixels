// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/BranchWatch.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"

namespace Core
{
namespace
{
// Snapshot files are plain text, one branch per line:
//   origin destination instruction total_hits hits_snapshot flags
// The last column packs everything needed to rebuild the collections and the selection:
//   bit 0      virtual (1) or physical (0) address space
//   bit 1      true path (1) or false path (0)
//   bit 2      part of the selection
//   bits 3-8   selection inspection flags
// The format has no version field, so the flags are written as a full 64-bit value to leave room.
using SnapshotFlags = unsigned long long;

static_assert(BranchWatchSelectionInspection::EndOfEnumeration ==
                  BranchWatchSelectionInspection{(1u << 5) + 1},
              "the inspection flags must fit in six bits");

constexpr SnapshotFlags FLAG_VIRTUAL = 1ULL << 0;
constexpr SnapshotFlags FLAG_TRUE_PATH = 1ULL << 1;
constexpr SnapshotFlags FLAG_SELECTED = 1ULL << 2;
constexpr unsigned INSPECTION_SHIFT = 3;
constexpr SnapshotFlags INSPECTION_MASK = 0x3FULL;

SnapshotFlags EncodeFlags(bool is_virtual, bool condition, bool selected,
                          BranchWatchSelectionInspection inspection)
{
  SnapshotFlags flags = 0;
  if (is_virtual)
    flags |= FLAG_VIRTUAL;
  if (condition)
    flags |= FLAG_TRUE_PATH;
  if (selected)
    flags |= FLAG_SELECTED;
  flags |= (static_cast<SnapshotFlags>(std::to_underlying(inspection)) & INSPECTION_MASK)
           << INSPECTION_SHIFT;
  return flags;
}

BranchWatchSelectionInspection DecodeInspection(SnapshotFlags flags)
{
  return static_cast<BranchWatchSelectionInspection>((flags >> INSPECTION_SHIFT) & INSPECTION_MASK);
}

PowerPC::RequestedAddressSpace AddressSpaceOf(bool is_virtual)
{
  return is_virtual ? PowerPC::RequestedAddressSpace::Virtual :
                      PowerPC::RequestedAddressSpace::Physical;
}
}  // namespace

void BranchWatch::Clear(const CPUThreadGuard&)
{
  m_selection.clear();
  for (Collection* part : {&m_collection_vt, &m_collection_vf, &m_collection_pt, &m_collection_pf})
    part->clear();
  m_blacklist_size = 0;
  m_recording_phase = Phase::Blacklist;
}

void BranchWatch::Save(const CPUThreadGuard&, std::FILE* file) const
{
  if (!CanSave())
  {
    ASSERT_MSG(CORE, false, "BranchWatch can not be saved.");
    return;
  }
  if (file == nullptr)
    return;

  // In the reduction phase only the selection matters; everything else was already ruled out.
  const bool only_selected = m_recording_phase == Phase::Reduction;

  const auto write_part = [&](const Collection& part, bool is_virtual, bool condition) {
    for (const Collection::value_type& entry : part)
    {
      const auto chosen =
          std::ranges::find(m_selection, &entry, &Selection::value_type::collection_ptr);
      const bool selected = chosen != m_selection.end();
      if (!selected && only_selected)
        continue;

      const SelectionInspection inspection = selected ? chosen->inspection : SelectionInspection{};
      fmt::println(file, "{:08x} {:08x} {:08x} {} {} {:x}", entry.first.origin_addr,
                   entry.first.destin_addr, entry.first.original_inst.hex, entry.second.total_hits,
                   entry.second.hits_snapshot,
                   EncodeFlags(is_virtual, condition, selected, inspection));
    }
  };

  // Taken paths first, virtual before physical.
  write_part(m_collection_vt, true, true);
  write_part(m_collection_pt, false, true);
  write_part(m_collection_vf, true, false);
  write_part(m_collection_pf, false, false);
}

void BranchWatch::Load(const CPUThreadGuard& guard, std::FILE* file)
{
  if (file == nullptr)
    return;

  Clear(guard);

  u32 origin = 0;
  u32 destination = 0;
  u32 instruction = 0;
  std::size_t total_hits = 0;
  std::size_t hits_snapshot = 0;
  SnapshotFlags flags = 0;
  while (std::fscanf(file, "%x %x %x %zu %zu %llx", &origin, &destination, &instruction,
                     &total_hits, &hits_snapshot, &flags) == 6)
  {
    const bool is_virtual = (flags & FLAG_VIRTUAL) != 0;
    const bool condition = (flags & FLAG_TRUE_PATH) != 0;

    Collection& part = GetCollection(is_virtual, condition);
    const BranchWatchCollectionKey key{{origin, destination}, instruction};
    const auto [where, inserted] =
        part.try_emplace(key, BranchWatchCollectionValue{total_hits, hits_snapshot});
    // A repeated line is ignored entirely.
    if (!inserted)
      continue;

    if ((flags & FLAG_SELECTED) != 0)
    {
      m_selection.push_back(
          BranchWatchSelectionValueType{&*where, is_virtual, condition, DecodeInspection(flags)});
    }
    else if (hits_snapshot != 0)
    {
      // Only meaningful for blacklist-phase snapshots, where a non-zero snapshot means
      // "blacklisted".
      ++m_blacklist_size;
    }
  }

  if (!m_selection.empty())
    m_recording_phase = Phase::Reduction;
}

void BranchWatch::IsolateHasExecuted(const CPUThreadGuard&)
{
  if (m_recording_phase == Phase::Reduction)
  {
    // Keep the branches that were hit since the last snapshot.
    std::erase_if(m_selection, [](const Selection::value_type& item) {
      BranchWatchCollectionValue& counts = item.collection_ptr->second;
      const bool unchanged = counts.total_hits == counts.hits_snapshot;
      if (!unchanged)
        counts.hits_snapshot = counts.total_hits;
      return unchanged;
    });
    return;
  }

  // Leaving the blacklist phase: everything that isn't blacklisted (snapshot still zero) becomes
  // the selection.
  m_selection.reserve(GetCollectionSize() - m_blacklist_size);
  const auto select_part = [this](Collection& part, bool is_virtual, bool condition) {
    for (Collection::value_type& entry : part)
    {
      if (entry.second.hits_snapshot != 0)
        continue;
      m_selection.push_back(
          BranchWatchSelectionValueType{&entry, is_virtual, condition, SelectionInspection{}});
      entry.second.hits_snapshot = entry.second.total_hits;
    }
  };
  select_part(m_collection_vt, true, true);
  select_part(m_collection_vf, true, false);
  select_part(m_collection_pt, false, true);
  select_part(m_collection_pf, false, false);
  m_recording_phase = Phase::Reduction;
}

void BranchWatch::IsolateNotExecuted(const CPUThreadGuard&)
{
  if (m_recording_phase == Phase::Reduction)
  {
    // Keep the branches that were not hit since the last snapshot.
    std::erase_if(m_selection, [](const Selection::value_type& item) {
      BranchWatchCollectionValue& counts = item.collection_ptr->second;
      const bool changed = counts.total_hits != counts.hits_snapshot;
      if (!changed)
        counts.hits_snapshot = counts.total_hits;
      return changed;
    });
    return;
  }

  // Blacklist everything seen so far.
  for (Collection* part : {&m_collection_vt, &m_collection_vf, &m_collection_pt, &m_collection_pf})
  {
    for (Collection::value_type& entry : *part)
      entry.second.hits_snapshot = entry.second.total_hits;
  }
  m_blacklist_size = GetCollectionSize();
}

void BranchWatch::IsolateOverwrittenShared(const CPUThreadGuard& guard,
                                           const std::function<bool(u32, u32)>& compare_func)
{
  if (Core::GetState(guard.GetSystem()) == Core::State::Uninitialized)
  {
    ASSERT_MSG(CORE, false, "Core is uninitialized.");
    return;
  }

  // compare_func(recorded instruction, instruction now in memory) tells which branches to drop.
  // Branches whose memory can't be read are kept.
  const auto should_drop = [&](const BranchWatchCollectionKey& key, bool is_virtual) {
    const auto current =
        PowerPC::MMU::HostTryReadInstruction(guard, key.origin_addr, AddressSpaceOf(is_virtual));
    return current.has_value() && compare_func(key.original_inst.hex, current->value);
  };

  if (m_recording_phase == Phase::Reduction)
  {
    std::erase_if(m_selection, [&](const Selection::value_type& item) {
      return should_drop(item.collection_ptr->first, item.is_virtual);
    });
    return;
  }

  // In the blacklist phase a non-zero hits_snapshot marks an entry as excluded, so dropped
  // entries simply get one (any non-zero value would do).
  const auto mark_part = [&](Collection& part, bool is_virtual) {
    for (Collection::value_type& entry : part)
    {
      if (entry.second.hits_snapshot == 0 && should_drop(entry.first, is_virtual))
        entry.second.hits_snapshot = ++m_blacklist_size;
    }
  };
  mark_part(m_collection_vt, true);
  mark_part(m_collection_vf, true);
  mark_part(m_collection_pt, false);
  mark_part(m_collection_pf, false);
}

void BranchWatch::IsolateWasOverwritten(const CPUThreadGuard& guard)
{
  // Drop the branches whose instruction is still the same.
  IsolateOverwrittenShared(guard, [](u32 recorded, u32 current) { return recorded == current; });
}

void BranchWatch::IsolateNotOverwritten(const CPUThreadGuard& guard)
{
  // Drop the branches whose instruction changed.
  IsolateOverwrittenShared(guard, [](u32 recorded, u32 current) { return recorded != current; });
}

void BranchWatch::UpdateHitsSnapshot()
{
  if (m_recording_phase != Phase::Reduction)
    return;
  for (const Selection::value_type& item : m_selection)
  {
    BranchWatchCollectionValue& counts = item.collection_ptr->second;
    counts.hits_snapshot = counts.total_hits;
  }
}

void BranchWatch::ClearSelectionInspection()
{
  for (Selection::value_type& item : m_selection)
    item.inspection = SelectionInspection{};
}

void BranchWatch::SetSelectedInspected(std::size_t idx, SelectionInspection inspection)
{
  m_selection[idx].inspection |= inspection;
}
}  // namespace Core
