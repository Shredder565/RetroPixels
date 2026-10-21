// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/JitBlockTableModel.h"

#include <array>
#include <chrono>
#include <span>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Settings.h"

namespace
{
using Column = JitBlockTableModelColumn::EnumType;

QString Dashes()
{
  return QStringLiteral(" --- ");
}

QVariant SymbolNameOrNothing(const Common::Symbol* symbol)
{
  if (!symbol)
    return QVariant{};
  return QString::fromStdString(symbol->name);
}

// "DR", "IR" and "PERFMON" joined by '|', in that order.
QString FeatureText(CPUEmuFeatureFlags flags)
{
  QStringList parts;
  if (flags & FEATURE_FLAG_MSR_DR)
    parts.append(QStringLiteral("DR"));
  if (flags & FEATURE_FLAG_MSR_IR)
    parts.append(QStringLiteral("IR"));
  if (flags & FEATURE_FLAG_PERFMON)
    parts.append(QStringLiteral("PERFMON"));
  return parts.join(QLatin1Char('|'));
}

// Guest instructions that are compiled more than once (the block's length minus the number of
// distinct instruction addresses).
size_t RepeatCount(const JitBlock& block)
{
  return block.originalSize -
         block.physical_addresses.get_stats().first / sizeof(UGeckoInstruction);
}

QString Percent(double share)
{
  return QStringLiteral("%1%").arg(share, 10, 'f', 6);
}
}  // namespace

JitBlockTableModel::JitBlockTableModel(Core::System& system, JitInterface& jit_interface,
                                       PPCSymbolDB& ppc_symbol_db, QObject* parent)
    : QAbstractTableModel(parent), m_system(system), m_jit(jit_interface),
      m_symbol_db(ppc_symbol_db)
{
}

JitBlockTableModel::~JitBlockTableModel() = default;

const JitBlock& JitBlockTableModel::GetJitBlock(const QModelIndex& index) const
{
  ASSERT(index.isValid());
  return m_blocks[index.row()];
}

void JitBlockTableModel::TotalUpCosts()
{
  m_total_cycles = 0;
  m_total_time = {};
  for (const JitBlock& block : m_blocks)
  {
    if (const auto* profile = block.profile_data.get())
    {
      m_total_cycles += profile->cycles_spent;
      m_total_time += profile->time_spent;
    }
  }
}

void JitBlockTableModel::LookUpSymbols()
{
  m_symbols.clear();
  m_symbols.reserve(m_blocks.size());

  // Sorting or filtering by symbol touches every row anyway, so resolve the names now; otherwise
  // each name is looked up the first time it is shown.
  const bool resolve_now = m_symbol_sort || m_symbol_filter;
  for (const JitBlock& block : m_blocks)
  {
    if (resolve_now)
    {
      m_symbols.emplace_back(
          SymbolNameOrNothing(m_symbol_db.GetSymbolFromAddr(block.effectiveAddress)));
    }
    else
    {
      m_symbols.emplace_back([this, &block] {
        return SymbolNameOrNothing(m_symbol_db.GetSymbolFromAddr(block.effectiveAddress));
      });
    }
  }
}

void JitBlockTableModel::Empty()
{
  emit layoutAboutToBeChanged();
  m_blocks.clear();
  m_symbols.clear();
  emit layoutChanged();
}

void JitBlockTableModel::Snapshot(Core::State state)
{
  emit layoutAboutToBeChanged();
  m_blocks.clear();
  // Blocks can only be read safely while the CPU is paused.
  if (state == Core::State::Paused)
  {
    m_blocks.reserve(static_cast<qsizetype>(m_jit.GetBlockCount()));
    m_jit.RunOnBlocks(Core::CPUThreadGuard{m_system},
                      [this](const JitBlock& block) { m_blocks.emplace_back(block); });
    TotalUpCosts();
  }
  LookUpSymbols();
  emit layoutChanged();
}

void JitBlockTableModel::SnapshotNow()
{
  Snapshot(Core::GetState(m_system));
}

void JitBlockTableModel::Listen(bool on)
{
  auto* const host = Host::GetInstance();
  auto* const settings = &Settings::Instance();
  if (!on)
  {
    for (const QMetaObject::Connection& link : m_links)
      disconnect(link);
    m_links.clear();
    return;
  }

  m_links.append(
      connect(host, &Host::JitCacheInvalidation, this, &JitBlockTableModel::SnapshotNow));
  // Wiping the profile only changes the numbers, not the rows.
  m_links.append(connect(host, &Host::JitProfileDataWiped, this, [this] {
    const int rows = rowCount();
    if (rows <= 0)
      return;
    TotalUpCosts();
    emit dataChanged(createIndex(0, Column::RunCount), createIndex(rows - 1, Column::TimePercent),
                     {Qt::DisplayRole});
  }));
  // Anything that may leave stale block references (including symbol loads, which can make
  // the HLE patcher invalidate blocks) takes a new snapshot.
  m_links.append(connect(host, &Host::UpdateDisasmDialog, this, &JitBlockTableModel::SnapshotNow));
  m_links.append(connect(host, &Host::PPCSymbolsChanged, this, &JitBlockTableModel::SnapshotNow));
  m_links.append(
      connect(host, &Host::PPCBreakpointsChanged, this, &JitBlockTableModel::SnapshotNow));
  m_links.append(
      connect(settings, &Settings::EmulationStateChanged, this, &JitBlockTableModel::Snapshot));
}

void JitBlockTableModel::OnShowSignal()
{
  Listen(true);
  // Catch up on whatever was missed while hidden.
  SnapshotNow();
}

void JitBlockTableModel::OnHideSignal()
{
  Listen(false);
  Empty();
}

void JitBlockTableModel::OnSortIndicatorChanged(int logicalIndex, Qt::SortOrder)
{
  m_symbol_sort = logicalIndex == Column::Symbol;
}

void JitBlockTableModel::OnFilterSymbolTextChanged(const QString& string)
{
  m_symbol_filter = !string.isEmpty();
}

QVariant JitBlockTableModel::TextFor(const QModelIndex& index) const
{
  const int col = index.column();
  if (col == Column::Symbol)
  {
    const QVariant& name = *m_symbols[index.row()];
    return name.isValid() ? name : QVariant(Dashes());
  }

  const JitBlock& block = m_blocks[index.row()];
  switch (col)
  {
  case Column::PPCFeatureFlags:
    return FeatureText(block.feature_flags);
  case Column::EffectiveAddress:
    return QString::number(block.effectiveAddress, 16);
  case Column::CodeBufferSize:
    return QString::number(block.originalSize * sizeof(UGeckoInstruction));
  case Column::RepeatInstructions:
    return QString::number(RepeatCount(block));
  case Column::HostNearCodeSize:
    return QString::number(block.near_end - block.near_begin);
  case Column::HostFarCodeSize:
    return QString::number(block.far_end - block.far_begin);
  default:
    break;
  }

  // The remaining columns are profiling numbers.
  const JitBlock::ProfileData* profile = block.profile_data.get();
  if (!profile)
    return Dashes();

  const bool never_ran = profile->run_count == 0;
  switch (col)
  {
  case Column::RunCount:
    return QString::number(profile->run_count);
  case Column::CyclesSpent:
    return QString::number(profile->cycles_spent);
  case Column::CyclesAverage:
    if (never_ran)
      return Dashes();
    return QString::number(static_cast<double>(profile->cycles_spent) /
                               static_cast<double>(profile->run_count),
                           'f', 6);
  case Column::CyclesPercent:
    if (m_total_cycles == 0)
      return Dashes();
    return Percent(100.0 * static_cast<double>(profile->cycles_spent) /
                   static_cast<double>(m_total_cycles));
  case Column::TimeSpent:
    return QString::number(
        std::chrono::duration_cast<std::chrono::nanoseconds>(profile->time_spent).count());
  case Column::TimeAverage:
  {
    if (never_ran)
      return Dashes();
    const double nanoseconds =
        std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(profile->time_spent)
            .count();
    return QString::number(nanoseconds / static_cast<double>(profile->run_count), 'f', 6);
  }
  case Column::TimePercent:
    if (m_total_time == JitBlock::ProfileData::Clock::duration{})
      return Dashes();
    return Percent(100.0 * static_cast<double>(profile->time_spent.count()) /
                   static_cast<double>(m_total_time.count()));
  default:
    return QVariant();
  }
}

QVariant JitBlockTableModel::AlignmentFor(const QModelIndex& index) const
{
  switch (index.column())
  {
  case Column::PPCFeatureFlags:
  case Column::EffectiveAddress:
    return Qt::AlignCenter;
  case Column::Symbol:
    return QVariant::fromValue(Qt::AlignLeft | Qt::AlignVCenter);
  default:
    // Every number column is right aligned.
    return QVariant::fromValue(Qt::AlignRight | Qt::AlignVCenter);
  }
}

QVariant JitBlockTableModel::SortKeyFor(const QModelIndex& index) const
{
  const int col = index.column();
  if (col == Column::Symbol)
    return *m_symbols[index.row()];

  const JitBlock& block = m_blocks[index.row()];
  switch (col)
  {
  case Column::PPCFeatureFlags:
    return block.feature_flags;
  case Column::EffectiveAddress:
    return block.effectiveAddress;
  case Column::CodeBufferSize:
    return static_cast<qulonglong>(block.originalSize);
  case Column::RepeatInstructions:
    return static_cast<qulonglong>(RepeatCount(block));
  case Column::HostNearCodeSize:
    return static_cast<qulonglong>(block.near_end - block.near_begin);
  case Column::HostFarCodeSize:
    return static_cast<qulonglong>(block.far_end - block.far_begin);
  default:
    break;
  }

  const JitBlock::ProfileData* profile = block.profile_data.get();
  if (!profile)
    return QVariant();

  // Percentages sort like the totals they are computed from.
  switch (col)
  {
  case Column::RunCount:
    return static_cast<qulonglong>(profile->run_count);
  case Column::CyclesSpent:
  case Column::CyclesPercent:
    return static_cast<qulonglong>(profile->cycles_spent);
  case Column::CyclesAverage:
    if (profile->run_count == 0)
      return QVariant();
    return static_cast<double>(profile->cycles_spent) / static_cast<double>(profile->run_count);
  case Column::TimeSpent:
  case Column::TimePercent:
    return static_cast<qulonglong>(profile->time_spent.count());
  case Column::TimeAverage:
    if (profile->run_count == 0)
      return QVariant();
    return static_cast<double>(profile->time_spent.count()) /
           static_cast<double>(profile->run_count);
  default:
    return QVariant();
  }
}

QVariant JitBlockTableModel::data(const QModelIndex& index, int role) const
{
  if (role == Qt::DisplayRole)
    return TextFor(index);
  if (role == Qt::TextAlignmentRole)
    return AlignmentFor(index);
  if (role == UserRole::SortRole)
    return SortKeyFor(index);
  return QVariant();
}

QVariant JitBlockTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
    return QVariant();

  // Short forms; the JIT widget's column menu shows the full names.
  static constexpr std::array<const char*, Column::NumberOfColumns> titles{
      // i18n: short header for the guest feature flags column
      QT_TR_NOOP("PPC Feat. Flags"),
      // i18n: short header for the effective address column
      QT_TR_NOOP("Eff. Address"),
      // i18n: short header for the code buffer size column
      QT_TR_NOOP("Code Buff. Size"),
      // i18n: short header for the repeated instructions column
      QT_TR_NOOP("Repeat Instr."),
      // i18n: short header for the near code size column
      QT_TR_NOOP("Host N. Size"),
      // i18n: short header for the far code size column
      QT_TR_NOOP("Host F. Size"),
      QT_TR_NOOP("Run Count"),
      QT_TR_NOOP("Cycles Spent"),
      // i18n: short header, average cycles per run
      QT_TR_NOOP("Cycles Avg."),
      // i18n: short header, share of all cycles
      QT_TR_NOOP("Cycles %"),
      QT_TR_NOOP("Time Spent (ns)"),
      // i18n: short header, average time per run in nanoseconds
      QT_TR_NOOP("Time Avg. (ns)"),
      // i18n: short header, share of all time
      QT_TR_NOOP("Time %"),
      QT_TR_NOOP("Symbol"),
  };
  return tr(titles[static_cast<size_t>(section)]);
}

int JitBlockTableModel::rowCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(m_blocks.size());
}

int JitBlockTableModel::columnCount(const QModelIndex& parent) const
{
  return parent.isValid() ? 0 : static_cast<int>(Column::NumberOfColumns);
}

bool JitBlockTableModel::removeRows(int row, int count, const QModelIndex& parent)
{
  if (parent.isValid() || row < 0)
    return false;
  if (count <= 0)
    return true;

  // Removing a row also throws the block out of the JIT cache.
  beginRemoveRows(parent, row, row + count - 1);
  for (const JitBlock& block : std::span{m_blocks.data() + row, static_cast<size_t>(count)})
    m_jit.EraseSingleBlock(block);
  m_blocks.remove(row, count);
  m_symbols.remove(row, count);
  endRemoveRows();
  return true;
}
