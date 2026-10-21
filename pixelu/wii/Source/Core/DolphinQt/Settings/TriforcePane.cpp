// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/TriforcePane.h"

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/DVD/AMMediaboard.h"

#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/QtUtils.h"

namespace
{
// Table columns.
enum Column : int
{
  EMULATED_IP = 0,
  REAL_IP = 1,
  NOTE = 2,
};

// Editable list of "emulated=real description" redirections. The table always ends with a
// blank row; typing into it appends a new blank row, and clearing an address cell of any
// other row deletes that row. (No Q_OBJECT: translations use the QDialog context.)
class IPRedirectionsDialog final : public QDialog
{
public:
  explicit IPRedirectionsDialog(QWidget* parent);

private:
  void ResetToBlankRow();
  void ShowConfig();
  void StoreConfig() const;
  void RestoreDefaults();
  void OnCellEdited(QTableWidgetItem* item);

  QTableWidget* m_table;
};

IPRedirectionsDialog::IPRedirectionsDialog(QWidget* parent) : QDialog{parent}
{
  setWindowTitle(tr("IP Address Redirections"));
  auto* const column = new QVBoxLayout{this};

  auto* const defaults_button = new QPushButton(tr("Default"));
  connect(defaults_button, &QPushButton::clicked, this, &IPRedirectionsDialog::RestoreDefaults);
  auto* const clear_button = new QPushButton(tr("Clear"));
  connect(clear_button, &QPushButton::clicked, this, &IPRedirectionsDialog::ResetToBlankRow);

  m_table = new QTableWidget;
  column->addWidget(m_table);
  m_table->setColumnCount(3);
  m_table->setHorizontalHeaderLabels({tr("Emulated"), tr("Real"), tr("Description")});
  m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  m_table->setSizeAdjustPolicy(QTableWidget::AdjustToContents);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);

  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  column->addWidget(buttons);
  buttons->addButton(defaults_button, QDialogButtonBox::ActionRole);
  buttons->addButton(clear_button, QDialogButtonBox::ActionRole);

  connect(buttons, &QDialogButtonBox::accepted, this, &IPRedirectionsDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &IPRedirectionsDialog::reject);
  connect(this, &IPRedirectionsDialog::accepted, this, &IPRedirectionsDialog::StoreConfig);
  connect(m_table, &QTableWidget::itemChanged, this, &IPRedirectionsDialog::OnCellEdited);

  ShowConfig();

  QtUtils::AdjustSizeWithinScreen(this);
}

void IPRedirectionsDialog::OnCellEdited(QTableWidgetItem* item)
{
  const int rows = m_table->rowCount();
  const int row = item->row();

  if (row == rows - 1)
  {
    // Typing into the trailing blank row opens a new blank row below it.
    if (!item->text().isEmpty())
      m_table->insertRow(rows);
    return;
  }

  // An inner row whose address was erased is dropped (the note alone doesn't count).
  if (item->column() != NOTE && item->text().isEmpty())
    m_table->removeRow(row);
}

void IPRedirectionsDialog::ResetToBlankRow()
{
  m_table->setRowCount(0);
  m_table->insertRow(0);
}

void IPRedirectionsDialog::ShowConfig()
{
  ResetToBlankRow();

  // Entries are inserted in front of the blank row, which therefore stays last.
  int next_row = 0;
  const std::string stored = Config::Get(Config::MAIN_TRIFORCE_IP_REDIRECTIONS);
  for (auto&& piece : stored | std::views::split(','))
  {
    const std::string_view entry{piece};
    const auto parsed = AMMediaboard::ParseIPRedirection(entry);
    if (!parsed)
    {
      ERROR_LOG_FMT(COMMON, "Bad IP redirection string: {}", entry);
      continue;
    }

    m_table->insertRow(next_row);
    const std::string_view cells[] = {parsed->original, parsed->replacement, parsed->description};
    for (int cell = EMULATED_IP; cell <= NOTE; ++cell)
      m_table->setItem(next_row, cell, new QTableWidgetItem(QString::fromUtf8(cells[cell])));
    ++next_row;
  }
}

void IPRedirectionsDialog::StoreConfig() const
{
  // The trailing blank row is not saved.
  const int entry_count = m_table->rowCount() - 1;

  // Rows missing an address still occupy a (blank) slot in the list.
  std::vector<std::string> entries(static_cast<std::size_t>(entry_count));
  for (int row = 0; row < entry_count; ++row)
  {
    const QTableWidgetItem* const emulated = m_table->item(row, EMULATED_IP);
    const QTableWidgetItem* const real = m_table->item(row, REAL_IP);
    if (!emulated || !real)
      continue;

    std::string& entry = entries[static_cast<std::size_t>(row)];
    entry = fmt::format("{}={}", StripWhitespace(emulated->text().toStdString()),
                        real->text().toStdString());

    if (const QTableWidgetItem* const note = m_table->item(row, NOTE))
    {
      const std::string note_text = note->text().toStdString();
      if (!note_text.empty())
        entry.append(1, ' ').append(note_text);
    }
  }

  Config::SetBaseOrCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS,
                           fmt::format("{}", fmt::join(entries, ",")));
}

void IPRedirectionsDialog::RestoreDefaults()
{
  // Note: this writes the config immediately, without waiting for OK.
  Config::SetBaseOrCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS, Config::DefaultState{});
  ShowConfig();
}
}  // namespace

TriforcePane::TriforcePane()
{
  auto* const page = new QVBoxLayout{this};

  // Each section is a group box holding a single "Configure" button.
  const auto add_section = [page](const QString& title) {
    auto* const group = new QGroupBox{title};
    page->addWidget(group);
    auto* const group_layout = new QVBoxLayout{group};
    auto* const button = new NonDefaultQPushButton{tr("Configure")};
    group_layout->addWidget(button);
    return button;
  };

  auto* const controllers = add_section(tr("Controllers"));
  connect(controllers, &QPushButton::clicked, this, [this] {
    auto* const mapping = new MappingWindow(this, MappingWindow::Type::MAPPING_AM_BASEBOARD, 0);
    mapping->setAttribute(Qt::WA_DeleteOnClose, true);
    mapping->setWindowModality(Qt::WindowModality::WindowModal);
    mapping->show();
  });

  auto* const redirections = add_section(tr("IP Address Redirections"));
  connect(redirections, &QPushButton::clicked, this, [this] {
    auto* const dialog = new IPRedirectionsDialog{this};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
  });

  page->addStretch(1);
}
