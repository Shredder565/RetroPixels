// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/CodeViewWidget.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <set>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QTableWidgetItem>
#include <QWheelEvent>

#include "Common/Assert.h"
#include "Common/GekkoDisassembler.h"
#include "Core/Core.h"
#include "Core/Debugger/CodeTrace.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/AssembleInstructionDialog.h"
#include "DolphinQt/Debugger/EditSymbolDialog.h"
#include "DolphinQt/Debugger/PatchInstructionDialog.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/FromStdString.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
enum TableColumn : int
{
  COL_BREAKPOINT,
  COL_ADDRESS,
  COL_INSTRUCTION,
  COL_PARAMETERS,
  COL_DESCRIPTION,
  COL_NOTE,
  COL_ARROWS,
  COL_COUNT,
};

constexpr u32 INSTRUCTION_BYTES = 4;
constexpr u32 BLR_INSTRUCTION = 0x4e800020;
constexpr u32 NOP_INSTRUCTION = 0x60000000;

// Each arrow gets a lane this many pixels wide; its vertical stroke sits this far into the lane.
constexpr int ARROW_LANE_WIDTH = 16;
constexpr int ARROW_STEM_OFFSET = 10;

// A wheel notch is reported as 120 eighths of a degree, i.e. 15 degrees.
constexpr double WHEEL_EIGHTHS_PER_NOTCH = 15.0 * 8;

// "0x" plus eight digits: a parameter that ends in an absolute branch target.
constexpr std::size_t TARGET_OPERAND_LENGTH = 10;

QString Hex8(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

// Splits disassembly at the tab between the opcode name and its operands.
std::pair<std::string, std::string> SplitDisassembly(const std::string& text)
{
  const std::size_t tab = text.find('\t');
  if (tab == std::string::npos)
    return {text, std::string{}};
  return {text.substr(0, tab), text.substr(tab + 1)};
}

// The disassembler renders a resolved branch target as "->0x........"; 0 if there is none.
u32 BranchTarget(const Core::CPUThreadGuard& guard, u32 address)
{
  const std::string text =
      guard.GetSystem().GetPowerPC().GetDebugInterface().Disassemble(&guard, address);
  const std::size_t arrow = text.find("->0x");
  if (arrow == std::string::npos)
    return 0;
  return static_cast<u32>(std::stoul(text.substr(arrow + 2), nullptr, 16));
}

// Mnemonics of linking branches end in "l" or "la", optionally followed by a hint sign.
bool SavesLinkRegister(std::string_view mnemonic)
{
  if (mnemonic.ends_with('+') || mnemonic.ends_with('-'))
    mnemonic.remove_suffix(1);
  return mnemonic.ends_with('l') || mnemonic.ends_with("la");
}

// Loads ("l..." except "li..."), stores and paired-single quantised loads and stores. The
// register values only make sense for the instruction at PC, so callers should keep that in mind.
bool AccessesMemory(std::string_view text)
{
  if (text.starts_with("st") || text.starts_with("psq_l") || text.starts_with("psq_s"))
    return true;
  return text.starts_with('l') && !text.starts_with("li");
}

// Reduces a set of tracked byte addresses to one entry per run of up to four adjacent bytes.
std::set<u32> CollapseByteRuns(const std::set<u32>& bytes)
{
  std::set<u32> starts;
  u32 run_start = 0;
  u32 previous = 0;
  bool in_run = false;
  for (const u32 address : bytes)
  {
    if (in_run && address == previous + 1 && address - run_start <= 3)
    {
      previous = address;
      continue;
    }
    starts.insert(address);
    run_start = previous = address;
    in_run = true;
  }
  return starts;
}

// The breakpoint icon, faded out when the breakpoint (or breaking as a whole) is off.
QPixmap BreakpointPixmap(int edge, bool active)
{
  const QPixmap full = Resources::GetThemeIcon("debugger_breakpoint").pixmap(QSize(edge, edge));
  if (active)
    return full;
  QPixmap faded(full.size());
  faded.fill(Qt::transparent);
  QPainter painter(&faded);
  painter.setOpacity(0.20);
  painter.drawPixmap(0, 0, full);
  painter.end();
  return faded;
}
}  // namespace

// Draws the branch arrows that cross or end at each row of the arrow column.
class CodeViewWidget::ArrowDelegate final : public QStyledItemDelegate
{
public:
  explicit ArrowDelegate(CodeViewWidget* owner) : QStyledItemDelegate(owner), m_owner(owner) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override
  {
    QStyledItemDelegate::paint(painter, option, index);

    painter->save();
    painter->setClipRect(option.rect);
    painter->setPen(m_owner->palette().text().color());

    const u32 here = m_owner->AddressForRow(index.row());
    const int mid_y = option.rect.top() + option.rect.height() / 2;
    for (const Arrow& arrow : m_owner->m_arrows)
    {
      const int lane_x = option.rect.left() + ARROW_LANE_WIDTH * static_cast<int>(arrow.lane);
      const int stem_x = lane_x + ARROW_STEM_OFFSET;
      if (arrow.links)
        DrawCallStub(painter, arrow, here, lane_x, stem_x, mid_y);
      else
        DrawSpan(painter, option.rect, arrow, here, lane_x, stem_x, mid_y);
    }

    painter->restore();
  }

private:
  // Calls usually jump far away, so instead of a full span they only get a short arrow pointing
  // out of the calling row.
  static void DrawCallStub(QPainter* painter, const Arrow& arrow, u32 here, int lane_x, int stem_x,
                           int mid_y)
  {
    if (here != arrow.from)
      return;
    painter->drawLine(lane_x, mid_y, stem_x, mid_y);
    painter->drawLine(stem_x, mid_y, stem_x - 6, mid_y - 3);
    painter->drawLine(stem_x, mid_y, stem_x - 6, mid_y + 3);
  }

  static void DrawSpan(QPainter* painter, const QRect& cell, const Arrow& arrow, u32 here,
                       int lane_x, int stem_x, int mid_y)
  {
    const auto [low, high] = std::minmax(arrow.from, arrow.to);
    if (here < low || here > high)
      return;

    // The stem runs through the row, stopping at the middle of the rows at either end.
    const bool at_low = here == low;
    const bool at_high = here == high;
    painter->drawLine(stem_x, at_low ? mid_y : cell.top(), stem_x, at_high ? mid_y : cell.bottom());
    if (at_low || at_high)
      painter->drawLine(lane_x, mid_y, stem_x, mid_y);
    if (here == arrow.to)
    {
      painter->drawLine(lane_x, mid_y, lane_x + 6, mid_y - 3);
      painter->drawLine(lane_x, mid_y, lane_x + 6, mid_y + 3);
    }
  }

  CodeViewWidget* m_owner;
};

// Values shared by every row of one refresh.
struct CodeViewWidget::RowStyle
{
  std::optional<u32> pc;
  int row_height = 0;
  bool dark = false;
  bool breaking = false;
};

CodeViewWidget::CodeViewWidget()
    : m_system(Core::System::GetInstance()), m_symbol_db(m_system.GetPPCSymbolDB())
{
  setColumnCount(COL_COUNT);
  setShowGrid(false);
  setContextMenuPolicy(Qt::CustomContextMenu);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

  verticalHeader()->hide();
  QHeaderView* const header = horizontalHeader();
  header->setSectionResizeMode(COL_BREAKPOINT, QHeaderView::Fixed);
  header->setStretchLastSection(true);

  setHorizontalHeaderItem(COL_BREAKPOINT, new QTableWidgetItem());
  setHorizontalHeaderItem(COL_ADDRESS, new QTableWidgetItem(tr("Address")));
  // i18n: abbreviation of "Instruction", used as a narrow column title
  setHorizontalHeaderItem(COL_INSTRUCTION, new QTableWidgetItem(tr("Instr.")));
  setHorizontalHeaderItem(COL_PARAMETERS, new QTableWidgetItem(tr("Parameters")));
  setHorizontalHeaderItem(COL_DESCRIPTION, new QTableWidgetItem(tr("Symbols")));
  setHorizontalHeaderItem(COL_NOTE, new QTableWidgetItem(tr("Notes")));
  setHorizontalHeaderItem(COL_ARROWS, new QTableWidgetItem(tr("Branches")));

  setFont(Settings::Instance().GetDebugFont());
  setItemDelegateForColumn(COL_ARROWS, new ArrowDelegate(this));
  FontBasedSizing();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme) { MarkPCSelection(); });
#endif

  connect(this, &CodeViewWidget::customContextMenuRequested, this,
          &CodeViewWidget::ShowContextMenu);
  connect(this, &CodeViewWidget::itemSelectionChanged, this, &CodeViewWidget::MarkPCSelection);

  const Settings* const settings = &Settings::Instance();
  const Host* const host = Host::GetInstance();
  connect(settings, &Settings::DebugFontChanged, this, &CodeViewWidget::ApplyDebugFont);
  connect(settings, &Settings::EmulationStateChanged, this, &CodeViewWidget::FollowPCUnlessLocked);
  connect(host, &Host::UpdateDisasmDialog, this, &CodeViewWidget::FollowPCUnlessLocked);
  connect(host, &Host::PPCSymbolsChanged, this, qOverload<>(&CodeViewWidget::Update));
  connect(host, &Host::PPCBreakpointsChanged, this, qOverload<>(&CodeViewWidget::Update));
  connect(settings, &Settings::ThemeChanged, this, qOverload<>(&CodeViewWidget::Update));
}

CodeViewWidget::~CodeViewWidget() = default;

void CodeViewWidget::FollowPCUnlessLocked()
{
  if (!m_lock_address && Core::GetState(m_system) == Core::State::Paused)
    m_address = m_system.GetPPCState().pc;
  Update();
}

void CodeViewWidget::FontBasedSizing()
{
  // Measured text alone comes out a little tight with some fonts.
  constexpr int padding = 8;
  const QFontMetrics metrics(font());
  const auto text_width = [&metrics](const QString& text) {
    return metrics.boundingRect(text).width() + padding;
  };

  const int line = metrics.height() + 1;
  verticalHeader()->setMaximumSectionSize(line);
  horizontalHeader()->setMinimumSectionSize(line + 5);
  setColumnWidth(COL_BREAKPOINT, line + 5);
  setColumnWidth(COL_ADDRESS, text_width(QStringLiteral("80000000")));

  // Size the opcode and operand columns for "rlwinm. r31, r31, 16, 16, 31 (ffff0000)": a long but
  // common line. Rarer, longer ones can be accommodated by resizing the column by hand.
  const auto [mnemonic, operands] =
      SplitDisassembly(Common::GekkoDisassembler::Disassemble(0x57ff843fu, 0));
  setColumnWidth(COL_INSTRUCTION, text_width(QString::fromStdString(mnemonic)));
  setColumnWidth(COL_PARAMETERS, text_width(QString::fromStdString(operands)));
  setColumnWidth(COL_DESCRIPTION, metrics.boundingRect(QChar(u'0')).width() * 25 + padding);

  Update();
}

u32 CodeViewWidget::AddressForRow(int row) const
{
  // m_address sits in the middle row; every row is one instruction.
  const u32 rows_above = static_cast<u32>(rowCount() / 2);
  return m_address - rows_above * INSTRUCTION_BYTES + static_cast<u32>(row) * INSTRUCTION_BYTES;
}

void CodeViewWidget::Update()
{
  if (!isVisible() || m_in_update)
    return;

  // Memory is only read while paused; otherwise the rows are drawn without contents.
  if (Core::GetState(m_system) != Core::State::Paused)
  {
    Update(nullptr);
    return;
  }
  const Core::CPUThreadGuard guard(m_system);
  Update(&guard);
}

void CodeViewWidget::Update(const Core::CPUThreadGuard* guard)
{
  if (!isVisible() || m_in_update)
    return;
  m_in_update = true;

  clearSelection();
  // rowHeight(0) needs at least one row to exist.
  if (rowCount() == 0)
    setRowCount(1);
  const int fitting = static_cast<int>(
      std::round(static_cast<float>(height()) / static_cast<float>(rowHeight(0)) - 0.25));
  setRowCount(fitting);

  PowerPC::PowerPCManager& power_pc = m_system.GetPowerPC();
  RowStyle style;
  style.row_height = QFontMetrics(Settings::Instance().GetDebugFont()).height() + 1;
  for (int row = 0; row < fitting; ++row)
    setRowHeight(row, style.row_height);
  if (guard != nullptr)
    style.pc = power_pc.GetPPCState().pc;
  style.dark = Settings::Instance().IsThemeDark();
  style.breaking = power_pc.GetBreakPoints().IsBreakingEnabled();

  m_arrows.clear();
  for (int row = 0; row < rowCount(); ++row)
    FillRow(row, guard, style);

  if (m_symbol_db.NoteExists())
    showColumn(COL_NOTE);
  else
    hideColumn(COL_NOTE);

  AssignArrowLanes();
  repaint();
  m_in_update = false;
}

void CodeViewWidget::FillRow(int row, const Core::CPUThreadGuard* guard, const RowStyle& style)
{
  PPCDebugInterface& debugger = m_system.GetPowerPC().GetDebugInterface();
  const u32 address = AddressForRow(row);
  const u32 colour = debugger.GetColor(guard, address);

  auto* const bp_cell = new QTableWidgetItem;
  auto* const address_cell = new QTableWidgetItem(Hex8(address));

  const auto [mnemonic, operands] = SplitDisassembly(debugger.Disassemble(guard, address));
  const std::string description = debugger.GetDescription(address);
  const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);

  // Padding the opcode and operand text to a minimum width keeps the columns from jumping
  // around while scrolling.
  auto* const mnemonic_cell = new QTableWidgetItem(
      QStringLiteral("%1").arg(QString::fromStdString(mnemonic), -7, QLatin1Char(' ')));
  auto* const operands_cell = new QTableWidgetItem(
      QStringLiteral("%1").arg(QString::fromStdString(operands), -19, QLatin1Char(' ')));
  auto* const description_cell =
      new QTableWidgetItem(QStringLiteral("%1   ").arg(QtUtils::FromStdString(description)));
  auto* const note_cell =
      new QTableWidgetItem(note ? QString::fromStdString(note->name) : QString{});
  auto* const arrow_cell = new QTableWidgetItem();

  const std::array<QTableWidgetItem*, COL_COUNT> cells{
      bp_cell, address_cell, mnemonic_cell, operands_cell, description_cell, note_cell, arrow_cell};
  const bool is_pc = style.pc == address;
  for (QTableWidgetItem* const cell : cells)
  {
    cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    cell->setData(Qt::UserRole, address);
    // The breakpoint column keeps the memory colour even on the PC row.
    if (is_pc && cell != bp_cell)
    {
      cell->setBackground(QColor(Qt::green));
      cell->setForeground(QColor(Qt::black));
    }
    else if (colour != 0xFFFFFF)
    {
      cell->setBackground(style.dark ? QColor(colour).darker(400) : QColor(colour));
    }
  }

  // An operand ending in a full "0x........" target marks a direct branch (unless the row has
  // no symbol information at all).
  const std::size_t hex_at = operands.find("0x");
  const bool has_target =
      hex_at != std::string::npos && operands.size() - hex_at == TARGET_OPERAND_LENGTH;
  if (guard != nullptr && has_target && description != "---")
  {
    const u32 target = BranchTarget(*guard, address);
    m_arrows.push_back({.from = address, .to = target, .links = SavesLinkRegister(mnemonic)});
    description_cell->setText(
        tr("--> %1").arg(QtUtils::FromStdString(debugger.GetDescription(target))));
    operands_cell->setForeground(style.dark ? QColor(255, 135, 255) : QColor(Qt::magenta));
  }

  if (mnemonic == "blr")
    mnemonic_cell->setForeground(style.dark ? QColor(0xa0FFa0) : QColor(Qt::darkGreen));

  if (const TBreakPoint* const bp =
          m_system.GetPowerPC().GetBreakPoints().GetRegularBreakpoint(address))
  {
    const bool active = style.breaking && bp->is_enabled;
    bp_cell->setData(Qt::DecorationRole, BreakpointPixmap(style.row_height - 2, active));
  }

  for (int col = 0; col < COL_COUNT; ++col)
    setItem(row, col, cells[static_cast<std::size_t>(col)]);

  if (address == GetAddress())
    selectRow(address_cell->row());
}

// Gives every visible arrow the leftmost lane that is free over all the rows it covers. Shorter
// arrows are placed first so they hug the code; calls only occupy their own row.
void CodeViewWidget::AssignArrowLanes()
{
  const int row_total = rowCount();
  const std::size_t lane_total = m_arrows.size();
  if (row_total < 1 || lane_total < 1)
    return;
  const std::size_t height_rows = static_cast<std::size_t>(row_total);

  std::ranges::stable_sort(m_arrows, std::less{}, [](const Arrow& arrow) -> u32 {
    if (arrow.links)
      return 0;
    return arrow.from > arrow.to ? arrow.from - arrow.to : arrow.to - arrow.from;
  });

  // occupied[lane * rows + row]
  std::vector<bool> occupied(lane_total * height_rows, false);
  const auto slot = [&](std::size_t lane, u32 row) -> std::size_t {
    ASSERT(lane < lane_total && row < height_rows);
    return lane * height_rows + row;
  };

  // Places one arrow within a stretch of contiguous addresses [low_addr, high_addr] that starts
  // at table row base_row.
  const auto place = [&](Arrow& arrow, u32 low_addr, u32 base_row, u32 high_addr) {
    const auto [lo, hi] = std::minmax(arrow.from, arrow.links ? arrow.from : arrow.to);
    if (std::max(lo, low_addr) > std::min(hi, high_addr))
      return;

    const u32 first =
        (std::clamp(lo, low_addr, high_addr) - low_addr) / INSTRUCTION_BYTES + base_row;
    const u32 last =
        (std::clamp(hi, low_addr, high_addr) - low_addr) / INSTRUCTION_BYTES + base_row;
    const auto lane_is_free = [&](std::size_t lane) {
      for (u32 row = first; row <= last; ++row)
      {
        if (occupied[slot(lane, row)])
          return false;
      }
      return true;
    };

    std::size_t lane = 0;
    while (lane < lane_total && !lane_is_free(lane))
      ++lane;
    if (lane == lane_total)
      return;

    arrow.lane = static_cast<u32>(lane);
    for (u32 row = first; row <= last; ++row)
      occupied[slot(lane, row)] = true;
  };

  const u32 top_addr = AddressForRow(0);
  const u32 bottom_addr = AddressForRow(row_total - 1);
  if (top_addr <= bottom_addr)
  {
    for (Arrow& arrow : m_arrows)
      place(arrow, top_addr, 0, bottom_addr);
    return;
  }

  // The view wraps past fffffffc to 00000000 (the default view around address 0 does this), so
  // the address range is handled as two contiguous pieces.
  int zero_row = -1;
  for (int row = 0; row < row_total && zero_row < 0; ++row)
  {
    if (AddressForRow(row) == 0)
      zero_row = row;
  }
  ASSERT(zero_row != -1);
  for (Arrow& arrow : m_arrows)
  {
    place(arrow, top_addr, 0, 0xfffffffc);
    place(arrow, 0x00000000, static_cast<u32>(zero_row), bottom_addr);
  }
}

u32 CodeViewWidget::GetAddress() const
{
  return m_address;
}

u32 CodeViewWidget::GetContextAddress() const
{
  return m_context_address;
}

void CodeViewWidget::OnLockAddress(bool lock)
{
  m_lock_address = lock;
}

void CodeViewWidget::SetAddress(u32 address, SetAddressUpdate update)
{
  if (address == m_address)
    return;
  m_address = address;

  if (update == SetAddressUpdate::WithUpdate)
    Update();
  else if (update == SetAddressUpdate::WithDetailedUpdate)
    emit UpdateCodeWidget();  // lets the owning CodeWidget refresh its call lists too
}

void CodeViewWidget::MarkPCSelection()
{
  // The selected row normally follows the PC; give it the PC colours when it is on the PC.
  if (m_address == m_system.GetPPCState().pc)
  {
    setStyleSheet(
        QStringLiteral("QTableView::item:selected {background-color: #00FF00; color: #000000;}"));
    return;
  }
  if (!styleSheet().isEmpty())
    setStyleSheet(QString{});
}

void CodeViewWidget::ApplyDebugFont(const QFont& new_font)
{
  setFont(new_font);
  FontBasedSizing();
}

void CodeViewWidget::ShowContextMenu()
{
  auto* const menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  const bool running = Core::IsRunning(m_system);
  const bool paused = Core::GetState(m_system) == Core::State::Paused;
  const u32 address = GetContextAddress();

  // Actions that need a running core are collected here and enabled together.
  std::vector<QAction*> needs_core;
  const auto add = [this, menu](const QString& text, void (CodeViewWidget::*slot)()) {
    return menu->addAction(text, this, slot);
  };

  QAction* const follow = add(tr("Follow &Branch"), &CodeViewWidget::FollowBranch);
  menu->addSeparator();

  add(tr("&Copy Address"), &CodeViewWidget::CopyAddress);
  needs_core.push_back(add(tr("Copy &Function"), &CodeViewWidget::CopyFunction));
  needs_core.push_back(add(tr("Copy Code &Line"), &CodeViewWidget::CopyLine));
  needs_core.push_back(add(tr("Copy &Hex"), &CodeViewWidget::CopyHex));
  menu->addAction(tr("Show in &Memory"), this, [this] { emit ShowMemory(GetContextAddress()); });
  QAction* const show_target = add(tr("Show Target in Memor&y"), &CodeViewWidget::ShowTarget);
  QAction* const copy_target = add(tr("Copy Tar&get Address"), &CodeViewWidget::CopyTarget);
  menu->addSeparator();

  needs_core.push_back(add(tr("&Add function symbol"), &CodeViewWidget::AddFunctionSymbol));
  needs_core.push_back(add(tr("&Edit function symbol"), &CodeViewWidget::EditFunctionSymbol));
  QAction* const add_note = add(tr("Add Note"), &CodeViewWidget::AddNote);
  QAction* const edit_note = add(tr("Edit Note"), &CodeViewWidget::EditNote);
  needs_core.push_back(add_note);
  needs_core.push_back(edit_note);
  menu->addSeparator();

  needs_core.push_back(menu->addAction(tr("Run &to Here"), this, [this] {
    m_system.GetPowerPC().GetDebugInterface().RunTo(GetContextAddress());
  }));
  needs_core.push_back(menu->addAction(tr("PPC vs Host"), this, [this] {
    emit RequestPPCComparison(GetContextAddress(), m_system.GetPPCState().msr.IR);
  }));
  needs_core.push_back(
      menu->addAction(tr("&Insert BLR"), this, [this] { WriteStub(BLR_INSTRUCTION); }));
  needs_core.push_back(
      menu->addAction(tr("Insert &NOP"), this, [this] { WriteStub(NOP_INSTRUCTION); }));
  needs_core.push_back(
      menu->addAction(tr("Re&place Instruction"), this, [this] { EditInstruction(false); }));
  needs_core.push_back(
      menu->addAction(tr("Assemble Instruction"), this, [this] { EditInstruction(true); }));
  QAction* const restore = add(tr("Restore Instruction"), &CodeViewWidget::RestoreInstruction);

  // While paused, look at the instruction at PC: its first operand is what "Run Until" tracks
  // (only offered when the menu was opened on the PC row), and whether it accesses memory
  // decides the two "Target" actions.
  QString tracked;
  bool pc_accesses_memory = false;
  bool can_follow = false;
  if (paused)
  {
    const Core::CPUThreadGuard guard(m_system);
    const u32 pc = m_system.GetPPCState().pc;
    const std::string pc_text = m_system.GetPowerPC().GetDebugInterface().Disassemble(&guard, pc);
    if (address == pc)
    {
      const std::size_t tab = pc_text.find('\t');
      const std::size_t comma = tab == std::string::npos ? tab : pc_text.find(',', tab);
      if (comma != std::string::npos)
        tracked = QString::fromStdString(pc_text.substr(tab + 1, comma - tab - 1));
    }
    pc_accesses_memory = AccessesMemory(pc_text);
    can_follow = BranchTarget(guard, address) != 0;
  }

  QMenu* const run_until = menu->addMenu(tr("Run Until (Ignoring Breakpoints)"));
  // i18n: entries of the "Run Until (Ignoring Breakpoints)" submenu; %1 is a register name.
  run_until->addAction(tr("%1's value is hit").arg(tracked), this,
                       [this] { RunUntil(CodeTrace::AutoStop::Always); });
  run_until->addAction(tr("%1's value is used").arg(tracked), this,
                       [this] { RunUntil(CodeTrace::AutoStop::Used); });
  run_until->addAction(tr("%1's value is changed").arg(tracked), this,
                       [this] { RunUntil(CodeTrace::AutoStop::Changed); });
  run_until->setEnabled(!tracked.isEmpty());
  follow->setEnabled(can_follow);

  for (QAction* const action : needs_core)
    action->setEnabled(running);
  show_target->setEnabled(pc_accesses_memory);
  copy_target->setEnabled(pc_accesses_memory);

  // Editing needs a note under the cursor; adding one is refused right at a note's start.
  const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);
  edit_note->setEnabled(note != nullptr);
  if (note != nullptr && note->address == address)
    add_note->setEnabled(false);

  restore->setEnabled(running &&
                      m_system.GetPowerPC().GetDebugInterface().HasEnabledPatch(address));

  menu->exec(QCursor::pos());
  Update();
}

// Steps until the value in the tracked (first) register is hit, used or changed, following it
// through moves between registers and memory, and reports where it ended up.
void CodeViewWidget::RunUntil(CodeTrace::AutoStop condition)
{
  const Core::CPUThreadGuard guard(m_system);
  CodeTrace tracer;
  bool resume = false;

  QMessageBox report(QMessageBox::NoIcon, tr("Run Until"), {}, QMessageBox::Cancel);
  QPushButton* const keep_going = report.addButton(tr("Keep Running"), QMessageBox::AcceptRole);

  do
  {
    const AutoStepResults results = tracer.AutoStepping(guard, resume, condition);
    emit Host::GetInstance() -> UpdateDisasmDialog();
    resume = true;

    // Nothing was stepped (e.g. an invalid instruction).
    if (results.count == 0)
      return;

    if (results.reg_tracked.empty() && results.mem_tracked.empty())
    {
      QMessageBox::warning(
          this, tr("Overwritten"),
          tr("Target value was overwritten by current instruction.\nInstructions executed:   %1")
              .arg(QString::number(results.count)),
          QMessageBox::Cancel);
      return;
    }

    // A time-out is not fatal: the user may keep going.
    report.setText(results.timed_out ?
                       tr("<font color='#ff0000'>AutoStepping timed out. Current instruction is "
                          "irrelevant.") :
                       tr("Value tracked to current instruction."));

    const std::string registers = fmt::format("{}", fmt::join(results.reg_tracked, ", "));
    const std::string memory =
        fmt::format("{:#x}", fmt::join(CollapseByteRuns(results.mem_tracked), ", "));
    report.setInformativeText(
        tr("Instructions executed:   %1\nValue contained in:\nRegisters:   %2\nMemory:   %3")
            .arg(QString::number(results.count))
            .arg(QString::fromStdString(registers))
            .arg(QString::fromStdString(memory)));
    report.exec();
  } while (report.clickedButton() == keep_going);
}

std::string CodeViewWidget::DisassembleContext() const
{
  const Core::CPUThreadGuard guard(m_system);
  return m_system.GetPowerPC().GetDebugInterface().Disassemble(&guard, GetContextAddress());
}

// The memory address the instruction under the cursor accesses, worked out from the current
// register values (so only while paused).
std::optional<u32> CodeViewWidget::ContextTarget() const
{
  if (Core::GetState(m_system) != Core::State::Paused)
    return std::nullopt;
  const std::string line = DisassembleContext();
  if (!AccessesMemory(line))
    return std::nullopt;
  return m_system.GetPowerPC().GetDebugInterface().GetMemoryAddressFromInstruction(line);
}

void CodeViewWidget::CopyAddress()
{
  QApplication::clipboard()->setText(Hex8(GetContextAddress()));
}

void CodeViewWidget::CopyTarget()
{
  if (const std::optional<u32> target = ContextTarget())
    QApplication::clipboard()->setText(Hex8(*target));
}

void CodeViewWidget::ShowTarget()
{
  if (const std::optional<u32> target = ContextTarget())
    emit ShowMemory(*target);
}

void CodeViewWidget::CopyLine()
{
  QApplication::clipboard()->setText(QString::fromStdString(DisassembleContext()));
}

void CodeViewWidget::CopyFunction()
{
  const Common::Symbol* const symbol = m_symbol_db.GetSymbolFromAddr(GetContextAddress());
  if (symbol == nullptr)
    return;

  std::string listing = symbol->name + "\r\n";
  {
    const Core::CPUThreadGuard guard(m_system);
    const PPCDebugInterface& debugger = m_system.GetPowerPC().GetDebugInterface();
    const u32 end = symbol->address + symbol->size;
    for (u32 pos = symbol->address; pos != end; pos += INSTRUCTION_BYTES)
    {
      fmt::format_to(std::back_inserter(listing), "{:08x}: {}\r\n", pos,
                     debugger.Disassemble(&guard, pos));
    }
  }
  QApplication::clipboard()->setText(QString::fromStdString(listing));
}

void CodeViewWidget::CopyHex()
{
  u32 word;
  {
    const Core::CPUThreadGuard guard(m_system);
    word = m_system.GetPowerPC().GetDebugInterface().ReadInstruction(guard, GetContextAddress());
  }
  QApplication::clipboard()->setText(Hex8(word));
}

void CodeViewWidget::FollowBranch()
{
  u32 target;
  {
    const Core::CPUThreadGuard guard(m_system);
    target = BranchTarget(guard, GetContextAddress());
  }
  if (target != 0)
    SetAddress(target, SetAddressUpdate::WithDetailedUpdate);
}

void CodeViewWidget::AddFunctionSymbol()
{
  const u32 address = GetContextAddress();
  const int answer =
      QMessageBox::warning(this, tr("Add Function Symbol"),
                           tr("Force new function symbol to be made at %1?").arg(address, 0, 16),
                           QMessageBox::Ok | QMessageBox::Cancel);
  if (answer != QMessageBox::Ok)
    return;

  const Core::CPUThreadGuard guard(m_system);
  m_symbol_db.AddFunction(guard, address);
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::EditFunctionSymbol()
{
  const Common::Symbol* const symbol = m_symbol_db.GetSymbolFromAddr(GetContextAddress());
  // Nothing to edit yet: offer to create one instead.
  if (symbol == nullptr)
  {
    AddFunctionSymbol();
    return;
  }

  std::string new_name = symbol->name;
  u32 new_size = symbol->size;
  EditSymbolDialog dialog(this, symbol->address, &new_size, &new_name);
  if (dialog.exec() != QDialog::Accepted)
    return;
  if (dialog.DeleteRequested())
  {
    DeleteFunctionSymbol();
    return;
  }

  if (new_name != symbol->name)
    m_symbol_db.RenameSymbol(*symbol, new_name);
  if (new_size != symbol->size)
  {
    // Re-analyse a copy (which already carries any new name) over the new length.
    const Core::CPUThreadGuard guard(m_system);
    Common::Symbol resized = *symbol;
    PPCAnalyst::ReanalyzeFunction(guard, symbol->address, resized, new_size);
    m_symbol_db.AddCompleteSymbol(resized);
  }
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::DeleteFunctionSymbol()
{
  const u32 address = GetContextAddress();
  const Common::Symbol* const symbol = m_symbol_db.GetSymbolFromAddr(address);
  if (symbol == nullptr)
    return;

  const QString question = tr("Delete function symbol: %1\nat %2?")
                               .arg(QString::fromStdString(symbol->name))
                               .arg(address, 0, 16);
  if (QMessageBox::warning(this, tr("Delete Function Symbol"), question,
                           QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
  {
    return;
  }

  m_symbol_db.DeleteFunction(symbol->address);
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::AddNote()
{
  const u32 address = GetContextAddress();
  std::string text;
  u32 length = INSTRUCTION_BYTES;
  EditSymbolDialog dialog(this, address, &length, &text, EditSymbolDialog::Type::Note);
  if (dialog.exec() != QDialog::Accepted || dialog.DeleteRequested())
    return;

  m_symbol_db.AddKnownNote(address, length, text);
  m_symbol_db.DetermineNoteLayers();
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::EditNote()
{
  const Common::Note* const note = m_symbol_db.GetNoteFromAddr(GetContextAddress());
  if (note == nullptr)
    return;

  const u32 start = note->address;
  std::string text = note->name;
  u32 length = note->size;
  EditSymbolDialog dialog(this, start, &length, &text, EditSymbolDialog::Type::Note);
  if (dialog.exec() != QDialog::Accepted)
    return;
  if (dialog.DeleteRequested())
  {
    DeleteNote();
    return;
  }

  // Re-adding a note at the same start replaces it.
  if (text != note->name || length != note->size)
  {
    m_symbol_db.AddKnownNote(start, length, text);
    m_symbol_db.DetermineNoteLayers();
  }
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::DeleteNote()
{
  const u32 address = GetContextAddress();
  const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);
  if (note == nullptr)
    return;

  const QString question =
      tr("Delete Note: %1\nat %2?").arg(QString::fromStdString(note->name)).arg(address, 0, 16);
  if (QMessageBox::warning(this, tr("Delete Note"), question,
                           QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
  {
    return;
  }

  m_symbol_db.DeleteNote(note->address);
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void CodeViewWidget::WriteStub(u32 instruction)
{
  const Core::CPUThreadGuard guard(m_system);
  m_system.GetPowerPC().GetDebugInterface().SetPatch(guard, GetContextAddress(), instruction);
  Update(&guard);
}

void CodeViewWidget::EditInstruction(bool assemble)
{
  const Core::CPUThreadGuard guard(m_system);
  const u32 address = GetContextAddress();
  if (!PowerPC::MMU::HostIsInstructionRAMAddress(guard, address))
    return;
  if (!guard.GetSystem().GetMMU().TryReadInstruction(address).valid)
    return;

  PPCDebugInterface& debugger = m_system.GetPowerPC().GetDebugInterface();
  const auto run = [&](auto& dialog) {
    if (dialog.exec() != QDialog::Accepted)
      return;
    debugger.SetPatch(guard, address, dialog.GetCode());
    Update(&guard);
  };
  if (assemble)
  {
    AssembleInstructionDialog dialog(this, address, debugger.ReadInstruction(guard, address));
    run(dialog);
  }
  else
  {
    PatchInstructionDialog dialog(this, address, debugger.ReadInstruction(guard, address));
    run(dialog);
  }
}

void CodeViewWidget::RestoreInstruction()
{
  const Core::CPUThreadGuard guard(m_system);
  m_system.GetPowerPC().GetDebugInterface().UnsetPatch(guard, GetContextAddress());
  Update(&guard);
}

void CodeViewWidget::Scroll(s64 instructions)
{
  m_address = static_cast<u32>(m_address + static_cast<u64>(instructions) * INSTRUCTION_BYTES);
  Update();
}

void CodeViewWidget::resizeEvent(QResizeEvent*)
{
  Update();
}

void CodeViewWidget::showEvent(QShowEvent*)
{
  Update();
}

void CodeViewWidget::keyPressEvent(QKeyEvent* event)
{
  const int key = event->key();
  if (key == Qt::Key_Up || key == Qt::Key_Down)
  {
    Scroll(key == Qt::Key_Up ? -1 : 1);
    return;
  }
  if (key == Qt::Key_PageUp || key == Qt::Key_PageDown)
  {
    Scroll(key == Qt::Key_PageUp ? -s64{rowCount()} : s64{rowCount()});
    return;
  }
  if (key == Qt::Key_G && event->modifiers() == Qt::ControlModifier)
  {
    emit ActivateSearch();
    return;
  }
  // Deliberately the plain QWidget handler: the table's own key navigation is not wanted.
  QWidget::keyPressEvent(event);
}

void CodeViewWidget::wheelEvent(QWheelEvent* event)
{
  const int notches =
      static_cast<int>(std::round(event->angleDelta().y() / WHEEL_EIGHTHS_PER_NOTCH));
  // Wheel up (positive) moves towards lower addresses.
  if (notches != 0)
    Scroll(-s64{notches});
}

void CodeViewWidget::mousePressEvent(QMouseEvent* event)
{
  const QTableWidgetItem* const cell = itemAt(event->pos());
  if (cell == nullptr)
    return;

  const u32 address = cell->data(Qt::UserRole).toUInt();
  m_context_address = address;
  if (event->button() != Qt::LeftButton)
    return;

  if (column(cell) == COL_BREAKPOINT)
    ToggleBreakpoint();
  else
    SetAddress(address, SetAddressUpdate::WithDetailedUpdate);
  Update();
}

void CodeViewWidget::ToggleBreakpoint()
{
  m_system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(GetContextAddress());
  emit Host::GetInstance() -> PPCBreakpointsChanged();
}

void CodeViewWidget::AddBreakpoint()
{
  m_system.GetPowerPC().GetBreakPoints().Add(GetContextAddress());
  emit Host::GetInstance() -> PPCBreakpointsChanged();
}
