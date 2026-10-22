// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/MemoryWidget.h"

#include <array>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/MemoryViewWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/Settings.h"

namespace
{
using ViewType = MemoryViewWidget::Type;

struct TypeChoice
{
  const char* label;
  ViewType type;
};

// Combo contents, in list order.
constexpr std::array<TypeChoice, 10> INPUT_TYPES{{
    {QT_TRANSLATE_NOOP("MemoryWidget", "Hex Byte String"), ViewType::HexString},
    {QT_TRANSLATE_NOOP("MemoryWidget", "ASCII"), ViewType::ASCII},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Float"), ViewType::Float32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Double"), ViewType::Double},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 8"), ViewType::Unsigned8},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 16"), ViewType::Unsigned16},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 32"), ViewType::Unsigned32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 8"), ViewType::Signed8},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 16"), ViewType::Signed16},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 32"), ViewType::Signed32},
}};

constexpr std::array<TypeChoice, 12> DISPLAY_TYPES{{
    {QT_TRANSLATE_NOOP("MemoryWidget", "Hex 8"), ViewType::Hex8},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Hex 16"), ViewType::Hex16},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Hex 32"), ViewType::Hex32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 8"), ViewType::Unsigned8},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 16"), ViewType::Unsigned16},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Unsigned 32"), ViewType::Unsigned32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 8"), ViewType::Signed8},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 16"), ViewType::Signed16},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Signed 32"), ViewType::Signed32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "ASCII"), ViewType::ASCII},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Float"), ViewType::Float32},
    {QT_TRANSLATE_NOOP("MemoryWidget", "Double"), ViewType::Double},
}};

void FillTypeCombo(QComboBox* combo, const auto& choices)
{
  combo->setMaxVisibleItems(20);
  for (const TypeChoice& choice : choices)
    combo->addItem(MemoryWidget::tr(choice.label), static_cast<int>(choice.type));
}

// The integer types are the only ones where the Hex checkbox means anything.
bool IsIntegerType(ViewType type)
{
  switch (type)
  {
  case ViewType::Unsigned8:
  case ViewType::Unsigned16:
  case ViewType::Unsigned32:
  case ViewType::Signed8:
  case ViewType::Signed16:
  case ViewType::Signed32:
    return true;
  default:
    return false;
  }
}

// The look of an input box: bold red when its contents are unusable, default otherwise.
void MarkInput(QWidget* box, bool bad)
{
  QFont face;
  QPalette look;
  if (bad)
  {
    face.setBold(true);
    look.setColor(QPalette::Text, Qt::red);
  }
  box->setFont(face);
  box->setPalette(look);
}

void WriteDump(const std::string& path, const u8* begin, const u8* end)
{
  if (begin == nullptr)
    return;

  File::IOFile file(path, "wb");
  const QString shown = QString::fromStdString(path);
  if (!file)
  {
    ModalMessageBox::critical(nullptr, QObject::tr("Error"),
                              QObject::tr("Failed to dump %1: Can't open file").arg(shown));
    return;
  }
  if (!file.WriteBytes(begin, static_cast<std::size_t>(std::distance(begin, end))))
  {
    ModalMessageBox::critical(nullptr, QObject::tr("Error"),
                              QObject::tr("Failed to dump %1: Failed to write to file").arg(shown));
  }
}

void DumpSpace(AddressSpace::Type space, unsigned int path_index)
{
  AddressSpace::Accessors* const memory = AddressSpace::GetAccessors(space);
  WriteDump(File::GetUserPath(path_index), memory->begin(), memory->end());
}
}  // namespace

MemoryWidget::MemoryWidget(Core::System& system, QWidget* parent)
    : QDockWidget(parent), m_system(system), m_symbol_db(system.GetPPCSymbolDB())
{
  setWindowTitle(tr("Memory"));
  setObjectName(QStringLiteral("memory"));
  Settings& settings = Settings::Instance();
  setHidden(!settings.IsMemoryVisible() || !settings.IsDebugModeEnabled());
  setAllowedAreas(Qt::AllDockWidgetAreas);

  // The construction order below also fixes the order widgets join the focus chain.
  QWidget* const address_row = BuildAddressRow();
  QLayout* const value_row = BuildValueRow();
  BuildValueTypes();
  QWidget* const search_box = BuildSearchBox();
  QWidget* const space_box = BuildAddressSpaceBox();
  QWidget* const display_box = BuildDisplayBox();
  QWidget* const breakpoint_box = BuildBreakpointBox();
  BuildLabelsBox();

  auto* const sidebar = new QWidget;
  BuildMenus(sidebar);

  // The first gap leaves room for the menu bar, which sits on top of the side bar.
  auto* const column = new QVBoxLayout;
  column->setSpacing(1);
  sidebar->setLayout(column);
  const auto gap = [column](int height) { column->addItem(new QSpacerItem(1, height)); };
  gap(20);
  column->addWidget(address_row);
  column->addLayout(value_row);
  column->addWidget(m_value_type);
  gap(10);
  column->addWidget(m_value_preview);
  column->addWidget(m_write_button);
  gap(10);
  column->addWidget(search_box);
  gap(10);
  column->addWidget(display_box);
  gap(10);
  column->addWidget(space_box);
  gap(10);
  column->addWidget(breakpoint_box);
  column->addWidget(m_labels_box);
  column->addItem(new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Expanding));

  m_splitter = new QSplitter(Qt::Horizontal);
  auto* const scroller = new QScrollArea;
  scroller->setWidget(sidebar);
  scroller->setWidgetResizable(true);
  m_memory_view = new MemoryViewWidget(m_system, this);
  m_splitter->addWidget(m_memory_view);
  m_splitter->addWidget(scroller);
  m_splitter->setStretchFactor(0, 3);
  m_splitter->setStretchFactor(1, 1);

  auto* const outer = new QHBoxLayout;
  outer->setContentsMargins(2, 2, 2, 2);
  outer->setSpacing(0);
  outer->addWidget(m_splitter);
  auto* const body = new QWidget;
  body->setLayout(outer);
  setWidget(body);
  FillNoteList();

  const QSettings& stored = Settings::GetQSettings();
  restoreGeometry(stored.value(QStringLiteral("memorywidget/geometry")).toByteArray());
  // Floating is applied after the hidden state, which macOS needs to place the window correctly.
  setFloating(stored.value(QStringLiteral("memorywidget/floating")).toBool());
  m_splitter->restoreState(stored.value(QStringLiteral("memorywidget/splitter")).toByteArray());

  connect(&settings, &Settings::MemoryVisibilityChanged, this,
          [this](bool visible) { setHidden(!visible); });
  connect(&settings, &Settings::DebugModeToggled, this,
          [this](bool enabled) { setHidden(!enabled || !Settings::Instance().IsMemoryVisible()); });
  // No per-frame refresh while the dock is tabbed away.
  connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
    if (visible && m_auto_update)
      StartFrameUpdates();
    else
      StopFrameUpdates();
  });

  RestoreChoices();
  Wire();
  AddressSpaceChosen();
  DisplayChosen();
}

MemoryWidget::~MemoryWidget()
{
  QSettings& stored = Settings::GetQSettings();
  stored.setValue(QStringLiteral("memorywidget/geometry"), saveGeometry());
  stored.setValue(QStringLiteral("memorywidget/floating"), isFloating());
  stored.setValue(QStringLiteral("memorywidget/splitter"), m_splitter->saveState());
  StoreChoices();
}

QWidget* MemoryWidget::BuildAddressRow()
{
  auto* const row = new QSplitter(Qt::Horizontal);

  m_address_box = new QComboBox;
  m_address_box->setInsertPolicy(QComboBox::InsertAtTop);
  m_address_box->setDuplicatesEnabled(false);
  m_address_box->setEditable(true);
  m_address_box->setMaxVisibleItems(8);
  m_offset_box = new QLineEdit;
  m_offset_box->setMaxLength(9);
  m_address_box->setPlaceholderText(tr("Search Address"));
  m_offset_box->setPlaceholderText(tr("Offset"));

  row->addWidget(m_address_box);
  row->addWidget(m_offset_box);
  row->setHandleWidth(1);
  row->setCollapsible(0, false);
  row->setStretchFactor(1, 2);
  return row;
}

QLayout* MemoryWidget::BuildValueRow()
{
  auto* const row = new QHBoxLayout;
  m_value_edit = new QLineEdit;
  m_hex_check = new QCheckBox(tr("Hex"));
  m_write_button = new QPushButton(tr("Set &Value"));
  m_value_preview = new QLabel;

  // Checkbox text on the left of its box.
  m_hex_check->setLayoutDirection(Qt::RightToLeft);
  m_value_edit->setPlaceholderText(tr("Value"));
  m_value_preview->setBackgroundRole(QPalette::AlternateBase);
  m_value_preview->setAutoFillBackground(true);

  row->addWidget(m_value_edit);
  row->addWidget(m_hex_check);
  return row;
}

void MemoryWidget::BuildValueTypes()
{
  m_value_type = new QComboBox;
  FillTypeCombo(m_value_type, INPUT_TYPES);
}

QWidget* MemoryWidget::BuildSearchBox()
{
  auto* const box = new QGroupBox(tr("Search"));
  auto* const stack = new QVBoxLayout;
  box->setLayout(stack);

  m_next_button = new QPushButton(tr("Find &Next"));
  m_previous_button = new QPushButton(tr("Find &Previous"));
  m_search_result = new QLabel;
  stack->addWidget(m_next_button);
  stack->addWidget(m_previous_button);
  stack->addWidget(m_search_result);
  stack->setSpacing(1);
  return box;
}

QWidget* MemoryWidget::BuildAddressSpaceBox()
{
  auto* const box = new QGroupBox(tr("Address Space"));
  auto* const stack = new QVBoxLayout;
  box->setLayout(stack);

  // i18n: an address space choice: addresses as the CPU uses them, which the MMU may translate.
  m_effective_radio = new QRadioButton(tr("Effective"));
  // i18n: an address space choice: the separate auxiliary RAM (ARAM).
  m_auxiliary_radio = new QRadioButton(tr("Auxiliary"));
  // i18n: an address space choice: addresses as the hardware (RAM etc.) is actually wired.
  m_physical_radio = new QRadioButton(tr("Physical"));

  stack->addWidget(m_effective_radio);
  stack->addWidget(m_auxiliary_radio);
  stack->addWidget(m_physical_radio);
  stack->setSpacing(1);
  return box;
}

QWidget* MemoryWidget::BuildDisplayBox()
{
  auto* const box = new QGroupBox(tr("Display Type"));
  auto* const stack = new QVBoxLayout;
  box->setLayout(stack);

  m_display_type = new QComboBox;
  FillTypeCombo(m_display_type, DISPLAY_TYPES);

  // Item data: 0 asks the view to align to the value size, 1 disables alignment. The first item
  // (alignment equal to the row length) is resolved when the display is applied.
  m_alignment = new QComboBox;
  // i18n: alignment that stays the same whatever the value type is
  m_alignment->addItem(tr("Fixed Alignment"));
  m_alignment->addItem(tr("Type-based Alignment"), 0);
  m_alignment->addItem(tr("No Alignment"), 1);

  m_row_length = new QComboBox;
  m_row_length->addItem(tr("4 Bytes"), 4);
  m_row_length->addItem(tr("8 Bytes"), 8);
  m_row_length->addItem(tr("16 Bytes"), 16);
  m_row_length->setCurrentIndex(2);

  m_dual_check = new QCheckBox(tr("Dual View"));

  stack->addWidget(m_display_type);
  stack->addWidget(m_alignment);
  stack->addWidget(m_row_length);
  stack->addWidget(m_dual_check);
  return box;
}

QWidget* MemoryWidget::BuildBreakpointBox()
{
  auto* const box = new QGroupBox(tr("Memory breakpoint options"));
  auto* const stack = new QVBoxLayout;
  box->setLayout(stack);

  // i18n: kind of memory breakpoint: it fires on reads and on writes. It does not mean that
  // something may be read and written.
  m_bp_both_radio = new QRadioButton(tr("Read and write"));
  // i18n: kind of memory breakpoint: it fires on reads only. Not "read-only" as in "cannot be
  // written".
  m_bp_read_radio = new QRadioButton(tr("Read only"));
  // i18n: kind of memory breakpoint: it fires on writes only. Not "write-only" as in "cannot be
  // read".
  m_bp_write_radio = new QRadioButton(tr("Write only"));
  m_bp_log_check = new QCheckBox(tr("Log"));

  stack->addWidget(m_bp_both_radio);
  stack->addWidget(m_bp_read_radio);
  stack->addWidget(m_bp_write_radio);
  stack->addWidget(m_bp_log_check);
  stack->setSpacing(1);
  return box;
}

void MemoryWidget::BuildLabelsBox()
{
  m_labels_box = new QGroupBox(tr("Labels"));
  auto* const tabs = new QTabWidget;
  m_note_list = new QListWidget;
  m_data_list = new QListWidget;
  m_symbol_list = new QListWidget;
  tabs->addTab(m_note_list, tr("Notes"));
  tabs->addTab(m_data_list, tr("Data"));
  tabs->addTab(m_symbol_list, tr("Symbols"));
  m_symbol_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto* const stack = new QVBoxLayout;
  m_label_filter = new QLineEdit;
  // i18n: "Filter" is a verb: typing here narrows the label lists to entries containing the text.
  m_label_filter->setPlaceholderText(tr("Filter Label List"));

  m_labels_box->setLayout(stack);
  stack->addWidget(tabs);
  stack->addWidget(m_label_filter);
}

void MemoryWidget::BuildMenus(QWidget* sidebar)
{
  auto* const bar = new QMenuBar(sidebar);
  bar->setNativeMenuBar(false);

  auto* const view_menu = new QMenu(tr("&View"), bar);
  bar->addMenu(view_menu);

  auto* const import_menu = new QMenu(tr("&Import"), bar);
  import_menu->addAction(tr("&Load file to current address"), this, &MemoryWidget::WriteFile);
  bar->addMenu(import_menu);

  // A checkable View entry, on by default; the handler receives the new state when clicked.
  const auto add_toggle = [this, view_menu](const QString& text, auto handler) {
    QAction* const toggle = view_menu->addAction(text, this, handler);
    toggle->setCheckable(true);
    toggle->setChecked(true);
  };
  add_toggle(tr("&Auto update memory values"), [this](bool on) {
    m_auto_update = on;
    if (on)
      StartFrameUpdates();
    else
      StopFrameUpdates();
  });
  // i18n: "Highlight" is a verb here; this entry is a checkbox
  add_toggle(tr("&Highlight recently changed values"),
             [this](bool on) { m_memory_view->ToggleHighlights(on); });
  // i18n: "Highlight" is a noun here; this entry opens a colour picker
  view_menu->addAction(tr("Highlight &color"), this,
                       [this] { m_memory_view->SetHighlightColor(); });
  add_toggle(tr("&Show symbols and notes"), [this](bool on) {
    m_labels_visible = on;
    m_memory_view->ShowSymbols(on);
    FillNoteList();
  });

  auto* const export_menu = new QMenu(tr("&Export"), bar);
  export_menu->addAction(tr("Dump &MRAM"), this,
                         [] { DumpSpace(AddressSpace::Type::Mem1, F_MEM1DUMP_IDX); });
  export_menu->addAction(tr("Dump &ExRAM"), this,
                         [] { DumpSpace(AddressSpace::Type::Mem2, F_MEM2DUMP_IDX); });
  export_menu->addAction(tr("Dump &ARAM"), this,
                         [] { DumpSpace(AddressSpace::Type::Auxiliary, F_ARAMDUMP_IDX); });
  export_menu->addAction(tr("Dump &FakeVMEM"), this,
                         [] { DumpSpace(AddressSpace::Type::Fake, F_FAKEVMEMDUMP_IDX); });
  bar->addMenu(export_menu);
}

void MemoryWidget::Wire()
{
  connect(m_address_box, &QComboBox::currentTextChanged, this, &MemoryWidget::JumpToTyped);
  connect(m_offset_box, &QLineEdit::textChanged, this, &MemoryWidget::JumpToTyped);
  connect(m_value_edit, &QLineEdit::textChanged, this, &MemoryWidget::PreviewValue);
  connect(m_value_type, &QComboBox::currentIndexChanged, this, &MemoryWidget::PreviewValue);
  connect(m_write_button, &QPushButton::clicked, this, &MemoryWidget::WriteValue);
  connect(m_next_button, &QPushButton::clicked, this, [this] { Search(true); });
  connect(m_previous_button, &QPushButton::clicked, this, [this] { Search(false); });

  for (QRadioButton* const radio : {m_effective_radio, m_auxiliary_radio, m_physical_radio})
    connect(radio, &QRadioButton::toggled, this, &MemoryWidget::AddressSpaceChosen);
  for (QComboBox* const combo : {m_display_type, m_alignment, m_row_length})
    connect(combo, &QComboBox::currentIndexChanged, this, &MemoryWidget::DisplayChosen);
  connect(m_dual_check, &QCheckBox::toggled, this, &MemoryWidget::DisplayChosen);
  for (QRadioButton* const radio : {m_bp_both_radio, m_bp_read_radio, m_bp_write_radio})
    connect(radio, &QRadioButton::toggled, this, &MemoryWidget::BreakpointKindChosen);
  connect(m_hex_check, &QCheckBox::toggled, this, &MemoryWidget::PreviewValue);
  connect(m_bp_log_check, &QCheckBox::toggled, this, &MemoryWidget::BreakpointLoggingChosen);

  for (QListWidget* const list : {m_symbol_list, m_data_list, m_note_list})
    connect(list, &QListWidget::itemClicked, this, &MemoryWidget::JumpToLabel);

  connect(Host::GetInstance(), &Host::PPCSymbolsChanged, this, &MemoryWidget::RefreshLabels);
  connect(m_label_filter, &QLineEdit::textChanged, this, &MemoryWidget::RefreshLabels);
  connect(m_memory_view, &MemoryViewWidget::ShowCode, this, &MemoryWidget::ShowCode);
  connect(m_memory_view, &MemoryViewWidget::RequestWatch, this, &MemoryWidget::RequestWatch);
  connect(m_memory_view, &MemoryViewWidget::ActivateSearch, this, &MemoryWidget::FocusAddressBox);
}

void MemoryWidget::closeEvent(QCloseEvent*)
{
  Settings::Instance().SetMemoryVisible(false);
  StopFrameUpdates();
}

void MemoryWidget::showEvent(QShowEvent*)
{
  if (m_auto_update)
    StartFrameUpdates();
  Update();
}

void MemoryWidget::hideEvent(QHideEvent*)
{
  StopFrameUpdates();
}

// Values are captured at the end of every emulated field while auto update is on.
void MemoryWidget::StartFrameUpdates()
{
  m_frame_hook = m_system.GetVideoEvents().vi_end_field_event.Register(
      [this] { m_memory_view->UpdateOnFrameEnd(); });
}

void MemoryWidget::StopFrameUpdates()
{
  m_frame_hook.reset();
}

void MemoryWidget::Update()
{
  if (!isVisible())
    return;
  m_memory_view->UpdateDispatcher(MemoryViewWidget::UpdateType::Addresses);
  update();
}

void MemoryWidget::RestoreChoices()
{
  const QSettings& stored = Settings::GetQSettings();
  m_value_type->setCurrentIndex(stored.value(QStringLiteral("memorywidget/inputcombo"), 1).toInt());

  // Set in this order; an exclusive radio button cannot be unchecked directly.
  m_effective_radio->setChecked(
      stored.value(QStringLiteral("memorywidget/addrspace_effective"), true).toBool());
  m_auxiliary_radio->setChecked(
      stored.value(QStringLiteral("memorywidget/addrspace_auxiliary"), false).toBool());
  m_physical_radio->setChecked(
      stored.value(QStringLiteral("memorywidget/addrspace_physical"), false).toBool());

  m_display_type->setCurrentIndex(
      stored.value(QStringLiteral("memorywidget/display_type"), 1).toInt());

  const bool both = stored.value(QStringLiteral("memorywidget/bpreadwrite"), true).toBool();
  const bool reads = stored.value(QStringLiteral("memorywidget/bpread"), false).toBool();
  const bool writes = stored.value(QStringLiteral("memorywidget/bpwrite"), false).toBool();
  const bool log = stored.value(QStringLiteral("memorywidget/bplog"), true).toBool();

  using BPType = MemoryViewWidget::BPType;
  m_memory_view->SetBPType(both ? BPType::ReadWrite : reads ? BPType::ReadOnly : BPType::WriteOnly);
  m_bp_both_radio->setChecked(both);
  m_bp_read_radio->setChecked(reads);
  m_bp_write_radio->setChecked(writes);
  m_bp_log_check->setChecked(log);
}

void MemoryWidget::StoreChoices()
{
  QSettings& stored = Settings::GetQSettings();
  const std::array<std::pair<const char*, QVariant>, 9> values{{
      {"memorywidget/inputcombo", m_value_type->currentIndex()},
      {"memorywidget/addrspace_effective", m_effective_radio->isChecked()},
      {"memorywidget/addrspace_auxiliary", m_auxiliary_radio->isChecked()},
      {"memorywidget/addrspace_physical", m_physical_radio->isChecked()},
      {"memorywidget/display_type", m_display_type->currentIndex()},
      {"memorywidget/bpreadwrite", m_bp_both_radio->isChecked()},
      {"memorywidget/bpread", m_bp_read_radio->isChecked()},
      {"memorywidget/bpwrite", m_bp_write_radio->isChecked()},
      {"memorywidget/bplog", m_bp_log_check->isChecked()},
  }};
  for (const auto& [key, value] : values)
    stored.setValue(QString::fromLatin1(key), value);
}

void MemoryWidget::AddressSpaceChosen()
{
  AddressSpace::Type space = AddressSpace::Type::Physical;
  if (m_effective_radio->isChecked())
    space = AddressSpace::Type::Effective;
  else if (m_auxiliary_radio->isChecked())
    space = AddressSpace::Type::Auxiliary;
  m_memory_view->SetAddressSpace(space);
  StoreChoices();
}

void MemoryWidget::DisplayChosen()
{
  const auto type = static_cast<ViewType>(m_display_type->currentData().toInt());
  int row_bytes = m_row_length->currentData().toInt();
  // A double does not fit a four-byte row.
  if (type == ViewType::Double && row_bytes == 4)
    row_bytes = 8;

  // "Fixed" aligns to the row length; the other entries carry their value as item data.
  const int alignment =
      m_alignment->currentIndex() == 0 ? row_bytes : m_alignment->currentData().toInt();

  m_memory_view->SetDisplay(type, row_bytes, alignment, m_dual_check->isChecked());
  StoreChoices();
}

void MemoryWidget::BreakpointLoggingChosen()
{
  m_memory_view->SetBPLoggingEnabled(m_bp_log_check->isChecked());
  StoreChoices();
}

void MemoryWidget::BreakpointKindChosen()
{
  using BPType = MemoryViewWidget::BPType;
  BPType kind = BPType::WriteOnly;
  if (m_bp_both_radio->isChecked())
    kind = BPType::ReadWrite;
  else if (m_bp_read_radio->isChecked())
    kind = BPType::ReadOnly;
  m_memory_view->SetBPType(kind);
  StoreChoices();
}

void MemoryWidget::SetAddress(u32 address)
{
  // Keep the previous address (if it was a valid one) and the new one in the history list,
  // without reacting to our own edits of the box.
  const QSignalBlocker quiet(m_address_box);
  const QString previous = m_address_box->currentText();
  const QString wanted = QString::number(address, 16);

  bool keep_previous = false;
  const u32 previous_address = previous.toUInt(&keep_previous, 16);
  if (keep_previous)
  {
    AddressSpace::Accessors* const memory =
        AddressSpace::GetAccessors(m_memory_view->GetAddressSpace());
    const Core::CPUThreadGuard guard(m_system);
    keep_previous = memory->IsValidAddress(guard, previous_address);
  }

  if (m_address_box->findText(previous) == -1 && keep_previous)
    m_address_box->insertItem(0, previous);
  if (m_address_box->findText(wanted) == -1)
    m_address_box->insertItem(0, wanted);
  m_address_box->setCurrentText(wanted);

  m_memory_view->SetAddress(address);
  Settings::Instance().SetMemoryVisible(true);
  raise();
  m_memory_view->setFocus();
}

void MemoryWidget::FocusAddressBox()
{
  m_address_box->setFocus();
  m_address_box->lineEdit()->selectAll();
}

void MemoryWidget::JumpToTyped()
{
  const Target target = ResolveTarget();
  if (target.address_ok && target.offset_ok)
    m_memory_view->SetAddress(target.address);
  MarkInput(m_address_box, !target.address_ok);
  MarkInput(m_offset_box, !target.offset_ok);
}

// Normalises the typed value, shows the bytes it would write (or marks the box), and keeps the
// Hex checkbox enabled only for integer types.
void MemoryWidget::PreviewValue()
{
  m_value_preview->clear();
  QString text = m_value_edit->text();
  const auto type = static_cast<ViewType>(m_value_type->currentData().toInt());
  m_hex_check->setEnabled(IsIntegerType(type));

  if (text.isEmpty())
    return;

  if (type != ViewType::ASCII)
    text.remove(QLatin1Char(' '));

  const QString hex_prefix = QStringLiteral("0x");
  if (type == ViewType::HexString)
  {
    // fromHex() does not accept a prefix.
    if (text.startsWith(hex_prefix, Qt::CaseInsensitive))
      text.remove(0, 2);
  }
  else if (m_hex_check->isChecked())
  {
    // Add the prefix (after a minus sign, if any) unless it is already there.
    const qsizetype digits_at = text.startsWith(QLatin1Char('-')) ? 1 : 0;
    if (!QStringView(text).sliced(digits_at).startsWith(hex_prefix, Qt::CaseInsensitive))
      text.insert(digits_at, hex_prefix);
  }

  const std::vector<u8> bytes = m_memory_view->ConvertTextToBytes(type, text);
  if (!bytes.empty())
  {
    // At most eight bytes are shown, space separated, with "..." when there are more.
    QStringList shown;
    for (std::size_t i = 0; i < bytes.size() && i < 8; ++i)
      shown.append(QString::fromStdString(fmt::format("{:02x}", bytes[i])));
    QString preview = shown.join(QLatin1Char(' '));
    if (bytes.size() > 8)
      preview.append(QStringLiteral("..."));
    m_value_preview->setText(preview);
  }
  MarkInput(m_value_edit, bytes.empty());
}

// The bytes to write or search for; empty when the input is missing or invalid.
QByteArray MemoryWidget::ValueBytes() const
{
  if (m_value_preview->text().isEmpty())
    return {};

  const auto type = static_cast<ViewType>(m_value_type->currentData().toInt());
  // The preview may be cut short for these, so they are taken from the edit box.
  if (type == ViewType::ASCII)
    return m_value_edit->text().toUtf8();
  if (type == ViewType::HexString)
    return QByteArray::fromHex(m_value_edit->text().toUtf8());
  return QByteArray::fromHex(m_value_preview->text().toUtf8());
}

// Address box (hex; empty means 0) plus offset box (signed hex; empty means 0). The offset is
// rejected if it would move the address past either end of the 32-bit space.
MemoryWidget::Target MemoryWidget::ResolveTarget() const
{
  Target target;
  const QString address_text = m_address_box->currentText();
  const QString offset_text = m_offset_box->text();

  const u32 base = address_text.toUInt(&target.address_ok, 16);
  target.address_ok = target.address_ok || address_text.isEmpty();

  const s32 offset = offset_text.toInt(&target.offset_ok, 16);
  target.offset_ok = target.offset_ok || offset_text.isEmpty();

  // Magnitude of a negative offset, computed without overflowing on the most negative value.
  const u32 back = offset == std::numeric_limits<s32>::min() ?
                       u32{1} << 31 :
                       static_cast<u32>(-static_cast<s64>(offset));
  if (offset < 0 && back > base)
    target.offset_ok = false;
  if (offset > 0 && std::numeric_limits<u32>::max() - static_cast<u32>(offset) < base)
    target.offset_ok = false;

  if (target.address_ok && target.offset_ok)
    target.address = offset < 0 ? base - back : base + static_cast<u32>(offset);
  return target;
}

bool MemoryWidget::ReportBadTarget(const Target& target)
{
  if (!target.address_ok)
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Bad address provided."));
    return true;
  }
  if (!target.offset_ok)
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Bad offset provided."));
    return true;
  }
  return false;
}

void MemoryWidget::WriteValue()
{
  if (!Core::IsRunning(m_system))
    return;

  const Target target = ResolveTarget();
  if (ReportBadTarget(target))
    return;

  const QByteArray bytes = ValueBytes();
  if (bytes.isEmpty())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Bad value provided."));
    return;
  }

  // The guard is held through the refresh at the end.
  const Core::CPUThreadGuard guard(m_system);
  AddressSpace::Accessors* const memory =
      AddressSpace::GetAccessors(m_memory_view->GetAddressSpace());
  const u32 last = target.address + static_cast<u32>(bytes.size()) - 1;
  if (!memory->IsValidAddress(guard, target.address) || !memory->IsValidAddress(guard, last))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Target address range is invalid."));
    return;
  }

  u32 address = target.address;
  for (const char byte : bytes)
    memory->WriteU8(guard, address++, static_cast<u8>(byte));
  Update();
}

void MemoryWidget::WriteFile()
{
  if (!Core::IsRunning(m_system))
    return;

  const Target target = ResolveTarget();
  if (ReportBadTarget(target))
    return;

  const QString path = QFileDialog::getOpenFileName(this, tr("Select a file"), QDir::currentPath(),
                                                    tr("All files (*)"));
  if (path.isNull())
    return;

  File::IOFile file(path.toStdString(), "rb");
  if (!file)
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Unable to open file."));
    return;
  }
  std::vector<u8> contents(file.GetSize());
  if (!file.ReadBytes(contents.data(), contents.size()))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Unable to read file."));
    return;
  }

  // Unlike WriteValue, the range is not checked up front.
  AddressSpace::Accessors* const memory =
      AddressSpace::GetAccessors(m_memory_view->GetAddressSpace());
  const Core::CPUThreadGuard guard(m_system);
  u32 address = target.address;
  for (const u8 byte : contents)
    memory->WriteU8(guard, address++, byte);
  Update();
}

void MemoryWidget::RefreshLabels()
{
  if (!m_labels_visible || m_symbol_db.IsEmpty())
  {
    m_labels_box->hide();
    return;
  }
  m_labels_box->show();
  FillSymbolLists();
  FillNoteList();
}

void MemoryWidget::JumpToLabel()
{
  // Whichever list's tab is showing.
  QList<QListWidgetItem*> picked;
  for (const QListWidget* const list : {m_note_list, m_symbol_list, m_data_list})
  {
    if (list->isVisible())
    {
      picked = list->selectedItems();
      break;
    }
  }
  if (!picked.isEmpty())
    SetAddress(picked.front()->data(Qt::UserRole).toUInt());
}

// Functions go to the Symbols tab, everything else to Data. Only the Symbols tab is sorted.
void MemoryWidget::FillSymbolLists()
{
  const QList<QListWidgetItem*> picked = m_symbol_list->selectedItems();
  const QString previous = picked.isEmpty() ? QString{} : picked.front()->text();
  m_symbol_list->clear();
  m_data_list->clear();

  const QString filter = m_label_filter->text();
  m_symbol_db.ForEachSymbol([&](const Common::Symbol& symbol) {
    QString name = QString::fromStdString(symbol.name);
    if (!symbol.object_name.empty())
      name += QString::fromStdString(fmt::format(" ({})", symbol.object_name));
    if (!name.contains(filter, Qt::CaseInsensitive))
      return;

    auto* const entry = new QListWidgetItem(name);
    if (name == previous)
      entry->setSelected(true);
    entry->setData(Qt::UserRole, symbol.address);
    (symbol.type == Common::Symbol::Type::Function ? m_symbol_list : m_data_list)->addItem(entry);
  });

  m_symbol_list->sortItems();
}

void MemoryWidget::FillNoteList()
{
  const QList<QListWidgetItem*> picked = m_note_list->selectedItems();
  const QString previous = picked.isEmpty() ? QStringLiteral("") : picked.front()->text();
  m_note_list->clear();

  const QString filter = m_label_filter->text();
  m_symbol_db.ForEachNote([&](const Common::Note& note) {
    const QString name = QString::fromStdString(note.name);
    if (!name.contains(filter, Qt::CaseInsensitive))
      return;

    auto* const entry = new QListWidgetItem(name);
    if (name == previous)
      entry->setSelected(true);
    entry->setData(Qt::UserRole, note.address);
    m_note_list->addItem(entry);
  });

  m_note_list->sortItems();
}

void MemoryWidget::Search(bool forward)
{
  Target target = ResolveTarget();
  if (!target.address_ok)
  {
    m_search_result->setText(tr("Bad address provided."));
    return;
  }
  if (!target.offset_ok)
  {
    m_search_result->setText(tr("Bad offset provided."));
    return;
  }

  const QByteArray needle = ValueBytes();
  if (needle.isEmpty())
  {
    m_search_result->setText(tr("Bad Value Given"));
    return;
  }

  // Step off the address in the box so the previous match is not found again.
  if (!m_address_box->currentText().isEmpty())
    target.address = forward ? target.address + 1 : target.address - 1;

  std::optional<u32> found;
  {
    AddressSpace::Accessors* const memory =
        AddressSpace::GetAccessors(m_memory_view->GetAddressSpace());
    const Core::CPUThreadGuard guard(m_system);
    found = memory->Search(guard, target.address, reinterpret_cast<const u8*>(needle.data()),
                           static_cast<u32>(needle.size()), forward);
  }

  if (!found)
  {
    m_search_result->setText(tr("No Match"));
    return;
  }

  m_search_result->setText(tr("Match Found"));
  m_address_box->setCurrentText(QStringLiteral("%1").arg(*found, 8, 16, QLatin1Char('0')));
  m_offset_box->clear();
  m_memory_view->SetAddress(*found);
}
