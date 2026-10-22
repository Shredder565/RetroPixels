// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QAbstractTableModel>
#include <QList>
#include <QVariant>

#include "Common/CommonTypes.h"
#include "Common/Lazy.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

namespace Core
{
enum class State;
class System;
}  // namespace Core
class JitInterface;
class PPCSymbolDB;
class QString;

namespace JitBlockTableModelColumn
{
enum EnumType : int
{
  PPCFeatureFlags = 0,
  EffectiveAddress,
  CodeBufferSize,
  RepeatInstructions,
  HostNearCodeSize,
  HostFarCodeSize,
  RunCount,
  CyclesSpent,
  CyclesAverage,
  CyclesPercent,
  TimeSpent,
  TimeAverage,
  TimePercent,
  Symbol,
  NumberOfColumns,
};
}

namespace JitBlockTableModelUserRole
{
enum EnumType : int
{
  SortRole = Qt::UserRole,
};
}

// The JIT's compiled blocks as a table, with profiling numbers. Rows are a snapshot of the block
// cache taken while emulation is paused; the model only listens for changes while it is shown.
class JitBlockTableModel final : public QAbstractTableModel
{
  Q_OBJECT

  using Column = JitBlockTableModelColumn::EnumType;
  using UserRole = JitBlockTableModelUserRole::EnumType;
  using JitBlockRefs = QList<std::reference_wrapper<const JitBlock>>;
  using SymbolListValueType = Common::Lazy<QVariant>;
  using SymbolList = QList<SymbolListValueType>;

public:
  explicit JitBlockTableModel(Core::System& system, JitInterface& jit_interface,
                              PPCSymbolDB& ppc_symbol_db, QObject* parent = nullptr);
  ~JitBlockTableModel() override;

  JitBlockTableModel(const JitBlockTableModel&) = delete;
  JitBlockTableModel(JitBlockTableModel&&) = delete;
  JitBlockTableModel& operator=(const JitBlockTableModel&) = delete;
  JitBlockTableModel& operator=(JitBlockTableModel&&) = delete;

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
  int columnCount(const QModelIndex& parent = QModelIndex{}) const override;
  bool removeRows(int row, int count, const QModelIndex& parent = QModelIndex{}) override;

  const JitBlock& GetJitBlock(const QModelIndex& index) const;
  const JitBlockRefs& GetJitBlockRefs() const { return m_blocks; }
  const SymbolList& GetSymbolList() const { return m_symbols; }

  // Always connected (driven by the owning widget).
  void OnShowSignal();
  void OnHideSignal();
  void OnSortIndicatorChanged(int logicalIndex, Qt::SortOrder order);
  void OnFilterSymbolTextChanged(const QString& string);

private:
  [[nodiscard]] QVariant TextFor(const QModelIndex& index) const;
  [[nodiscard]] QVariant AlignmentFor(const QModelIndex& index) const;
  [[nodiscard]] QVariant SortKeyFor(const QModelIndex& index) const;

  void TotalUpCosts();
  void LookUpSymbols();
  void Snapshot(Core::State state);
  void SnapshotNow();
  void Empty();
  void Listen(bool on);

  Core::System& m_system;
  JitInterface& m_jit;
  PPCSymbolDB& m_symbol_db;

  QList<QMetaObject::Connection> m_links;
  JitBlockRefs m_blocks;
  SymbolList m_symbols;
  u64 m_total_cycles = 0;
  JitBlock::ProfileData::Clock::duration m_total_time{};
  bool m_symbol_sort = false;
  bool m_symbol_filter = false;
};
