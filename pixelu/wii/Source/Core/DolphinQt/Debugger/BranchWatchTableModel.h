// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstdio>

#include <QAbstractTableModel>
#include <QFont>
#include <QList>
#include <QVariant>

#include "Common/SymbolDB.h"

namespace Core
{
class BranchWatch;
enum class BranchWatchSelectionInspection : u8;
struct BranchWatchSelectionValueType;
class CPUThreadGuard;
class System;
}  // namespace Core
class PPCSymbolDB;

namespace BranchWatchTableModelColumn
{
enum EnumType : int
{
  Instruction = 0,
  Condition,
  Origin,
  Destination,
  RecentHits,
  TotalHits,
  OriginSymbol,
  DestinSymbol,
  NumberOfColumns,
};
}

namespace BranchWatchTableModelUserRole
{
enum EnumType : int
{
  ClickRole = Qt::UserRole,
  SortRole,
};
}

// Symbol names and start addresses for both ends of one branch. A field is an invalid QVariant
// when no symbol covers that end.
struct BranchWatchTableModelSymbolListValueType
{
  explicit BranchWatchTableModelSymbolListValueType(const Common::Symbol* const origin_symbol,
                                                    const Common::Symbol* const destin_symbol);

  QVariant origin_name, origin_addr;
  QVariant destin_name, destin_addr;
};

// Presents the Branch Watch selection as a table. Row N is selection entry N; the symbol columns
// come from a side list that is only refreshed once recording has reached the reduction phase.
class BranchWatchTableModel final : public QAbstractTableModel
{
  Q_OBJECT

public:
  using Column = BranchWatchTableModelColumn::EnumType;
  using UserRole = BranchWatchTableModelUserRole::EnumType;
  using SymbolListValueType = BranchWatchTableModelSymbolListValueType;
  using SymbolList = QList<SymbolListValueType>;

  explicit BranchWatchTableModel(Core::System& system, Core::BranchWatch& branch_watch,
                                 PPCSymbolDB& ppc_symbol_db, QObject* parent = nullptr);
  ~BranchWatchTableModel() override;

  BranchWatchTableModel(const BranchWatchTableModel&) = delete;
  BranchWatchTableModel(BranchWatchTableModel&&) = delete;
  BranchWatchTableModel& operator=(const BranchWatchTableModel&) = delete;
  BranchWatchTableModel& operator=(BranchWatchTableModel&&) = delete;

  QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role = Qt::DisplayRole) const override;
  int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
  int columnCount(const QModelIndex& parent = QModelIndex{}) const override;
  bool removeRows(int row, int count, const QModelIndex& parent = QModelIndex{}) override;
  void setFont(const QFont& font);

  void OnClearBranchWatch(const Core::CPUThreadGuard& guard);
  void OnCodePathWasTaken(const Core::CPUThreadGuard& guard);
  void OnCodePathNotTaken(const Core::CPUThreadGuard& guard);
  void OnBranchWasOverwritten(const Core::CPUThreadGuard& guard);
  void OnBranchNotOverwritten(const Core::CPUThreadGuard& guard);
  void OnWipeRecentHits();
  void OnWipeInspection();
  void OnDebugFontChanged(const QFont& font);
  void OnPPCSymbolsChanged();

  void Save(const Core::CPUThreadGuard& guard, std::FILE* file) const;
  void Load(const Core::CPUThreadGuard& guard, std::FILE* file);
  void UpdateSymbols();
  void UpdateHits();
  void SetInspected(const QModelIndex& index);

  const Core::BranchWatchSelectionValueType&
  GetBranchWatchSelection(const QModelIndex& index) const;
  const SymbolList& GetSymbolList() const { return m_symbols; }

private:
  template <typename Change>
  void Reshape(Change&& change);
  void RefreshColumns(Column first, Column last, const QList<int>& roles);

  void Mark(int row, Column column, Core::BranchWatchSelectionInspection flag);
  void MarkOrigins(u32 address);
  void MarkDestinations(u32 address, bool from_symbol);
  void MarkSymbols(u32 address, bool from_destination);
  void RebuildSymbols();

  [[nodiscard]] bool IsMarked(const QModelIndex& index) const;
  [[nodiscard]] QVariant Text(const QModelIndex& index) const;
  [[nodiscard]] QVariant Styled(const QModelIndex& index) const;
  [[nodiscard]] QVariant Alignment(const QModelIndex& index) const;
  [[nodiscard]] QVariant Colour(const QModelIndex& index) const;
  [[nodiscard]] QVariant ClickTarget(const QModelIndex& index) const;
  [[nodiscard]] QVariant SortKey(const QModelIndex& index) const;

  Core::System& m_system;
  Core::BranchWatch& m_watch;
  PPCSymbolDB& m_symbol_db;

  SymbolList m_symbols;
  QFont m_base_font;
};
