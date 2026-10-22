// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/MemoryViewWidget.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <type_traits>

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QtGlobal>

#include "Common/Align.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/AddressSpace.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/EditSymbolDialog.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"

namespace
{
using ViewType = MemoryViewWidget::Type;

// Table layout: column 0 is the row breakpoint toggle, column 1 the row address, then the value
// columns, then (optionally) the symbol column.
constexpr int BREAKPOINT_COLUMN = 0;
constexpr int ADDRESS_COLUMN = 1;
constexpr int FIRST_VALUE_COLUMN = 2;

// Per-cell data roles.
constexpr int ROLE_ROW_TOGGLE = Qt::UserRole;
constexpr int ROLE_ADDRESS = Qt::UserRole + 1;
constexpr int ROLE_VALUE_TYPE = Qt::UserRole + 2;
constexpr int ROLE_READABLE = Qt::UserRole + 3;

// The scroll bar never runs out: its handle is returned to the middle after every move, and only
// the distance moved matters.
constexpr int SCROLL_MIN = 0;
constexpr int SCROLL_MAX = 20000;
constexpr int SCROLL_PAGE = 250;
constexpr int SCROLL_MIDDLE = SCROLL_MAX / 2;

// A wheel notch is 120 eighths of a degree.
constexpr double WHEEL_EIGHTHS_PER_NOTCH = 15.0 * 8;

QString Unreadable()
{
  return QStringLiteral("-");
}

QString Hex8Text(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

struct TypeInfo
{
  int bytes;
  // Widest text the type produces, plus one character of gap.
  int chars;
};

TypeInfo InfoOf(ViewType type)
{
  switch (type)
  {
  case ViewType::Hex8:
    return {1, 3};
  case ViewType::Hex16:
    return {2, 5};
  case ViewType::Hex32:
    return {4, 9};
  case ViewType::Hex64:
    return {8, 17};
  case ViewType::Unsigned8:
    return {1, 4};
  case ViewType::Unsigned16:
    return {2, 6};
  case ViewType::Unsigned32:
    return {4, 11};
  case ViewType::Signed8:
    return {1, 5};
  case ViewType::Signed16:
    return {2, 7};
  case ViewType::Signed32:
    return {4, 12};
  case ViewType::ASCII:
    return {1, 2};
  case ViewType::Float32:
    return {4, 11};  // e.g. -3.403e+38
  case ViewType::Double:
    return {8, 12};  // e.g. -1.798e+308
  default:
    return {1, 10};
  }
}

int BytesOf(ViewType type)
{
  return InfoOf(type).bytes;
}

bool RightAligned(ViewType type)
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

// value as bytes in big-endian (emulated memory) order.
template <typename T>
std::vector<u8> GuestBytes(T value)
{
  using U = std::make_unsigned_t<T>;
  const U bits = static_cast<U>(value);
  std::vector<u8> out(sizeof(T));
  for (std::size_t i = 0; i < sizeof(T); ++i)
    out[i] = static_cast<u8>(bits >> (8 * (sizeof(T) - 1 - i)));
  return out;
}

std::vector<u8> RawBytes(const QByteArray& array)
{
  return std::vector<u8>(array.begin(), array.end());
}

// Floating point values get a leading space when positive so the digits line up with negatives.
QString FloatText(double value)
{
  QString text = QString::number(value, 'g', 4);
  if (!text.startsWith(QLatin1Char('-')))
    text.prepend(QLatin1Char(' '));
  return text;
}
}  // namespace

void TableEditDelegate::setModelData(QWidget* editor, QAbstractItemModel*,
                                     const QModelIndex& index) const
{
  // The model is deliberately left untouched; the view refreshes the cell from memory.
  emit editFinished(index.row(), index.column(), qobject_cast<QLineEdit*>(editor)->text());
}

// The table part of the view. Keyboard, wheel and mouse input move or act on the owning widget.
class MemoryViewWidget::Grid final : public QTableWidget
{
public:
  explicit Grid(MemoryViewWidget* owner) : QTableWidget(owner), m_owner(owner)
  {
    horizontalHeader()->hide();
    verticalHeader()->hide();
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setShowGrid(false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    // Selection is only ever set from code (to mark the searched address).
    setSelectionMode(NoSelection);
    setTextElideMode(Qt::TextElideMode::ElideNone);

    auto* const editing = new TableEditDelegate(this);
    setItemDelegate(editing);

    // Keep the highlight colours the same whether or not the table has focus.
    QPalette colours(m_owner->palette());
    colours.setBrush(QPalette::Inactive, QPalette::Highlight, colours.brush(QPalette::Highlight));
    colours.setBrush(QPalette::Inactive, QPalette::HighlightedText,
                     colours.brush(QPalette::HighlightedText));
    setPalette(colours);

    setRowCount(30);
    setColumnCount(8);

    connect(this, &Grid::customContextMenuRequested, m_owner, &MemoryViewWidget::ShowCellMenu);
    connect(editing, &TableEditDelegate::editFinished, m_owner, &MemoryViewWidget::WriteTypedText);
  }

  // How many rows of the current height fit.
  int FittingRows() const
  {
    return static_cast<int>(
        std::round(static_cast<float>(height()) / static_cast<float>(rowHeight(0)) - 0.25));
  }

protected:
  void resizeEvent(QResizeEvent* event) override
  {
    QTableWidget::resizeEvent(event);
    if (FittingRows() != rowCount())
      m_owner->UpdateDispatcher(UpdateType::Full);
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    const int key = event->key();
    if (key == Qt::Key_Up || key == Qt::Key_Down)
    {
      m_owner->ScrollRows(key == Qt::Key_Up ? -1 : 1);
      return;
    }
    if (key == Qt::Key_PageUp || key == Qt::Key_PageDown)
    {
      const s64 page = rowCount();
      m_owner->ScrollRows(key == Qt::Key_PageUp ? -page : page);
      return;
    }
    if (key == Qt::Key_G && event->modifiers() == Qt::ControlModifier)
    {
      emit m_owner->ActivateSearch();
      return;
    }
    // Not the table's own handler: its cursor-key navigation is not wanted here.
    QWidget::keyPressEvent(event);
  }

  void wheelEvent(QWheelEvent* event) override
  {
    const int notches =
        static_cast<int>(std::round(event->angleDelta().y() / WHEEL_EIGHTHS_PER_NOTCH));
    if (notches != 0)
      m_owner->ScrollRows(-s64{notches});
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    // Right clicks are handled by the context menu; nothing else reacts to other buttons.
    if (event->button() != Qt::LeftButton)
      return;
    const QTableWidgetItem* const cell = itemAt(event->pos());
    if (cell == nullptr)
      return;

    if (cell->data(ROLE_ROW_TOGGLE).toBool())
      m_owner->ToggleBreakpoint(cell->data(ROLE_ADDRESS).toUInt(), true);
    else
      QTableWidget::mousePressEvent(event);
  }

private:
  MemoryViewWidget* m_owner;
};

MemoryViewWidget::MemoryViewWidget(Core::System& system, QWidget* parent)
    : QWidget(parent), m_system(system), m_symbol_db(m_system.GetPPCSymbolDB())
{
  auto* const row = new QHBoxLayout();
  row->setContentsMargins(0, 0, 0, 0);

  m_grid = new Grid(this);
  row->addWidget(m_grid);

  m_scroll = new QScrollBar(this);
  m_scroll->setRange(SCROLL_MIN, SCROLL_MAX);
  m_scroll->setPageStep(SCROLL_PAGE);
  m_scroll->setValue(SCROLL_MIDDLE);
  connect(m_scroll, &QScrollBar::actionTriggered, this, &MemoryViewWidget::ScrollBarAction);
  connect(m_scroll, &QScrollBar::sliderReleased, this, &MemoryViewWidget::RecentreScrollBar);
  row->addWidget(m_scroll);
  setLayout(row);

  const Settings* const settings = &Settings::Instance();
  const Host* const host = Host::GetInstance();
  connect(settings, &Settings::DebugFontChanged, this, &MemoryViewWidget::UpdateFont);
  connect(host, &Host::PPCSymbolsChanged, this, [this] { UpdateDispatcher(UpdateType::Symbols); });
  connect(host, &Host::PPCBreakpointsChanged, this, &MemoryViewWidget::RefreshBreakpointMarks);
  connect(settings, &Settings::EmulationStateChanged, this, [this] {
    // Pauses already arrive through UpdateDisasmDialog.
    if (Core::GetState(m_system) != Core::State::Paused)
      UpdateDispatcher(UpdateType::Values);
  });
  connect(host, &Host::UpdateDisasmDialog, this, [this] {
    // That signal fires constantly while running; only react to steps and breaks.
    if (Core::GetState(m_system) != Core::State::Running)
      UpdateDispatcher(UpdateType::Values);
  });
  // Values captured on the CPU thread are shown from here, on the GUI thread.
  connect(this, &MemoryViewWidget::AutoUpdate, this,
          [this] { UpdateDispatcher(UpdateType::Auto); });
  connect(settings, &Settings::ThemeChanged, this, [this] { UpdateDispatcher(UpdateType::Full); });

  // Builds the table as well.
  UpdateFont(Settings::Instance().GetDebugFont());
}

void MemoryViewWidget::UpdateFont(const QFont& font)
{
  const QFontMetrics metrics(font);
  m_line_height = metrics.lineSpacing() + 4;
  // The advance of '0' (which includes inter-character spacing) sizes the value columns; a
  // bounding box differs too much between fonts.
  m_char_width = metrics.horizontalAdvance(QLatin1Char('0'));
  m_grid->setFont(font);
  UpdateDispatcher(UpdateType::Full);
}

int MemoryViewWidget::DataSpan() const
{
  return m_bytes_per_row / BytesOf(m_type);
}

// In dual view the left half shows the same memory as plain hex of the same width.
std::optional<MemoryViewWidget::Type> MemoryViewWidget::DualViewType() const
{
  if (!m_dual_view)
    return std::nullopt;
  switch (BytesOf(m_type))
  {
  case 1:
    return Type::Hex8;
  case 2:
    return Type::Hex16;
  case 8:
    return Type::Hex64;
  default:
    return Type::Hex32;
  }
}

void MemoryViewWidget::UpdateDispatcher(UpdateType type)
{
  std::unique_lock lock(m_busy, std::defer_lock);
  // A full rebuild may change the row count, so it waits for the lock instead of skipping.
  if (type == UpdateType::Full)
    lock.lock();
  else if (!isVisible() || !lock.try_lock())
    return;

  // Anything but a full update needs the table to exist already.
  if (m_grid->item(2, 1) == nullptr)
    type = UpdateType::Full;

  if (type == UpdateType::Auto)
  {
    if (!m_values.empty())
      RefreshValueCells();
    return;
  }

  // Each kind of update also does everything the cheaper kinds below it do.
  const bool rebuild = type == UpdateType::Full;
  const bool readdress = rebuild || type == UpdateType::Addresses;
  const bool reread = readdress || type == UpdateType::Values;
  if (rebuild)
    CreateTable();
  if (readdress)
    Update();
  if (reread)
  {
    if (Core::GetState(m_system) == Core::State::Paused)
      GetValues();
    RefreshValueCells();
  }
  UpdateSymbols();
}

void MemoryViewWidget::CreateTable()
{
  m_grid->clearContents();

  // Row heights (and so the vertical spacing) only take reliably when set here.
  m_grid->verticalHeader()->setDefaultSectionSize(m_line_height);
  m_grid->verticalHeader()->setMinimumSectionSize(m_line_height);
  m_grid->horizontalHeader()->setMinimumSectionSize(m_char_width * 2);

  const int span = DataSpan();
  m_value_columns = m_dual_view ? span * 2 : span;
  const int columns = FIRST_VALUE_COLUMN + m_value_columns + (m_show_symbols ? 1 : 0);
  const int rows = m_grid->FittingRows();
  m_grid->setColumnCount(columns);
  m_grid->setRowCount(rows);
  m_grid->setColumnWidth(BREAKPOINT_COLUMN, m_grid->rowHeight(0));

  const std::optional<Type> left_type = DualViewType();

  QTableWidgetItem toggle_cell;
  toggle_cell.setFlags(Qt::ItemIsEnabled);
  toggle_cell.setData(ROLE_ROW_TOGGLE, true);
  toggle_cell.setData(ROLE_VALUE_TYPE, static_cast<int>(Type::Null));

  QTableWidgetItem address_cell(Unreadable());
  address_cell.setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
  address_cell.setData(ROLE_ROW_TOGGLE, false);
  address_cell.setData(ROLE_VALUE_TYPE, static_cast<int>(Type::Null));

  // One prototype is reused for every value cell. Its alignment is only ever set for right-hand
  // cells, so dual-view left cells after the first row inherit whatever was set last.
  QTableWidgetItem value_cell(Unreadable());
  value_cell.setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
  value_cell.setData(ROLE_ROW_TOGGLE, false);
  const Qt::Alignment right_alignment =
      (RightAligned(m_type) ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter;

  for (int r = 0; r < rows; ++r)
  {
    m_grid->setItem(r, BREAKPOINT_COLUMN, toggle_cell.clone());
    m_grid->setItem(r, ADDRESS_COLUMN, address_cell.clone());
    for (int c = 0; c < m_value_columns; ++c)
    {
      if (left_type && c < span)
      {
        value_cell.setData(ROLE_VALUE_TYPE, static_cast<int>(*left_type));
      }
      else
      {
        value_cell.setData(ROLE_VALUE_TYPE, static_cast<int>(m_type));
        value_cell.setTextAlignment(right_alignment);
      }
      m_grid->setItem(r, FIRST_VALUE_COLUMN + c, value_cell.clone());
    }

    if (m_show_symbols)
    {
      auto* const label = new QTableWidgetItem(QStringLiteral("-"));
      label->setFlags(Qt::ItemIsEnabled);
      m_grid->setItem(r, m_grid->columnCount() - 1, label);
    }
  }

  // Widths: the dual-view left half (with a wider gap after it), then everything else, the
  // symbol column included.
  int next = FIRST_VALUE_COLUMN;
  if (left_type)
  {
    const int left_width = m_char_width * InfoOf(*left_type).chars;
    for (int c = 0; c < span - 1; ++c)
      m_grid->setColumnWidth(next + c, left_width);
    m_grid->setColumnWidth(next + span - 1, left_width + m_char_width * 2);
    next += span;
  }
  const int width = m_char_width * InfoOf(m_type).chars;
  for (int c = next; c < columns; ++c)
    m_grid->setColumnWidth(c, width);
}

void MemoryViewWidget::Update()
{
  m_grid->clearSelection();

  const int rows = m_grid->rowCount();
  const u32 row_bytes = static_cast<u32>(m_bytes_per_row);
  const u32 value_bytes = static_cast<u32>(BytesOf(m_type));
  const u32 centre = Common::AlignDown(m_address, static_cast<std::size_t>(m_alignment));
  const u32 first = centre - static_cast<u32>(rows / 2) * row_bytes;
  m_shown_range = {first, first + static_cast<u32>(rows) * row_bytes};

  const int span = DataSpan();
  for (int r = 0; r < rows; ++r)
  {
    const u32 row_address = first + static_cast<u32>(r) * row_bytes;
    m_grid->item(r, BREAKPOINT_COLUMN)->setData(ROLE_ADDRESS, row_address);
    QTableWidgetItem* const address_cell = m_grid->item(r, ADDRESS_COLUMN);
    address_cell->setText(Hex8Text(row_address));
    address_cell->setData(ROLE_ADDRESS, row_address);

    for (int c = 0; c < m_value_columns; ++c)
    {
      // The right half of a dual view repeats the addresses of the left half.
      const int slot = (m_dual_view && c >= span) ? c - span : c;
      QTableWidgetItem* const cell = m_grid->item(r, FIRST_VALUE_COLUMN + c);
      cell->setData(ROLE_ADDRESS, row_address + static_cast<u32>(slot) * value_bytes);
      cell->setBackground(Qt::transparent);
      cell->setData(ROLE_READABLE, false);
    }

    if (m_show_symbols)
      m_grid->item(r, m_grid->columnCount() - 1)->setData(ROLE_ADDRESS, row_address);
  }

  RefreshBreakpointMarks();
}

// Writes the captured values into the cells. A cell whose value changed is tinted with the
// highlight colour, which then fades by one alpha step per refresh.
void MemoryViewWidget::RefreshValueCells()
{
  const auto lookup = [](const ValueMap& values, u32 address) -> std::optional<QString> {
    const auto found = values.find(address);
    return found == values.end() ? std::nullopt : found->second;
  };

  for (int r = 0; r < m_grid->rowCount(); ++r)
  {
    for (int c = 0; c < m_value_columns; ++c)
    {
      QTableWidgetItem* const cell = m_grid->item(r, FIRST_VALUE_COLUMN + c);
      if (cell == nullptr)
        return;

      const u32 address = cell->data(ROLE_ADDRESS).toUInt();
      const Type type = static_cast<Type>(cell->data(ROLE_VALUE_TYPE).toInt());
      // Only the dual-view left half has a type other than m_type.
      const std::optional<QString> text =
          lookup(type == m_type ? m_values : m_dual_values, address);

      if (address == m_highlight_address)
        cell->setSelected(true);

      QColor tint = cell->background().color();
      const bool was_readable = cell->data(ROLE_READABLE).toBool();
      if (!text)
      {
        cell->setBackground(Qt::transparent);
        cell->setData(ROLE_READABLE, false);
        cell->setText(Unreadable());
      }
      else if (!was_readable)
      {
        // Becoming readable is not a change worth highlighting.
        cell->setData(ROLE_READABLE, true);
        cell->setText(*text);
      }
      else if (tint.rgb() != m_highlight.rgb() && tint != Qt::transparent)
      {
        // Some other colour (a breakpoint) is in charge of this cell.
        cell->setText(*text);
      }
      else if (cell->text() != *text)
      {
        cell->setBackground(m_highlight);
        cell->setText(*text);
      }
      else if (tint.alpha() > 0)
      {
        tint.setAlpha(tint.alpha() - 1);
        cell->setBackground(tint);
      }
    }
  }
}

void MemoryViewWidget::UpdateSymbols()
{
  if (!m_show_symbols)
    return;

  const int label_column = m_grid->columnCount() - 1;
  for (int r = 0; r < m_grid->rowCount(); ++r)
  {
    QTableWidgetItem* const label = m_grid->item(r, label_column);
    if (label == nullptr)
      continue;

    // A note takes precedence over the symbol description.
    const u32 address = label->data(ROLE_ADDRESS).toUInt();
    const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);
    const std::string text = note != nullptr ? note->name : m_symbol_db.GetDescription(address);
    label->setText(QString::fromStdString(" " + text));
  }
  m_grid->resizeColumnToContents(label_column);
}

// Called on the CPU thread at the end of a frame. Never blocks, and never touches widgets.
void MemoryViewWidget::UpdateOnFrameEnd()
{
  const std::unique_lock lock(m_busy, std::try_to_lock);
  if (!lock)
    return;
  GetValues();
  emit AutoUpdate();
}

void MemoryViewWidget::GetValues()
{
  m_values.clear();
  m_dual_values.clear();
  const std::optional<Type> left_type = DualViewType();

  const Core::CPUThreadGuard guard(m_system);
  const u32 step = static_cast<u32>(BytesOf(m_type));
  const auto [begin, end] = m_shown_range;
  const u32 count = (end - begin) / step;
  for (u32 i = 0; i < count; ++i)
  {
    const u32 address = begin + i * step;
    m_values.emplace(address, ValueToString(guard, address, m_type));
    if (left_type)
      m_dual_values.emplace(address, ValueToString(guard, address, *left_type));
  }
}

// Must only be called with the CPU thread's role held (see the guard).
std::optional<QString> MemoryViewWidget::ValueToString(const Core::CPUThreadGuard& guard,
                                                       u32 address, Type type)
{
  const AddressSpace::Accessors* const memory = AddressSpace::GetAccessors(m_address_space);
  if (!memory->IsValidAddress(guard, address))
    return std::nullopt;

  const auto padded_hex = [](u64 value, int digits) {
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0'));
  };

  switch (type)
  {
  case Type::Hex8:
    return padded_hex(memory->ReadU8(guard, address), 2);
  case Type::Hex16:
    return padded_hex(memory->ReadU16(guard, address), 4);
  case Type::Hex32:
    return padded_hex(memory->ReadU32(guard, address), 8);
  case Type::Hex64:
    return padded_hex(memory->ReadU64(guard, address), 16);
  case Type::ASCII:
  {
    const char character = static_cast<char>(memory->ReadU8(guard, address));
    return QString{QChar::fromLatin1(Common::IsPrintableCharacter(character) ? character : '.')};
  }
  case Type::Unsigned8:
    return QString::number(memory->ReadU8(guard, address));
  case Type::Unsigned16:
    return QString::number(memory->ReadU16(guard, address));
  case Type::Unsigned32:
    return QString::number(memory->ReadU32(guard, address));
  case Type::Signed8:
    return QString::number(std::bit_cast<s8>(memory->ReadU8(guard, address)));
  case Type::Signed16:
    return QString::number(std::bit_cast<s16>(memory->ReadU16(guard, address)));
  case Type::Signed32:
    return QString::number(std::bit_cast<s32>(memory->ReadU32(guard, address)));
  case Type::Float32:
    return FloatText(memory->ReadF32(guard, address));
  case Type::Double:
    return FloatText(std::bit_cast<double>(memory->ReadU64(guard, address)));
  default:
    return std::nullopt;
  }
}

// Paints value cells covered by a memory breakpoint red and shows the row icon when any are.
void MemoryViewWidget::RefreshBreakpointMarks()
{
  const bool effective = m_address_space == AddressSpace::Type::Effective;
  const int value_bytes = BytesOf(m_type);
  MemChecks& checks = m_system.GetPowerPC().GetMemChecks();

  for (int r = 0; r < m_grid->rowCount(); ++r)
  {
    bool marked = false;
    for (int c = 0; c < m_value_columns; ++c)
    {
      QTableWidgetItem* const cell = m_grid->item(r, FIRST_VALUE_COLUMN + c);
      const u32 address = cell->data(ROLE_ADDRESS).toUInt();
      // Address 0 is treated as "no address" and also clears what earlier cells found.
      if (address == 0)
      {
        marked = false;
        continue;
      }

      const bool covered =
          effective &&
          checks.GetMemCheck(address, static_cast<std::size_t>(value_bytes)) != nullptr;
      cell->setBackground(covered ? QBrush(Qt::red) : QBrush(Qt::transparent));
      marked = marked || covered;
    }

    QTableWidgetItem* const toggle = m_grid->item(r, BREAKPOINT_COLUMN);
    if (marked)
    {
      const int edge = m_grid->rowHeight(0) - 3;
      toggle->setData(Qt::DecorationRole,
                      Resources::GetThemeIcon("debugger_breakpoint").pixmap(QSize(edge, edge)));
    }
    else
    {
      toggle->setData(Qt::DecorationRole, QIcon());
    }
  }
}

void MemoryViewWidget::SetAddressSpace(AddressSpace::Type address_space)
{
  if (address_space == m_address_space)
    return;
  m_address_space = address_space;
  UpdateDispatcher(UpdateType::Addresses);
}

AddressSpace::Type MemoryViewWidget::GetAddressSpace() const
{
  return m_address_space;
}

// Parses text typed into a cell of the given type into the bytes to store, most significant
// first. Returns nothing if the text does not fit the type.
std::vector<u8> MemoryViewWidget::ConvertTextToBytes(Type type, QStringView input_text) const
{
  bool ok = false;
  // 0 lets Qt pick the base from the prefix (0x, 0); the hex types force 16.
  const int base = (type == Type::Hex8 || type == Type::Hex16 || type == Type::Hex32) ? 16 : 0;

  switch (type)
  {
  case Type::ASCII:
    return RawBytes(input_text.toUtf8());
  case Type::Float32:
  {
    const float value = input_text.toFloat(&ok);
    return ok ? GuestBytes(std::bit_cast<u32>(value)) : std::vector<u8>{};
  }
  case Type::Double:
  {
    const double value = input_text.toDouble(&ok);
    return ok ? GuestBytes(std::bit_cast<u64>(value)) : std::vector<u8>{};
  }
  case Type::Signed8:
  {
    const short value = input_text.toShort(&ok, base);
    ok = ok && value >= std::numeric_limits<s8>::min() && value <= std::numeric_limits<s8>::max();
    return ok ? GuestBytes(static_cast<u8>(value)) : std::vector<u8>{};
  }
  case Type::Signed16:
  {
    const short value = input_text.toShort(&ok, base);
    return ok ? GuestBytes(static_cast<u16>(value)) : std::vector<u8>{};
  }
  case Type::Signed32:
  {
    const int value = input_text.toInt(&ok, base);
    return ok ? GuestBytes(static_cast<u32>(value)) : std::vector<u8>{};
  }
  case Type::Hex8:
  case Type::Unsigned8:
  {
    const unsigned short value = input_text.toUShort(&ok, base);
    ok = ok && value <= 0xFF;
    return ok ? GuestBytes(static_cast<u8>(value)) : std::vector<u8>{};
  }
  case Type::Hex16:
  case Type::Unsigned16:
  {
    const unsigned short value = input_text.toUShort(&ok, base);
    return ok ? GuestBytes(static_cast<u16>(value)) : std::vector<u8>{};
  }
  case Type::Hex32:
  case Type::Unsigned32:
  {
    const u32 value = input_text.toUInt(&ok, base);
    return ok ? GuestBytes(value) : std::vector<u8>{};
  }
  case Type::Hex64:
  {
    const u64 value = input_text.toULongLong(&ok, 16);
    return ok ? GuestBytes(value) : std::vector<u8>{};
  }
  case Type::HexString:
  {
    // Whole bytes only: an even number of hex digits.
    static const QRegularExpression whole_bytes(QStringLiteral("^([0-9A-F]{2})*$"),
                                                QRegularExpression::CaseInsensitiveOption);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const bool matches = whole_bytes.matchView(input_text).hasMatch();
#else
    const bool matches = whole_bytes.match(input_text).hasMatch();
#endif
    return matches ? RawBytes(QByteArray::fromHex(input_text.toUtf8())) : std::vector<u8>{};
  }
  default:
    return {};
  }
}

void MemoryViewWidget::SetDisplay(Type type, int bytes_per_row, int alignment, bool dual_view)
{
  m_type = type;
  m_bytes_per_row = bytes_per_row;
  m_dual_view = dual_view;
  // An alignment of 0 means "align to the value size".
  m_alignment = alignment != 0 ? alignment : BytesOf(type);
  UpdateDispatcher(UpdateType::Full);
}

void MemoryViewWidget::ToggleHighlights(bool enabled)
{
  // The colour is kept while disabled; only its alpha says whether highlighting is on.
  m_highlight.setAlpha(enabled ? 100 : 0);
  if (enabled)
    return;

  // Clear the current tints straight away (matters while paused).
  for (int r = 0; r < m_grid->rowCount(); ++r)
  {
    for (int c = 0; c < m_value_columns; ++c)
      m_grid->item(r, FIRST_VALUE_COLUMN + c)->setBackground(m_highlight);
  }
}

void MemoryViewWidget::SetHighlightColor()
{
  QColor picked = QColorDialog::getColor(m_highlight);
  if (!picked.isValid())
    return;

  const bool enabled = m_highlight.alpha() != 0;
  m_highlight = picked;
  m_highlight.setAlpha(enabled ? 100 : 0);
  if (!enabled)
    return;

  // Recolour the existing tints, each keeping how far it has faded.
  for (int r = 0; r < m_grid->rowCount(); ++r)
  {
    for (int c = 0; c < m_value_columns; ++c)
    {
      QTableWidgetItem* const cell = m_grid->item(r, FIRST_VALUE_COLUMN + c);
      picked.setAlpha(cell->background().color().alpha());
      cell->setBackground(picked);
    }
  }
}

void MemoryViewWidget::SetBPType(BPType type)
{
  m_bp_type = type;
}

void MemoryViewWidget::SetAddress(u32 address)
{
  m_highlight_address = address;
  if (address == m_address)
    return;
  m_address = address;
  UpdateDispatcher(UpdateType::Addresses);
}

void MemoryViewWidget::SetBPLoggingEnabled(bool enabled)
{
  m_log_hits = enabled;
}

void MemoryViewWidget::ScrollRows(s64 rows)
{
  m_address = static_cast<u32>(m_address + static_cast<u64>(rows * m_bytes_per_row));
  UpdateDispatcher(UpdateType::Addresses);
}

// Toggles a memory breakpoint on one value, or on every value of the row starting at addr. For a
// row, any existing breakpoint on it means "clear the row" rather than "fill it".
void MemoryViewWidget::ToggleBreakpoint(u32 addr, bool row)
{
  if (m_address_space != AddressSpace::Type::Effective)
    return;

  const int length = BytesOf(m_type);
  const int count = row ? m_bytes_per_row / length : 1;
  MemChecks& checks = m_system.GetPowerPC().GetMemChecks();
  const bool clear_only = row && checks.OverlapsMemcheck(addr, static_cast<u32>(m_bytes_per_row));

  {
    const Core::CPUThreadGuard guard(m_system);
    DelayedMemCheckUpdate pending(&checks);
    for (int i = 0; i < count; ++i)
    {
      const u32 address = addr + static_cast<u32>(length * i);
      const TMemCheck* const existing =
          checks.GetMemCheck(address, static_cast<std::size_t>(length));
      if (existing != nullptr)
      {
        // Removing by the found start also handles checks that are not aligned to the view.
        pending |= checks.Remove(existing->start_address);
        continue;
      }
      if (clear_only)
        continue;

      TMemCheck check;
      check.start_address = address;
      check.end_address = address + static_cast<u32>(length) - 1;
      check.is_ranged = length > 0;
      check.is_break_on_read = m_bp_type != BPType::WriteOnly;
      check.is_break_on_write = m_bp_type != BPType::ReadOnly;
      check.log_on_hit = m_log_hits;
      check.break_on_hit = true;
      pending |= checks.Add(std::move(check));
    }
  }

  emit Host::GetInstance() -> PPCBreakpointsChanged();
}

void MemoryViewWidget::CopyAddress(u32 address) const
{
  QApplication::clipboard()->setText(Hex8Text(address));
}

void MemoryViewWidget::CopyHex(u32 address) const
{
  // Read a full 64-bit word and keep as many leading digits as the value type has.
  const qsizetype digits = BytesOf(m_type) * 2;
  const u64 word =
      AddressSpace::GetAccessors(m_address_space)->ReadU64(Core::CPUThreadGuard{m_system}, address);
  QApplication::clipboard()->setText(
      QStringLiteral("%1").arg(word, 16, 16, QLatin1Char('0')).left(digits));
}

void MemoryViewWidget::ShowSymbols(bool enable)
{
  m_show_symbols = enable;
  UpdateDispatcher(UpdateType::Full);
}

void MemoryViewWidget::EditLabel(EditSymbolType kind, u32 address)
{
  // Defaults for a new note or region at the clicked value.
  std::string name;
  std::string object_name;
  u32 size = static_cast<u32>(BytesOf(m_type));
  u32 start = address;
  EditSymbolDialog::Type dialog_type = EditSymbolDialog::Type::Note;

  if (kind == EditSymbolType::EditRegion)
  {
    dialog_type = EditSymbolDialog::Type::Symbol;
    if (const Common::Symbol* const symbol = m_symbol_db.GetSymbolFromAddr(address))
    {
      // Functions are edited from the code view; only data regions are handled here.
      if (symbol->type != Common::Symbol::Type::Data)
        return;
      name = symbol->name;
      object_name = symbol->object_name;
      size = symbol->size;
      start = symbol->address;
    }
  }
  else if (kind == EditSymbolType::EditNote)
  {
    const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);
    if (note == nullptr)
      return;
    name = note->name;
    size = note->size;
    start = note->address;
  }

  EditSymbolDialog dialog(this, start, &size, &name, dialog_type);
  if (dialog.exec() != QDialog::Accepted)
    return;

  const bool region = kind == EditSymbolType::EditRegion;
  if (dialog.DeleteRequested())
  {
    if (region)
      m_symbol_db.DeleteFunction(start);
    else
      m_symbol_db.DeleteNote(start);
  }
  else if (region)
  {
    m_symbol_db.AddKnownSymbol(Core::CPUThreadGuard{m_system}, start, size, name, object_name,
                               Common::Symbol::Type::Data);
  }
  else
  {
    m_symbol_db.AddKnownNote(start, size, name);
    m_symbol_db.DetermineNoteLayers();
  }
  emit Host::GetInstance() -> PPCSymbolsChanged();
}

void MemoryViewWidget::ShowCellMenu(const QPoint& pos)
{
  const QTableWidgetItem* const cell = m_grid->itemAt(pos);
  // Empty space and the row breakpoint cells have no menu.
  if (cell == nullptr || cell->data(ROLE_ROW_TOGGLE).toBool())
    return;

  const int cell_row = cell->row();
  const int cell_column = cell->column();
  const u32 address = cell->data(ROLE_ADDRESS).toUInt();
  const bool has_value = cell->data(ROLE_VALUE_TYPE).toInt() != static_cast<int>(Type::Null) &&
                         AddressSpace::GetAccessors(m_address_space)
                             ->IsValidAddress(Core::CPUThreadGuard{m_system}, address);

  auto* const menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  menu->addAction(tr("Copy Address"), this, [this, address] { CopyAddress(address); });
  menu->addAction(tr("Copy Hex"), this, [this, address] { CopyHex(address); })
      ->setEnabled(has_value);
  menu->addAction(tr("Copy Value"), this,
                  [this, cell_row, cell_column] {
                    // Looked up again: the table may have been rebuilt while the menu was open.
                    if (const QTableWidgetItem* const now = m_grid->item(cell_row, cell_column))
                      QApplication::clipboard()->setText(now->text());
                  })
      ->setEnabled(has_value);
  menu->addSeparator();

  QAction* const add_note = menu->addAction(
      tr("Add Note"), this, [this, address] { EditLabel(EditSymbolType::AddNote, address); });
  QAction* const edit_note = menu->addAction(
      tr("Edit Note"), this, [this, address] { EditLabel(EditSymbolType::EditNote, address); });
  menu->addAction(tr("Add or edit region label"), this,
                  [this, address] { EditLabel(EditSymbolType::EditRegion, address); });

  // Notes can be edited anywhere inside them, but a new one cannot start where one already does.
  const Common::Note* const note = m_symbol_db.GetNoteFromAddr(address);
  edit_note->setEnabled(note != nullptr);
  if (note != nullptr && note->address == address)
    add_note->setEnabled(false);
  menu->addSeparator();

  menu->addAction(tr("Show in code"), this, [this, address] { emit ShowCode(address); });
  menu->addSeparator();

  menu->addAction(tr("Add to watch"), this, [this, address] {
    emit RequestWatch(QStringLiteral("mem_%1").arg(address, 8, 16, QLatin1Char('0')), address);
  });
  menu->addAction(tr("Toggle Breakpoint"), this,
                  [this, address] { ToggleBreakpoint(address, false); });

  menu->exec(QCursor::pos());
}

void MemoryViewWidget::WriteTypedText(int row, int column, const QString& text)
{
  const QTableWidgetItem* const cell = m_grid->item(row, column);
  const Type type = static_cast<Type>(cell->data(ROLE_VALUE_TYPE).toInt());
  const std::vector<u8> bytes = ConvertTextToBytes(type, text);
  u32 address = cell->data(ROLE_ADDRESS).toUInt();
  const u32 last = address + static_cast<u32>(bytes.size()) - 1;

  AddressSpace::Accessors* const memory = AddressSpace::GetAccessors(m_address_space);
  {
    // Written only if both ends are mapped.
    const Core::CPUThreadGuard guard(m_system);
    if (!bytes.empty() && memory->IsValidAddress(guard, address) &&
        memory->IsValidAddress(guard, last))
    {
      for (const u8 byte : bytes)
        memory->WriteU8(guard, address++, byte);
    }
  }
  UpdateDispatcher(UpdateType::Values);
}

// Dragging moves by the distance dragged; a click on an arrow moves one row and a click on the
// trough one page. Clicks put the handle straight back in the middle.
void MemoryViewWidget::ScrollBarAction()
{
  const int moved = m_scroll->sliderPosition() - m_scroll->value();
  if (moved == 0)
    return;

  if (m_scroll->isSliderDown())
  {
    ScrollRows(moved);
    return;
  }

  ScrollRows(std::abs(moved) == 1 ? moved : (moved < 0 ? -1 : 1) * m_grid->rowCount());
  m_scroll->setSliderPosition(SCROLL_MIDDLE);
}

void MemoryViewWidget::RecentreScrollBar()
{
  m_scroll->setValue(SCROLL_MIDDLE);
}

void MemoryViewWidget::SetFocus() const
{
  m_grid->setFocus();
}
