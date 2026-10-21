// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/CheatSearchWidget.h"

#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QString>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Core/ActionReplay.h"
#include "Core/CheatGeneration.h"
#include "Core/CheatSearch.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "DolphinQt/Config/CheatCodeEditor.h"
#include "DolphinQt/Config/CheatWarningWidget.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"

Q_DECLARE_METATYPE(Cheats::CompareType);
Q_DECLARE_METATYPE(Cheats::FilterType);

namespace
{
// Only this many results are put in the table; the search itself keeps all of them.
constexpr size_t MAX_SHOWN_ROWS = 1000;

// Every cell of a row carries its address and its index in the search results.
constexpr int ROLE_ADDRESS = Qt::UserRole;
constexpr int ROLE_RESULT_INDEX = Qt::UserRole + 1;

enum TableColumn : int
{
  COL_DESCRIPTION = 0,
  COL_ADDRESS = 1,
  COL_LAST_VALUE = 2,
  COL_CURRENT_VALUE = 3,
  COL_COUNT = 4,
};

const QString PARSE_HEX_KEY = QStringLiteral("cheatsearchwidget/parsehex");
const QString SHOW_HEX_KEY = QStringLiteral("cheatsearchwidget/displayhex");
const QString AUTO_REFRESH_KEY = QStringLiteral("cheatsearchwidget/autoupdatecurrentvalues");

u32 AddressOf(const QTableWidgetItem* item)
{
  return item->data(ROLE_ADDRESS).toUInt();
}

u32 ResultIndexOf(const QTableWidgetItem* item)
{
  return item->data(ROLE_RESULT_INDEX).toUInt();
}

int ValueSize(Cheats::DataType type)
{
  switch (type)
  {
  case Cheats::DataType::U16:
  case Cheats::DataType::S16:
    return 2;
  case Cheats::DataType::U32:
  case Cheats::DataType::S32:
  case Cheats::DataType::F32:
    return 4;
  case Cheats::DataType::U64:
  case Cheats::DataType::S64:
  case Cheats::DataType::F64:
    return 8;
  default:
    return 1;
  }
}

QString Hex32(u64 value)
{
  return QString::fromStdString(fmt::format("0x{:08x}", value));
}

// First and last address of a range, as text.
std::pair<QString, QString> RangeBounds(const Cheats::MemoryRange& range)
{
  return {Hex32(range.m_start), Hex32(range.m_start + range.m_length - 1)};
}
}  // namespace

CheatSearchWidget::CheatSearchWidget(Core::System& system,
                                     std::unique_ptr<Cheats::CheatSearchSessionBase> session,
                                     QWidget* parent)
    : QWidget(parent), m_system(system), m_session(std::move(session))
{
  setAttribute(Qt::WA_DeleteOnClose);
  BuildLayout();
  ConnectSignals();
  OnFilterTypeChanged();
  RebuildTable();
}

CheatSearchWidget::~CheatSearchWidget()
{
  QSettings& settings = Settings::GetQSettings();
  settings.setValue(SHOW_HEX_KEY, m_show_hex->isChecked());
  settings.setValue(AUTO_REFRESH_KEY, m_auto_refresh->isChecked());
  // The hex parsing box is only offered for integer searches.
  if (m_session->IsIntegerType())
    settings.setValue(PARSE_HEX_KEY, m_parse_hex->isChecked());
}

QString CheatSearchWidget::DescribeSession() const
{
  QString ranges;
  const size_t range_count = m_session->GetMemoryRangeCount();
  if (range_count == 1)
  {
    const auto [start, last] = RangeBounds(m_session->GetMemoryRange(0));
    ranges = tr("[%1, %2]").arg(start).arg(last);
  }
  else if (range_count == 2)
  {
    const auto [start0, last0] = RangeBounds(m_session->GetMemoryRange(0));
    const auto [start1, last1] = RangeBounds(m_session->GetMemoryRange(1));
    ranges = tr("[%1, %2] and [%3, %4]").arg(start0).arg(last0).arg(start1).arg(last1);
  }
  else
  {
    ranges = tr("%1 memory ranges").arg(range_count);
  }

  QString space;
  switch (m_session->GetAddressSpace())
  {
  case PowerPC::RequestedAddressSpace::Effective:
    space = tr("Address space by CPU state");
    break;
  case PowerPC::RequestedAddressSpace::Physical:
    space = tr("Physical address space");
    break;
  case PowerPC::RequestedAddressSpace::Virtual:
    space = tr("Virtual address space");
    break;
  default:
    space = tr("Unknown address space");
    break;
  }

  QString type;
  switch (m_session->GetDataType())
  {
  case Cheats::DataType::U8:
    type = tr("8-bit Unsigned Integer");
    break;
  case Cheats::DataType::U16:
    type = tr("16-bit Unsigned Integer");
    break;
  case Cheats::DataType::U32:
    type = tr("32-bit Unsigned Integer");
    break;
  case Cheats::DataType::U64:
    type = tr("64-bit Unsigned Integer");
    break;
  case Cheats::DataType::S8:
    type = tr("8-bit Signed Integer");
    break;
  case Cheats::DataType::S16:
    type = tr("16-bit Signed Integer");
    break;
  case Cheats::DataType::S32:
    type = tr("32-bit Signed Integer");
    break;
  case Cheats::DataType::S64:
    type = tr("64-bit Signed Integer");
    break;
  case Cheats::DataType::F32:
    type = tr("32-bit Float");
    break;
  case Cheats::DataType::F64:
    type = tr("64-bit Float");
    break;
  default:
    type = tr("Unknown data type");
    break;
  }

  const QString alignment = m_session->GetAligned() ? tr("aligned") : tr("unaligned");
  return tr("%1, %2, %3, %4").arg(ranges).arg(space).arg(type).arg(alignment);
}

void CheatSearchWidget::BuildLayout()
{
  auto* const session_label = new QLabel();
  session_label->setText(DescribeSession());
  session_label->setWordWrap(true);

  // i18n: This label is followed by a dropdown where the user can select things like "is equal to"
  // or "is less than or equal to", followed by another dropdown where the user can select "any
  // value", "last value", or "this value:". These three UI elements are intended to form a sentence
  // together. Because the UI elements can't be reordered by a translation, you may have to give
  // up on the idea of having them form a sentence depending on the grammar of your target language.
  auto* const sentence_label = new QLabel(tr("Keep addresses where value in memory"));

  // The filter row reads as a sentence: "<compare> <filter> [value] [Parse as Hex]".
  auto* const filter_row = new QHBoxLayout();
  m_compare_type = new QComboBox();
  const std::pair<QString, Cheats::CompareType> comparisons[] = {
      {tr("is equal to"), Cheats::CompareType::Equal},
      {tr("is not equal to"), Cheats::CompareType::NotEqual},
      {tr("is less than"), Cheats::CompareType::Less},
      {tr("is less than or equal to"), Cheats::CompareType::LessOrEqual},
      {tr("is greater than"), Cheats::CompareType::Greater},
      {tr("is greater than or equal to"), Cheats::CompareType::GreaterOrEqual},
  };
  for (const auto& [text, compare] : comparisons)
    m_compare_type->addItem(text, QVariant::fromValue(compare));
  filter_row->addWidget(m_compare_type);

  m_filter_type = new QComboBox();
  const std::pair<QString, Cheats::FilterType> filters[] = {
      {tr("this value:"), Cheats::FilterType::CompareAgainstSpecificValue},
      {tr("last value"), Cheats::FilterType::CompareAgainstLastValue},
      {tr("any value"), Cheats::FilterType::DoNotFilter},
  };
  for (const auto& [text, filter] : filters)
    m_filter_type->addItem(text, QVariant::fromValue(filter));
  filter_row->addWidget(m_filter_type);

  m_value = new QLineEdit();
  filter_row->addWidget(m_value);

  const QSettings& settings = Settings::GetQSettings();

  // Only shown (and so only parented) for integer searches.
  m_parse_hex = new QCheckBox(tr("Parse as Hex"));
  if (m_session->IsIntegerType())
  {
    m_parse_hex->setChecked(settings.value(PARSE_HEX_KEY).toBool());
    filter_row->addWidget(m_parse_hex);
  }

  auto* const button_row = new QHBoxLayout();
  m_scan_button = new QPushButton(tr("Search and Filter"));
  m_refresh_button = new QPushButton(tr("Refresh Current Values"));
  m_reset_button = new QPushButton(tr("Reset Results"));
  for (QPushButton* const button : {m_scan_button, m_refresh_button, m_reset_button})
    button_row->addWidget(button);

  m_table = new QTableWidget();
  m_table->setContextMenuPolicy(Qt::CustomContextMenu);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);

  m_status = new QLabel(tr("Waiting for first scan..."));
  m_counts = new QLabel();

  auto* const option_row = new QHBoxLayout();
  m_show_hex = new QCheckBox(tr("Display values in Hex"));
  m_show_hex->setChecked(settings.value(SHOW_HEX_KEY).toBool());
  option_row->addWidget(m_show_hex);
  option_row->setStretchFactor(m_show_hex, 1);

  m_auto_refresh = new QCheckBox(tr("Automatically update Current Values"));
  m_auto_refresh->setChecked(settings.value(AUTO_REFRESH_KEY, true).toBool());
  option_row->addWidget(m_auto_refresh);
  option_row->setStretchFactor(m_auto_refresh, 2);

  auto* const layout = new QVBoxLayout{this};
  layout->addWidget(session_label);
  layout->addWidget(sentence_label);
  layout->addLayout(filter_row);
  layout->addLayout(button_row);
  layout->addLayout(option_row);
  layout->addWidget(m_status);
  layout->addWidget(m_counts);
  layout->addWidget(m_table);
}

void CheatSearchWidget::ConnectSignals()
{
  connect(m_scan_button, &QPushButton::clicked, this, &CheatSearchWidget::RunScan);
  connect(m_refresh_button, &QPushButton::clicked, this,
          [this] { UpdateTableAllCurrentValues(UpdateSource::User); });
  connect(m_reset_button, &QPushButton::clicked, this, &CheatSearchWidget::ResetResults);
  connect(m_table, &QTableWidget::itemChanged, this, &CheatSearchWidget::OnItemEdited);
  connect(m_table, &QTableWidget::customContextMenuRequested, this,
          &CheatSearchWidget::ShowResultsMenu);
  connect(m_filter_type, &QComboBox::currentTextChanged, this,
          &CheatSearchWidget::OnFilterTypeChanged);
  connect(m_show_hex, &QCheckBox::toggled, this, &CheatSearchWidget::OnHexDisplayToggled);
}

void CheatSearchWidget::RunScan()
{
  Core::CPUThreadGuard guard{m_system};

  const bool had_results = m_session->WasFirstSearchDone();
  const auto filter = m_filter_type->currentData().value<Cheats::FilterType>();
  if (filter == Cheats::FilterType::CompareAgainstLastValue && !had_results)
  {
    m_status->setText(tr("Cannot compare against last value on first search."));
    return;
  }

  m_session->SetFilterType(filter);
  m_session->SetCompareType(m_compare_type->currentData().value<Cheats::CompareType>());

  if (filter == Cheats::FilterType::CompareAgainstSpecificValue)
  {
    QString text = m_value->text();
    // Numbers may be typed with spaces between digit groups.
    if (m_session->IsIntegerType() || m_session->IsFloatingType())
      text = text.simplified().remove(QLatin1Char(' '));
    if (!m_session->SetValueFromString(text.toStdString(), m_parse_hex->isChecked()))
    {
      m_status->setText(tr("Failed to parse given value into target data type."));
      return;
    }
  }

  const size_t count_before = m_session->GetResultCount();
  const Cheats::SearchErrorCode result = m_session->RunSearch(guard);

  switch (result)
  {
  case Cheats::SearchErrorCode::Success:
    break;
  case Cheats::SearchErrorCode::NoEmulationActive:
    m_status->setText(tr("No game is running."));
    return;
  case Cheats::SearchErrorCode::InvalidParameters:
    m_status->setText(tr("Invalid parameters given to search."));
    return;
  case Cheats::SearchErrorCode::VirtualAddressesCurrentlyNotAccessible:
    m_status->setText(tr("Search currently not possible in virtual address space. Please run "
                         "the game for a bit and try again."));
    return;
  default:
    m_status->setText(tr("Unknown error occurred."));
    return;
  }

  m_status->setText(tr("Scan succeeded."));
  ReportScanCounts(had_results, count_before, m_session->GetResultCount(),
                   m_session->GetValidValueCount());

  // The values just read become the current values too.
  m_current_values.clear();
  const bool hex = m_show_hex->isChecked();
  const size_t shown = ShownRowCount();
  for (size_t i = 0; i < shown; ++i)
    m_current_values[m_session->GetResultAddress(i)] = m_session->GetResultValueAsString(i, hex);

  RebuildTable();
}

void CheatSearchWidget::ReportScanCounts(bool had_results, size_t before, size_t after,
                                         size_t accessible)
{
  const bool all_accessible = accessible == after;
  const auto inaccessible_text = [&] {
    return tr("%n address(es) could not be accessed in emulated memory.", "",
              static_cast<int>(after - accessible));
  };

  if (!had_results)
  {
    const QString found = tr("Found %n address(es).", "", static_cast<int>(after));
    m_counts->setText(all_accessible ? found : tr("%1 %2").arg(found).arg(inaccessible_text()));
    return;
  }

  const QString removed = tr("%n address(es) were removed.", "", static_cast<int>(before - after));
  const QString remaining = tr("%n address(es) remain.", "", static_cast<int>(after));
  if (all_accessible)
    m_counts->setText(tr("%1 %2").arg(removed).arg(remaining));
  else
    m_counts->setText(tr("%1 %2 %3").arg(removed).arg(remaining).arg(inaccessible_text()));
}

bool CheatSearchWidget::RefreshRows(const Core::CPUThreadGuard& guard, size_t begin, size_t end,
                                    UpdateSource source)
{
  const bool by_user = source == UpdateSource::User;

  // Read the values again through an unfiltered copy of just these results.
  const auto reread = m_session->ClonePartial(begin, end);
  reread->SetFilterType(Cheats::FilterType::DoNotFilter);
  if (reread->RunSearch(guard) != Cheats::SearchErrorCode::Success)
  {
    if (by_user)
      m_status->setText(tr("Refresh failed. Please run the game for a bit and try again."));
    return false;
  }

  const bool hex = m_show_hex->isChecked();
  const size_t count = reread->GetResultCount();
  for (size_t i = 0; i < count; ++i)
    m_current_values[reread->GetResultAddress(i)] = reread->GetResultValueAsString(i, hex);

  ShowCurrentValues(begin, end);
  if (by_user)
    m_status->setText(tr("Refreshed current values."));
  return true;
}

void CheatSearchWidget::UpdateTableVisibleCurrentValues(UpdateSource source)
{
  if (source == UpdateSource::Auto && !m_auto_refresh->isChecked())
    return;
  if (m_table->rowCount() == 0)
    return;

  const auto begin = static_cast<size_t>(FirstVisibleRow());
  const auto end = static_cast<size_t>(EndOfVisibleRows());
  RefreshRows(Core::CPUThreadGuard{m_system}, begin, end, source);
}

bool CheatSearchWidget::UpdateTableAllCurrentValues(UpdateSource source)
{
  if (source == UpdateSource::Auto && !m_auto_refresh->isChecked())
    return false;

  const auto rows = static_cast<size_t>(m_table->rowCount());
  if (rows == 0)
  {
    if (source == UpdateSource::User)
      m_status->setText(tr("Cannot refresh without results."));
    return false;
  }

  return RefreshRows(Core::CPUThreadGuard{m_system}, 0, rows, source);
}

void CheatSearchWidget::ResetResults()
{
  m_session->ResetResults();
  m_current_values.clear();

  RebuildTable();
  m_status->setText(tr("Waiting for first scan..."));
  m_counts->clear();
}

void CheatSearchWidget::OnItemEdited(QTableWidgetItem* item)
{
  // Only descriptions can be edited.
  const u32 address = AddressOf(item);
  if (item->column() == COL_DESCRIPTION)
    m_user_data[address].m_description = item->text().toStdString();
}

int CheatSearchWidget::FirstVisibleRow() const
{
  return m_table->rowAt(0);
}

int CheatSearchWidget::EndOfVisibleRows() const
{
  const int bottom_row = m_table->rowAt(m_table->height());
  if (bottom_row == -1)
    return m_table->rowCount();
  return bottom_row + 1;
}

void CheatSearchWidget::ShowResultsMenu()
{
  if (m_table->selectedItems().isEmpty())
    return;

  const std::vector<const QTableWidgetItem*> rows = SelectedRowItems();

  auto* const menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose, true);

  menu->addAction(tr("Show in memory"), this,
                  [this] { emit ShowMemory(AddressOf(m_table->selectedItems()[0])); });
  menu->addAction(tr("Add to watch"), this, [this, rows] {
    for (const QTableWidgetItem* const row : rows)
    {
      const u32 address = AddressOf(row);
      emit RequestWatch(QStringLiteral("mem_%1").arg(address, 8, 16, QLatin1Char('0')), address);
    }
  });
  menu->addAction(tr("Generate Action Replay Code(s)"), this, &CheatSearchWidget::GenerateCodes);
  menu->addAction(tr("Write value"), this, &CheatSearchWidget::WriteToSelection);
  menu->addAction(tr("Delete Address"), this, [this, rows] {
    // From the back, so the indices of the rows still to go stay valid.
    for (const QTableWidgetItem* const row : rows | std::views::reverse)
      m_session->RemoveResult(ResultIndexOf(row));
    RebuildTable();
  });

  // Breakpoints work on virtual addresses only.
  if (m_session->GetAddressSpace() == PowerPC::RequestedAddressSpace::Virtual)
  {
    menu->addAction(tr("Add Read/Write Breakpoint(s)"), this, [this] { AddBreakpoints("rw"); });
    menu->addAction(tr("Add Write Breakpoint(s)"), this, [this] { AddBreakpoints("w"); });
    menu->addAction(tr("Add Read Breakpoint(s)"), this, [this] { AddBreakpoints("r"); });
  }

  menu->exec(QCursor::pos());
}

void CheatSearchWidget::AddBreakpoints(std::string_view kind)
{
  if (m_session->GetAddressSpace() != PowerPC::RequestedAddressSpace::Virtual)
    return;

  // Each breakpoint covers every byte of the value: "<first> <last> nbl<kind>".
  const auto last_byte = static_cast<u32>(ValueSize(m_session->GetDataType()) - 1);
  MemChecks::TMemChecksStr specs;
  for (const QTableWidgetItem* const row : SelectedRowItems())
  {
    const u32 first = AddressOf(row);
    specs.push_back(fmt::format("{:08x} {:08x} nbl{}", first, first + last_byte, kind));
  }

  m_system.GetPowerPC().GetMemChecks().AddFromStrings(specs);
  emit Host::GetInstance()->PPCBreakpointsChanged();
}

void CheatSearchWidget::OnFilterTypeChanged()
{
  const bool needs_value = m_filter_type->currentData().value<Cheats::FilterType>() ==
                           Cheats::FilterType::CompareAgainstSpecificValue;
  m_value->setEnabled(needs_value);
  m_parse_hex->setEnabled(needs_value && m_session->IsIntegerType());
}

void CheatSearchWidget::OnHexDisplayToggled()
{
  if (!m_session->WasFirstSearchDone())
    return;

  // While the game runs, the cheats manager refreshes the values every field anyway.
  if (Core::GetState(m_system) != Core::State::Running)
    UpdateTableAllCurrentValues(UpdateSource::User);
}

void CheatSearchWidget::GenerateCodes()
{
  if (m_table->selectedItems().isEmpty())
    return;

  bool generated_any = false;
  bool mixed_errors = false;
  std::optional<Cheats::GenerateActionReplayCodeErrorCode> first_error;

  for (const QTableWidgetItem* const row : SelectedRowItems())
  {
    const auto code = Cheats::GenerateActionReplayCode(*m_session, ResultIndexOf(row));
    if (code)
    {
      emit ActionReplayCodeGenerated(*code);
      generated_any = true;
      continue;
    }

    if (!first_error)
      first_error = code.error();
    else if (*first_error != code.error())
      mixed_errors = true;
  }

  if (!first_error)
  {
    if (generated_any)
      m_status->setText(tr("Generated AR code(s)."));
    return;
  }

  if (mixed_errors)
  {
    m_status->setText(tr("Multiple errors occurred while generating AR codes."));
    return;
  }

  switch (*first_error)
  {
  case Cheats::GenerateActionReplayCodeErrorCode::NotVirtualMemory:
    m_status->setText(tr("Can only generate AR code for values in virtual memory."));
    break;
  case Cheats::GenerateActionReplayCodeErrorCode::InvalidAddress:
    m_status->setText(tr("Cannot generate AR code for this address."));
    break;
  default:
    m_status->setText(tr("Internal error while generating AR code."));
    break;
  }
}

void CheatSearchWidget::WriteToSelection()
{
  if (m_table->selectedItems().isEmpty())
    return;

  bool accepted = false;
  const QString text = QInputDialog::getText(this, tr("Write value"), tr("Value:"),
                                             QLineEdit::Normal, QString{}, &accepted);
  if (!accepted || !m_session->SetValueFromString(text.toStdString(), m_parse_hex->isChecked()))
  {
    m_status->setText(tr("Invalid value."));
    return;
  }

  std::vector<u32> addresses;
  for (const QTableWidgetItem* const row : SelectedRowItems())
    addresses.push_back(AddressOf(row));

  // The refresh below runs while the CPU is still held.
  Core::CPUThreadGuard guard{m_system};
  if (!m_session->WriteValue(guard, std::span<u32>(addresses)))
    m_status->setText(tr("There was an error writing (some) values."));
  UpdateTableAllCurrentValues(UpdateSource::User);
}

size_t CheatSearchWidget::ShownRowCount() const
{
  return std::min(MAX_SHOWN_ROWS, m_session->GetResultCount());
}

void CheatSearchWidget::ShowCurrentValue(QTableWidgetItem* item)
{
  const auto known = m_current_values.find(AddressOf(item));
  if (known == m_current_values.end())
    item->setText(QStringLiteral("---"));
  else
    item->setText(QString::fromStdString(known->second));
}

void CheatSearchWidget::ShowCurrentValues(size_t begin, size_t end)
{
  for (size_t row = begin; row < end; ++row)
    ShowCurrentValue(m_table->item(static_cast<int>(row), COL_CURRENT_VALUE));
}

std::vector<const QTableWidgetItem*> CheatSearchWidget::SelectedRowItems() const
{
  // One item per selected row (all cells of a row hold the same data), top to bottom, so that a
  // row with several selected cells is not handled several times.
  QModelIndexList rows = m_table->selectionModel()->selectedRows();
  std::ranges::sort(rows, {}, &QModelIndex::row);

  std::vector<const QTableWidgetItem*> items;
  items.reserve(static_cast<size_t>(rows.size()));
  for (const QModelIndex& row : rows)
    items.push_back(m_table->item(row.row(), COL_DESCRIPTION));
  return items;
}

void CheatSearchWidget::RebuildTable()
{
  const QSignalBlocker no_edit_signals(m_table);

  m_table->clear();
  m_table->setColumnCount(COL_COUNT);
  m_table->setHorizontalHeaderLabels(
      {tr("Description"), tr("Address"), tr("Last Value"), tr("Current Value")});

  const int row_count = static_cast<int>(ShownRowCount());
  m_table->setRowCount(row_count);

  const Qt::ItemFlags read_only = Qt::ItemIsEnabled | Qt::ItemIsSelectable;

  for (int row = 0; row < row_count; ++row)
  {
    const auto result = static_cast<size_t>(row);
    const u32 address = m_session->GetResultAddress(result);

    const auto add_cell = [&](int column, Qt::ItemFlags flags, const QString* text) {
      auto* const cell = new QTableWidgetItem();
      cell->setFlags(flags);
      if (text)
        cell->setText(*text);
      cell->setData(ROLE_ADDRESS, address);
      cell->setData(ROLE_RESULT_INDEX, static_cast<u32>(row));
      m_table->setItem(row, column, cell);
      return cell;
    };

    const auto user = m_user_data.find(address);
    QString description;
    if (user != m_user_data.end())
      description = QString::fromStdString(user->second.m_description);
    add_cell(COL_DESCRIPTION, read_only | Qt::ItemIsEditable,
             user != m_user_data.end() ? &description : nullptr);

    const QString address_text = QStringLiteral("0x%1").arg(address, 8, 16, QLatin1Char('0'));
    add_cell(COL_ADDRESS, read_only, &address_text);

    const QString last_value = QString::fromStdString(
        m_session->GetResultValueAsString(result, m_show_hex->isChecked()));
    add_cell(COL_LAST_VALUE, read_only, &last_value);

    ShowCurrentValue(add_cell(COL_CURRENT_VALUE, read_only, nullptr));
  }
}
