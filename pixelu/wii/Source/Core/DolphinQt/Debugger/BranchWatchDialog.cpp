// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/BranchWatchDialog.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVariant>
#include <fmt/format.h>

#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/BranchWatchTableModel.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
// The twelve branch forms the "Branch Type" filter distinguishes. Each even entry is a form
// without LK and the entry after it the same form with LK set.
enum BranchKind : std::size_t
{
  KindB,
  KindBL,
  KindBC,
  KindBCL,
  KindBLR,
  KindBLRL,
  KindBCLR,
  KindBCLRL,
  KindBCTR,
  KindBCTRL,
  KindBCCTR,
  KindBCCTRL,
  KindCount,
};

// BO values of the form 1z1zz branch regardless of CR and CTR.
constexpr u32 BO_ALWAYS = 0b10100;

bool BranchesAlways(UGeckoInstruction inst)
{
  return (inst.BO & BO_ALWAYS) == BO_ALWAYS;
}

std::optional<BranchKind> KindOf(UGeckoInstruction inst)
{
  std::size_t base;
  if (inst.OPCD == 18)
    base = KindB;
  else if (inst.OPCD == 16)
    base = KindBC;
  else if (inst.OPCD == 19 && inst.SUBOP10 == 16)
    base = BranchesAlways(inst) ? KindBLR : KindBCLR;
  else if (inst.OPCD == 19 && inst.SUBOP10 == 528)
    base = BranchesAlways(inst) ? KindBCTR : KindBCCTR;
  else
    return std::nullopt;
  return static_cast<BranchKind>(base + (inst.LK ? 1 : 0));
}

// Which end of the branch an address filter bounds, and from which side.
enum AddressBound : std::size_t
{
  OriginLow,
  OriginHigh,
  DestinLow,
  DestinHigh,
  BoundCount,
};

constexpr int POLL_INTERVAL_MS = 100;

constexpr u32 NOP_INSTRUCTION = 0x60000000;
constexpr u32 BLR_INSTRUCTION = 0x4e800020;

std::string DefaultSnapshotPath()
{
  return fmt::format("{}{}.txt", File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX),
                     SConfig::GetInstance().GetGameID());
}

QString SnapshotFolder()
{
  return QString::fromStdString(File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX));
}

u32 FlipConditionBit(u32 hex)
{
  UGeckoInstruction inst{hex};
  inst.BO = (inst.BO ^ 0b01000u) & 0x1Fu;
  return inst.hex;
}

u32 FlipDecrementBit(u32 hex)
{
  UGeckoInstruction inst{hex};
  inst.BO = (inst.BO ^ 0b00010u) & 0x1Fu;
  return inst.hex;
}

u32 ForceUnconditional(u32 hex)
{
  UGeckoInstruction inst{hex};
  inst.BO = BO_ALWAYS;
  return inst.hex;
}
}  // namespace

// Filters the Branch Watch table by branch form, condition, address ranges and symbol names.
class BranchWatchProxyModel final : public QSortFilterProxyModel
{
public:
  explicit BranchWatchProxyModel(const Core::BranchWatch& branch_watch, QObject* parent = nullptr)
      : QSortFilterProxyModel(parent), m_watch(branch_watch)
  {
  }

  void SetTableModel(BranchWatchTableModel* model)
  {
    m_table_model = model;
    setSourceModel(model);
  }

  void AllowKind(BranchKind kind, bool allow)
  {
    m_kinds.set(kind, allow);
    invalidateRowsFilter();
  }

  void AllowCondition(bool condition, bool allow)
  {
    (condition ? m_allow_true : m_allow_false) = allow;
    invalidateRowsFilter();
  }

  // Unparsable text (including an empty box) removes the bound.
  void SetBound(AddressBound bound, const QString& text)
  {
    bool ok = false;
    const u32 address = text.toUInt(&ok, 16);
    m_bounds[bound] = ok ? std::optional<u32>{address} : std::nullopt;
    invalidateRowsFilter();
  }

  void SetSymbolFilter(bool origin, const QString& text)
  {
    (origin ? m_origin_name : m_destin_name) = text;
    invalidateRowsFilter();
  }

  void SetInspected(const QModelIndex& index) const
  {
    m_table_model->SetInspected(mapToSource(index));
  }

  const Core::BranchWatchSelectionValueType& Entry(const QModelIndex& index) const
  {
    return m_table_model->GetBranchWatchSelection(mapToSource(index));
  }

protected:
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
  static bool InRange(u32 address, const std::optional<u32>& low, const std::optional<u32>& high)
  {
    return !(low && address < *low) && !(high && address > *high);
  }

  static bool NameMatches(const QVariant& name, const QString& filter)
  {
    if (filter.isEmpty())
      return true;
    return name.isValid() && name.toString().contains(filter, Qt::CaseInsensitive);
  }

  const Core::BranchWatch& m_watch;
  BranchWatchTableModel* m_table_model = nullptr;

  std::bitset<KindCount> m_kinds;
  bool m_allow_true = false;
  bool m_allow_false = false;
  std::array<std::optional<u32>, BoundCount> m_bounds{};
  QString m_origin_name;
  QString m_destin_name;
};

bool BranchWatchProxyModel::filterAcceptsRow(int source_row, const QModelIndex&) const
{
  const std::size_t row = static_cast<std::size_t>(source_row);
  const Core::BranchWatch::Selection::value_type& entry = m_watch.GetSelection()[row];
  if (!(entry.condition ? m_allow_true : m_allow_false))
    return false;

  const Core::BranchWatchCollectionKey& key = entry.collection_ptr->first;
  const std::optional<BranchKind> kind = KindOf(key.original_inst);
  if (!kind || !m_kinds.test(*kind))
    return false;

  if (!InRange(key.origin_addr, m_bounds[OriginLow], m_bounds[OriginHigh]) ||
      !InRange(key.destin_addr, m_bounds[DestinLow], m_bounds[DestinHigh]))
  {
    return false;
  }

  // The symbol list is only consulted when a name filter is actually set.
  if (m_origin_name.isEmpty() && m_destin_name.isEmpty())
    return true;
  const BranchWatchTableModel::SymbolListValueType& symbols =
      m_table_model->GetSymbolList()[source_row];
  return NameMatches(symbols.origin_name, m_origin_name) &&
         NameMatches(symbols.destin_name, m_destin_name);
}

BranchWatchDialog::BranchWatchDialog(Core::System& system, Core::BranchWatch& branch_watch,
                                     PPCSymbolDB& ppc_symbol_db, CodeWidget* code_widget,
                                     QWidget* parent)
    : QDialog(parent), m_system(system), m_watch(branch_watch), m_code_widget(code_widget)
{
  setWindowTitle(tr("Branch Watch Tool"));
  setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);

  BuildTable(ppc_symbol_db);

  m_status = new QStatusBar(nullptr);
  m_status->setSizeGripEnabled(false);

  m_controls = new QToolBar(nullptr);
  BuildToolControls();
  auto* const gap = new QWidget(nullptr);
  gap->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
  m_controls->addWidget(gap);
  BuildBranchTypeFilters();
  BuildAddressFilters();
  BuildConditionFilters();
  BuildMiscControls();

  BuildRowMenus();
  RestoreSettings();
  BuildColumnMenu();
  QWidget* const menu_bar = BuildMenuBar(BuildToolbarMenu());

  m_poll = new QTimer(this);
  connect(m_poll, &QTimer::timeout, this, &BranchWatchDialog::Update);
  connect(m_filter, &BranchWatchProxyModel::layoutChanged, this, &BranchWatchDialog::RefreshStatus);

  auto* const column = new QVBoxLayout(nullptr);
  column->setMenuBar(menu_bar);
  column->addWidget(m_controls);
  column->addWidget(m_table);
  column->addWidget(m_status);
  setLayout(column);
}

BranchWatchDialog::~BranchWatchDialog()
{
  StoreSettings();
}

void BranchWatchDialog::BuildTable(PPCSymbolDB& ppc_symbol_db)
{
  m_table = new QTableView(nullptr);
  m_filter = new BranchWatchProxyModel(m_watch, m_table);
  m_model = new BranchWatchTableModel(m_system, m_watch, ppc_symbol_db, m_filter);

  m_filter->SetTableModel(m_model);
  m_filter->setSortRole(UserRole::SortRole);
  m_filter->setSortCaseSensitivity(Qt::CaseInsensitive);

  m_table->setModel(m_filter);
  m_table->setSortingEnabled(true);
  m_table->sortByColumn(Column::Origin, Qt::AscendingOrder);
  m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setCornerButtonEnabled(false);
  m_table->verticalHeader()->hide();
  // Narrow mnemonic columns, wide symbol columns; everything else keeps Qt's default width.
  m_table->setColumnWidth(Column::Instruction, 50);
  m_table->setColumnWidth(Column::Condition, 50);
  m_table->setColumnWidth(Column::OriginSymbol, 250);
  m_table->setColumnWidth(Column::DestinSymbol, 250);

  QHeaderView* const header = m_table->horizontalHeader();
  header->setContextMenuPolicy(Qt::CustomContextMenu);
  header->setStretchLastSection(true);
  header->setSectionsMovable(true);
  header->setFirstSectionMovable(true);

  connect(m_table, &QTableView::clicked, this, &BranchWatchDialog::ClickCell);
  connect(m_table, &QTableView::customContextMenuRequested, this, &BranchWatchDialog::OpenRowMenu);
  connect(header, &QHeaderView::customContextMenuRequested, this,
          &BranchWatchDialog::OpenColumnMenu);
  auto* const delete_key = new QShortcut(QKeySequence(Qt::Key_Delete), this);
  connect(delete_key, &QShortcut::activated, this, &BranchWatchDialog::DeleteSelectedRows);
}

void BranchWatchDialog::BuildToolControls()
{
  const auto make_button = [](const QString& text) {
    auto* const button = new QPushButton(text, nullptr);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    return button;
  };

  m_record_button = make_button(tr("Start Branch Watch"));
  connect(m_record_button, &QPushButton::toggled, this, &BranchWatchDialog::ToggleRecording);
  m_record_button->setCheckable(true);

  m_clear_button = make_button(tr("Clear Branch Watch"));
  connect(m_clear_button, &QPushButton::clicked, this, &BranchWatchDialog::ClearWatch);

  m_taken_button = make_button(tr("Code Path Was Taken"));
  connect(m_taken_button, &QPushButton::clicked, this, &BranchWatchDialog::PathWasTaken);

  m_not_taken_button = make_button(tr("Code Path Not Taken"));
  connect(m_not_taken_button, &QPushButton::clicked, this, &BranchWatchDialog::PathNotTaken);

  // Two columns: recording on the left, the path tests on the right.
  auto* const grid = new QGridLayout(nullptr);
  grid->addWidget(m_record_button, 0, 0);
  grid->addWidget(m_clear_button, 1, 0);
  grid->addWidget(m_taken_button, 0, 1);
  grid->addWidget(m_not_taken_button, 1, 1);

  auto* const box = new QGroupBox(tr("Tool Controls"), nullptr);
  box->setLayout(grid);
  box->setAlignment(Qt::AlignHCenter);
  m_controls->addWidget(box);
}

void BranchWatchDialog::BuildBranchTypeFilters()
{
  struct KindBox
  {
    const char* mnemonic;
    const char* description;
  };
  // In BranchKind order; laid out four to a row.
  static constexpr std::array<KindBox, KindCount> boxes{{
      {"b", QT_TR_NOOP("Branch")},
      {"bl", QT_TR_NOOP("Branch (LR saved)")},
      {"bc", QT_TR_NOOP("Branch Conditional")},
      {"bcl", QT_TR_NOOP("Branch Conditional (LR saved)")},
      {"blr", QT_TR_NOOP("Branch to Link Register")},
      {"blrl", QT_TR_NOOP("Branch to Link Register (LR saved)")},
      {"bclr", QT_TR_NOOP("Branch Conditional to Link Register")},
      {"bclrl", QT_TR_NOOP("Branch Conditional to Link Register (LR saved)")},
      {"bctr", QT_TR_NOOP("Branch to Count Register")},
      {"bctrl", QT_TR_NOOP("Branch to Count Register (LR saved)")},
      {"bcctr", QT_TR_NOOP("Branch Conditional to Count Register")},
      {"bcctrl", QT_TR_NOOP("Branch Conditional to Count Register (LR saved)")},
  }};

  auto* const grid = new QGridLayout(nullptr);
  for (std::size_t i = 0; i < boxes.size(); ++i)
  {
    const BranchKind kind = static_cast<BranchKind>(i);
    auto* const check = new QCheckBox(QString::fromLatin1(boxes[i].mnemonic), nullptr);
    check->setToolTip(tr(boxes[i].description));
    grid->addWidget(check, static_cast<int>(i / 4), static_cast<int>(i % 4));
    connect(check, &QCheckBox::toggled, this, [this, kind](bool on) {
      m_filter->AllowKind(kind, on);
      RefreshStatus();
    });
    check->setChecked(true);
  }

  auto* const box = new QGroupBox(tr("Branch Type"), nullptr);
  box->setLayout(grid);
  box->setAlignment(Qt::AlignHCenter);
  m_branch_type_section = m_controls->addWidget(box);
}

void BranchWatchDialog::BuildAddressFilters()
{
  auto* const grid = new QGridLayout(nullptr);
  const auto add_box = [this, grid](const QString& hint, int row, int col,
                                    std::function<void(const QString&)> apply) {
    auto* const edit = new QLineEdit(nullptr);
    grid->addWidget(edit, row, col, 1, 1);
    connect(edit, &QLineEdit::textChanged, this,
            [this, run = std::move(apply)](const QString& text) {
              run(text);
              RefreshStatus();
            });
    edit->setPlaceholderText(hint);
    return edit;
  };
  const auto symbol = [this](bool origin) {
    return [this, origin](const QString& text) { m_filter->SetSymbolFilter(origin, text); };
  };
  const auto bound = [this](AddressBound which) {
    return [this, which](const QString& text) { m_filter->SetBound(which, text); };
  };

  add_box(tr("Origin Symbol"), 0, 0, symbol(true));
  add_box(tr("Origin Min"), 1, 0, bound(OriginLow))->setMaxLength(8);
  add_box(tr("Origin Max"), 2, 0, bound(OriginHigh))->setMaxLength(8);
  add_box(tr("Destination Symbol"), 0, 1, symbol(false));
  add_box(tr("Destination Min"), 1, 1, bound(DestinLow))->setMaxLength(8);
  add_box(tr("Destination Max"), 2, 1, bound(DestinHigh))->setMaxLength(8);

  auto* const box = new QGroupBox(tr("Origin and Destination"), nullptr);
  box->setLayout(grid);
  box->setAlignment(Qt::AlignHCenter);
  m_address_section = m_controls->addWidget(box);
}

void BranchWatchDialog::BuildConditionFilters()
{
  auto* const stack = new QVBoxLayout(nullptr);
  stack->setAlignment(Qt::AlignHCenter);
  const auto add_check = [this, stack](const QString& text, bool condition) {
    auto* const check = new QCheckBox(text, nullptr);
    stack->addWidget(check);
    connect(check, &QCheckBox::toggled, this, [this, condition](bool on) {
      m_filter->AllowCondition(condition, on);
      RefreshStatus();
    });
    check->setChecked(true);
    return check;
  };

  QCheckBox* const when_true = add_check(tr("true"), true);
  when_true->setToolTip(tr("This will also filter unconditional branches.\n"
                           "To filter for or against unconditional branches,\n"
                           "use the Branch Type filter options."));
  add_check(tr("false"), false);

  auto* const box = new QGroupBox(tr("Condition"), nullptr);
  box->setLayout(stack);
  box->setAlignment(Qt::AlignHCenter);
  m_condition_section = m_controls->addWidget(box);
}

void BranchWatchDialog::BuildMiscControls()
{
  const auto make_button = [this](const QString& text, void (BranchWatchDialog::*slot)()) {
    auto* const button = new QPushButton(text, nullptr);
    connect(button, &QPushButton::clicked, this, slot);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    return button;
  };
  m_overwritten_button =
      make_button(tr("Branch Was Overwritten"), &BranchWatchDialog::WasOverwritten);
  m_not_overwritten_button =
      make_button(tr("Branch Not Overwritten"), &BranchWatchDialog::NotOverwritten);

  m_wipe_hits_button = new QPushButton(tr("Wipe Recent Hits"), nullptr);
  connect(m_wipe_hits_button, &QPushButton::clicked, this, [this] { m_model->OnWipeRecentHits(); });
  m_wipe_hits_button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  // Only meaningful once the reduction phase has started.
  m_wipe_hits_button->setEnabled(false);

  auto* const stack = new QVBoxLayout(nullptr);
  for (QPushButton* const button :
       {m_overwritten_button, m_not_overwritten_button, m_wipe_hits_button})
  {
    stack->addWidget(button);
  }

  auto* const box = new QGroupBox(tr("Misc. Controls"), nullptr);
  box->setLayout(stack);
  box->setAlignment(Qt::AlignHCenter);
  m_misc_section = m_controls->addWidget(box);
}

void BranchWatchDialog::BuildRowMenus()
{
  const auto make_action = [this](const QString& text, void (BranchWatchDialog::*slot)() const) {
    auto* const action = new QAction(text, this);
    connect(action, &QAction::triggered, this, slot);
    return action;
  };

  QAction* const remove = make_action(tr("&Delete"), &BranchWatchDialog::DeleteMenuRows);
  m_invert_condition_action = new QAction(tr("Invert &Condition"), this);
  connect(m_invert_condition_action, &QAction::triggered, this,
          [this] { PatchOrigins(&FlipConditionBit); });
  m_invert_decrement_action = new QAction(tr("Invert &Decrement Check"), this);
  connect(m_invert_decrement_action, &QAction::triggered, this,
          [this] { PatchOrigins(&FlipDecrementBit); });
  m_unconditional_action = new QAction(tr("Make &Unconditional"), this);
  connect(m_unconditional_action, &QAction::triggered, this,
          [this] { PatchOrigins(&ForceUnconditional); });
  m_copy_action = make_action(tr("&Copy Address"), &BranchWatchDialog::CopyAddresses);
  m_nop_action = new QAction(tr("Insert &NOP"), this);
  connect(m_nop_action, &QAction::triggered, this, [this] { PatchTargets(NOP_INSTRUCTION); });
  m_blr_action = new QAction(tr("Insert &BLR"), this);
  connect(m_blr_action, &QAction::triggered, this, [this] { PatchTargets(BLR_INSTRUCTION); });

  m_breakpoint_menu = new QMenu(tr("Set Brea&kpoint"), this);
  m_break_action = m_breakpoint_menu->addAction(tr("&Break on Hit"), this,
                                                [this] { AddBreakpoints(true, false); });
  m_log_action = m_breakpoint_menu->addAction(tr("&Log on Hit"), this,
                                              [this] { AddBreakpoints(false, true); });
  m_break_log_action = m_breakpoint_menu->addAction(tr("Break &and Log on Hit"), this,
                                                    [this] { AddBreakpoints(true, true); });
  QAction* const breakpoint = m_breakpoint_menu->menuAction();

  const auto make_menu = [this](const QList<QAction*>& actions) {
    auto* const menu = new QMenu(this);
    menu->addActions(actions);
    return menu;
  };
  m_instruction_menu = make_menu({remove, m_invert_condition_action, m_invert_decrement_action});
  m_condition_menu = make_menu({remove, m_unconditional_action});
  m_origin_menu = make_menu({remove, m_nop_action, m_copy_action, breakpoint});
  m_destination_menu = make_menu({remove, m_blr_action, m_copy_action, breakpoint});
  m_symbol_menu = make_menu({remove, m_blr_action, m_copy_action, breakpoint});
  m_hits_menu = make_menu({remove});
}

void BranchWatchDialog::BuildColumnMenu()
{
  // Longer than the table's own header labels.
  static constexpr std::array<const char*, Column::NumberOfColumns> names{
      QT_TR_NOOP("Instruction"),   QT_TR_NOOP("Condition"),          QT_TR_NOOP("Origin"),
      QT_TR_NOOP("Destination"),   QT_TR_NOOP("Recent Hits"),        QT_TR_NOOP("Total Hits"),
      QT_TR_NOOP("Origin Symbol"), QT_TR_NOOP("Destination Symbol"),
  };

  m_column_menu = new QMenu(this);
  for (int col = 0; col < Column::NumberOfColumns; ++col)
  {
    QAction* const toggle =
        m_column_menu->addAction(tr(names[static_cast<std::size_t>(col)]), [this, col](bool shown) {
          m_table->setColumnHidden(col, !shown);
        });
    // Checked state first, then checkable, as in the rest of this dialog.
    toggle->setChecked(!m_table->isColumnHidden(col));
    toggle->setCheckable(true);
  }
}

QMenu* BranchWatchDialog::BuildToolbarMenu()
{
  auto* const menu = new QMenu(this);
  const std::array<std::pair<QString, QAction*>, 4> sections{{
      {tr("&Branch Type"), m_branch_type_section},
      {tr("&Origin and Destination"), m_address_section},
      {tr("&Condition"), m_condition_section},
      {tr("&Misc. Controls"), m_misc_section},
  }};
  for (const auto& [text, section] : sections)
  {
    QAction* const toggle = menu->addAction(text, section, &QAction::setVisible);
    toggle->setChecked(section->isVisible());
    toggle->setCheckable(true);
  }
  return menu;
}

QWidget* BranchWatchDialog::BuildMenuBar(QMenu* toolbar_menu)
{
  auto* const bar = new QMenuBar(nullptr);
  bar->setNativeMenuBar(false);

  QMenu* const file_menu = bar->addMenu(tr("&File"));
  file_menu->addAction(tr("&Save Branch Watch"), this, &BranchWatchDialog::SaveToDefault);
  file_menu->addAction(tr("Save Branch Watch &As..."), this, &BranchWatchDialog::SaveToChosen);
  file_menu->addAction(tr("&Load Branch Watch"), this, &BranchWatchDialog::LoadFromDefault);
  file_menu->addAction(tr("Load Branch Watch &From..."), this, &BranchWatchDialog::LoadFromChosen);
  m_autosave_action = file_menu->addAction(tr("A&uto Save"));
  m_autosave_action->setCheckable(true);
  connect(m_autosave_action, &QAction::toggled, this, &BranchWatchDialog::ChooseAutoSaveTarget);

  QMenu* const tool_menu = bar->addMenu(tr("&Tool"));
  tool_menu->setToolTipsVisible(true);
  QAction* const hide_controls =
      tool_menu->addAction(tr("Hide &Controls"), this, &BranchWatchDialog::ToggleControls);
  hide_controls->setCheckable(true);

  QAction* const skip_apploader = tool_menu->addAction(tr("Ignore &Apploader Branch Hits"));
  skip_apploader->setToolTip(tr("This only applies to the initial boot of the emulated software."));
  skip_apploader->setChecked(m_system.IsBranchWatchIgnoreApploader());
  skip_apploader->setCheckable(true);
  connect(skip_apploader, &QAction::toggled, this,
          [this](bool on) { m_system.SetIsBranchWatchIgnoreApploader(on); });

  tool_menu->addMenu(m_column_menu)->setText(tr("Column &Visibility"));
  tool_menu->addMenu(toolbar_menu)->setText(tr("&Toolbar Visibility"));
  tool_menu->addAction(tr("Wipe &Inspection Data"), this, [this] { m_model->OnWipeInspection(); });
  tool_menu->addAction(tr("&Help"), this, &BranchWatchDialog::ShowHelp);
  return bar;
}

void BranchWatchDialog::hideEvent(QHideEvent* event)
{
  Detach();
  QDialog::hideEvent(event);
}

void BranchWatchDialog::showEvent(QShowEvent* event)
{
  Attach();
  QDialog::showEvent(event);
}

void BranchWatchDialog::Attach()
{
  const Settings* const settings = &Settings::Instance();
  m_links = {
      connect(settings, &Settings::EmulationStateChanged, this, &BranchWatchDialog::StateChanged),
      connect(settings, &Settings::ThemeChanged, this, &BranchWatchDialog::ReloadIcons),
      connect(settings, &Settings::DebugFontChanged, m_model, &BranchWatchTableModel::setFont),
      connect(Host::GetInstance(), &Host::PPCSymbolsChanged, m_model,
              &BranchWatchTableModel::UpdateSymbols),
  };

  // Catch up on whatever was signalled while the dialog was hidden.
  StateChanged(Core::GetState(m_system));
  ReloadIcons();
  m_model->OnDebugFontChanged(Settings::Instance().GetDebugFont());
  m_model->OnPPCSymbolsChanged();
}

void BranchWatchDialog::Detach()
{
  for (const QMetaObject::Connection& link : std::as_const(m_links))
    disconnect(link);
  m_links.clear();
  if (m_poll->isActive())
    m_poll->stop();
}

void BranchWatchDialog::ToggleRecording(bool on) const
{
  m_watch.SetRecordingActive(Core::CPUThreadGuard{m_system}, on);
  if (on)
  {
    m_record_button->setText(tr("Pause Branch Watch"));
    if (Core::GetState(m_system) > Core::State::Paused)
      m_poll->start(POLL_INTERVAL_MS);
  }
  else
  {
    m_record_button->setText(tr("Start Branch Watch"));
    if (m_poll->isActive())
      m_poll->stop();
  }
  Update();
}

void BranchWatchDialog::RunStep(ModelStep step)
{
  const Core::CPUThreadGuard guard{m_system};
  (m_model->*step)(guard);
  AutoSave(guard);
}

void BranchWatchDialog::ClearWatch()
{
  RunStep(&BranchWatchTableModel::OnClearBranchWatch);
  m_wipe_hits_button->setEnabled(false);
  RefreshStatus();
}

void BranchWatchDialog::PathWasTaken()
{
  RunStep(&BranchWatchTableModel::OnCodePathWasTaken);
  m_wipe_hits_button->setEnabled(true);
  RefreshStatus();
}

void BranchWatchDialog::PathNotTaken()
{
  RunStep(&BranchWatchTableModel::OnCodePathNotTaken);
  RefreshStatus();
}

void BranchWatchDialog::WasOverwritten()
{
  RunStep(&BranchWatchTableModel::OnBranchWasOverwritten);
  RefreshStatus();
}

void BranchWatchDialog::NotOverwritten()
{
  RunStep(&BranchWatchTableModel::OnBranchNotOverwritten);
  RefreshStatus();
}

void BranchWatchDialog::SaveToDefault()
{
  if (!m_watch.CanSave())
  {
    ModalMessageBox::warning(this, tr("Error"), tr("There is nothing to save!"));
    return;
  }
  WriteSnapshot(Core::CPUThreadGuard{m_system}, DefaultSnapshotPath());
}

void BranchWatchDialog::SaveToChosen()
{
  if (!m_watch.CanSave())
  {
    ModalMessageBox::warning(this, tr("Error"), tr("There is nothing to save!"));
    return;
  }
  const QString chosen =
      DolphinFileDialog::getSaveFileName(this, tr("Save Branch Watch Snapshot"), SnapshotFolder(),
                                         tr("Text file (*.txt);;All Files (*)"));
  if (!chosen.isEmpty())
    WriteSnapshot(Core::CPUThreadGuard{m_system}, chosen.toStdString());
}

void BranchWatchDialog::LoadFromDefault()
{
  ReadSnapshot(Core::CPUThreadGuard{m_system}, DefaultSnapshotPath());
}

void BranchWatchDialog::LoadFromChosen()
{
  const QString chosen = DolphinFileDialog::getOpenFileName(
      this, tr("Load Branch Watch Snapshot"), SnapshotFolder(),
      tr("Text file (*.txt);;All Files (*)"), nullptr, QFileDialog::Option::ReadOnly);
  if (!chosen.isEmpty())
    ReadSnapshot(Core::CPUThreadGuard{m_system}, chosen.toStdString());
}

void BranchWatchDialog::ChooseAutoSaveTarget(bool on)
{
  if (!on)
    return;

  // i18n: picking a file makes auto-save write there; Cancel makes it write to the default
  // snapshot file in the user folder instead.
  const QString chosen = DolphinFileDialog::getSaveFileName(
      this, tr("Select Branch Watch Snapshot Auto-Save File (for user folder location, cancel)"),
      SnapshotFolder(), tr("Text file (*.txt);;All Files (*)"));
  m_autosave_path =
      chosen.isEmpty() ? std::nullopt : std::optional<std::string>{chosen.toStdString()};
}

void BranchWatchDialog::ToggleControls(bool hidden) const
{
  m_controls->setHidden(hidden);
}

void BranchWatchDialog::ShowHelp()
{
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (1/4)"),
      tr("Branch Watch is a code-searching tool that can isolate branches tracked by "
         "the emulated CPU by testing candidate branches with simple criteria. If you are "
         "familiar with Cheat Engine's Ultimap, Branch Watch is similar to that.\n\nPress the "
         "\"Start Branch Watch\" button to activate Branch Watch. Branch Watch persists across "
         "emulation sessions, and a snapshot of your progress can be saved to and loaded from "
         "the User Directory to persist after Dolphin Emulator is closed. \"Save As...\" and "
         "\"Load From...\" actions are also available, and auto-saving can be enabled to save "
         "a snapshot at every step of a search. The \"Pause Branch Watch\" button will halt "
         "Branch Watch from tracking further branch hits until it is told to resume. Press the "
         "\"Clear Branch Watch\" button to clear all candidates and return to the blacklist "
         "phase."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (2/4)"),
      tr("Branch Watch starts in the blacklist phase, meaning no candidates have been "
         "chosen yet, but candidates found so far can be excluded from the candidacy by "
         "pressing the \"Code Path Not Taken\", \"Branch Was Overwritten\", and \"Branch Not "
         "Overwritten\" buttons. Once the \"Code Path Was Taken\" button is pressed for the "
         "first time, Branch Watch will switch to the reduction phase, and the table will "
         "populate with all eligible candidates."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (3/4)"),
      tr("Once in the reduction phase, it is time to start narrowing down the candidates "
         "shown in the table. Further reduce the candidates by checking whether a code path "
         "was or was not taken since the last time it was checked. It is also possible to "
         "reduce the candidates by determining whether a branch instruction has or has not "
         "been overwritten since it was first hit. Filter the candidates by branch kind, "
         "branch condition, origin or destination address, and origin or destination symbol "
         "name.\n\nAfter enough passes and experimentation, you may be able to find function "
         "calls and conditional code paths that are only taken when an action is performed "
         "in the emulated software."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (4/4)"),
      tr("Rows in the table can be left-clicked on the origin, destination, and symbol "
         "columns to view the associated address in Code View. Right-clicking the selected "
         "row(s) will bring up a context menu.\n\nIf the origin, destination, or symbol "
         "columns are right-clicked, an action copy the relevant address(es) to the clipboard "
         "will be available, and an action to set a breakpoint at the relevant address(es) "
         "will be available. Note that, for the origin / destination symbol columns, these "
         "actions will only be enabled if every row in the selection has a symbol.\n\nIf the "
         "instruction column of a row selection is right-clicked, an action to invert the "
         "branch instruction's condition and an action to invert the branch instruction's "
         "decrement check will be available, but only if the branch instruction is a "
         "conditional one.\n\nIf the condition column of a row selection is right-clicked, an "
         "action to make the branch instruction unconditional will be available, but only if "
         "the branch instruction is a conditional one.\n\nIf the origin column of a row "
         "selection is right-clicked, an action to replace the branch instruction at the "
         "origin(s) with a NOP instruction (No Operation) will be available.\n\nIf the "
         "destination column of a row selection is right-clicked, an action to replace the "
         "instruction at the destination(s) with a BLR instruction (Branch to Link Register) "
         "will be available, but will only be enabled if the branch instruction at every "
         "origin updates the link register.\n\nIf the origin / destination symbol column of a "
         "row selection is right-clicked, an action to replace the instruction at the start "
         "of the symbol(s) with a BLR instruction will be available, but will only be enabled "
         "if every row in the selection has a symbol.\n\nAll context menus have the action to "
         "delete the selected row(s) from the candidates."));
}

void BranchWatchDialog::ClickCell(const QModelIndex& index) const
{
  const int col = index.column();
  const bool is_symbol = col == Column::OriginSymbol || col == Column::DestinSymbol;
  if (!is_symbol && col != Column::Origin && col != Column::Destination)
    return;

  const QVariant target = m_filter->data(index, UserRole::ClickRole);
  // A row without a symbol has nothing to jump to.
  if (is_symbol && !target.isValid())
    return;
  m_code_widget->SetAddress(target.value<u32>(),
                            CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void BranchWatchDialog::OpenRowMenu(const QPoint& pos) const
{
  const QPoint where = m_table->viewport()->mapToGlobal(pos);
  // With every column hidden there are no cells to click, so offer a way to bring them back.
  if (m_table->horizontalHeader()->hiddenSectionCount() == Column::NumberOfColumns)
  {
    m_column_menu->exec(where);
    return;
  }

  const QModelIndex index = m_table->indexAt(pos);
  if (!index.isValid())
    return;
  m_menu_rows = m_table->selectionModel()->selectedRows(index.column());
  PrepareRowMenu(index.column())->exec(where);
  m_menu_rows = {};
}

void BranchWatchDialog::OpenColumnMenu(const QPoint& pos) const
{
  m_column_menu->exec(m_table->horizontalHeader()->mapToGlobal(pos));
}

void BranchWatchDialog::DeleteMenuRows() const
{
  QModelIndexList rows;
  rows.reserve(m_menu_rows.size());
  for (const QModelIndex& index : std::as_const(m_menu_rows))
    rows.append(m_filter->mapToSource(index));
  // Remove from the bottom up so the row numbers still to be removed stay valid.
  std::ranges::sort(rows, std::less{});
  for (auto it = rows.crbegin(); it != rows.crend(); ++it)
  {
    if (it->isValid())
      m_model->removeRow(it->row());
  }
  RefreshStatus();
}

void BranchWatchDialog::DeleteSelectedRows() const
{
  m_menu_rows = m_table->selectionModel()->selectedRows();
  DeleteMenuRows();
  m_menu_rows = {};
}

void BranchWatchDialog::CopyAddresses() const
{
  if (m_menu_rows.isEmpty())
    return;

  QStringList lines;
  lines.reserve(m_menu_rows.size());
  for (const QModelIndex& index : std::as_const(m_menu_rows))
    lines.append(QString::number(m_filter->data(index, UserRole::ClickRole).value<u32>(), 16));
  QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void BranchWatchDialog::PatchTargets(u32 instruction) const
{
  PPCDebugInterface& debugger = m_system.GetPowerPC().GetDebugInterface();
  {
    const Core::CPUThreadGuard guard{m_system};
    for (const QModelIndex& index : std::as_const(m_menu_rows))
    {
      const u32 address = m_filter->data(index, UserRole::ClickRole).value<u32>();
      debugger.SetPatch(guard, address, instruction);
      m_filter->SetInspected(index);
    }
  }
  // There is no narrower "memory edited from the UI" signal, so refresh the code view directly
  // rather than let it miss the patch.
  m_code_widget->Update();
}

void BranchWatchDialog::PatchOrigins(u32 (*edit)(u32)) const
{
  PPCDebugInterface& debugger = m_system.GetPowerPC().GetDebugInterface();
  {
    const Core::CPUThreadGuard guard{m_system};
    for (const QModelIndex& index : std::as_const(m_menu_rows))
    {
      // Unlike PatchTargets, the edit always lands on the branch itself.
      const Core::BranchWatchCollectionKey& key = m_filter->Entry(index).collection_ptr->first;
      debugger.SetPatch(guard, key.origin_addr, edit(key.original_inst.hex));
      m_filter->SetInspected(index);
    }
  }
  m_code_widget->Update();
}

void BranchWatchDialog::AddBreakpoints(bool break_on_hit, bool log_on_hit) const
{
  BreakPoints& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  for (const QModelIndex& index : std::as_const(m_menu_rows))
  {
    breakpoints.Add(m_filter->data(index, UserRole::ClickRole).value<u32>(), break_on_hit,
                    log_on_hit, {});
  }
  emit Host::GetInstance() -> PPCBreakpointsChanged();
}

// Each breakpoint action gets an icon if any target already has that kind of breakpoint: the
// "full" icon when all of them do, the "partial" one otherwise.
void BranchWatchDialog::ShowBreakpointCoverage() const
{
  qsizetype breaks = 0;
  qsizetype logs = 0;
  qsizetype both = 0;
  const BreakPoints& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  for (const QModelIndex& index : std::as_const(m_menu_rows))
  {
    const TBreakPoint* const existing =
        breakpoints.GetRegularBreakpoint(m_filter->data(index, UserRole::ClickRole).value<u32>());
    if (existing == nullptr)
      continue;
    if (existing->break_on_hit && existing->log_on_hit)
    {
      ++both;
    }
    else
    {
      breaks += existing->break_on_hit ? 1 : 0;
      logs += existing->log_on_hit ? 1 : 0;
    }
  }

  const qsizetype total = m_menu_rows.size();
  const auto decorate = [&](QAction* action, qsizetype count) {
    action->setIconVisibleInMenu(count != 0);
    action->setIcon(count == total ? m_all_icon : m_some_icon);
  };
  decorate(m_break_action, breaks);
  decorate(m_log_action, logs);
  decorate(m_break_log_action, both);
}

bool BranchWatchDialog::AllMenuRowsConditional() const
{
  return std::ranges::all_of(m_menu_rows, [this](const QModelIndex& index) {
    return BranchIsConditional(m_filter->Entry(index).collection_ptr->first.original_inst);
  });
}

QMenu* BranchWatchDialog::PrepareRowMenu(int column) const
{
  const bool running = Core::GetState(m_system) != Core::State::Uninitialized;
  static_assert(Column::NumberOfColumns == 8);
  switch (column)
  {
  case Column::Instruction:
    PrepareInstructionMenu(running);
    return m_instruction_menu;
  case Column::Condition:
    PrepareConditionMenu(running);
    return m_condition_menu;
  case Column::Origin:
    PrepareOriginMenu(running);
    return m_origin_menu;
  case Column::Destination:
    PrepareDestinationMenu(running);
    return m_destination_menu;
  case Column::OriginSymbol:
  case Column::DestinSymbol:
    PrepareSymbolMenu(running);
    return m_symbol_menu;
  case Column::RecentHits:
  case Column::TotalHits:
  default:
    return m_hits_menu;
  }
}

void BranchWatchDialog::RefreshOpenRowMenu(bool running) const
{
  if (m_instruction_menu->isVisible())
    PrepareInstructionMenu(running);
  else if (m_condition_menu->isVisible())
    PrepareConditionMenu(running);
  else if (m_origin_menu->isVisible())
    PrepareOriginMenu(running);
  else if (m_destination_menu->isVisible())
    PrepareDestinationMenu(running);
  else if (m_symbol_menu->isVisible())
    PrepareSymbolMenu(running);
}

// Patching needs a running core; the selection is only inspected when it is.

void BranchWatchDialog::PrepareInstructionMenu(bool running) const
{
  const bool allowed = running && AllMenuRowsConditional();
  m_invert_condition_action->setEnabled(allowed);
  m_invert_decrement_action->setEnabled(allowed);
}

void BranchWatchDialog::PrepareConditionMenu(bool running) const
{
  m_unconditional_action->setEnabled(running && AllMenuRowsConditional());
}

void BranchWatchDialog::PrepareOriginMenu(bool running) const
{
  ShowBreakpointCoverage();
  m_nop_action->setEnabled(running);
  m_copy_action->setEnabled(true);
  m_breakpoint_menu->setEnabled(true);
}

void BranchWatchDialog::PrepareDestinationMenu(bool running) const
{
  ShowBreakpointCoverage();
  // A BLR at the destination only returns correctly if every branch there saved LR.
  const bool allowed =
      running && std::ranges::all_of(m_menu_rows, [this](const QModelIndex& index) {
        return m_filter->Entry(index).collection_ptr->first.original_inst.LK != 0;
      });
  m_blr_action->setEnabled(allowed);
  m_copy_action->setEnabled(true);
  m_breakpoint_menu->setEnabled(true);
}

void BranchWatchDialog::PrepareSymbolMenu(bool running) const
{
  ShowBreakpointCoverage();
  const bool every_row_has_symbol =
      std::ranges::all_of(m_menu_rows, [this](const QModelIndex& index) {
        return m_filter->data(index, UserRole::ClickRole).isValid();
      });
  m_blr_action->setEnabled(running && every_row_has_symbol);
  m_copy_action->setEnabled(every_row_has_symbol);
  m_breakpoint_menu->setEnabled(every_row_has_symbol);
}

void BranchWatchDialog::StateChanged(Core::State state) const
{
  const bool running = state != Core::State::Uninitialized;
  RefreshOpenRowMenu(running);
  m_overwritten_button->setEnabled(running);
  m_not_overwritten_button->setEnabled(running);

  // Hit counts only move while recording and the emulated CPU is running.
  if (m_watch.GetRecordingActive() && state > Core::State::Paused)
    m_poll->start(POLL_INTERVAL_MS);
  else if (m_poll->isActive())
    m_poll->stop();
  Update();
}

void BranchWatchDialog::ReloadIcons()
{
  m_all_icon = Resources::GetThemeIcon("debugger_breakpoint");
  m_some_icon = Resources::GetThemeIcon("stop");
}

void BranchWatchDialog::RestoreSettings()
{
  const QSettings& settings = Settings::GetQSettings();
  restoreGeometry(settings.value(QStringLiteral("branchwatchdialog/geometry")).toByteArray());
  // The header state also carries which columns are hidden.
  const QVariant header = settings.value(QStringLiteral("branchwatchdialog/tableheader/state"));
  m_table->horizontalHeader()->restoreState(header.toByteArray());

  const std::array<std::pair<QAction*, QString>, 4> sections{{
      {m_branch_type_section, QStringLiteral("branchwatchdialog/toolbar/branch_type_hidden")},
      {m_address_section, QStringLiteral("branchwatchdialog/toolbar/origin_destin_hidden")},
      {m_condition_section, QStringLiteral("branchwatchdialog/toolbar/condition_hidden")},
      {m_misc_section, QStringLiteral("branchwatchdialog/toolbar/misc_controls_hidden")},
  }};
  for (const auto& [section, key] : sections)
    section->setVisible(!settings.value(key).toBool());
}

void BranchWatchDialog::StoreSettings() const
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(QStringLiteral("branchwatchdialog/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("branchwatchdialog/tableheader/state"),
                    m_table->horizontalHeader()->saveState());
  // Stored inverted: a missing key means the section is shown.
  const auto store_hidden = [&settings](const QString& key, const QAction* section) {
    settings.setValue(key, !section->isVisible());
  };
  store_hidden(QStringLiteral("branchwatchdialog/toolbar/branch_type_hidden"),
               m_branch_type_section);
  store_hidden(QStringLiteral("branchwatchdialog/toolbar/origin_destin_hidden"), m_address_section);
  store_hidden(QStringLiteral("branchwatchdialog/toolbar/condition_hidden"), m_condition_section);
  store_hidden(QStringLiteral("branchwatchdialog/toolbar/misc_controls_hidden"), m_misc_section);
}

void BranchWatchDialog::Update() const
{
  if (m_watch.GetRecordingPhase() == Core::BranchWatch::Phase::Blacklist)
    RefreshStatus();
  m_model->UpdateHits();
}

void BranchWatchDialog::RefreshStatus() const
{
  if (m_watch.GetRecordingPhase() == Core::BranchWatch::Phase::Blacklist)
  {
    const std::size_t found = m_watch.GetCollectionSize();
    const std::size_t excluded = m_watch.GetBlacklistSize();
    if (excluded == 0)
    {
      m_status->showMessage(tr("Candidates: %1").arg(found));
    }
    else
    {
      m_status->showMessage(tr("Candidates: %1 | Excluded: %2 | Remaining: %3")
                                .arg(found)
                                .arg(excluded)
                                .arg(found - excluded));
    }
    return;
  }

  const std::size_t selected = m_watch.GetSelection().size();
  if (selected == 0)
  {
    m_status->showMessage(tr("Zero candidates remaining."));
    return;
  }
  const std::size_t shown = static_cast<std::size_t>(m_filter->rowCount());
  m_status->showMessage(tr("Candidates: %1 | Filtered: %2 | Remaining: %3")
                            .arg(selected)
                            .arg(selected - shown)
                            .arg(shown));
}

void BranchWatchDialog::WriteSnapshot(const Core::CPUThreadGuard& guard, const std::string& path)
{
  File::IOFile file(path, "w");
  if (!file.IsOpen())
  {
    ModalMessageBox::warning(
        this, tr("Error"),
        tr("Failed to save Branch Watch snapshot \"%1\"").arg(QString::fromStdString(path)));
    return;
  }
  m_model->Save(guard, file.GetHandle());
}

void BranchWatchDialog::ReadSnapshot(const Core::CPUThreadGuard& guard, const std::string& path)
{
  File::IOFile file(path, "r");
  if (!file.IsOpen())
  {
    ModalMessageBox::warning(
        this, tr("Error"),
        tr("Failed to open Branch Watch snapshot \"%1\"").arg(QString::fromStdString(path)));
    return;
  }
  m_model->Load(guard, file.GetHandle());
  m_wipe_hits_button->setEnabled(m_watch.GetRecordingPhase() ==
                                 Core::BranchWatch::Phase::Reduction);
}

void BranchWatchDialog::AutoSave(const Core::CPUThreadGuard& guard)
{
  if (!m_autosave_action->isChecked() || !m_watch.CanSave())
    return;
  WriteSnapshot(guard, m_autosave_path ? *m_autosave_path : DefaultSnapshotPath());
}
