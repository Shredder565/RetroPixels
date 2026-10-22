// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/RegisterWidget.h"

#include <array>
#include <utility>

#include <QActionGroup>
#include <QCursor>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Core/Core.h"
#include "Core/Debugger/CodeTrace.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Settings.h"

namespace
{
// The grid: r0-r31 in columns 0/1, f0-f31 (both halves) in 2/3/4, special registers in 5/6, segment
// registers, GQRs and HIDs in 7/8.
constexpr int GRID_COLUMNS = 9;
constexpr int GPR_COLUMN = 0;
constexpr int FPR_COLUMN = 2;
constexpr int PS1_COLUMN = 4;
constexpr int SPECIAL_COLUMN = 5;
constexpr int SIDE_COLUMN = 7;
constexpr int REGISTER_ROWS = 32;
}  // namespace

RegisterWidget::RegisterWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance())
{
  setWindowTitle(tr("Registers"));
  setObjectName(QStringLiteral("registers"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsRegistersVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  BuildLayout();

  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("registerwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("registerwidget/floating")).toBool());

  FillGrid();

  connect(m_grid, &QTableWidget::customContextMenuRequested, this, &RegisterWidget::ShowCellMenu);
  connect(m_grid, &QTableWidget::itemChanged, this, &RegisterWidget::CellEdited);
  connect(&settings, &Settings::DebugFontChanged, this, &RegisterWidget::ApplyFont);

  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &RegisterWidget::RefreshIfPaused);
  connect(&settings, &Settings::RegistersVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsRegistersVisible());
  });
}

RegisterWidget::~RegisterWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("registerwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("registerwidget/floating"), isFloating());
}

void RegisterWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetRegistersVisible(false);
}

void RegisterWidget::showEvent(QShowEvent*)
{
  RefreshIfPaused();
}

void RegisterWidget::BuildLayout()
{
  m_grid = new QTableWidget;
  m_grid->setTabKeyNavigation(false);
  m_grid->setColumnCount(GRID_COLUMNS);
  m_grid->verticalHeader()->setVisible(false);
  m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
  m_grid->setSelectionMode(QAbstractItemView::NoSelection);

  ApplyFont(Settings::Instance().GetDebugFont());

  // Blank header labels: the header is only there so that columns can be resized.
  QStringList blank_headers;
  for (int i = 0; i < GRID_COLUMNS; ++i)
    blank_headers.append(QString{});
  m_grid->setHorizontalHeaderLabels(blank_headers);

  auto* column = new QVBoxLayout;
  column->addWidget(m_grid);
  column->setContentsMargins(2, 2, 2, 2);
  auto* body = new QWidget;
  body->setLayout(column);
  setWidget(body);
}

void RegisterWidget::ApplyFont(const QFont& font)
{
  if (!m_grid)
    return;
  m_grid->setFont(font);
  const int line = m_grid->fontMetrics().height();
  m_grid->verticalHeader()->setDefaultSectionSize(line);
  m_grid->horizontalHeader()->setFixedHeight(line * 5 / 4);
}

void RegisterWidget::CellEdited(QTableWidgetItem* item)
{
  // Only value cells carry a register type; labels are ignored, and so are our own redraws.
  if (m_refreshing || item->data(DATA_TYPE).isNull())
    return;
  static_cast<RegisterColumn*>(item)->SetValue();
}

void RegisterWidget::SetFormatQuietly(RegisterColumn* cell, RegisterDisplay format)
{
  m_refreshing = true;
  cell->SetDisplay(format);
  m_refreshing = false;
}

void RegisterWidget::ShowCellMenu()
{
  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  QTableWidgetItem* current = m_grid->currentItem();
  if (current && !current->data(DATA_TYPE).isNull())
  {
    auto* cell = static_cast<RegisterColumn*>(current);
    const auto kind = static_cast<RegisterType>(cell->data(DATA_TYPE).toInt());
    const RegisterDisplay shown_as = cell->GetDisplay();

    // i18n: "watch" as in monitoring an emulated memory location (not a clock)
    menu->addAction(tr("Add to &watch"), this, [this, cell] {
      const u32 address = static_cast<u32>(cell->GetValue());
      emit RequestWatch(QStringLiteral("reg_%1").arg(address, 8, 16, QLatin1Char('0')), address);
    });
    menu->addAction(tr("Add memory &breakpoint"), this, [this, cell] {
      emit RequestMemoryBreakpoint(static_cast<u32>(cell->GetValue()));
    });
    menu->addAction(tr("View &memory"), this,
                    [this, cell] { emit RequestViewInMemory(static_cast<u32>(cell->GetValue())); });
    menu->addAction(tr("View &code"), this,
                    [this, cell] { emit RequestViewInCode(static_cast<u32>(cell->GetValue())); });
    menu->addSeparator();

    // One choice for this cell, then the same choices for the whole column.
    auto* exclusive = new QActionGroup(menu);
    exclusive->setExclusive(true);
    std::array<QAction*, 5> this_cell{};
    this_cell[0] = menu->addAction(tr("Hexadecimal"));
    this_cell[1] = menu->addAction(tr("Signed Integer"));
    this_cell[2] = menu->addAction(tr("Unsigned Integer"));
    // i18n: display a value as a single precision float
    this_cell[3] = menu->addAction(tr("Float"));
    // i18n: display a value as a 64-bit float
    this_cell[4] = menu->addAction(tr("Double"));
    menu->addSeparator();

    std::array<QAction*, 5> whole_column{};
    whole_column[0] = menu->addAction(tr("All Hexadecimal"));
    whole_column[1] = menu->addAction(tr("All Signed Integer"));
    whole_column[2] = menu->addAction(tr("All Unsigned Integer"));
    // i18n: display a value as a single precision float
    whole_column[3] = menu->addAction(tr("All Float"));
    // i18n: display a value as a 64-bit float
    whole_column[4] = menu->addAction(tr("All Double"));
    for (size_t i = 0; i < whole_column.size(); ++i)
      whole_column[i]->setData(static_cast<int>(i));

    if (kind == RegisterType::gpr || kind == RegisterType::fpr)
    {
      menu->addSeparator();
      const std::string reg =
          fmt::format("{}{}", kind == RegisterType::gpr ? "r" : "f", m_grid->currentItem()->row());
      menu->addAction(tr("Run until hit (ignoring breakpoints)"),
                      [this, reg] { RunUntilRegisterChanges(reg); });
    }

    for (QAction* action : this_cell)
    {
      action->setCheckable(true);
      action->setVisible(false);
      action->setActionGroup(exclusive);
    }
    for (QAction* action : whole_column)
      action->setVisible(false);

    this_cell[static_cast<size_t>(shown_as)]->setChecked(true);

    // Integer registers offer the 32-bit views, float registers hex and double.
    std::array<bool, 5> offered{};
    if (kind == RegisterType::gpr)
      offered = {true, true, true, true, false};
    else if (kind == RegisterType::fpr)
      offered = {true, false, false, false, true};
    for (size_t i = 0; i < offered.size(); ++i)
    {
      if (!offered[i])
        continue;
      this_cell[i]->setVisible(true);
      whole_column[i]->setVisible(true);
    }

    for (size_t i = 0; i < this_cell.size(); ++i)
    {
      const auto format = static_cast<RegisterDisplay>(i);
      connect(this_cell[i], &QAction::triggered, this,
              [this, cell, format] { SetFormatQuietly(cell, format); });
    }
    for (QAction* action : whole_column)
    {
      connect(action, &QAction::triggered, this, [this, action] {
        m_refreshing = true;
        const int col = m_grid->currentItem()->column();
        const auto format = static_cast<RegisterDisplay>(action->data().toInt());
        for (int row = 0; row < REGISTER_ROWS; ++row)
          static_cast<RegisterColumn*>(m_grid->item(row, col))->SetDisplay(format);
        m_refreshing = false;
      });
    }

    menu->addSeparator();
  }

  menu->addAction(tr("Update"), this, [this] { emit RequestTableUpdate(); });
  menu->exec(QCursor::pos());
}

void RegisterWidget::RunUntilRegisterChanges(const std::string& reg) const
{
  CodeTrace trace;
  trace.SetRegTracked(reg);

  QMessageBox timeout_box(
      QMessageBox::NoIcon, tr("Timed Out"),
      tr("<font color='#ff0000'>AutoStepping timed out. Current instruction is irrelevant."),
      QMessageBox::Cancel);
  QPushButton* keep_going = timeout_box.addButton(tr("Keep Running"), QMessageBox::AcceptRole);

  // Step until the register changes; after a time-out the user may let it run on.
  for (;;)
  {
    AutoStepResults results;
    {
      const Core::CPUThreadGuard guard(m_system);
      results = trace.AutoStepping(guard, true);
    }
    emit Host::GetInstance()->UpdateDisasmDialog();

    if (!results.timed_out)
      return;
    timeout_box.exec();
    if (timeout_box.clickedButton() != static_cast<QAbstractButton*>(keep_going))
      return;
  }
}

void RegisterWidget::FillGrid()
{
  auto state = [this]() -> PowerPC::PowerPCState& { return m_system.GetPPCState(); };

  for (int i = 0; i < REGISTER_ROWS; ++i)
  {
    const size_t n = static_cast<size_t>(i);
    Place(
        i, GPR_COLUMN, RegisterType::gpr, fmt::format("r{}", i),
        [state, n] { return u64{state().gpr[n]}; },
        [state, n](u64 v) { state().gpr[n] = static_cast<u32>(v); });
    Place(
        i, FPR_COLUMN, RegisterType::fpr, fmt::format("f{}", i),
        [state, n] { return state().ps[n].PS0AsU64(); },
        [state, n](u64 v) { state().ps[n].SetPS0(v); });
    Place(
        i, PS1_COLUMN, RegisterType::fpr, "", [state, n] { return state().ps[n].PS1AsU64(); },
        [state, n](u64 v) { state().ps[n].SetPS1(v); });
  }

  // BATs: upper word in the high half, lower word in the low half. BAT4-7 live in a separate SPR
  // block from BAT0-3.
  const auto bat_reader = [state](int upper_spr, int lower_spr) {
    return [state, upper_spr, lower_spr] {
      const auto& ppc = state();
      return (u64{ppc.spr[upper_spr]} << 32) + ppc.spr[lower_spr];
    };
  };
  for (int i = 0; i < 4; ++i)
  {
    Place(i, SPECIAL_COLUMN, RegisterType::ibat, fmt::format("IBAT{}", i),
          bat_reader(SPR_IBAT0U + i * 2, SPR_IBAT0L + i * 2), nullptr);
    Place(i + 4, SPECIAL_COLUMN, RegisterType::ibat, fmt::format("IBAT{}", i + 4),
          bat_reader(SPR_IBAT4U + i * 2, SPR_IBAT4L + i * 2), nullptr);
    Place(i + 8, SPECIAL_COLUMN, RegisterType::dbat, fmt::format("DBAT{}", i),
          bat_reader(SPR_DBAT0U + i * 2, SPR_DBAT0L + i * 2), nullptr);
    Place(i + 12, SPECIAL_COLUMN, RegisterType::dbat, fmt::format("DBAT{}", i + 4),
          bat_reader(SPR_DBAT4U + i * 2, SPR_DBAT4L + i * 2), nullptr);
  }

  for (int i = 0; i < 8; ++i)
  {
    const int spr = SPR_GQR0 + i;
    Place(
        i + 16, SIDE_COLUMN, RegisterType::gqr, fmt::format("GQR{}", i),
        [state, spr] { return u64{state().spr[spr]}; }, nullptr);
  }

  // Readers and writers for a plain 32-bit SPR.
  const auto spr_read = [state](int spr) { return [state, spr] { return u64{state().spr[spr]}; }; };
  const auto spr_write = [state](int spr) {
    return [state, spr](u64 v) { state().spr[spr] = static_cast<u32>(v); };
  };

  Place(24, SIDE_COLUMN, RegisterType::hid, "HID0", spr_read(SPR_HID0), spr_write(SPR_HID0));
  Place(25, SIDE_COLUMN, RegisterType::hid, "HID1", spr_read(SPR_HID1), spr_write(SPR_HID1));
  Place(26, SIDE_COLUMN, RegisterType::hid, "HID2", spr_read(SPR_HID2), spr_write(SPR_HID2));
  Place(27, SIDE_COLUMN, RegisterType::hid, "HID4", spr_read(SPR_HID4), spr_write(SPR_HID4));

  for (int i = 0; i < 16; ++i)
  {
    const size_t n = static_cast<size_t>(i);
    Place(
        i, SIDE_COLUMN, RegisterType::sr, fmt::format("SR{}", i),
        [state, n] { return u64{state().sr[n]}; },
        [this, state, n](u64 v) {
          state().sr[n] = static_cast<u32>(v);
          m_system.GetMMU().SRUpdated();
        });
  }

  Place(
      16, SPECIAL_COLUMN, RegisterType::tb, "TB",
      [this] { return m_system.GetPowerPC().ReadFullTimeBaseValue(); }, nullptr);
  Place(
      17, SPECIAL_COLUMN, RegisterType::pc, "PC", [state] { return u64{state().pc}; },
      [state](u64 v) { state().pc = static_cast<u32>(v); });
  Place(18, SPECIAL_COLUMN, RegisterType::lr, "LR", spr_read(SPR_LR), spr_write(SPR_LR));
  Place(19, SPECIAL_COLUMN, RegisterType::ctr, "CTR", spr_read(SPR_CTR), spr_write(SPR_CTR));
  Place(
      20, SPECIAL_COLUMN, RegisterType::cr, "CR", [state] { return u64{state().cr.Get()}; },
      [state](u64 v) { state().cr.Set(static_cast<u32>(v)); });
  Place(
      21, SPECIAL_COLUMN, RegisterType::xer, "XER", [state] { return u64{state().GetXER().Hex}; },
      [state](u64 v) { state().SetXER(UReg_XER(static_cast<u32>(v))); });
  Place(
      22, SPECIAL_COLUMN, RegisterType::fpscr, "FPSCR",
      [state] { return u64{state().fpscr.Hex}; },
      [state](u64 v) { state().fpscr = static_cast<u32>(v); });
  Place(
      23, SPECIAL_COLUMN, RegisterType::msr, "MSR", [state] { return u64{state().msr.Hex}; },
      [this, state](u64 v) {
        state().msr.Hex = static_cast<u32>(v);
        m_system.GetPowerPC().MSRUpdated();
      });
  Place(24, SPECIAL_COLUMN, RegisterType::srr, "SRR0", spr_read(SPR_SRR0), spr_write(SPR_SRR0));
  Place(25, SPECIAL_COLUMN, RegisterType::srr, "SRR1", spr_read(SPR_SRR1), spr_write(SPR_SRR1));
  Place(
      26, SPECIAL_COLUMN, RegisterType::exceptions, "Exceptions",
      [state] { return u64{state().Exceptions}; },
      [state](u64 v) { state().Exceptions = static_cast<u32>(v); });
  Place(
      27, SPECIAL_COLUMN, RegisterType::int_mask, "Int Mask",
      [this] { return u64{m_system.GetProcessorInterface().GetMask()}; }, nullptr);
  Place(
      28, SPECIAL_COLUMN, RegisterType::int_cause, "Int Cause",
      [this] { return u64{m_system.GetProcessorInterface().GetCause()}; }, nullptr);
  Place(29, SPECIAL_COLUMN, RegisterType::dsisr, "DSISR", spr_read(SPR_DSISR),
        spr_write(SPR_DSISR));
  Place(30, SPECIAL_COLUMN, RegisterType::dar, "DAR", spr_read(SPR_DAR), spr_write(SPR_DAR));
  Place(
      31, SPECIAL_COLUMN, RegisterType::pt_hashmask, "Hash Mask",
      [state] {
        const auto& ppc = state();
        return u64{ppc.pagetable_mask | ppc.pagetable_base};
      },
      nullptr);

  emit RequestTableUpdate();
  m_grid->resizeColumnsToContents();
}

void RegisterWidget::Place(int row, int column, RegisterType type, const std::string& name,
                           std::function<u64()> read, std::function<void(u64)> write)
{
  auto* cell = new RegisterColumn(type, std::move(read), std::move(write));

  if (row >= m_grid->rowCount())
    m_grid->setRowCount(row + 1);

  // Unnamed cells (the second paired-single half) sit directly in the given column.
  int value_column = column;
  if (!name.empty())
  {
    auto* label = new QTableWidgetItem(QString::fromStdString(name));
    label->setFlags(Qt::ItemIsEnabled);
    QFont bold = label->font();
    bold.setBold(true);
    label->setFont(bold);
    m_grid->setItem(row, column, label);
    value_column = column + 1;
  }
  m_grid->setItem(row, value_column, cell);
  cell->setTextAlignment(Qt::AlignVCenter | Qt::AlignRight);

  connect(this, &RegisterWidget::UpdateTable, this, [cell] { cell->RefreshValue(); });
}

void RegisterWidget::RefreshIfPaused()
{
  if (!isVisible() || Core::GetState(m_system) != Core::State::Paused)
    return;
  m_refreshing = true;
  emit UpdateTable();
  m_refreshing = false;
}
