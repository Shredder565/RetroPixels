// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/JITWidget.h"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <utility>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

#include <fmt/ostream.h>

#include "Common/GekkoDisassembler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"

#include "DolphinQt/Debugger/JitBlockTableModel.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ClickableStatusBar.h"
#include "DolphinQt/QtUtils/FromStdString.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"
#include "UICommon/UICommon.h"

// Filters the block table by effective address range, a covered physical address and a symbol name
// substring.
class JitBlockProxyModel final : public QSortFilterProxyModel
{
public:
  enum class AddressFilter
  {
    MinEffective,
    MaxEffective,
    CoversPhysical,
  };

  using QSortFilterProxyModel::QSortFilterProxyModel;

  void SetBlocks(JitBlockTableModel* blocks)
  {
    m_blocks = blocks;
    QSortFilterProxyModel::setSourceModel(blocks);
  }

  const JitBlock& GetJitBlock(const QModelIndex& index) const
  {
    return m_blocks->GetJitBlock(mapToSource(index));
  }

  void SetSymbolText(const QString& text)
  {
    m_symbol_part = text;
    invalidateRowsFilter();
  }

  // An empty or non-hex text removes that filter.
  void SetAddressText(AddressFilter which, const QString& text)
  {
    bool ok = false;
    const u32 value = text.toUInt(&ok, 16);
    std::optional<u32>& slot = which == AddressFilter::MinEffective ? m_min_effective :
                               which == AddressFilter::MaxEffective ? m_max_effective :
                                                                      m_covered_physical;
    slot = ok ? std::optional<u32>(value) : std::nullopt;
    invalidateRowsFilter();
  }

protected:
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override
  {
    if (source_parent.isValid())
      return false;

    if (!m_symbol_part.isEmpty())
    {
      const QVariant& name = *m_blocks->GetSymbolList()[source_row];
      if (!name.isValid() || !name.toString().contains(m_symbol_part, Qt::CaseInsensitive))
        return false;
    }

    const JitBlock& block = m_blocks->GetJitBlockRefs()[source_row];
    if (m_min_effective && block.effectiveAddress < *m_min_effective)
      return false;
    if (m_max_effective && block.effectiveAddress > *m_max_effective)
      return false;
    if (m_covered_physical && !block.physical_addresses.contains(*m_covered_physical))
      return false;
    return true;
  }

private:
  JitBlockTableModel* m_blocks = nullptr;
  std::optional<u32> m_min_effective;
  std::optional<u32> m_max_effective;
  std::optional<u32> m_covered_physical;
  QString m_symbol_part;
};

namespace
{
// Guest instructions of a block, one per line, with a symbol description line before each run of
// consecutive addresses.
void WriteGuestListing(const JitBlock& block, PPCSymbolDB& symbols, std::ostream& out)
{
  // Instruction addresses are multiples of 4, so 1 never matches the first one.
  u32 expected = 1;
  for (const auto& [address, instruction] : block.original_buffer)
  {
    if (address != expected)
      out << symbols.GetDescription(address) << '\n';
    fmt::print(out, "0x{:08x}\t{}\n", address,
               Common::GekkoDisassembler::Disassemble(instruction.hex, address));
    expected = address + static_cast<u32>(sizeof(UGeckoInstruction));
  }
}

// Takes what was written so far and leaves the stream empty for the next listing.
QString Drain(std::ostringstream& stream)
{
  return QString::fromStdString(std::move(stream).str());
}
}  // namespace

JITWidget::JITWidget(Core::System& system, QWidget* parent) : QDockWidget(parent), m_system(system)
{
  setWindowTitle(tr("JIT Blocks"));
  setObjectName(QStringLiteral("jitwidget"));
  setAllowedAreas(Qt::AllDockWidgetAreas);

  auto* const settings = &Settings::Instance();
  connect(settings, &Settings::JITVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(settings, &Settings::DebugModeToggled, this,
          [this](bool enabled) { setHidden(!enabled || !Settings::Instance().IsJITVisible()); });

  // The model belongs to the proxy, which belongs to the view.
  m_blocks_view = new QTableView(nullptr);
  m_filter_model = new JitBlockProxyModel(m_blocks_view);
  m_blocks_model = new JitBlockTableModel(m_system, m_system.GetJitInterface(),
                                          m_system.GetPPCSymbolDB(), m_filter_model);
  connect(this, &JITWidget::HideSignal, m_blocks_model, &JitBlockTableModel::OnHideSignal);
  connect(this, &JITWidget::ShowSignal, m_blocks_model, &JitBlockTableModel::OnShowSignal);

  m_filter_model->SetBlocks(m_blocks_model);
  m_filter_model->setSortRole(UserRole::SortRole);
  m_filter_model->setSortCaseSensitivity(Qt::CaseInsensitive);

  m_blocks_view->setModel(m_filter_model);
  m_blocks_view->setSortingEnabled(true);
  m_blocks_view->sortByColumn(Column::EffectiveAddress, Qt::AscendingOrder);
  m_blocks_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_blocks_view->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_blocks_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_blocks_view->setContextMenuPolicy(Qt::CustomContextMenu);
  m_blocks_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_blocks_view->setCornerButtonEnabled(false);
  m_blocks_view->verticalHeader()->hide();
  connect(m_blocks_view, &QTableView::doubleClicked, this, [this](const QModelIndex& index) {
    emit SetCodeAddress(m_filter_model->GetJitBlock(index).effectiveAddress);
  });
  connect(m_blocks_view, &QTableView::customContextMenuRequested, this, &JITWidget::ShowTableMenu);

  QHeaderView* const header = m_blocks_view->horizontalHeader();
  header->setContextMenuPolicy(Qt::CustomContextMenu);
  header->setStretchLastSection(true);
  header->setSectionsMovable(true);
  header->setFirstSectionMovable(true);
  connect(header, &QHeaderView::sortIndicatorChanged, m_blocks_model,
          &JitBlockTableModel::OnSortIndicatorChanged);
  connect(header, &QHeaderView::customContextMenuRequested, this, [this](const QPoint& pos) {
    m_columns_menu->exec(m_blocks_view->horizontalHeader()->mapToGlobal(pos));
  });

  connect(m_blocks_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          [this](const QModelIndex& current, const QModelIndex&) { ShowRow(current); });

  auto* const filter_row = new QHBoxLayout(nullptr);
  const auto add_address_filter = [&](QLineEdit* field, const QString& hint,
                                      JitBlockProxyModel::AddressFilter which) {
    field->setPlaceholderText(hint);
    connect(field, &QLineEdit::textChanged, m_filter_model,
            [proxy = m_filter_model, which](const QString& text) {
              proxy->SetAddressText(which, text);
            });
    filter_row->addWidget(field);
  };
  add_address_filter(new QLineEdit(nullptr), tr("Min Effective Address"),
                     JitBlockProxyModel::AddressFilter::MinEffective);
  add_address_filter(new QLineEdit(nullptr), tr("Max Effective Address"),
                     JitBlockProxyModel::AddressFilter::MaxEffective);
  m_covers_filter = new QLineEdit(nullptr);
  add_address_filter(m_covers_filter, tr("Recompiles Physical Address"),
                     JitBlockProxyModel::AddressFilter::CoversPhysical);

  auto* const symbol_filter = new QLineEdit(nullptr);
  symbol_filter->setPlaceholderText(tr("Symbol Name"));
  // The model is told first, so that it can resolve all symbol names before the proxy filters.
  connect(symbol_filter, &QLineEdit::textChanged, m_blocks_model,
          &JitBlockTableModel::OnFilterSymbolTextChanged);
  connect(symbol_filter, &QLineEdit::textChanged, m_filter_model,
          [proxy = m_filter_model](const QString& text) { proxy->SetSymbolText(text); });
  filter_row->addWidget(symbol_filter);

  m_profiling_button = new QPushButton(nullptr);
  m_profiling_button->setToolTip(
      tr("Toggle software JIT block profiling (will clear the JIT cache)."));
  m_profiling_button->setCheckable(true);
  connect(m_profiling_button, &QPushButton::toggled, this, [](bool enabled) {
    Config::SetBaseOrCurrent(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING, enabled);
  });
  filter_row->addWidget(m_profiling_button);

  m_clear_button = new QPushButton(tr("Clear Cache"), nullptr);
  connect(m_clear_button, &QPushButton::clicked, this,
          [this] { m_system.GetJitInterface().ClearCache(Core::CPUThreadGuard{m_system}); });
  filter_row->addWidget(m_clear_button);

  m_wipe_button = new QPushButton(tr("Wipe Profiling"), nullptr);
  m_wipe_button->setToolTip(tr("Re-initialize software JIT block profiling data."));
  connect(m_wipe_button, &QPushButton::clicked, this, [this] {
    m_system.GetJitInterface().WipeBlockProfilingData(Core::CPUThreadGuard{m_system});
  });
  filter_row->addWidget(m_wipe_button);

  BuildPanes();
  BuildMenus();
  RestoreLayout();

  // Full names for the column menu; the table header uses short forms of these.
  static constexpr std::array<const char*, Column::NumberOfColumns> column_names{
      QT_TR_NOOP("PPC Feature Flags"),
      // i18n: an address as the guest sees it, before any MMU translation.
      QT_TR_NOOP("Effective Address"),
      QT_TR_NOOP("Code Buffer Size"),
      // i18n: how many guest instructions in the block were already compiled elsewhere.
      QT_TR_NOOP("Repeat Instructions"),
      // i18n: size of the code the JIT emitted into its near (hot) code area.
      QT_TR_NOOP("Host Near Code Size"),
      // i18n: size of the code the JIT emitted into its far (cold) code area.
      QT_TR_NOOP("Host Far Code Size"),
      QT_TR_NOOP("Run Count"),
      // i18n: the three "Cycles" columns count CPU instruction cycles.
      QT_TR_NOOP("Cycles Spent"),
      QT_TR_NOOP("Cycles Average"),
      QT_TR_NOOP("Cycles Percent"),
      // i18n: ns = nanoseconds.
      QT_TR_NOOP("Time Spent (ns)"),
      // i18n: ns = nanoseconds.
      QT_TR_NOOP("Time Average (ns)"),
      QT_TR_NOOP("Time Percent"),
      // i18n: the name of the debug symbol the block starts in.
      QT_TR_NOOP("Symbol"),
  };
  m_columns_menu = new QMenu(this);
  for (int col = 0; col < Column::NumberOfColumns; ++col)
  {
    QAction* toggle = m_columns_menu->addAction(
        tr(column_names[static_cast<size_t>(col)]),
        [this, col](bool shown) { m_blocks_view->setColumnHidden(col, !shown); });
    // Set before the action becomes checkable, as it always was.
    toggle->setChecked(!m_blocks_view->isColumnHidden(col));
    toggle->setCheckable(true);
  }

  auto* const column = new QVBoxLayout(nullptr);
  column->setContentsMargins(2, 2, 2, 2);
  column->setSpacing(0);
  column->addLayout(filter_row);
  column->addWidget(m_vertical_split);
  column->addWidget(m_status);

  auto* const body = new QWidget(nullptr);
  body->setLayout(column);
  setWidget(body);
}

JITWidget::~JITWidget()
{
  StoreLayout();
}

void JITWidget::BuildPanes()
{
  m_horizontal_split = new QSplitter(Qt::Horizontal, nullptr);
  const auto add_pane = [this](const QString& hint) {
    auto* pane = new QPlainTextEdit(nullptr);
    pane->setWordWrapMode(QTextOption::NoWrap);
    pane->setPlaceholderText(hint);
    pane->setReadOnly(true);
    m_horizontal_split->addWidget(pane);
    return pane;
  };
  m_guest_pane = add_pane(tr("PPC Instruction Coverage"));
  m_near_pane = add_pane(tr("Host Near Code Cache"));
  m_far_pane = add_pane(tr("Host Far Code Cache"));

  m_vertical_split = new QSplitter(Qt::Vertical, nullptr);
  m_vertical_split->addWidget(m_blocks_view);
  m_vertical_split->addWidget(m_horizontal_split);

  m_status = new ClickableStatusBar(nullptr);
  m_status->setSizeGripEnabled(false);
  connect(m_status, &ClickableStatusBar::pressed, this, [this] {
    if (Core::GetState(m_system) == Core::State::Paused)
      ShowFreeSpace();
  });
}

void JITWidget::BuildMenus()
{
  m_row_menu = new QMenu(this);
  m_row_menu->addAction(tr("View &Code"), this, [this] {
    // The code view takes effective addresses only, so the block's effective address is used as is.
    const QModelIndex current = m_blocks_view->currentIndex();
    if (current.isValid())
      emit SetCodeAddress(m_filter_model->GetJitBlock(current).effectiveAddress);
  });
  // Erasing clears the selection, and the current-row handler then refreshes the panes.
  m_row_menu->addAction(tr("&Erase Block(s)"), this, &JITWidget::EraseSelectedBlocks);
}

void JITWidget::RestoreLayout()
{
  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("jitwidget/geometry")).toByteArray());
  // Visibility goes before floating so that a floating dock comes up correctly.
  setHidden(!Settings::Instance().IsJITVisible() || !Settings::Instance().IsDebugModeEnabled());
  setFloating(stored.value(QStringLiteral("jitwidget/floating")).toBool());
  m_blocks_view->horizontalHeader()->restoreState(
      stored.value(QStringLiteral("jitwidget/tableheader/state")).toByteArray());
  m_vertical_split->restoreState(
      stored.value(QStringLiteral("jitwidget/tablesplitter")).toByteArray());
  m_horizontal_split->restoreState(
      stored.value(QStringLiteral("jitwidget/disasmsplitter")).toByteArray());
}

void JITWidget::StoreLayout() const
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("jitwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("jitwidget/floating"), isFloating());
  stored.setValue(QStringLiteral("jitwidget/tableheader/state"),
                  m_blocks_view->horizontalHeader()->saveState());
  stored.setValue(QStringLiteral("jitwidget/tablesplitter"), m_vertical_split->saveState());
  stored.setValue(QStringLiteral("jitwidget/disasmsplitter"), m_horizontal_split->saveState());
}

void JITWidget::Listen(bool on)
{
  if (!on)
  {
    for (const QMetaObject::Connection& link : m_links)
      disconnect(link);
    m_links.clear();
    return;
  }

  auto* const host = Host::GetInstance();
  auto* const settings = &Settings::Instance();
  m_links.append(connect(host, &Host::JitCacheInvalidation, this, [this] {
    if (Core::GetState(m_system) != Core::State::Paused)
      return;
    ClearPanes();
    ShowFreeSpace();
  }));
  m_links.append(
      connect(host, &Host::UpdateDisasmDialog, this, &JITWidget::ShowCurrentRowIfPaused));
  m_links.append(connect(host, &Host::PPCSymbolsChanged, this, &JITWidget::ShowCurrentRowIfPaused));
  // Changing breakpoints can invalidate blocks, so the selection may point at stale rows. Clearing
  // it makes the current-row handler refresh the panes.
  m_links.append(connect(host, &Host::PPCBreakpointsChanged, this,
                         [this] { m_blocks_view->selectionModel()->clear(); }));
  m_links.append(
      connect(settings, &Settings::ConfigChanged, this, &JITWidget::RefreshProfilingButton));
  m_links.append(connect(settings, &Settings::DebugFontChanged, this, &JITWidget::ApplyFont));
  m_links.append(
      connect(settings, &Settings::EmulationStateChanged, this, &JITWidget::StateChanged));
}

void JITWidget::RefreshProfilingButton()
{
  const QSignalBlocker quiet(m_profiling_button);
  const bool profiling = Config::Get(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING);
  m_profiling_button->setText(profiling ? tr("Stop Profiling") : tr("Start Profiling"));
  m_profiling_button->setChecked(profiling);
}

void JITWidget::RefreshButtons()
{
  const bool have_jit = m_system.GetJitInterface().GetCore() != nullptr;
  m_clear_button->setEnabled(have_jit);
  m_wipe_button->setEnabled(have_jit);
}

void JITWidget::ApplyFont(const QFont& font)
{
  for (QWidget* target : {static_cast<QWidget*>(m_blocks_view), static_cast<QWidget*>(m_guest_pane),
                          static_cast<QWidget*>(m_near_pane), static_cast<QWidget*>(m_far_pane)})
  {
    target->setFont(font);
  }
}

void JITWidget::ClearPanes()
{
  m_guest_pane->clear();
  m_near_pane->clear();
  m_far_pane->clear();
  m_status->clearMessage();
}

void JITWidget::ShowFreeSpace()
{
  QString message = tr("Free memory:");
  for (const auto& [region, stats] : m_system.GetJitInterface().GetMemoryStats())
  {
    const auto& [free_bytes, fragmentation] = stats;
    // i18n: one entry per JIT memory region. %1 = free space with a unit such as MiB,
    // %2 = the region's (untranslated) name, %3 = how fragmented it is, in percent.
    message.append(tr(" %1 %2 (%3% fragmented)")
                       .arg(QString::fromStdString(UICommon::FormatSize(free_bytes, 2)))
                       .arg(QtUtils::FromStdString(region))
                       .arg(fragmentation * 100.0, 0, 'f', 2));
  }
  m_status->showMessage(message);
}

void JITWidget::StateChanged(Core::State state)
{
  RefreshButtons();
  ClearPanes();
  if (state == Core::State::Paused)
    ShowFreeSpace();
}

void JITWidget::ShowBlock(const JitBlock& block)
{
  std::ostringstream listing;
  WriteGuestListing(block, m_system.GetPPCSymbolDB(), listing);
  m_guest_pane->setPlainText(Drain(listing));

  auto& jit = m_system.GetJitInterface();
  const size_t near_count = jit.DisassembleNearCode(block, listing);
  m_near_pane->setPlainText(Drain(listing));
  const size_t far_count = jit.DisassembleFarCode(block, listing);
  m_far_pane->setPlainText(Drain(listing));

  // How many times larger the host code is than the guest code, as a percentage over 100%.
  const double blowup =
      static_cast<double>(100 * (near_count + far_count)) / block.originalSize - 100.0;
  // i18n: %1 = host instructions emitted into the near code cache, %2 = into the far code
  // cache; %3 = extra host code relative to the guest instructions, as a percentage.
  m_status->showMessage(tr("Host instruction count: %1 near %2 far (%3% blowup)")
                            .arg(near_count)
                            .arg(far_count)
                            .arg(blowup, 0, 'f', 2));
}

void JITWidget::ShowRow(const QModelIndex& index)
{
  if (!index.isValid())
  {
    ClearPanes();
    if (Core::GetState(m_system) == Core::State::Paused)
      ShowFreeSpace();
    return;
  }
  ShowBlock(m_filter_model->GetJitBlock(index));
}

void JITWidget::ShowCurrentRowIfPaused()
{
  if (Core::GetState(m_system) != Core::State::Paused)
    return;
  ShowRow(m_blocks_view->currentIndex());
}

void JITWidget::EraseSelectedBlocks()
{
  QItemSelectionModel* const selection = m_blocks_view->selectionModel();
  QModelIndexList rows = selection->selectedRows();
  // Clearing the selection changes the current row, which refreshes the panes.
  selection->clear();

  for (QModelIndex& row : rows)
    row = m_filter_model->mapToSource(row);
  // Remove from the bottom up so that earlier rows keep their numbers.
  std::sort(rows.begin(), rows.end());
  for (auto it = rows.crbegin(); it != rows.crend(); ++it)
  {
    if (it->isValid())
      m_blocks_model->removeRow(it->row());
  }
}

void JITWidget::ShowTableMenu(const QPoint& pos)
{
  const QPoint where = m_blocks_view->viewport()->mapToGlobal(pos);
  // With every column hidden, offer the column menu so that the user can get them back.
  if (m_blocks_view->horizontalHeader()->hiddenSectionCount() == Column::NumberOfColumns)
    m_columns_menu->exec(where);
  else
    m_row_menu->exec(where);
}

void JITWidget::OnRequestPPCComparison(u32 address, bool translate_address)
{
  Settings::Instance().SetJITVisible(true);
  raise();

  if (translate_address)
  {
    const std::optional<u32> physical = m_system.GetMMU().GetTranslatedAddress(address);
    if (!physical)
    {
      ModalMessageBox::warning(
          this, tr("Error"),
          tr("Effective address %1 has no physical address translation.").arg(address, 0, 16));
      return;
    }
    address = *physical;
  }
  m_covers_filter->setText(QString::number(address, 16));
}

void JITWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetJITVisible(false);
}

void JITWidget::showEvent(QShowEvent*)
{
  emit ShowSignal();
  Listen(true);
  // Catch up on settings and state that may have changed while hidden. Block, symbol and
  // breakpoint changes need nothing here.
  RefreshProfilingButton();
  ApplyFont(Settings::Instance().GetDebugFont());
  StateChanged(Core::GetState(m_system));
}

void JITWidget::hideEvent(QHideEvent*)
{
  emit HideSignal();
  Listen(false);
  ClearPanes();
}
