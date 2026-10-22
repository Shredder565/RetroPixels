// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/WatchWidget.h"

#include <algorithm>
#include <array>
#include <functional>
#include <string>

#include <QCursor>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
// Every cell stores its watch index (or -1 on the trailing blank row) and its column.
constexpr int ROLE_WATCH = Qt::UserRole;
constexpr int ROLE_COLUMN = Qt::UserRole + 1;

enum Column : int
{
  COL_LABEL,
  COL_ADDRESS,
  COL_HEX,
  COL_DECIMAL,
  COL_STRING,
  COL_FLOAT,
  COL_LOCKED,
  COLUMN_COUNT
};

QString EightHex(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

size_t Slot(int index)
{
  return static_cast<size_t>(index);
}

std::string GameIniPath()
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini";
}
}  // namespace

WatchWidget::WatchWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance())
{
  // i18n: "watch" as in monitoring an emulated memory location (not a clock)
  setWindowTitle(tr("Watch"));
  setObjectName(QStringLiteral("watch"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsWatchVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  BuildLayout();

  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("watchwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("watchwidget/floating")).toBool());

  connect(m_list, &QTableWidget::customContextMenuRequested, this, &WatchWidget::ShowRowMenu);
  connect(m_list, &QTableWidget::itemChanged, this, &WatchWidget::CellEdited);

  connect(&settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    RefreshButtons();
    if (state != Core::State::Starting)
      Refill();
  });
  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &WatchWidget::Refill);
  connect(&settings, &Settings::WatchVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this,
          [this](bool enabled) { setHidden(!enabled || !Settings::Instance().IsWatchVisible()); });
  connect(&settings, &Settings::ThemeChanged, this, &WatchWidget::RefreshIcons);

  RefreshIcons();
}

WatchWidget::~WatchWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("watchwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("watchwidget/floating"), isFloating());
}

void WatchWidget::BuildLayout()
{
  m_bar = new QToolBar;
  m_bar->setContentsMargins(0, 0, 0, 0);
  m_bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  m_list = new QTableWidget;
  m_list->setTabKeyNavigation(false);
  m_list->setContentsMargins(0, 0, 0, 0);
  m_list->setColumnCount(COLUMN_COUNT);
  m_list->verticalHeader()->setHidden(true);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_list->setHorizontalHeaderLabels({tr("Label"), tr("Address"), tr("Hexadecimal"),
                                     // i18n: The base 10 numeral system. Not related to
                                     // non-integer numbers
                                     tr("Decimal"),
                                     // i18n: a text value read from memory
                                     tr("String"),
                                     // i18n: a value shown as a float
                                     tr("Float"), tr("Locked")});
  m_list->setRowCount(1);
  AppendBlankRow(0);

  m_new_action = m_bar->addAction(tr("New"), this, &WatchWidget::AskForNewWatch);
  m_delete_action = m_bar->addAction(tr("Delete"), this, [this] {
    if (!m_list->selectedItems().empty())
      RemoveSelected();
  });
  m_clear_action = m_bar->addAction(tr("Clear"), this, &WatchWidget::RemoveAll);
  m_load_action = m_bar->addAction(tr("Load"), this, &WatchWidget::LoadFromIni);
  m_save_action = m_bar->addAction(tr("Save"), this, &WatchWidget::SaveToIni);
  for (QAction* action :
       {m_new_action, m_delete_action, m_clear_action, m_load_action, m_save_action})
  {
    action->setEnabled(false);
  }

  auto* column = new QVBoxLayout;
  column->setContentsMargins(2, 2, 2, 2);
  column->setSpacing(0);
  column->addWidget(m_bar);
  column->addWidget(m_list);

  auto* body = new QWidget;
  body->setLayout(column);
  setWidget(body);
}

void WatchWidget::RefreshIcons()
{
  // There is no dedicated "add watch" icon; the add-breakpoint one stands in.
  m_new_action->setIcon(Resources::GetThemeIcon("debugger_add_breakpoint"));
  m_delete_action->setIcon(Resources::GetThemeIcon("debugger_delete"));
  m_clear_action->setIcon(Resources::GetThemeIcon("debugger_clear"));
  m_load_action->setIcon(Resources::GetThemeIcon("debugger_load"));
  m_save_action->setIcon(Resources::GetThemeIcon("debugger_save"));
}

void WatchWidget::RefreshButtons()
{
  if (!isVisible())
    return;
  const bool running = Core::IsRunning(m_system);
  for (QAction* action :
       {m_new_action, m_delete_action, m_clear_action, m_load_action, m_save_action})
  {
    action->setEnabled(running);
  }
}

void WatchWidget::Refill()
{
  if (!isVisible())
    return;

  // Values can only be read while the emulated CPU is paused.
  m_refilling = true;
  if (Core::GetState(m_system) != Core::State::Paused)
  {
    m_list->setDisabled(true);
    m_refilling = false;
    return;
  }

  m_list->setDisabled(false);
  m_list->clearContents();

  const Core::CPUThreadGuard guard(m_system);
  auto& debug = guard.GetSystem().GetPowerPC().GetDebugInterface();

  const int watch_count = static_cast<int>(debug.GetWatches().size());
  m_list->setRowCount(watch_count + 1);

  for (int index = 0; index < watch_count; ++index)
  {
    const auto& watch = debug.GetWatch(Slot(index));

    std::array<QTableWidgetItem*, COLUMN_COUNT> cells{};
    cells[COL_LABEL] = new QTableWidgetItem(QString::fromStdString(watch.name));
    cells[COL_ADDRESS] = new QTableWidgetItem(EightHex(watch.address));
    for (const int col : {COL_HEX, COL_DECIMAL, COL_STRING, COL_FLOAT, COL_LOCKED})
      cells[Slot(col)] = new QTableWidgetItem;
    cells[COL_LOCKED]->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);

    // The address turns red when it cannot be read.
    QBrush address_colour = QPalette().brush(QPalette::Text);
    const bool running = Core::IsRunning(m_system);
    if (!running || !PowerPC::MMU::HostIsRAMAddress(guard, watch.address))
      address_colour.setColor(Qt::red);

    if (running && PowerPC::MMU::HostIsRAMAddress(guard, watch.address))
    {
      cells[COL_HEX]->setText(EightHex(PowerPC::MMU::HostRead<u32>(guard, watch.address)));
      cells[COL_DECIMAL]->setText(
          QString::number(PowerPC::MMU::HostRead<u32>(guard, watch.address)));
      cells[COL_STRING]->setText(
          QString::fromStdString(PowerPC::MMU::HostGetString(guard, watch.address, 32)));
      cells[COL_FLOAT]->setText(
          QString::number(PowerPC::MMU::HostRead<float>(guard, watch.address)));
      cells[COL_LOCKED]->setCheckState(watch.locked ? Qt::Checked : Qt::Unchecked);
    }

    cells[COL_ADDRESS]->setForeground(address_colour);
    cells[COL_STRING]->setFlags(Qt::ItemIsEnabled);

    for (int col = 0; col < COLUMN_COUNT; ++col)
    {
      QTableWidgetItem* cell = cells[Slot(col)];
      cell->setData(ROLE_WATCH, index);
      cell->setData(ROLE_COLUMN, col);
      m_list->setItem(index, col, cell);
    }
  }

  AppendBlankRow(watch_count);
  m_refilling = false;
}

void WatchWidget::AppendBlankRow(int row)
{
  // Typing a label into the blank row adds a watch at address 0.
  auto* label = new QTableWidgetItem;
  label->setData(ROLE_WATCH, -1);
  m_list->setItem(row, COL_LABEL, label);

  for (int col = COL_LABEL + 1; col < COLUMN_COUNT; ++col)
  {
    auto* filler = new QTableWidgetItem;
    filler->setFlags(Qt::ItemIsEnabled);
    m_list->setItem(row, col, filler);
  }
}

void WatchWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetWatchVisible(false);
}

void WatchWidget::showEvent(QShowEvent*)
{
  RefreshButtons();
  Refill();
}

void WatchWidget::RemoveAll()
{
  m_system.GetPowerPC().GetDebugInterface().ClearWatches();
  Refill();
}

void WatchWidget::AskForNewWatch()
{
  const QString typed =
      QInputDialog::getText(this, tr("Input"), tr("Enter address to watch:"), QLineEdit::Normal,
                            QString{}, nullptr, Qt::WindowCloseButtonHint);
  bool ok = false;
  const u32 address = typed.toUInt(&ok, 16);
  if (!ok)
  {
    ModalMessageBox::warning(this, tr("Error"), tr("Invalid watch address: %1").arg(typed));
    return;
  }
  AddWatch(QStringLiteral("mem_%1").arg(address, 8, 16, QLatin1Char('0')), address);
}

void WatchWidget::LoadFromIni()
{
  Common::IniFile ini;
  std::vector<std::string> lines;
  if (!ini.Load(GameIniPath(), false))
    return;

  const Core::CPUThreadGuard guard(m_system);
  if (ini.GetLines("Watches", &lines, false))
  {
    // Release any locked values before the old watches go away.
    auto& debug = guard.GetSystem().GetPowerPC().GetDebugInterface();
    for (const auto& watch : debug.GetWatches())
      debug.UnsetPatch(guard, watch.address);
    debug.ClearWatches();
    debug.LoadWatchesFromStrings(lines);
  }

  Refill();
}

void WatchWidget::SaveToIni()
{
  Common::IniFile ini;
  ini.Load(GameIniPath(), false);
  ini.SetLines("Watches", m_system.GetPowerPC().GetDebugInterface().SaveWatchesToStrings());
  ini.Save(GameIniPath());
}

void WatchWidget::ShowRowMenu()
{
  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  if (!m_list->selectedItems().empty())
  {
    const qsizetype rows = m_list->selectionModel()->selectedRows().count();
    if (rows > 1)
    {
      // i18n: "watch" as in monitoring an emulated memory location (not a clock)
      menu->addAction(tr("&Delete Watches"), this, [this] { RemoveSelected(); });
      // i18n: "watch" as in monitoring an emulated memory location (not a clock)
      menu->addAction(tr("&Lock Watches"), this, [this] { SetSelectedLocked(true); });
      // i18n: "watch" as in monitoring an emulated memory location (not a clock)
      menu->addAction(tr("&Unlock Watches"), this, [this] { SetSelectedLocked(false); });
    }
    else if (rows == 1)
    {
      const QVariant stored = m_list->selectedItems().first()->data(ROLE_WATCH);
      const int index = stored.isNull() ? -1 : stored.toInt();
      if (index >= 0)
      {
        const auto address_of = [this, index] {
          return m_system.GetPowerPC().GetDebugInterface().GetWatch(Slot(index)).address;
        };
        menu->addAction(tr("Show in Memory"), this,
                        [this, address_of] { emit ShowMemory(address_of()); });
        // i18n: "watch" as in monitoring an emulated memory location (not a clock)
        menu->addAction(tr("&Delete Watch"), this, [this, index] { RemoveWatchAndRefill(index); });
        menu->addAction(tr("&Add Memory Breakpoint"), this,
                        [this, address_of] { emit RequestMemoryBreakpoint(address_of()); });
      }
    }
  }

  menu->addSeparator();
  menu->addAction(tr("Update"), this, &WatchWidget::Refill);
  menu->exec(QCursor::pos());
}

void WatchWidget::CellEdited(QTableWidgetItem* item)
{
  if (m_refilling || item->data(ROLE_WATCH).isNull())
    return;

  const int index = item->data(ROLE_WATCH).toInt();
  const int col = item->data(ROLE_COLUMN).toInt();

  if (index == -1)
  {
    // A label typed into the blank row.
    if (item->text().isEmpty())
      return;
    AddWatch(item->text(), 0);
    Refill();
    return;
  }

  auto& debug = m_system.GetPowerPC().GetDebugInterface();
  switch (col)
  {
  case COL_LABEL:
    // Clearing the label deletes the watch.
    if (item->text().isEmpty())
      RemoveWatchAndRefill(index);
    else
      debug.UpdateWatchName(Slot(index), item->text().toStdString());
    break;

  case COL_ADDRESS:
  case COL_HEX:
  case COL_DECIMAL:
  {
    bool ok = false;
    const u32 typed = item->text().toUInt(&ok, col == COL_DECIMAL ? 10 : 16);
    if (!ok)
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Invalid input provided"));
      break;
    }

    const Core::CPUThreadGuard guard(m_system);
    if (col == COL_ADDRESS)
    {
      // Moving a locked watch moves its lock too.
      const auto& watch = debug.GetWatch(Slot(index));
      debug.UnsetPatch(guard, watch.address);
      debug.UpdateWatchAddress(Slot(index), typed);
      if (watch.locked)
        FreezeValue(guard, typed);
    }
    else
    {
      PowerPC::MMU::HostWrite<u32>(guard, typed, debug.GetWatch(Slot(index)).address);
    }
    break;
  }

  case COL_LOCKED:
  {
    debug.UpdateWatchLockedState(Slot(index), item->checkState() == Qt::Checked);
    const auto& watch = debug.GetWatch(Slot(index));
    const Core::CPUThreadGuard guard(m_system);
    if (watch.locked)
      FreezeValue(guard, watch.address);
    else
      debug.UnsetPatch(guard, watch.address);
    break;
  }

  default:
    break;
  }

  Refill();
}

void WatchWidget::FreezeValue(const Core::CPUThreadGuard& guard, u32 address)
{
  // Re-apply the current four bytes every frame.
  const std::string current = PowerPC::MMU::HostGetString(guard, address, 4);
  const std::vector<u8> bytes(current.begin(), current.end());
  m_system.GetPowerPC().GetDebugInterface().SetFramePatch(guard, address, bytes);
}

std::vector<int> WatchWidget::SelectedWatchIndexes() const
{
  std::vector<int> indexes;
  for (const QModelIndex& selected : m_list->selectionModel()->selectedRows())
  {
    const QVariant stored = m_list->item(selected.row(), selected.column())->data(ROLE_WATCH);
    if (!stored.isNull())
      indexes.push_back(stored.toInt());
  }
  return indexes;
}

void WatchWidget::RemoveSelected()
{
  {
    const Core::CPUThreadGuard guard(m_system);
    std::vector<int> doomed = SelectedWatchIndexes();
    // Highest first, so that removing one does not shift the ones still to go.
    std::ranges::sort(doomed, std::greater<>{});
    for (const int index : doomed)
      RemoveWatch(guard, index);
  }
  Refill();
}

void WatchWidget::RemoveWatch(const Core::CPUThreadGuard& guard, int index)
{
  auto& debug = m_system.GetPowerPC().GetDebugInterface();
  debug.UnsetPatch(guard, debug.GetWatch(Slot(index)).address);
  debug.RemoveWatch(Slot(index));
}

void WatchWidget::RemoveWatchAndRefill(int index)
{
  {
    const Core::CPUThreadGuard guard(m_system);
    RemoveWatch(guard, index);
  }
  Refill();
}

void WatchWidget::SetSelectedLocked(bool locked)
{
  {
    const Core::CPUThreadGuard guard(m_system);
    auto& debug = m_system.GetPowerPC().GetDebugInterface();
    for (const int index : SelectedWatchIndexes())
    {
      const auto& watch = debug.GetWatch(Slot(index));
      if (watch.locked == locked)
        continue;
      debug.UpdateWatchLockedState(Slot(index), locked);
      if (locked)
        FreezeValue(guard, watch.address);
      else
        debug.UnsetPatch(guard, watch.address);
    }
  }
  Refill();
}

void WatchWidget::AddWatch(const QString& name, u32 addr)
{
  m_system.GetPowerPC().GetDebugInterface().SetWatch(addr, name.toStdString());
  Refill();
}
