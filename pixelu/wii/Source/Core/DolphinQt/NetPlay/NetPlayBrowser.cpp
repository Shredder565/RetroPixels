// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/NetPlayBrowser.h"

#include <array>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpacerItem>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "Common/Version.h"

#include "Core/Config/NetplaySettings.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

namespace
{
// QSettings keys, all under the "netplaybrowser/" group.
const QString KEY_GEOMETRY = QStringLiteral("netplaybrowser/geometry");
const QString KEY_REGION = QStringLiteral("netplaybrowser/region");
const QString KEY_NAME = QStringLiteral("netplaybrowser/name");
const QString KEY_GAME_ID = QStringLiteral("netplaybrowser/game_id");
const QString KEY_VISIBILITY = QStringLiteral("netplaybrowser/visibility");
const QString KEY_HIDE_INCOMPATIBLE = QStringLiteral("netplaybrowser/hide_incompatible");
const QString KEY_HIDE_INGAME = QStringLiteral("netplaybrowser/hide_ingame");

constexpr int COLUMN_COUNT = 7;

// The browser can only join sessions running exactly the same build.
bool IsJoinable(const NetPlaySession& session)
{
  return Common::GetScmDescStr() == session.version;
}
}  // namespace

NetPlayBrowser::NetPlayBrowser(QWidget* owner) : QDialog(owner)
{
  setWindowTitle(tr("NetPlay Session Browser"));

  // Saved filters are applied before the change signals are connected, so restoring them does
  // not fire a refresh per field.
  CreateWidgets();
  RestoreSettings();
  ConnectWidgets();

  resize(750, 500);

  // Plain rows: no row numbers, alternating background.
  m_table_widget->verticalHeader()->setHidden(true);
  m_table_widget->setAlternatingRowColors(true);

  // Start the worker before the first query is queued.
  m_refresh_run.Set(true);
  m_refresh_thread = std::thread([this] { RefreshLoop(); });

  UpdateList();
  Refresh();
}

NetPlayBrowser::~NetPlayBrowser()
{
  // Wake the worker so it notices it should stop, then wait for it.
  m_refresh_run.Set(false);
  m_refresh_event.Set();
  if (m_refresh_thread.joinable())
    m_refresh_thread.join();

  SaveSettings();
}

QWidget* NetPlayBrowser::CreateFilterBox()
{
  m_region_combo = new QComboBox;
  m_region_combo->addItem(tr("Any Region"));
  for (const auto& [code, region_name] : NetPlayIndex::GetRegions())
  {
    const QString code_text = QString::fromStdString(code);
    m_region_combo->addItem(tr("%1 (%2)").arg(tr(region_name.c_str())).arg(code_text), code_text);
  }
  m_region_combo->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

  m_edit_name = new QLineEdit;
  m_edit_game_id = new QLineEdit;
  m_check_hide_incompatible = new QCheckBox(tr("Hide Incompatible Sessions"));
  m_check_hide_ingame = new QCheckBox(tr("Hide In-Game Sessions"));
  m_check_hide_incompatible->setChecked(true);

  m_radio_all = new QRadioButton(tr("Private and Public"));
  m_radio_private = new QRadioButton(tr("Private"));
  m_radio_public = new QRadioButton(tr("Public"));
  m_radio_all->setChecked(true);

  auto* const box = new QGroupBox(tr("Filters"));
  auto* const grid = new QGridLayout;
  box->setLayout(grid);

  // Labels sit in column 0; each field fills the rest of its row.
  int row = 0;
  for (const auto& [caption, field] :
       std::array<std::pair<QString, QWidget*>, 3>{{{tr("Region:"), m_region_combo},
                                                    {tr("Name:"), m_edit_name},
                                                    {tr("Game ID:"), m_edit_game_id}}})
  {
    grid->addWidget(new QLabel(caption), row, 0);
    grid->addWidget(field, row, 1, 1, -1);
    ++row;
  }

  grid->addWidget(m_radio_all, row, 1);
  grid->addWidget(m_radio_public, row, 2);
  grid->addWidget(m_radio_private, row, 3);
  grid->addItem(new QSpacerItem(4, 1, QSizePolicy::Expanding), row, 4);
  ++row;
  grid->addWidget(m_check_hide_incompatible, row++, 1, 1, -1);
  grid->addWidget(m_check_hide_ingame, row, 1, 1, -1);

  return box;
}

void NetPlayBrowser::CreateWidgets()
{
  auto* const main_layout = new QVBoxLayout;

  m_table_widget = new QTableWidget;
  m_table_widget->setTabKeyNavigation(false);
  m_table_widget->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table_widget->setSelectionMode(QAbstractItemView::SingleSelection);
  m_table_widget->setWordWrap(false);

  QWidget* const filters = CreateFilterBox();

  m_status_label = new QLabel;
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  m_button_refresh = new NonDefaultQPushButton(tr("Refresh"));

  main_layout->addWidget(m_table_widget);
  main_layout->addWidget(filters);
  main_layout->addWidget(m_status_label);
  main_layout->addWidget(m_button_box);

  m_button_box->addButton(m_button_refresh, QDialogButtonBox::ResetRole);
  // Enabled once a joinable row is selected.
  m_button_box->button(QDialogButtonBox::Ok)->setEnabled(false);

  setLayout(main_layout);
}

void NetPlayBrowser::ConnectWidgets()
{
  connect(m_region_combo, &QComboBox::currentIndexChanged, this, &NetPlayBrowser::Refresh);

  connect(m_button_box, &QDialogButtonBox::accepted, this, &NetPlayBrowser::accept);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &NetPlayBrowser::reject);
  connect(m_button_refresh, &QPushButton::clicked, this, &NetPlayBrowser::Refresh);

  // Toggling "all" or "private" covers every change of the visibility group, since one of the
  // two always flips when the selection moves.
  for (QAbstractButton* const toggle :
       {static_cast<QAbstractButton*>(m_radio_all), static_cast<QAbstractButton*>(m_radio_private),
        static_cast<QAbstractButton*>(m_check_hide_incompatible),
        static_cast<QAbstractButton*>(m_check_hide_ingame)})
  {
    connect(toggle, &QAbstractButton::toggled, this, &NetPlayBrowser::Refresh);
  }

  connect(m_edit_name, &QLineEdit::textChanged, this, &NetPlayBrowser::Refresh);
  connect(m_edit_game_id, &QLineEdit::textChanged, this, &NetPlayBrowser::Refresh);

  connect(m_table_widget, &QTableWidget::itemSelectionChanged, this,
          &NetPlayBrowser::OnSelectionChanged);
  connect(m_table_widget, &QTableWidget::itemDoubleClicked, this, &NetPlayBrowser::accept);

  // The worker emits these; queue them so the handlers run on the GUI thread.
  connect(this, &NetPlayBrowser::UpdateStatusRequested, this,
          &NetPlayBrowser::OnUpdateStatusRequested, Qt::QueuedConnection);
  connect(this, &NetPlayBrowser::UpdateListRequested, this, &NetPlayBrowser::OnUpdateListRequested,
          Qt::QueuedConnection);
}

NetPlayBrowser::FilterMap NetPlayBrowser::CurrentFilters() const
{
  FilterMap filters;

  if (m_check_hide_incompatible->isChecked())
    filters["version"] = Common::GetScmDescStr();

  const QString name = m_edit_name->text();
  if (!name.isEmpty())
    filters["name"] = name.toStdString();

  const QString game = m_edit_game_id->text();
  if (!game.isEmpty())
    filters["game"] = game.toStdString();

  // "1" asks for password-protected sessions only, "0" for open ones only.
  if (!m_radio_all->isChecked())
    filters["password"] = m_radio_private->isChecked() ? "1" : "0";

  if (m_region_combo->currentIndex() != 0)
    filters["region"] = m_region_combo->currentData().toString().toStdString();

  if (m_check_hide_ingame->isChecked())
    filters["in_game"] = "0";

  return filters;
}

void NetPlayBrowser::Refresh()
{
  FilterMap filters = CurrentFilters();

  // Only the newest request matters; an unhandled older one is simply replaced.
  std::unique_lock<std::mutex> guard(m_refresh_filters_mutex);
  m_refresh_filters = std::move(filters);
  m_refresh_event.Set();
}

void NetPlayBrowser::RefreshLoop()
{
  while (m_refresh_run.IsSet())
  {
    m_refresh_event.Wait();

    std::unique_lock<std::mutex> guard(m_refresh_filters_mutex);
    if (!m_refresh_filters)
      continue;

    FilterMap request = std::move(*m_refresh_filters);
    m_refresh_filters.reset();
    guard.unlock();

    emit UpdateStatusRequested(tr("Refreshing..."));

    NetPlayIndex index;
    auto found = index.List(request);
    if (found)
    {
      emit UpdateListRequested(std::move(*found));
    }
    else
    {
      emit UpdateStatusRequested(
          tr("Error obtaining session list: %1").arg(QString::fromStdString(index.GetLastError())));
    }
  }
}

void NetPlayBrowser::UpdateList()
{
  const int session_count = static_cast<int>(m_sessions.size());

  m_table_widget->clear();
  m_table_widget->setColumnCount(COLUMN_COUNT);
  m_table_widget->setHorizontalHeaderLabels({tr("Region"), tr("Name"), tr("Password?"),
                                             tr("In-Game?"), tr("Game"), tr("Players"),
                                             tr("Version")});

  QHeaderView* const header = m_table_widget->horizontalHeader();
  // Name and game take the spare width; the short columns hug their contents.
  static constexpr std::array<QHeaderView::ResizeMode, 5> modes{
      QHeaderView::ResizeToContents, QHeaderView::Stretch, QHeaderView::ResizeToContents,
      QHeaderView::ResizeToContents, QHeaderView::Stretch};
  for (int column = 0; column < static_cast<int>(modes.size()); ++column)
    header->setSectionResizeMode(column, modes[static_cast<size_t>(column)]);
  header->setHighlightSections(false);

  m_table_widget->setRowCount(session_count);

  for (int row = 0; row < session_count; ++row)
  {
    const NetPlaySession& session = m_sessions[static_cast<size_t>(row)];

    const std::array<QTableWidgetItem*, COLUMN_COUNT> cells{
        new QTableWidgetItem(QString::fromStdString(session.region)),
        new QTableWidgetItem(QString::fromStdString(session.name)),
        new QTableWidgetItem(session.has_password ? tr("Yes") : tr("No")),
        new QTableWidgetItem(session.in_game ? tr("Yes") : tr("No")),
        new QTableWidgetItem(QString::fromStdString(session.game_id)),
        new QTableWidgetItem(QStringLiteral("%1").arg(session.player_count)),
        new QTableWidgetItem(QString::fromStdString(session.version)),
    };

    // Rows from other builds stay visible but cannot be picked.
    const Qt::ItemFlags flags =
        IsJoinable(session) ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    for (QTableWidgetItem* const cell : cells)
      cell->setFlags(flags);

    for (int column = 0; column < COLUMN_COUNT; ++column)
      m_table_widget->setItem(row, column, cells[static_cast<size_t>(column)]);
  }

  const QString found_text = session_count == 1 ? tr("%1 session found") : tr("%1 sessions found");
  m_status_label->setText(found_text.arg(session_count));
}

void NetPlayBrowser::OnSelectionChanged()
{
  const bool has_selection = !m_table_widget->selectedItems().isEmpty();
  m_button_box->button(QDialogButtonBox::Ok)->setEnabled(has_selection);
}

void NetPlayBrowser::OnUpdateStatusRequested(const QString& status)
{
  m_status_label->setText(status);
}

void NetPlayBrowser::OnUpdateListRequested(std::vector<NetPlaySession> sessions)
{
  m_sessions = std::move(sessions);
  UpdateList();
}

void NetPlayBrowser::accept()
{
  const auto selection = m_table_widget->selectedItems();
  if (selection.isEmpty())
    return;

  const NetPlaySession& session = m_sessions[static_cast<size_t>(selection[0]->row())];
  std::string server_id = session.server_id;

  // Protected sessions publish a scrambled id that only the right password restores.
  if (session.has_password)
  {
    QInputDialog prompt(this);
    prompt.setWindowTitle(tr("Enter password"));
    prompt.setLabelText(tr("This session requires a password:"));
    prompt.setWindowModality(Qt::WindowModal);
    prompt.setTextEchoMode(QLineEdit::Password);

    if (prompt.exec() != QDialog::Accepted)
      return;

    const std::string password = prompt.textValue().toStdString();
    const std::optional<std::string> real_id = session.DecryptID(password);
    if (!real_id)
    {
      ModalMessageBox::warning(this, tr("Error"), tr("Invalid password provided."));
      return;
    }
    server_id = *real_id;
  }

  QDialog::accept();

  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, session.method);
  Config::SetBaseOrCurrent(Config::NETPLAY_CONNECT_PORT, session.port);

  // Traversal sessions are reached by host code, direct ones by address.
  const auto& target_setting =
      session.method == "traversal" ? Config::NETPLAY_HOST_CODE : Config::NETPLAY_ADDRESS;
  Config::SetBaseOrCurrent(target_setting, server_id);

  emit Join();
}

void NetPlayBrowser::SaveSettings() const
{
  auto& settings = Settings::Instance().GetQSettings();

  settings.setValue(KEY_GEOMETRY, saveGeometry());
  settings.setValue(KEY_REGION, m_region_combo->currentText());
  settings.setValue(KEY_NAME, m_edit_name->text());
  settings.setValue(KEY_GAME_ID, m_edit_game_id->text());

  QString visibility = QStringLiteral("all");
  if (m_radio_public->isChecked())
    visibility = QStringLiteral("public");
  else if (m_radio_private->isChecked())
    visibility = QStringLiteral("private");
  settings.setValue(KEY_VISIBILITY, visibility);

  settings.setValue(KEY_HIDE_INCOMPATIBLE, m_check_hide_incompatible->isChecked());
  settings.setValue(KEY_HIDE_INGAME, m_check_hide_ingame->isChecked());
}

void NetPlayBrowser::RestoreSettings()
{
  const auto& settings = Settings::Instance().GetQSettings();

  const QByteArray geometry = settings.value(KEY_GEOMETRY).toByteArray();
  if (!geometry.isEmpty())
    restoreGeometry(geometry);

  // A region saved by another build may no longer exist; keep the default then.
  const QString region = settings.value(KEY_REGION).toString();
  if (m_region_combo->findText(region) != -1)
    m_region_combo->setCurrentText(region);

  m_edit_name->setText(settings.value(KEY_NAME).toString());
  m_edit_game_id->setText(settings.value(KEY_GAME_ID).toString());

  const QString visibility = settings.value(KEY_VISIBILITY).toString();
  if (visibility == QStringLiteral("public"))
    m_radio_public->setChecked(true);
  else if (visibility == QStringLiteral("private"))
    m_radio_private->setChecked(true);

  m_check_hide_incompatible->setChecked(settings.value(KEY_HIDE_INCOMPATIBLE, true).toBool());
  m_check_hide_ingame->setChecked(settings.value(KEY_HIDE_INGAME).toBool());
}
