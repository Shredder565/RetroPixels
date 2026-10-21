// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/CodeWidget.h"

#include <chrono>
#include <initializer_list>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "Common/Event.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/HW/CPU.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/BranchWatchDialog.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
// A dashed rule between the stacked side panes.
QString SidePaneSplitterStyle()
{
  return QStringLiteral("QSplitter::handle { border-top: 1px dashed black; width: 1px; "
                        "margin-left: 10px; margin-right: 10px; }");
}

constexpr int MESSAGE_MS = 2000;
constexpr u32 RFI_INSTRUCTION = 0x4C000064u;

// Whether executing inst returns from the current function: an rfi, or a non-linking bclr
// whose CTR and CR conditions are satisfied right now.
bool ReturnsNow(Core::System& system, UGeckoInstruction inst)
{
  if (inst.hex == RFI_INSTRUCTION)
    return true;
  if (inst.OPCD_7 != 0b010011 || inst.XO != 16 || inst.LK_3)
    return false;

  const PowerPC::PowerPCState& state = system.GetPPCState();
  const u32 bo = inst.BO_2;
  const bool ignore_ctr = ((bo >> 2) & 1) != 0;
  const bool ctr_ok = ignore_ctr || ((CTR(state) != 0) != (((bo >> 1) & 1) != 0));
  const bool ignore_cr = (bo >> 4) != 0;
  const bool cr_ok = ignore_cr || state.cr.GetBit(inst.BI_2) == ((bo >> 3) & 1);
  return ctr_ok && cr_ok;
}
}  // namespace

CodeWidget::CodeWidget(QWidget* parent)
    : QDockWidget(parent), m_system(Core::System::GetInstance()),
      m_symbol_db(m_system.GetPPCSymbolDB()),
      m_demangle(Settings::Instance().IsShowDemangledNames())
{
  setWindowTitle(tr("Code"));
  setObjectName(QStringLiteral("code"));
  Settings& settings = Settings::Instance();
  setHidden(!settings.IsCodeVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  QWidget* const code_pane = BuildCodePane();
  BuildSidePanes();

  m_main_split = new QSplitter(Qt::Horizontal);
  m_main_split->addWidget(m_side_split);
  m_main_split->addWidget(code_pane);

  auto* const outer = new QHBoxLayout;
  outer->setContentsMargins(2, 2, 2, 2);
  outer->setSpacing(0);
  outer->addWidget(m_main_split);
  auto* const body = new QWidget(this);
  body->setLayout(outer);
  setWidget(body);

  const QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("codewidget/geometry")).toByteArray());
  // Floating is applied only after the hidden state above, which macOS needs to place the window
  // correctly.
  setFloating(stored.value(QStringLiteral("codewidget/floating")).toBool());

  connect(&settings, &Settings::CodeVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this, &CodeWidget::FollowPCUnlessLocked);
  connect(&settings, &Settings::DebugModeToggled, this,
          [this](bool enabled) { setHidden(!enabled || !Settings::Instance().IsCodeVisible()); });
  connect(&settings, &Settings::EmulationStateChanged, this, &CodeWidget::Update);
  connect(&settings, &Settings::ShowDemangledNamesChanged, this,
          &CodeWidget::DemangleSettingChanged);

  Wire();

  m_main_split->restoreState(stored.value(QStringLiteral("codewidget/codesplitter")).toByteArray());
  m_side_split->restoreState(stored.value(QStringLiteral("codewidget/boxsplitter")).toByteArray());
}

CodeWidget::~CodeWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("codewidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("codewidget/floating"), isFloating());
  stored.setValue(QStringLiteral("codewidget/codesplitter"), m_main_split->saveState());
  stored.setValue(QStringLiteral("codewidget/boxsplitter"), m_side_split->saveState());
}

// The address box, lock and Branch Watch buttons above the disassembly.
QWidget* CodeWidget::BuildCodePane()
{
  m_address_box = new QLineEdit;
  m_address_box->setPlaceholderText(tr("Search Address"));

  m_lock_button = new QToolButton();
  m_lock_button->setIcon(Resources::GetThemeIcon("pause"));
  m_lock_button->setCheckable(true);
  m_lock_button->setMinimumSize(24, 24);
  m_lock_button->setToolTip(tr("When enabled, prevents automatic updates to the code view."));

  m_branch_watch_button = new QPushButton(tr("Branch Watch"));

  auto* const toolbar_row = new QHBoxLayout;
  for (QWidget* const part :
       std::initializer_list<QWidget*>{m_address_box, m_lock_button, m_branch_watch_button})
  {
    toolbar_row->addWidget(part);
  }

  m_code_view = new CodeViewWidget;
  auto* const column = new QVBoxLayout;
  column->addLayout(toolbar_row);
  column->addWidget(m_code_view);

  // The push button is taller than the line edit; pull the column up and together by half the
  // difference so the row lines up with the side panes.
  const int excess =
      m_branch_watch_button->sizeHint().height() - m_address_box->sizeHint().height();
  QMargins margins = column->contentsMargins();
  margins.setTop(margins.top() - excess / 2);
  column->setContentsMargins(margins);
  column->setSpacing(column->spacing() - excess / 2);

  auto* const pane = new QWidget;
  pane->setLayout(column);
  return pane;
}

// Callstack, symbols/notes, calls and callers, stacked in a vertical splitter, each with a
// filter box.
void CodeWidget::BuildSidePanes()
{
  m_side_split = new QSplitter(Qt::Vertical);
  m_side_split->setStyleSheet(SidePaneSplitterStyle());

  m_callstack = new QListWidget;
  m_callstack_filter = AddSidePane(tr("Callstack"), m_callstack);

  auto* const tabs = new QTabWidget;
  m_symbols = new QListWidget;
  m_notes = new QListWidget;
  tabs->addTab(m_symbols, tr("Symbols"));
  tabs->addTab(m_notes, tr("Notes"));
  m_symbols_filter = AddSidePane(tr("Symbols"), tabs);

  m_calls = new QListWidget;
  m_calls_filter = AddSidePane(tr("Calls"), m_calls);

  m_callers = new QListWidget;
  m_callers_filter = AddSidePane(tr("Callers"), m_callers);
}

QLineEdit* CodeWidget::AddSidePane(const QString& title, QWidget* content)
{
  auto* const pane = new QWidget;
  auto* const grid = new QGridLayout;
  auto* const caption = new QLabel(title);
  auto* const filter = new QLineEdit;

  pane->setLayout(grid);
  grid->addWidget(caption, 0, 0);
  grid->addWidget(filter, 0, 1);
  grid->addWidget(content, 1, 0, -1, -1);
  m_side_split->addWidget(pane);
  return filter;
}

void CodeWidget::Wire()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme) { m_side_split->setStyleSheet(SidePaneSplitterStyle()); });
#endif

  connect(m_address_box, &QLineEdit::textChanged, this, &CodeWidget::JumpToTypedAddress);
  connect(m_address_box, &QLineEdit::returnPressed, this, &CodeWidget::JumpToTypedAddress);
  connect(m_lock_button, &QToolButton::toggled, m_code_view, &CodeViewWidget::OnLockAddress);
  connect(m_symbols_filter, &QLineEdit::textChanged, this, &CodeWidget::SymbolFilterChanged);
  connect(m_calls_filter, &QLineEdit::textChanged, this, [this] {
    if (const Common::Symbol* const symbol = SymbolAtView())
      RefreshCalls(symbol);
  });
  connect(m_callers_filter, &QLineEdit::textChanged, this, [this] {
    if (const Common::Symbol* const symbol = SymbolAtView())
      RefreshCallers(symbol);
  });
  connect(m_callstack_filter, &QLineEdit::textChanged, this, &CodeWidget::RefreshCallstack);

  connect(m_branch_watch_button, &QPushButton::clicked, this, &CodeWidget::OnBranchWatchDialog);
  connect(m_notes, &QListWidget::itemPressed, this, &CodeWidget::JumpToNote);
  connect(m_symbols, &QListWidget::itemPressed, this, &CodeWidget::JumpToSymbol);
  for (const QListWidget* const list : {m_callstack, m_calls, m_callers})
  {
    connect(list, &QListWidget::itemPressed, this, [this, list] { JumpToListedAddress(list); });
  }

  connect(Host::GetInstance(), &Host::PPCSymbolsChanged, this, &CodeWidget::SymbolsChanged);
  connect(m_code_view, &CodeViewWidget::UpdateCodeWidget, this, &CodeWidget::Update);
  connect(m_code_view, &CodeViewWidget::RequestPPCComparison, this,
          &CodeWidget::RequestPPCComparison);
  connect(m_code_view, &CodeViewWidget::ShowMemory, this, &CodeWidget::ShowMemory);
  connect(m_code_view, &CodeViewWidget::ActivateSearch, this, &CodeWidget::ActivateSearchAddress);
}

void CodeWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetCodeVisible(false);
}

void CodeWidget::showEvent(QShowEvent*)
{
  Update();
}

void CodeWidget::FollowPCUnlessLocked()
{
  if (!m_lock_button->isChecked() && Core::GetState(m_system) == Core::State::Paused)
    SetAddress(m_system.GetPPCState().pc, CodeViewWidget::SetAddressUpdate::WithoutUpdate);
  Update();
}

void CodeWidget::OnBranchWatchDialog()
{
  // Created on first use and kept around (hidden) afterwards.
  if (m_branch_watch_dialog == nullptr)
  {
    m_branch_watch_dialog = new BranchWatchDialog(m_system, m_system.GetPowerPC().GetBranchWatch(),
                                                  m_symbol_db, this, this);
  }
  m_branch_watch_dialog->show();
  m_branch_watch_dialog->raise();
  m_branch_watch_dialog->activateWindow();
}

void CodeWidget::OnSetCodeAddress(u32 address)
{
  SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
}

void CodeWidget::DemangleSettingChanged()
{
  m_demangle = Settings::Instance().IsShowDemangledNames();
  UpdateSymbols();
  if (const Common::Symbol* const symbol = SymbolAtView())
  {
    RefreshCalls(symbol);
    RefreshCallers(symbol);
  }
}

void CodeWidget::SymbolsChanged()
{
  UpdateSymbols();
  RefreshNotes();
  RefreshCallstack();
  const Common::Symbol* const symbol = SymbolAtView();
  RefreshCalls(symbol);
  RefreshCallers(symbol);
}

void CodeWidget::ActivateSearchAddress()
{
  m_address_box->setFocus();
  m_address_box->selectAll();
}

void CodeWidget::JumpToTypedAddress()
{
  const QString text = m_address_box->text();
  bool parsed = true;
  const u32 address = text.toUInt(&parsed, 16);

  // Unparsable (non-empty) input is shown in bold red; otherwise the defaults are restored.
  QPalette look;
  QFont face;
  if (!parsed && !text.isEmpty())
  {
    face.setBold(true);
    look.setColor(QPalette::Text, Qt::red);
  }
  m_address_box->setPalette(look);
  m_address_box->setFont(face);

  if (parsed)
    m_code_view->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
  m_address_box->setFocus();
}

void CodeWidget::SymbolFilterChanged()
{
  m_symbol_filter = m_symbols_filter->text();
  UpdateSymbols();
  RefreshNotes();
}

void CodeWidget::JumpToSymbol()
{
  const QList<QListWidgetItem*> picked = m_symbols->selectedItems();
  if (picked.isEmpty())
    return;

  const u32 address = picked.front()->data(Qt::UserRole).toUInt();
  const Common::Symbol* const symbol = m_symbol_db.GetSymbolFromAddr(address);
  m_code_view->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithUpdate);
  RefreshCallstack();
  RefreshCalls(symbol);
  RefreshCallers(symbol);
  m_code_view->setFocus();
}

void CodeWidget::JumpToNote()
{
  const QList<QListWidgetItem*> picked = m_notes->selectedItems();
  if (!picked.isEmpty())
  {
    m_code_view->SetAddress(picked.front()->data(Qt::UserRole).toUInt(),
                            CodeViewWidget::SetAddressUpdate::WithUpdate);
  }
}

void CodeWidget::JumpToListedAddress(const QListWidget* list)
{
  const QList<QListWidgetItem*> picked = list->selectedItems();
  if (picked.isEmpty())
    return;
  m_code_view->SetAddress(picked.front()->data(Qt::UserRole).toUInt(),
                          CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::SetAddress(u32 address, CodeViewWidget::SetAddressUpdate update)
{
  m_code_view->SetAddress(address, update);
  if (update == CodeViewWidget::SetAddressUpdate::WithoutUpdate)
    return;

  // A visible jump also brings the dock forward.
  Settings::Instance().SetCodeVisible(true);
  raise();
  m_code_view->setFocus();
}

const Common::Symbol* CodeWidget::SymbolAtView() const
{
  return m_symbol_db.GetSymbolFromAddr(m_code_view->GetAddress());
}

void CodeWidget::Update()
{
  if (!isVisible())
    return;

  const Common::Symbol* const symbol = SymbolAtView();
  RefreshCallstack();
  m_code_view->Update();
  m_code_view->setFocus();
  RefreshCalls(symbol);
  RefreshCallers(symbol);
}

void CodeWidget::RefreshCallstack()
{
  m_callstack->clear();
  if (Core::GetState(m_system) != Core::State::Paused)
    return;

  std::vector<Dolphin_Debugger::CallstackEntry> frames;
  if (!Dolphin_Debugger::GetCallstack(Core::CPUThreadGuard{m_system}, frames))
  {
    m_callstack->addItem(tr("Invalid callstack"));
    return;
  }

  const QString filter = m_callstack_filter->text();
  for (const Dolphin_Debugger::CallstackEntry& frame : frames)
  {
    // Entry names come with a trailing newline.
    const std::string_view trimmed = std::string_view(frame.Name).substr(0, frame.Name.size() - 1);
    const QString name = QString::fromUtf8(trimmed.data(), static_cast<qsizetype>(trimmed.size()));
    if (!name.contains(filter, Qt::CaseInsensitive))
      continue;

    auto* const entry = new QListWidgetItem(name);
    entry->setData(Qt::UserRole, frame.vAddress);
    m_callstack->addItem(entry);
  }
}

void CodeWidget::UpdateSymbols()
{
  const QList<QListWidgetItem*> picked = m_symbols->selectedItems();
  const QString previous = picked.isEmpty() ? QString{} : picked.front()->text();
  m_symbols->clear();

  m_symbol_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    QString name = QString::fromStdString(DisplayName(symbol));
    if (!symbol.object_name.empty())
      name += QString::fromStdString(fmt::format(" ({})", symbol.object_name));
    if (!name.contains(m_symbol_filter, Qt::CaseInsensitive))
      return;

    auto* const entry = new QListWidgetItem(name);
    if (name == previous)
      entry->setSelected(true);
    // Only functions can be jumped to; other symbols are listed but inert.
    if (symbol.type != Common::Symbol::Type::Function)
      entry->setFlags(Qt::NoItemFlags);
    entry->setData(Qt::UserRole, symbol.address);
    m_symbols->addItem(entry);
  });

  m_symbols->sortItems();
}

void CodeWidget::RefreshNotes()
{
  const QList<QListWidgetItem*> picked = m_notes->selectedItems();
  const QString previous = picked.isEmpty() ? QStringLiteral("") : picked.front()->text();
  m_notes->clear();

  const QString wanted = m_symbol_filter.toUpper();
  m_symbol_db.ForEachNote([&](const Common::Note& note) {
    const QString name = QString::fromStdString(note.name);
    if (name.toUpper().indexOf(wanted) == -1)
      return;

    auto* const entry = new QListWidgetItem(name);
    if (name == previous)
      entry->setSelected(true);
    entry->setData(Qt::UserRole, note.address);
    m_notes->addItem(entry);
  });

  m_notes->sortItems();
}

// "< name (address)", or "< name (object, address)" when the symbol records its object file.
QString CodeWidget::CallEntry(const Common::Symbol& target, u32 address) const
{
  const std::string& name = DisplayName(target);
  if (target.object_name.empty())
    return QString::fromStdString(fmt::format("< {} ({:08x})", name, address));
  return QString::fromStdString(
      fmt::format("< {} ({}, {:08x})", name, target.object_name, address));
}

void CodeWidget::RefreshCalls(const Common::Symbol* symbol)
{
  m_calls->clear();
  if (symbol == nullptr)
    return;

  const QString filter = m_calls_filter->text();
  for (const Common::SCall& call : symbol->calls)
  {
    const u32 address = call.function;
    const Common::Symbol* const callee = m_symbol_db.GetSymbolFromAddr(address);
    if (callee == nullptr)
      continue;
    const QString text = CallEntry(*callee, address);
    if (!text.contains(filter, Qt::CaseInsensitive))
      continue;

    auto* const entry = new QListWidgetItem(text);
    entry->setData(Qt::UserRole, address);
    m_calls->addItem(entry);
  }
}

void CodeWidget::RefreshCallers(const Common::Symbol* symbol)
{
  m_callers->clear();
  if (symbol == nullptr)
    return;

  const QString filter = m_callers_filter->text();
  for (const Common::SCall& caller : symbol->callers)
  {
    // Listed by the function that contains the call site.
    const u32 address = caller.call_address;
    const Common::Symbol* const container = m_symbol_db.GetSymbolFromAddr(address);
    if (container == nullptr)
      continue;
    const QString text = CallEntry(*container, address);
    if (!text.contains(filter, Qt::CaseInsensitive))
      continue;

    auto* const entry = new QListWidgetItem(text);
    entry->setData(Qt::UserRole, address);
    m_callers->addItem(entry);
  }
}

const std::string& CodeWidget::DisplayName(const Common::Symbol& symbol) const
{
  return symbol.GetDisplayName(m_demangle);
}

void CodeWidget::Step()
{
  CPU::CPUManager& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;

  // Single-step in the interpreter, whatever core is configured, and wait briefly for it.
  PowerPC::PowerPCManager& power_pc = m_system.GetPowerPC();
  const PowerPC::CoreMode configured = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  Common::Event done;
  cpu.StepOpcode(&done);
  done.WaitFor(std::chrono::milliseconds(20));
  power_pc.SetMode(configured);
  Core::DisplayMessage(tr("Step successful!").toStdString(), MESSAGE_MS);
  // The view itself refreshes on the UpdateDisasmDialog that follows.

  // Branch Watch is not told about steps through any signal, so poke it directly.
  if (m_branch_watch_dialog != nullptr)
    m_branch_watch_dialog->Update();
}

void CodeWidget::StepOver()
{
  CPU::CPUManager& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;

  UGeckoInstruction inst;
  {
    const Core::CPUThreadGuard guard(m_system);
    inst = PowerPC::MMU::HostRead_Instruction(guard, m_system.GetPPCState().pc);
  }

  if (!inst.LK)
  {
    Step();
    return;
  }

  // A call: run until it comes back to the following instruction.
  m_system.GetPowerPC().GetBreakPoints().SetTemporary(m_system.GetPPCState().pc + 4);
  cpu.SetStepping(false);
  Core::DisplayMessage(tr("Step over in progress...").toStdString(), MESSAGE_MS);
}

void CodeWidget::StepOut()
{
  CPU::CPUManager& cpu = m_system.GetCPU();
  if (!cpu.IsStepping())
    return;

  // Steps (over calls) until a returning instruction has executed, a breakpoint is reached, or
  // five seconds pass.
  using SteadyClock = std::chrono::steady_clock;
  const SteadyClock::time_point deadline = SteadyClock::now() + std::chrono::seconds(5);
  const auto out_of_time = [deadline] { return SteadyClock::now() >= deadline; };

  PowerPC::PowerPCManager& power_pc = m_system.GetPowerPC();
  {
    PowerPC::PowerPCState& state = power_pc.GetPPCState();
    const Core::CPUThreadGuard guard(m_system);
    const PowerPC::CoreMode configured = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);

    const auto current = [&] { return PowerPC::MMU::HostRead_Instruction(guard, state.pc); };
    UGeckoInstruction inst = current();
    while (true)
    {
      if (ReturnsNow(m_system, inst))
      {
        power_pc.SingleStep();
        break;
      }

      if (inst.LK)
      {
        const u32 resume_at = state.pc + 4;
        do
        {
          power_pc.SingleStep();
        } while (state.pc != resume_at && !out_of_time() && !power_pc.CheckBreakPoints());
      }
      else
      {
        power_pc.SingleStep();
      }

      inst = current();
      if (out_of_time() || power_pc.CheckBreakPoints())
        break;
    }

    power_pc.SetMode(configured);
  }

  emit Host::GetInstance() -> UpdateDisasmDialog();

  QString outcome;
  if (power_pc.CheckBreakPoints())
    outcome = tr("Breakpoint encountered! Step out aborted.");
  else if (out_of_time())
    outcome = tr("Step out timed out!");
  else
    outcome = tr("Step out successful!");
  Core::DisplayMessage(outcome.toStdString(), MESSAGE_MS);
}

void CodeWidget::Skip()
{
  m_system.GetPPCState().pc += 4;
  ShowPC();
}

void CodeWidget::ShowPC()
{
  m_code_view->SetAddress(m_system.GetPPCState().pc, CodeViewWidget::SetAddressUpdate::WithUpdate);
  Update();
}

void CodeWidget::SetPC()
{
  m_system.GetPPCState().pc = m_code_view->GetAddress();
  Update();
}

void CodeWidget::ToggleBreakpoint()
{
  m_code_view->ToggleBreakpoint();
}

void CodeWidget::AddBreakpoint()
{
  m_code_view->AddBreakpoint();
}
