// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/BreakpointWidget.h"

#include <array>
#include <string>
#include <utility>

#include <QApplication>
#include <QCursor>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include "Common/Contains.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Debugger/BreakpointDialog.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
// Item data roles on the first cell of each row.
constexpr int ROLE_ADDRESS = Qt::UserRole;
constexpr int ROLE_IS_MEMCHECK = Qt::UserRole + 1;

// Table columns, left to right.
enum Column : int
{
  COL_ACTIVE,
  COL_TYPE,
  COL_FUNCTION,
  COL_START,
  COL_END,
  COL_BREAK,
  COL_LOG,
  COL_READ,
  COL_WRITE,
  COL_CONDITION,
  COLUMN_COUNT
};

// Draws a cell's decoration pixmap centred in the cell instead of at the left edge.
class CentredIconDelegate final : public QStyledItemDelegate
{
public:
  using QStyledItemDelegate::QStyledItemDelegate;

private:
  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override
  {
    Q_ASSERT(index.isValid());

    QStyleOptionViewItem plain = option;
    initStyleOption(&plain, index);
    plain.icon = QIcon();
    plain.decorationSize = QSize(0, 0);
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &plain, painter, nullptr);

    const QPixmap pixmap = qvariant_cast<QPixmap>(index.data(Qt::DecorationRole));
    if (pixmap.isNull())
      return;
    const QSize size = pixmap.deviceIndependentSize().toSize();
    const QRect cell = option.rect;
    const QPoint offset((cell.width() - size.width()) / 2, (cell.height() - size.height()) / 2);
    painter->drawPixmap(cell.topLeft() + offset, pixmap);
  }
};

QString EightHex(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

std::optional<Expression> ParseCondition(const QString& text)
{
  if (text.isEmpty())
    return std::nullopt;
  return Expression::TryParse(text.toUtf8().constData());
}

// Hit markers are drawn with a hole in the middle while breaking is switched off.
QPixmap PunchHole(const QPixmap& source)
{
  QImage image = source.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
  {
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::transparent);
    const float outer = static_cast<float>(image.height()) / 2.0f;
    painter.drawEllipse(QPointF(outer, outer), outer / 2.0f, outer / 2.0f);
  }
  return QPixmap::fromImage(image);
}

std::string GameIniPath()
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + SConfig::GetInstance().GetGameID() + ".ini";
}

void AnnounceChange()
{
  emit Host::GetInstance()->PPCBreakpointsChanged();
}
}  // namespace

BreakpointWidget::BreakpointWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance())
{
  setWindowTitle(tr("Breakpoints"));
  setObjectName(QStringLiteral("breakpoints"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsBreakpointsVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  BuildLayout();

  // Visibility above is applied first so that floating docks come up in the right state.
  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("breakpointwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("breakpointwidget/floating")).toBool());

  connect(&settings, &Settings::EmulationStateChanged, this, [this](Core::State state) {
    UpdateButtonsEnabled();
    if (state == Core::State::Uninitialized)
      Update();
  });
  connect(m_list, &QTableWidget::itemChanged, this, &BreakpointWidget::CellEdited);
  connect(&settings, &Settings::DebugFontChanged, this, &BreakpointWidget::ApplyFont);
  connect(&settings, &Settings::BreakpointsVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsBreakpointsVisible());
  });
  connect(&settings, &Settings::ThemeChanged, this, [this] {
    RefreshIcons();
    Update();
  });
  connect(Host::GetInstance(), &Host::PPCSymbolsChanged, this, &BreakpointWidget::Update);
  connect(Host::GetInstance(), &Host::PPCBreakpointsChanged, this, &BreakpointWidget::Update);

  RefreshIcons();
}

BreakpointWidget::~BreakpointWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("breakpointwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("breakpointwidget/floating"), isFloating());
}

void BreakpointWidget::BuildLayout()
{
  m_bar = new QToolBar;
  m_bar->setContentsMargins(0, 0, 0, 0);
  m_bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

  m_list = new QTableWidget;
  m_list->setItemDelegate(new CentredIconDelegate(this));
  m_list->setTabKeyNavigation(false);
  m_list->setContentsMargins(0, 0, 0, 0);
  m_list->setColumnCount(COLUMN_COUNT);
  m_list->setSelectionMode(QAbstractItemView::NoSelection);
  m_list->verticalHeader()->hide();

  ApplyFont(Settings::Instance().GetDebugFont());

  connect(m_list, &QTableWidget::itemClicked, this, &BreakpointWidget::CellClicked);
  connect(m_list, &QTableWidget::customContextMenuRequested, this,
          &BreakpointWidget::ShowRowMenu);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);

  auto* column = new QVBoxLayout;
  column->addWidget(m_bar);
  column->addWidget(m_list);
  column->setContentsMargins(2, 2, 2, 2);
  column->setSpacing(0);

  m_toggle_action = m_bar->addAction(tr("Disable"), this, &BreakpointWidget::ToggleBreaking);
  m_new_action = m_bar->addAction(tr("New"), this, &BreakpointWidget::CreateNew);
  m_clear_action = m_bar->addAction(tr("Clear"), this, &BreakpointWidget::ClearAll);
  m_load_action = m_bar->addAction(tr("Load"), this, &BreakpointWidget::LoadFromIni);
  m_save_action = m_bar->addAction(tr("Save"), this, &BreakpointWidget::SaveToIni);
  m_load_action->setEnabled(false);
  m_save_action->setEnabled(false);

  auto* body = new QWidget;
  body->setLayout(column);
  setWidget(body);
}

void BreakpointWidget::RefreshIcons()
{
  const bool breaking = m_system.GetPowerPC().GetBreakPoints().IsBreakingEnabled();
  m_toggle_action->setIcon(Resources::GetThemeIcon(breaking ? "pause" : "play"));
  m_new_action->setIcon(Resources::GetThemeIcon("debugger_add_breakpoint"));
  m_clear_action->setIcon(Resources::GetThemeIcon("debugger_clear"));
  m_load_action->setIcon(Resources::GetThemeIcon("debugger_load"));
  m_save_action->setIcon(Resources::GetThemeIcon("debugger_save"));
}

void BreakpointWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetBreakpointsVisible(false);
}

void BreakpointWidget::showEvent(QShowEvent*)
{
  UpdateButtonsEnabled();
  Update();
}

void BreakpointWidget::ApplyFont(const QFont& font)
{
  m_list->setFont(font);
  m_list->verticalHeader()->setDefaultSectionSize(m_list->fontMetrics().height());
  Update();
}

void BreakpointWidget::UpdateButtonsEnabled()
{
  if (!isVisible())
    return;
  const bool running = Core::IsRunning(m_system);
  m_load_action->setEnabled(running);
  m_save_action->setEnabled(running);
}

void BreakpointWidget::Update()
{
  if (!isVisible())
    return;

  const QSignalBlocker quiet(m_list);

  m_list->clear();
  m_list->setHorizontalHeaderLabels({tr("Active"), tr("Type"), tr("Function"), tr("Address"),
                                     // i18n: last address covered by the breakpoint
                                     tr("End Addr"), tr("Break"), tr("Log"), tr("Read"),
                                     tr("Write"), tr("Condition")});
  m_list->horizontalHeader()->setStretchLastSection(true);

  // Measure a row with one temporary row, then start from an empty table.
  m_list->setRowCount(1);
  const int row_height = m_list->rowHeight(0);
  m_list->setRowCount(0);

  auto& power_pc = m_system.GetPowerPC();
  auto& breakpoints = power_pc.GetBreakPoints();
  auto& memchecks = power_pc.GetMemChecks();
  auto& symbols = power_pc.GetSymbolDB();

  const int marker_size = static_cast<int>(0.8 * row_height);
  QPixmap marker =
      Resources::GetThemeIcon("debugger_breakpoint").pixmap(QSize(marker_size, marker_size));
  if (!breakpoints.IsBreakingEnabled())
    marker = PunchHole(marker);

  // Templates for the flag cells.
  QTableWidgetItem flag_off;
  flag_off.setFlags(Qt::NoItemFlags);
  QTableWidgetItem flag_on;
  flag_on.setData(Qt::DecorationRole, marker);
  QTableWidgetItem not_applicable;
  not_applicable.setFlags(Qt::NoItemFlags);
  const QColor grey =
      Settings::Instance().IsThemeDark() ? QColor(75, 75, 75) : QColor(225, 225, 225);
  not_applicable.setBackground(grey);

  const auto text_cell = [](const QString& text = {}) {
    auto* cell = new QTableWidgetItem(text);
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return cell;
  };
  const auto address_cell = [&](u32 address) {
    QTableWidgetItem* cell = text_cell(EightHex(address));
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
    return cell;
  };
  const auto flag_cell = [&](bool set) { return (set ? flag_on : flag_off).clone(); };
  const auto function_cell = [&](u32 address) {
    QTableWidgetItem* cell = text_cell();
    if (const Common::Symbol* symbol = symbols.GetSymbolFromAddr(address))
      cell->setText(QString::fromStdString(symbol->name).simplified());
    return cell;
  };
  const auto active_cell = [&](u32 address, bool memcheck, bool enabled) {
    QTableWidgetItem* cell = text_cell();
    cell->setData(ROLE_ADDRESS, address);
    cell->setData(ROLE_IS_MEMCHECK, memcheck);
    if (enabled)
      cell->setData(Qt::DecorationRole, marker);
    return cell;
  };
  const auto condition_cell = [&](const std::optional<Expression>& condition) {
    return text_cell(condition ? QString::fromStdString(condition->GetText()) : QString());
  };
  const auto add_row = [&](std::array<QTableWidgetItem*, COLUMN_COUNT> cells, bool greyed) {
    const int row = m_list->rowCount();
    m_list->setRowCount(row + 1);
    for (int col = 0; col < COLUMN_COUNT; ++col)
      m_list->setItem(row, col, cells[static_cast<size_t>(col)]);
    if (!greyed)
      return;
    for (int col = 0; col < m_list->columnCount(); ++col)
      m_list->item(row, col)->setBackground(grey);
  };

  for (const TBreakPoint& bp : breakpoints.GetBreakPoints())
  {
    std::array<QTableWidgetItem*, COLUMN_COUNT> cells{};
    cells[COL_ACTIVE] = active_cell(bp.address, false, bp.is_enabled);
    cells[COL_TYPE] = text_cell(QStringLiteral("BP"));
    cells[COL_FUNCTION] = function_cell(bp.address);
    cells[COL_START] = address_cell(bp.address);
    cells[COL_END] = not_applicable.clone();
    cells[COL_BREAK] = flag_cell(bp.break_on_hit);
    cells[COL_LOG] = flag_cell(bp.log_on_hit);
    cells[COL_READ] = not_applicable.clone();
    cells[COL_WRITE] = not_applicable.clone();
    cells[COL_CONDITION] = condition_cell(bp.condition);

    const bool does_nothing = !bp.log_on_hit && !bp.break_on_hit;
    add_row(cells, !bp.is_enabled || does_nothing);
  }

  // Instruction breakpoints are sorted by address; memory checks follow in their own order.
  m_list->sortItems(COL_START);

  for (const TMemCheck& mc : memchecks.GetMemChecks())
  {
    std::array<QTableWidgetItem*, COLUMN_COUNT> cells{};
    cells[COL_ACTIVE] = active_cell(mc.start_address, true, mc.is_enabled);
    cells[COL_TYPE] = text_cell(QStringLiteral("MBP"));
    cells[COL_FUNCTION] = function_cell(mc.start_address);
    cells[COL_START] = address_cell(mc.start_address);
    cells[COL_END] = address_cell(mc.end_address);
    cells[COL_END]->setData(ROLE_ADDRESS, mc.end_address);
    cells[COL_BREAK] = flag_cell(mc.break_on_hit);
    cells[COL_LOG] = flag_cell(mc.log_on_hit);
    cells[COL_READ] = flag_cell(mc.is_break_on_read);
    cells[COL_WRITE] = flag_cell(mc.is_break_on_write);
    cells[COL_CONDITION] = condition_cell(mc.condition);

    const bool no_access = !mc.is_break_on_write && !mc.is_break_on_read;
    const bool does_nothing = !mc.break_on_hit && !mc.log_on_hit;
    add_row(cells, !mc.is_enabled || no_access || does_nothing);
  }

  for (const int col : {COL_ACTIVE, COL_TYPE, COL_BREAK, COL_LOG, COL_READ, COL_WRITE})
    m_list->resizeColumnToContents(col);
}

void BreakpointWidget::CellClicked(QTableWidgetItem* item)
{
  if (!item)
    return;

  const int col = item->column();
  // Addresses are edited in place and the function name is not editable.
  if (col == COL_FUNCTION || col == COL_START || col == COL_END)
    return;

  const QTableWidgetItem* key = m_list->item(item->row(), COL_ACTIVE);
  const u32 address = static_cast<u32>(key->data(ROLE_ADDRESS).toUInt());

  if (col == COL_ACTIVE)
  {
    auto& power_pc = m_system.GetPowerPC();
    if (item->data(ROLE_IS_MEMCHECK).toBool())
      power_pc.GetMemChecks().ToggleEnable(address);
    else
      power_pc.GetBreakPoints().ToggleEnable(address);
    AnnounceChange();
    return;
  }

  // Every other column toggles its flag; the condition column asks for a new expression, and an
  // empty answer removes the condition.
  std::optional<QString> new_condition;
  if (col == COL_CONDITION)
  {
    bool ok = false;
    const QString typed = QInputDialog::getMultiLineText(
        this, tr("Edit Conditional"), tr("Edit conditional expression"), item->text(), &ok);
    if (!ok || typed == item->text())
      return;
    if (!typed.isEmpty())
      new_condition = typed;
  }

  if (key->data(ROLE_IS_MEMCHECK).toBool())
    ReplaceMemCheck(address, col, new_condition);
  else
    ReplaceBreakpoint(address, col, new_condition);
}

void BreakpointWidget::CellEdited(QTableWidgetItem* item)
{
  const int col = item->column();
  if (col != COL_START && col != COL_END)
    return;

  bool ok = false;
  const u32 typed = item->text().toUInt(&ok, 16);
  if (!ok)
    return;

  const QTableWidgetItem* key = m_list->item(item->row(), COL_ACTIVE);
  const u32 start = static_cast<u32>(key->data(ROLE_ADDRESS).toUInt());

  if (!key->data(ROLE_IS_MEMCHECK).toBool())
  {
    if (col == COL_START && typed != start)
      ReplaceBreakpoint(start, col, item->text());
    return;
  }

  const u32 end =
      static_cast<u32>(m_list->item(item->row(), COL_END)->data(ROLE_ADDRESS).toUInt());
  const bool editing_start = col == COL_START;
  if (typed == (editing_start ? start : end))
    return;

  // The range must stay ordered; anything else is reverted by redrawing the table.
  const bool ordered = editing_start ? typed <= end : typed >= start;
  if (ordered)
    ReplaceMemCheck(start, col, item->text());
  else
    Update();
}

void BreakpointWidget::ShowRowMenu(const QPoint& pos)
{
  const QTableWidgetItem* key = m_list->item(m_list->rowAt(pos.y()), COL_ACTIVE);
  if (!key)
    return;

  const u32 address = static_cast<u32>(key->data(ROLE_ADDRESS).toUInt());
  const bool memcheck = key->data(ROLE_IS_MEMCHECK).toBool();
  auto& power_pc = m_system.GetPowerPC();

  // Rows can be stale until the next redraw; skip ones whose entry is gone.
  const bool exists =
      memcheck ?
          Common::Contains(power_pc.GetMemChecks().GetMemChecks(), address,
                           &TMemCheck::start_address) :
          Common::Contains(power_pc.GetBreakPoints().GetBreakPoints(), address,
                           &TBreakPoint::address);
  if (!exists)
    return;

  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  if (memcheck)
  {
    menu->addAction(tr("Show in Memory"), [this, address] { emit ShowMemory(address); });
    menu->addAction(tr("Edit..."), [this, address] { OpenEditor(address, false); });
    menu->addAction(tr("Delete"), [this, address] {
      const QSignalBlocker quiet(Settings::Instance());
      m_system.GetPowerPC().GetMemChecks().Remove(address);
      AnnounceChange();
    });
  }
  else
  {
    menu->addAction(tr("Show in Code"), [this, address] { emit ShowCode(address); });
    menu->addAction(tr("Edit..."), [this, address] { OpenEditor(address, true); });
    menu->addAction(tr("Delete"), [this, address] {
      m_system.GetPowerPC().GetBreakPoints().Remove(address);
      AnnounceChange();
    });
  }

  menu->exec(QCursor::pos());
}

void BreakpointWidget::ToggleBreaking()
{
  auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  auto& memchecks = m_system.GetPowerPC().GetMemChecks();

  // Code and memory breakpoints are switched together. Memory checks report none while breaking
  // is off, so they are counted while it is on.
  const bool turning_on = !breakpoints.IsBreakingEnabled();
  bool any_memchecks = false;
  if (!turning_on)
    any_memchecks = memchecks.HasAny();
  breakpoints.EnableBreaking(turning_on);
  memchecks.EnableBreaking(turning_on);
  if (turning_on)
    any_memchecks = memchecks.HasAny();

  m_toggle_action->setText(turning_on ? tr("Disable") : tr("Enable"));
  m_toggle_action->setIcon(Resources::GetThemeIcon(turning_on ? "pause" : "play"));

  if (any_memchecks || !breakpoints.GetBreakPoints().empty())
    AnnounceChange();
}

void BreakpointWidget::ClearAll()
{
  auto& debug = m_system.GetPowerPC().GetDebugInterface();
  debug.ClearAllBreakpoints();
  {
    const QSignalBlocker quiet(Settings::Instance());
    debug.ClearAllMemChecks();
  }
  m_list->setRowCount(0);
  AnnounceChange();
}

void BreakpointWidget::CreateNew()
{
  auto* dialog = new BreakpointDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  dialog->exec();
}

void BreakpointWidget::OpenEditor(u32 address, bool instruction)
{
  auto& power_pc = m_system.GetPowerPC();
  BreakpointDialog* dialog =
      instruction ? new BreakpointDialog(this, power_pc.GetBreakPoints().GetRegularBreakpoint(address)) :
                    new BreakpointDialog(this, power_pc.GetMemChecks().GetMemCheck(address));
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  dialog->exec();

  AnnounceChange();
}

void BreakpointWidget::LoadFromIni()
{
  Common::IniFile ini;
  if (!ini.Load(GameIniPath(), false))
    return;

  BreakPoints::TBreakPointsStr code_lines;
  if (ini.GetLines("BreakPoints", &code_lines, false))
  {
    auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
    breakpoints.Clear();
    breakpoints.AddFromStrings(code_lines);
  }

  MemChecks::TMemChecksStr memory_lines;
  if (ini.GetLines("MemoryBreakPoints", &memory_lines, false))
  {
    auto& memchecks = m_system.GetPowerPC().GetMemChecks();
    memchecks.Clear();
    const QSignalBlocker quiet(Settings::Instance());
    memchecks.AddFromStrings(memory_lines);
  }

  AnnounceChange();
}

void BreakpointWidget::SaveToIni()
{
  Common::IniFile ini;
  ini.Load(GameIniPath(), false);
  ini.SetLines("BreakPoints", m_system.GetPowerPC().GetBreakPoints().GetStrings());
  ini.SetLines("MemoryBreakPoints", m_system.GetPowerPC().GetMemChecks().GetStrings());
  ini.Save(GameIniPath());
}

void BreakpointWidget::AddBP(u32 addr)
{
  AddBP(addr, true, true, {});
}

void BreakpointWidget::AddBP(u32 addr, bool break_on_hit, bool log_on_hit, const QString& condition)
{
  m_system.GetPowerPC().GetBreakPoints().Add(addr, break_on_hit, log_on_hit,
                                             ParseCondition(condition));
  AnnounceChange();
}

void BreakpointWidget::AddAddressMBP(u32 addr, bool on_read, bool on_write, bool do_log,
                                     bool do_break, const QString& condition)
{
  StoreMemCheck(addr, addr, false, on_read, on_write, do_log, do_break, condition);
}

void BreakpointWidget::AddRangedMBP(u32 from, u32 to, bool do_read, bool do_write, bool do_log,
                                    bool do_break, const QString& condition)
{
  StoreMemCheck(from, to, true, do_read, do_write, do_log, do_break, condition);
}

void BreakpointWidget::StoreMemCheck(u32 from, u32 to, bool ranged, bool on_read, bool on_write,
                                     bool do_log, bool do_break, const QString& condition)
{
  TMemCheck check;
  check.start_address = from;
  check.end_address = to;
  check.is_ranged = ranged;
  check.is_break_on_read = on_read;
  check.is_break_on_write = on_write;
  check.log_on_hit = do_log;
  check.break_on_hit = do_break;
  check.condition = ParseCondition(condition);
  {
    const QSignalBlocker quiet(Settings::Instance());
    m_system.GetPowerPC().GetMemChecks().Add(std::move(check));
  }
  AnnounceChange();
}

void BreakpointWidget::ReplaceBreakpoint(u32 address, int column, std::optional<QString> text)
{
  auto& breakpoints = m_system.GetPowerPC().GetBreakPoints();
  const TBreakPoint* current = breakpoints.GetRegularBreakpoint(address);

  TBreakPoint updated;
  updated.is_enabled = current->is_enabled != (column == COL_ACTIVE);
  updated.log_on_hit = current->log_on_hit != (column == COL_LOG);
  updated.break_on_hit = current->break_on_hit != (column == COL_BREAK);
  updated.address = address;

  if (column == COL_START && text)
  {
    bool ok = false;
    updated.address = text->toUInt(&ok, 16);
    if (!ok)
      return;
  }

  if (column == COL_CONDITION)
  {
    if (text)
      updated.condition = Expression::TryParse(text->toUtf8().constData());
  }
  else if (current->condition)
  {
    updated.condition = Expression::TryParse(current->condition->GetText());
  }

  // Adding never replaces an instruction breakpoint, so drop the old one first.
  breakpoints.Remove(address);
  breakpoints.Add(std::move(updated));
  AnnounceChange();
}

void BreakpointWidget::ReplaceMemCheck(u32 address, int column, std::optional<QString> text)
{
  auto& memchecks = m_system.GetPowerPC().GetMemChecks();
  const TMemCheck* current = memchecks.GetMemCheck(address);

  TMemCheck updated;
  updated.is_enabled = current->is_enabled != (column == COL_ACTIVE);
  updated.log_on_hit = current->log_on_hit != (column == COL_LOG);
  updated.break_on_hit = current->break_on_hit != (column == COL_BREAK);
  updated.is_break_on_read = current->is_break_on_read != (column == COL_READ);
  updated.is_break_on_write = current->is_break_on_write != (column == COL_WRITE);
  updated.start_address = current->start_address;
  updated.end_address = current->end_address;

  // A new start address makes a new entry (the old one is removed below); a new end address
  // updates the existing entry in place.
  bool moved = false;
  if ((column == COL_START || column == COL_END) && text)
  {
    bool ok = false;
    const u32 typed = text->toUInt(&ok, 16);
    if (!ok)
      return;
    if (column == COL_START)
    {
      updated.start_address = typed;
      moved = true;
    }
    else
    {
      updated.end_address = typed;
    }
  }

  updated.is_ranged = updated.start_address != updated.end_address;

  if (column == COL_CONDITION)
  {
    if (text)
      updated.condition = Expression::TryParse(text->toUtf8().constData());
  }
  else if (current->condition)
  {
    updated.condition = Expression::TryParse(current->condition->GetText());
  }

  {
    const QSignalBlocker quiet(Settings::Instance());
    memchecks.Add(std::move(updated));
    if (moved)
      memchecks.Remove(address);
  }
  AnnounceChange();
}
