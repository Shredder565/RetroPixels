// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/BranchWatchTableModel.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <QBrush>

#include "Common/Assert.h"
#include "Common/GekkoDisassembler.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/PowerPC/PPCSymbolDB.h"

namespace
{
using Inspection = Core::BranchWatchSelectionInspection;
using Entry = Core::BranchWatch::Selection::value_type;
using Record = Core::BranchWatch::Collection::value_type;

QVariant SymbolName(const Common::Symbol* symbol)
{
  if (symbol == nullptr)
    return {};
  return QString::fromStdString(symbol->name);
}

QVariant SymbolStart(const Common::Symbol* symbol)
{
  if (symbol == nullptr)
    return {};
  return QVariant::fromValue<u32>(symbol->address);
}

// The inspection flag that a click on the given column toggles, if that column has one.
std::optional<Inspection> FlagForColumn(int column)
{
  switch (column)
  {
  case BranchWatchTableModelColumn::Instruction:
    return Inspection::InvertBranchOption;
  case BranchWatchTableModelColumn::Condition:
    return Inspection::MakeUnconditional;
  case BranchWatchTableModelColumn::Origin:
    return Inspection::SetOriginNOP;
  case BranchWatchTableModelColumn::Destination:
    return Inspection::SetDestinBLR;
  case BranchWatchTableModelColumn::OriginSymbol:
    return Inspection::SetOriginSymbolBLR;
  case BranchWatchTableModelColumn::DestinSymbol:
    return Inspection::SetDestinSymbolBLR;
  default:
    return std::nullopt;
  }
}

// Only the opcode name: the disassembler separates it from the operands with a tab.
QString OpcodeName(u32 instruction)
{
  const std::string text = Common::GekkoDisassembler::Disassemble(instruction, 0);
  const std::size_t tab = text.find('\t');
  if (tab == std::string::npos)
    return QString::fromStdString(text);
  return QString::fromLatin1(text.c_str(), static_cast<qsizetype>(tab));
}

// Sort weight of the condition column: not taken < taken < unconditional.
int ConditionRank(const Entry& entry)
{
  if (!entry.condition)
    return 0;
  return BranchIsConditional(entry.collection_ptr->first.original_inst) ? 1 : 2;
}

qulonglong RecentHitCount(const Record& record)
{
  return qulonglong{record.second.total_hits - record.second.hits_snapshot};
}
}  // namespace

BranchWatchTableModelSymbolListValueType::BranchWatchTableModelSymbolListValueType(
    const Common::Symbol* const origin_symbol, const Common::Symbol* const destin_symbol)
    : origin_name(SymbolName(origin_symbol)), origin_addr(SymbolStart(origin_symbol)),
      destin_name(SymbolName(destin_symbol)), destin_addr(SymbolStart(destin_symbol))
{
}

BranchWatchTableModel::BranchWatchTableModel(Core::System& system,
                                             Core::BranchWatch& branch_watch,
                                             PPCSymbolDB& ppc_symbol_db, QObject* parent)
    : QAbstractTableModel(parent), m_system(system), m_watch(branch_watch),
      m_symbol_db(ppc_symbol_db)
{
}

BranchWatchTableModel::~BranchWatchTableModel() = default;

void BranchWatchTableModel::setFont(const QFont& font)
{
  m_base_font = font;
}

QVariant BranchWatchTableModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid())
    return {};

  if (role == Qt::DisplayRole)
    return Text(index);
  if (role == Qt::FontRole)
    return Styled(index);
  if (role == Qt::TextAlignmentRole)
    return Alignment(index);
  if (role == Qt::ForegroundRole)
    return Colour(index);
  if (role == UserRole::ClickRole)
    return ClickTarget(index);
  if (role == UserRole::SortRole)
    return SortKey(index);
  return {};
}

QVariant BranchWatchTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
    return {};

  static constexpr std::array<const char*, Column::NumberOfColumns> titles{
      QT_TR_NOOP("Instr."),
      QT_TR_NOOP("Cond."),
      QT_TR_NOOP("Origin"),
      QT_TR_NOOP("Destination"),
      QT_TR_NOOP("Recent Hits"),
      QT_TR_NOOP("Total Hits"),
      QT_TR_NOOP("Origin Symbol"),
      QT_TR_NOOP("Destination Symbol"),
  };
  return tr(titles[static_cast<std::size_t>(section)]);
}

int BranchWatchTableModel::rowCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_watch.GetSelection().size());
}

int BranchWatchTableModel::columnCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : int{Column::NumberOfColumns};
}

bool BranchWatchTableModel::removeRows(int row, int count, const QModelIndex& parent)
{
  if (row < 0 || parent.isValid())
    return false;
  if (count < 1)
    return true;

  // Qt's range is inclusive at both ends.
  beginRemoveRows(parent, row, row + count - 1);
  Core::BranchWatch::Selection& selection = m_watch.GetSelection();
  const auto first = selection.begin() + row;
  selection.erase(first, first + count);
  m_symbols.remove(row, count);
  endRemoveRows();
  return true;
}

// Runs a change that may reorder or drop selection rows, then rebuilds the symbol side list.
template <typename Change>
void BranchWatchTableModel::Reshape(Change&& change)
{
  emit layoutAboutToBeChanged();
  std::forward<Change>(change)();
  RebuildSymbols();
  emit layoutChanged();
}

void BranchWatchTableModel::OnClearBranchWatch(const Core::CPUThreadGuard& guard)
{
  emit layoutAboutToBeChanged();
  m_watch.Clear(guard);
  m_symbols.clear();
  emit layoutChanged();
}

void BranchWatchTableModel::OnCodePathWasTaken(const Core::CPUThreadGuard& guard)
{
  Reshape([&] { m_watch.IsolateHasExecuted(guard); });
}

void BranchWatchTableModel::OnCodePathNotTaken(const Core::CPUThreadGuard& guard)
{
  Reshape([&] { m_watch.IsolateNotExecuted(guard); });
}

void BranchWatchTableModel::OnBranchWasOverwritten(const Core::CPUThreadGuard& guard)
{
  Reshape([&] { m_watch.IsolateWasOverwritten(guard); });
}

void BranchWatchTableModel::OnBranchNotOverwritten(const Core::CPUThreadGuard& guard)
{
  Reshape([&] { m_watch.IsolateNotOverwritten(guard); });
}

void BranchWatchTableModel::Load(const Core::CPUThreadGuard& guard, std::FILE* file)
{
  Reshape([&] { m_watch.Load(guard, file); });
}

void BranchWatchTableModel::Save(const Core::CPUThreadGuard& guard, std::FILE* file) const
{
  m_watch.Save(guard, file);
}

void BranchWatchTableModel::RefreshColumns(Column first, Column last, const QList<int>& roles)
{
  const int bottom = rowCount() - 1;
  emit dataChanged(createIndex(0, first), createIndex(bottom, last), roles);
}

// The four refreshers below do nothing at all (not even their side effect) on an empty table.

void BranchWatchTableModel::OnWipeRecentHits()
{
  if (rowCount() < 1)
    return;
  m_watch.UpdateHitsSnapshot();
  RefreshColumns(Column::RecentHits, Column::RecentHits, {Qt::DisplayRole});
}

void BranchWatchTableModel::OnWipeInspection()
{
  if (rowCount() < 1)
    return;
  m_watch.ClearSelectionInspection();
  const QList<int> roles{Qt::FontRole, Qt::ForegroundRole};
  RefreshColumns(Column::Origin, Column::Destination, roles);
  RefreshColumns(Column::OriginSymbol, Column::DestinSymbol, roles);
}

void BranchWatchTableModel::UpdateSymbols()
{
  if (rowCount() < 1)
    return;
  RebuildSymbols();
  RefreshColumns(Column::OriginSymbol, Column::DestinSymbol, {Qt::DisplayRole});
}

void BranchWatchTableModel::UpdateHits()
{
  if (rowCount() < 1)
    return;
  RefreshColumns(Column::RecentHits, Column::TotalHits, {Qt::DisplayRole});
}

void BranchWatchTableModel::OnDebugFontChanged(const QFont& font)
{
  setFont(font);
}

void BranchWatchTableModel::OnPPCSymbolsChanged()
{
  UpdateSymbols();
}

void BranchWatchTableModel::Mark(int row, Column column, Inspection flag)
{
  m_watch.SetSelectedInspected(static_cast<std::size_t>(row), flag);
  const QModelIndex cell = createIndex(row, column);
  emit dataChanged(cell, cell, {Qt::FontRole, Qt::ForegroundRole});
}

void BranchWatchTableModel::SetInspected(const QModelIndex& index)
{
  const int row = index.row();
  const Column column = static_cast<Column>(index.column());
  switch (column)
  {
  case Column::Instruction:
  case Column::Condition:
    // These two only ever affect the clicked cell.
    Mark(row, column, *FlagForColumn(column));
    break;
  case Column::Origin:
    MarkOrigins(GetBranchWatchSelection(index).collection_ptr->first.origin_addr);
    break;
  case Column::Destination:
    MarkDestinations(GetBranchWatchSelection(index).collection_ptr->first.destin_addr, false);
    break;
  case Column::OriginSymbol:
    MarkSymbols(m_symbols[row].origin_addr.value<u32>(), false);
    break;
  case Column::DestinSymbol:
    MarkSymbols(m_symbols[row].destin_addr.value<u32>(), false);
    break;
  default:
    break;
  }
}

// NOP-ing an origin affects every row that branches from that address.
void BranchWatchTableModel::MarkOrigins(u32 address)
{
  const Core::BranchWatch::Selection& selection = m_watch.GetSelection();
  for (std::size_t row = 0; row < selection.size(); ++row)
  {
    if (selection[row].collection_ptr->first.origin_addr == address)
      Mark(static_cast<int>(row), Column::Origin, Inspection::SetOriginNOP);
  }
}

// A BLR written at a destination also lands at the start of any symbol beginning there, and the
// other way round; the flag stops the two from bouncing back and forth.
void BranchWatchTableModel::MarkDestinations(u32 address, bool from_symbol)
{
  const Core::BranchWatch::Selection& selection = m_watch.GetSelection();
  for (std::size_t row = 0; row < selection.size(); ++row)
  {
    if (selection[row].collection_ptr->first.destin_addr == address)
      Mark(static_cast<int>(row), Column::Destination, Inspection::SetDestinBLR);
  }
  if (!from_symbol)
    MarkSymbols(address, true);
}

void BranchWatchTableModel::MarkSymbols(u32 address, bool from_destination)
{
  const auto starts_at = [address](const QVariant& start) {
    return start.isValid() && start.value<u32>() == address;
  };
  for (qsizetype row = 0; row < m_symbols.size(); ++row)
  {
    const SymbolListValueType& symbols = m_symbols[row];
    if (starts_at(symbols.origin_addr))
      Mark(static_cast<int>(row), Column::OriginSymbol, Inspection::SetOriginSymbolBLR);
    if (starts_at(symbols.destin_addr))
      Mark(static_cast<int>(row), Column::DestinSymbol, Inspection::SetDestinSymbolBLR);
  }
  if (!from_destination)
    MarkDestinations(address, true);
}

const Core::BranchWatchSelectionValueType&
BranchWatchTableModel::GetBranchWatchSelection(const QModelIndex& index) const
{
  ASSERT(index.isValid());
  return m_watch.GetSelection()[static_cast<std::size_t>(index.row())];
}

void BranchWatchTableModel::RebuildSymbols()
{
  // Before the reduction phase the selection is not meaningful, so the list is left alone.
  if (m_watch.GetRecordingPhase() != Core::BranchWatch::Phase::Reduction)
    return;

  const Core::BranchWatch::Selection& selection = m_watch.GetSelection();
  m_symbols.clear();
  m_symbols.reserve(static_cast<qsizetype>(selection.size()));
  for (const Entry& entry : selection)
  {
    const auto& key = entry.collection_ptr->first;
    m_symbols.emplace_back(m_symbol_db.GetSymbolFromAddr(key.origin_addr),
                           m_symbol_db.GetSymbolFromAddr(key.destin_addr));
  }
}

bool BranchWatchTableModel::IsMarked(const QModelIndex& index) const
{
  // Columns without a flag never touch the selection here.
  const std::optional<Inspection> flag = FlagForColumn(index.column());
  if (!flag)
    return false;
  const Inspection marks = GetBranchWatchSelection(index).inspection;
  return (marks & *flag) != Inspection{};
}

QVariant BranchWatchTableModel::Text(const QModelIndex& index) const
{
  const int column = index.column();
  if (column == Column::OriginSymbol || column == Column::DestinSymbol)
  {
    const SymbolListValueType& symbols = m_symbols[index.row()];
    const QVariant& name =
        column == Column::OriginSymbol ? symbols.origin_name : symbols.destin_name;
    return name.isValid() ? name : QVariant{QStringLiteral(" --- ")};
  }

  const Entry& entry = GetBranchWatchSelection(index);
  const Record& record = *entry.collection_ptr;
  switch (column)
  {
  case Column::Instruction:
    return OpcodeName(record.first.original_inst.hex);
  case Column::Condition:
    if (!entry.condition)
      return tr("false");
    if (BranchIsConditional(record.first.original_inst))
      return tr("true");
    return QStringLiteral("");
  case Column::Origin:
    return QString::number(record.first.origin_addr, 16);
  case Column::Destination:
    return QString::number(record.first.destin_addr, 16);
  case Column::RecentHits:
    return QString::number(RecentHitCount(record));
  case Column::TotalHits:
    return QString::number(qulonglong{record.second.total_hits});
  default:
    break;
  }
  static_assert(Column::NumberOfColumns == 8);
  std::unreachable();
}

QVariant BranchWatchTableModel::Styled(const QModelIndex& index) const
{
  QFont styled = m_base_font;
  styled.setBold(IsMarked(index));
  return styled;
}

QVariant BranchWatchTableModel::Alignment(const QModelIndex& index) const
{
  const int column = index.column();
  static_assert(Column::NumberOfColumns == 8);
  if (column == Column::Condition || column == Column::Origin || column == Column::Destination)
    return int{Qt::AlignCenter};
  // A combined alignment is a QFlags value, which has to be wrapped explicitly.
  if (column == Column::RecentHits || column == Column::TotalHits)
    return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
  if (column == Column::Instruction || column == Column::OriginSymbol ||
      column == Column::DestinSymbol)
  {
    return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
  }
  std::unreachable();
}

QVariant BranchWatchTableModel::Colour(const QModelIndex& index) const
{
  if (!IsMarked(index))
    return {};
  return QBrush(Qt::red);
}

QVariant BranchWatchTableModel::ClickTarget(const QModelIndex& index) const
{
  const int column = index.column();
  if (column == Column::OriginSymbol)
    return m_symbols[index.row()].origin_addr;
  if (column == Column::DestinSymbol)
    return m_symbols[index.row()].destin_addr;

  const auto& key = GetBranchWatchSelection(index).collection_ptr->first;
  switch (column)
  {
  case Column::Instruction:
    return QVariant::fromValue<u32>(key.original_inst.hex);
  case Column::Origin:
    return QVariant::fromValue<u32>(key.origin_addr);
  case Column::Destination:
    return QVariant::fromValue<u32>(key.destin_addr);
  default:
    return {};
  }
}

QVariant BranchWatchTableModel::SortKey(const QModelIndex& index) const
{
  const int column = index.column();
  if (column == Column::OriginSymbol)
    return m_symbols[index.row()].origin_name;
  if (column == Column::DestinSymbol)
    return m_symbols[index.row()].destin_name;

  // Counts are widened to qulonglong: QVariant has no constructor for std::size_t as such.
  const Entry& entry = GetBranchWatchSelection(index);
  const Record& record = *entry.collection_ptr;
  switch (column)
  {
  case Column::Instruction:
    return OpcodeName(record.first.original_inst.hex);
  case Column::Condition:
    return ConditionRank(entry);
  case Column::Origin:
    return QVariant::fromValue<u32>(record.first.origin_addr);
  case Column::Destination:
    return QVariant::fromValue<u32>(record.first.destin_addr);
  case Column::RecentHits:
    return RecentHitCount(record);
  case Column::TotalHits:
    return qulonglong{record.second.total_hits};
  default:
    break;
  }
  static_assert(Column::NumberOfColumns == 8);
  std::unreachable();
}
