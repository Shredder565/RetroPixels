// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/ThreadWidget.h"

#include <array>
#include <bit>
#include <optional>
#include <utility>

#include <QCursor>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Core/Core.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/FromStdString.h"
#include "DolphinQt/Settings.h"

namespace
{
// Dolphin OS globals in low memory (see YAGCD, "Dolphin OS Globals").
constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_DEFAULT_THREAD = 0x800000D8;
constexpr u32 OS_ACTIVE_QUEUE_HEAD = 0x800000DC;
constexpr u32 OS_ACTIVE_QUEUE_TAIL = 0x800000E0;
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;

// How many stack frames the stack walk follows at most.
constexpr int MAX_FRAMES = 16;

QString Hex8(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

QString HexOptional(const std::optional<u32>& value)
{
  return value ? Hex8(*value) : QString{};
}

// The context view shows every scalar register as one 32-bit word (FPSCR keeps its low half).
template <typename T>
std::optional<u32> Narrow(const std::optional<T>& value)
{
  if (!value)
    return std::nullopt;
  return static_cast<u32>(*value);
}

template <typename Table>
QString HexAt(const Table& table, size_t index)
{
  if (!table || index >= table->size())
    return QString{};
  return Hex8(table->at(index));
}

template <typename Table>
QString DoubleBitsAt(const Table& table, size_t index)
{
  if (!table || index >= table->size())
    return QString{};
  return QStringLiteral("%1").arg(std::bit_cast<u64>(table->at(index)), 16, 16, QLatin1Char('0'));
}

QString ReadWordAsHex(const Core::CPUThreadGuard& guard, u32 address)
{
  const u32 word =
      PowerPC::MMU::HostIsRAMAddress(guard, address) ? PowerPC::MMU::HostRead<u32>(guard, address) : 0;
  return Hex8(word);
}

QLineEdit* ReadOnlyHexField()
{
  auto* field = new QLineEdit(QStringLiteral("00000000"));
  field->setReadOnly(true);
  field->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
  return field;
}

void SetCell(QTableWidget* table, int row, int col, const QString& text)
{
  table->setItem(row, col, new QTableWidgetItem(text));
}
}  // namespace

ThreadWidget::ThreadWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("Threads"));
  setObjectName(QStringLiteral("threads"));

  auto& settings = Settings::Instance();
  setHidden(!settings.IsThreadsVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  BuildLayout();

  QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("threadwidget/geometry")).toByteArray());
  setFloating(stored.value(QStringLiteral("threadwidget/floating")).toBool());

  connect(m_thread_list->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          [this](const QItemSelection& selected, const QItemSelection&) {
            const QModelIndexList picked = selected.indexes();
            ThreadPicked(picked.empty() ? -1 : picked.first().row());
          });
  connect(m_context_grid, &QTableWidget::customContextMenuRequested, this,
          [this] { ShowAddressMenu(m_context_grid); });
  connect(m_stack_list, &QTableWidget::customContextMenuRequested, this,
          [this] { ShowAddressMenu(m_stack_list); });

  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &ThreadWidget::Refresh);
  connect(&settings, &Settings::ThreadsVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this, [this](bool enabled) {
    setHidden(!enabled || !Settings::Instance().IsThreadsVisible());
  });
}

ThreadWidget::~ThreadWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("threadwidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("threadwidget/floating"), isFloating());
}

void ThreadWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetThreadsVisible(false);
}

void ThreadWidget::showEvent(QShowEvent*)
{
  Refresh();
}

void ThreadWidget::BuildLayout()
{
  m_globals_box = new QGroupBox(tr("State"));
  auto* globals_row = new QHBoxLayout;
  m_globals_box->setLayout(globals_row);
  globals_row->addWidget(BuildGlobalsBox());
  globals_row->addWidget(BuildQueueBox());
  globals_row->setContentsMargins(2, 2, 2, 2);
  globals_row->setSpacing(1);

  auto* body = new QWidget;
  auto* column = new QVBoxLayout;
  body->setLayout(column);
  column->addWidget(m_globals_box);
  column->addWidget(BuildThreadListBox());
  column->addWidget(BuildContextBox());
  column->addWidget(BuildStackBox());
  column->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding));
  setWidget(body);

  Refresh();
  ShowContext({});
}

QGroupBox* ThreadWidget::BuildGlobalsBox()
{
  auto* box = new QGroupBox(tr("Thread context"));
  auto* grid = new QGridLayout;
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);
  box->setLayout(grid);

  const std::array<std::pair<QString, QLineEdit**>, 3> rows{{
      {tr("Current context"), &m_context_field},
      {tr("Current thread"), &m_thread_field},
      {tr("Default thread"), &m_default_field},
  }};
  for (int r = 0; r < static_cast<int>(rows.size()); ++r)
  {
    const auto& [caption, field] = rows[static_cast<size_t>(r)];
    grid->addWidget(new QLabel(caption), r, 0);
    *field = ReadOnlyHexField();
    grid->addWidget(*field, r, 1);
  }
  grid->setSpacing(1);
  return box;
}

QGroupBox* ThreadWidget::BuildQueueBox()
{
  auto* box = new QGroupBox(tr("Active thread queue"));
  auto* grid = new QGridLayout;
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);
  box->setLayout(grid);

  grid->addWidget(new QLabel(tr("Head")), 0, 0);
  m_head_field = ReadOnlyHexField();
  grid->addWidget(m_head_field, 0, 1);
  grid->addWidget(new QLabel(tr("Tail")), 1, 0);
  m_tail_field = ReadOnlyHexField();
  grid->addWidget(m_tail_field, 1, 1);
  grid->setSpacing(1);
  return box;
}

QGroupBox* ThreadWidget::BuildThreadListBox()
{
  auto* box = new QGroupBox(tr("Active threads"));
  auto* grid = new QGridLayout;
  box->setLayout(grid);

  m_thread_list = new QTableWidget();
  const QStringList headings{
      tr("Address"),
      tr("State"),
      tr("Detached"),
      tr("Suspended"),
      QStringLiteral("%1\n(%2)").arg(tr("Base priority"), tr("Effective priority")),
      QStringLiteral("%1\n%2").arg(tr("Stack end"), tr("Stack start")),
      tr("errno"),
      tr("Specific")};
  m_thread_list->setColumnCount(static_cast<int>(headings.size()));
  m_thread_list->setHorizontalHeaderLabels(headings);
  m_thread_list->setTabKeyNavigation(false);
  m_thread_list->verticalHeader()->setVisible(false);
  m_thread_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_thread_list->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_thread_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_thread_list->setWordWrap(false);

  grid->addWidget(m_thread_list, 0, 0);
  grid->setSpacing(1);
  return box;
}

QGroupBox* ThreadWidget::BuildContextBox()
{
  auto* box = new QGroupBox(tr("Selected thread context"));
  auto* grid = new QGridLayout;
  box->setLayout(grid);

  // Name/value pairs: GPRs, FPRs, paired-single slots, then GQRs and the other registers.
  m_context_grid = new QTableWidget();
  m_context_grid->setColumnCount(8);
  m_context_grid->setRowCount(32);
  m_context_grid->setTabKeyNavigation(false);
  m_context_grid->horizontalHeader()->setVisible(false);
  m_context_grid->verticalHeader()->setVisible(false);
  m_context_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_context_grid->setSelectionBehavior(QAbstractItemView::SelectItems);
  m_context_grid->setSelectionMode(QAbstractItemView::SingleSelection);
  m_context_grid->setContextMenuPolicy(Qt::CustomContextMenu);

  grid->addWidget(m_context_grid, 0, 0);
  grid->setSpacing(1);
  return box;
}

QGroupBox* ThreadWidget::BuildStackBox()
{
  auto* box = new QGroupBox(tr("Selected thread callstack"));
  auto* grid = new QGridLayout;
  box->setLayout(grid);

  m_stack_list = new QTableWidget();
  const QStringList headings{tr("Address"), tr("Back Chain"), tr("LR Save"), tr("Description")};
  m_stack_list->setColumnCount(static_cast<int>(headings.size()));
  m_stack_list->setHorizontalHeaderLabels(headings);
  m_stack_list->setRowCount(0);
  m_stack_list->setTabKeyNavigation(false);
  m_stack_list->verticalHeader()->setVisible(false);
  m_stack_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_stack_list->setSelectionBehavior(QAbstractItemView::SelectItems);
  m_stack_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_stack_list->setContextMenuPolicy(Qt::CustomContextMenu);

  grid->addWidget(m_stack_list, 0, 0);
  grid->setSpacing(1);
  return box;
}

void ThreadWidget::ShowAddressMenu(QTableWidget* table)
{
  const QTableWidgetItem* cell = table->currentItem();
  if (!cell)
    return;

  bool ok = false;
  const u32 address = cell->text().toUInt(&ok, 16);
  if (!ok)
    return;

  auto* menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  const QString watch_name = QStringLiteral("thread_context_%1").arg(Hex8(address));
  menu->addAction(tr("Add &breakpoint"), this, [this, address] { emit RequestBreakpoint(address); });
  menu->addAction(tr("Add memory breakpoint"), this,
                  [this, address] { emit RequestMemoryBreakpoint(address); });
  menu->addAction(tr("Add to &watch"), this,
                  [this, address, watch_name] { emit RequestWatch(watch_name, address); });
  menu->addAction(tr("View &memory"), this, [this, address] { emit RequestViewInMemory(address); });
  menu->addAction(tr("View &code"), this, [this, address] { emit RequestViewInCode(address); });
  menu->exec(QCursor::pos());
}

void ThreadWidget::Refresh()
{
  if (!isVisible())
    return;

  auto& system = Core::System::GetInstance();
  const Core::State state = Core::GetState(system);
  if (state == Core::State::Stopping || state == Core::State::Uninitialized)
  {
    // Nothing to show once the game is gone.
    m_thread_list->setRowCount(0);
    ShowContext({});
    const Core::CPUThreadGuard guard(system);
    ShowStack(guard, {});
  }
  if (state != Core::State::Paused)
    return;

  const auto state_text = [](u16 code) {
    QString name;
    switch (code)
    {
    case 1:
      name = tr("READY");
      break;
    case 2:
      name = tr("RUNNING");
      break;
    case 4:
      name = tr("WAITING");
      break;
    case 8:
      name = tr("MORIBUND");
      break;
    default:
      name = tr("UNKNOWN");
      break;
    }
    return QStringLiteral("%1 (%2)").arg(QString::number(code), name);
  };

  {
    const Core::CPUThreadGuard guard(system);

    m_context_field->setText(ReadWordAsHex(guard, OS_CURRENT_CONTEXT));
    m_thread_field->setText(ReadWordAsHex(guard, OS_CURRENT_THREAD));
    m_default_field->setText(ReadWordAsHex(guard, OS_DEFAULT_THREAD));
    m_head_field->setText(ReadWordAsHex(guard, OS_ACTIVE_QUEUE_HEAD));
    m_tail_field->setText(ReadWordAsHex(guard, OS_ACTIVE_QUEUE_TAIL));

    m_snapshot = guard.GetSystem().GetPowerPC().GetDebugInterface().GetThreads(guard);

    m_thread_list->setRowCount(0);
    int row = 0;
    for (const auto& thread : m_snapshot)
    {
      m_thread_list->insertRow(row);
      SetCell(m_thread_list, row, 0, Hex8(thread->GetAddress()));
      SetCell(m_thread_list, row, 1, state_text(thread->GetState()));
      SetCell(m_thread_list, row, 2, QString::number(thread->IsDetached()));
      SetCell(m_thread_list, row, 3, QString::number(thread->IsSuspended()));
      SetCell(m_thread_list, row, 4,
              QStringLiteral("%1 (%2)").arg(QString::number(thread->GetBasePriority()),
                                            QString::number(thread->GetEffectivePriority())));
      SetCell(m_thread_list, row, 5,
              QStringLiteral("%1\n%2").arg(Hex8(thread->GetStackEnd()),
                                           Hex8(thread->GetStackStart())));
      SetCell(m_thread_list, row, 6, QString::number(thread->GetErrno()));
      SetCell(m_thread_list, row, 7, QString::fromStdString(thread->GetSpecific(guard)));
      ++row;
    }
  }

  m_thread_list->resizeColumnsToContents();
  m_thread_list->resizeRowsToContents();
  ShowContext({});
}

void ThreadWidget::ShowContext(const Common::Debug::PartialContext& context)
{
  // Registers in the right-hand pair of columns, by row; rows past these are greyed out.
  const std::array<std::pair<const char*, std::optional<u32>>, 9> others{{
      {"CR", context.cr},
      {"LR", context.lr},
      {"CTR", context.ctr},
      {"XER", context.xer},
      {"FPSCR", Narrow(context.fpscr)},
      {"SRR0", context.srr0},
      {"SRR1", context.srr1},
      {"DUMMY", Narrow(context.dummy)},
      {"STATE", Narrow(context.state)},
  }};

  m_context_grid->setRowCount(0);
  for (int row = 0; row < 32; ++row)
  {
    const size_t n = static_cast<size_t>(row);
    m_context_grid->insertRow(row);
    SetCell(m_context_grid, row, 0, QStringLiteral("GPR%1").arg(row));
    SetCell(m_context_grid, row, 1, HexAt(context.gpr, n));
    SetCell(m_context_grid, row, 2, QStringLiteral("FPR%1").arg(row));
    SetCell(m_context_grid, row, 3, DoubleBitsAt(context.fpr, n));
    SetCell(m_context_grid, row, 4, QStringLiteral("PSF%1").arg(row));
    SetCell(m_context_grid, row, 5, DoubleBitsAt(context.psf, n));

    if (row < 8)
    {
      SetCell(m_context_grid, row, 6, QStringLiteral("GQR%1").arg(row));
      SetCell(m_context_grid, row, 7, HexAt(context.gqr, n));
    }
    else if (n - 8 < others.size())
    {
      const auto& [name, value] = others[n - 8];
      SetCell(m_context_grid, row, 6, QString::fromLatin1(name));
      SetCell(m_context_grid, row, 7, HexOptional(value));
    }
    else
    {
      for (const int col : {6, 7})
      {
        auto* filler = new QTableWidgetItem();
        filler->setFlags(filler->flags() &
                         ~(Qt::ItemIsSelectable | Qt::ItemIsEditable | Qt::ItemIsEnabled));
        filler->setBackground(Qt::gray);
        m_context_grid->setItem(row, col, filler);
      }
    }
  }
  m_context_grid->resizeColumnsToContents();
}

void ThreadWidget::ShowStack(const Core::CPUThreadGuard& guard,
                             const Common::Debug::PartialContext& context)
{
  m_stack_list->setRowCount(0);
  if (!context.gpr)
    return;

  // Follow the back chain from r1: each frame holds the caller's stack pointer at +0 and the
  // saved LR at +4.
  u32 frame = context.gpr->at(1);
  for (int row = 0; row < MAX_FRAMES; ++row)
  {
    if (frame == 0 || frame == 0xffffffff || !PowerPC::MMU::HostIsRAMAddress(guard, frame))
      break;

    m_stack_list->insertRow(row);
    SetCell(m_stack_list, row, 0, Hex8(frame));

    if (PowerPC::MMU::HostIsRAMAddress(guard, frame + 4))
    {
      const u32 saved_lr = PowerPC::MMU::HostRead<u32>(guard, frame + 4);
      SetCell(m_stack_list, row, 2, Hex8(saved_lr));
      SetCell(m_stack_list, row, 3,
              QtUtils::FromStdString(
                  guard.GetSystem().GetPowerPC().GetDebugInterface().GetDescription(saved_lr)));
    }
    else
    {
      SetCell(m_stack_list, row, 2, QStringLiteral("--------"));
    }

    frame = PowerPC::MMU::HostRead<u32>(guard, frame);
    SetCell(m_stack_list, row, 1, Hex8(frame));
  }
}

void ThreadWidget::ThreadPicked(int row)
{
  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  Common::Debug::PartialContext context;
  if (row >= 0 && static_cast<size_t>(row) < m_snapshot.size())
    context = m_snapshot[static_cast<size_t>(row)]->GetContext(guard);

  ShowContext(context);
  ShowStack(guard, context);
}
