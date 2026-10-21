// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/NetPlaySetupDialog.h"

#include <memory>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>

#include "Core/Config/NetplaySettings.h"
#include "Core/NetPlayProto.h"

#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/UTF8CodePointCountValidator.h"
#include "DolphinQt/Settings.h"

#include "UICommon/GameFile.h"
#include "UICommon/NetPlayIndex.h"

namespace
{
// Index of each entry in the connection type box.
constexpr int DIRECT_CONNECTION = 0;

// Tab order: joining comes first, hosting second.
constexpr int CONNECT_TAB = 0;

constexpr int MAX_PORT = 65535;

// Remembers which game was last picked for hosting.
const QString KEY_HOST_GAME = QStringLiteral("netplay/hostgame");

using GamePtr = std::shared_ptr<const UICommon::GameFile>;
}  // namespace

NetPlaySetupDialog::NetPlaySetupDialog(const GameListModel& game_list_model, QWidget* parent)
    : QDialog(parent), m_game_list_model(game_list_model)
{
  setWindowTitle(tr("NetPlay Setup"));

  CreateMainLayout();
  LoadSettings();

  // Apply the per-type visibility once before the change signal is hooked up.
  OnConnectionTypeChanged(m_connection_type->currentIndex());

  ConnectWidgets();
}

void NetPlaySetupDialog::LoadSettings()
{
  const bool listed = Config::Get(Config::NETPLAY_USE_INDEX);
  const std::string listed_region = Config::Get(Config::NETPLAY_INDEX_REGION);
  const std::string listed_name = Config::Get(Config::NETPLAY_INDEX_NAME);
  const std::string listed_password = Config::Get(Config::NETPLAY_INDEX_PASSWORD);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);
  const std::string traversal_choice = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE);
  const int connect_port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  const int host_port = Config::Get(Config::NETPLAY_HOST_PORT);
  const int listen_port = Config::Get(Config::NETPLAY_LISTEN_PORT);
  const bool limit_uploads = Config::Get(Config::NETPLAY_ENABLE_CHUNKED_UPLOAD_LIMIT);
  const u32 upload_limit = Config::Get(Config::NETPLAY_CHUNKED_UPLOAD_LIMIT);
#ifdef USE_UPNP
  m_host_upnp->setChecked(Config::Get(Config::NETPLAY_USE_UPNP));
#endif

  m_nickname_edit->setText(QString::fromStdString(nickname));
  m_connection_type->setCurrentIndex(traversal_choice == "direct" ? DIRECT_CONNECTION : 1);
  m_connect_port_box->setValue(connect_port);
  m_host_port_box->setValue(host_port);

  // The forced port is only editable while its checkbox is ticked, which starts out unticked.
  m_host_force_port_box->setValue(listen_port);
  m_host_force_port_box->setEnabled(false);

  m_host_server_browser->setChecked(listed);

  // The listing details only matter when the session is published.
  m_host_server_region->setEnabled(listed);
  m_host_server_region->setCurrentIndex(
      m_host_server_region->findData(QString::fromStdString(listed_region)));
  m_host_server_name->setEnabled(listed);
  m_host_server_name->setText(QString::fromStdString(listed_name));
  m_host_server_password->setEnabled(listed);
  m_host_server_password->setText(QString::fromStdString(listed_password));

  m_host_chunked_upload_limit_check->setChecked(limit_uploads);
  m_host_chunked_upload_limit_box->setValue(static_cast<int>(upload_limit));
  m_host_chunked_upload_limit_box->setEnabled(limit_uploads);
}

QWidget* NetPlaySetupDialog::CreateConnectTab()
{
  auto* const page = new QWidget;
  auto* const grid = new QGridLayout;

  m_ip_label = new QLabel;
  m_ip_edit = new QLineEdit;
  m_connect_port_label = new QLabel(tr("Port:"));
  m_connect_port_box = new QSpinBox;
  m_connect_button = new NonDefaultQPushButton(tr("Connect"));

  m_connect_port_box->setMaximum(MAX_PORT);

  grid->addWidget(m_ip_label, 0, 0);
  grid->addWidget(m_ip_edit, 0, 1);
  grid->addWidget(m_connect_port_label, 0, 2);
  grid->addWidget(m_connect_port_box, 0, 3);

  auto* const notice = new QLabel(
      tr("ALERT:\n\nAll players must use the same Dolphin version.\nIf enabled, SD cards must be "
         "identical between players.\nIf DSP LLE is used, DSP ROMs must be identical between "
         "players.\nIf a game is hanging on boot, it may not support Dual Core Netplay. Disable "
         "Dual Core.\nIf connecting directly, the host must have the chosen UDP port "
         "open/forwarded!\n\nWii Remote support in netplay is experimental and may not work "
         "correctly.\nUse at your own risk.\n"));
  // A fixed height leaves all of the spare vertical room to the spacer underneath.
  notice->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

  grid->addWidget(notice, 1, 0, 1, -1);
  grid->addItem(new QSpacerItem(1, 1), 2, 0, -1, -1);
  grid->addWidget(m_connect_button, 3, 3, Qt::AlignRight);

  page->setLayout(grid);
  return page;
}

QWidget* NetPlaySetupDialog::CreateHostTab()
{
  auto* const page = new QWidget;
  auto* const grid = new QGridLayout;

  m_host_port_label = new QLabel(tr("Port:"));
  m_host_port_box = new QSpinBox;
  m_host_force_port_check = new QCheckBox(tr("Force Listen Port:"));
  m_host_force_port_box = new QSpinBox;
  m_host_chunked_upload_limit_check = new QCheckBox(tr("Limit Chunked Upload Speed:"));
  m_host_chunked_upload_limit_box = new QSpinBox;
  m_host_server_browser = new QCheckBox(tr("Show in server browser"));
  m_host_server_name = new QLineEdit;
  m_host_server_password = new QLineEdit;
  m_host_server_region = new QComboBox;
#ifdef USE_UPNP
  m_host_upnp = new QCheckBox(tr("Forward port (UPnP)"));
#endif
  m_host_games = new QListWidget;
  m_host_button = new NonDefaultQPushButton(tr("Host"));

  m_host_port_box->setMaximum(MAX_PORT);
  m_host_force_port_box->setMaximum(MAX_PORT);
  m_host_chunked_upload_limit_box->setRange(1, 1000000);
  m_host_chunked_upload_limit_box->setSingleStep(100);
  m_host_chunked_upload_limit_box->setSuffix(QStringLiteral(" kbps"));

  m_host_chunked_upload_limit_check->setToolTip(tr("This will limit the speed of chunked "
                                                   "uploading per client, which is used for "
                                                   "save sync."));

  m_host_server_name->setToolTip(tr("Name of your session shown in the server browser"));
  m_host_server_name->setPlaceholderText(tr("Name"));
  m_host_server_password->setToolTip(tr("Password for joining your game (leave empty for none)"));
  m_host_server_password->setPlaceholderText(tr("Password"));

  for (const auto& [code, region_name] : NetPlayIndex::GetRegions())
  {
    const QString code_text = QString::fromStdString(code);
    m_host_server_region->addItem(tr("%1 (%2)").arg(tr(region_name.c_str())).arg(code_text),
                                  code_text);
  }

  // Row 0: port (and UPnP); row 1: listing options; row 2: games; rows 3-4: extras + button.
  grid->addWidget(m_host_port_label, 0, 0);
  grid->addWidget(m_host_port_box, 0, 1);
#ifdef USE_UPNP
  grid->addWidget(m_host_upnp, 0, 2);
#endif
  grid->addWidget(m_host_server_browser, 1, 0);
  grid->addWidget(m_host_server_region, 1, 1);
  grid->addWidget(m_host_server_name, 1, 2);
  grid->addWidget(m_host_server_password, 1, 3);
  grid->addWidget(m_host_games, 2, 0, 1, -1);
  grid->addWidget(m_host_force_port_check, 3, 0);
  grid->addWidget(m_host_force_port_box, 3, 1, Qt::AlignLeft);
  grid->addWidget(m_host_chunked_upload_limit_check, 4, 0);
  grid->addWidget(m_host_chunked_upload_limit_box, 4, 1, Qt::AlignLeft);
  grid->addWidget(m_host_button, 4, 3, 2, 1, Qt::AlignRight);

  page->setLayout(grid);
  return page;
}

void NetPlaySetupDialog::CreateMainLayout()
{
  m_main_layout = new QGridLayout;
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Cancel);
  m_nickname_edit = new QLineEdit;
  m_connection_type = new QComboBox;
  m_reset_traversal_button = new NonDefaultQPushButton(tr("Reset Traversal Settings"));
  m_tab_widget = new QTabWidget;

  // Nicknames are capped in code points, not bytes or UTF-16 units.
  m_nickname_edit->setValidator(
      new UTF8CodePointCountValidator(NetPlay::MAX_NAME_LENGTH, m_nickname_edit));

  QWidget* const connect_page = CreateConnectTab();
  QWidget* const host_page = CreateHostTab();

  // Index 0 is DIRECT_CONNECTION.
  for (const QString& kind : {tr("Direct Connection"), tr("Traversal Server")})
    m_connection_type->addItem(kind);

  QGridLayout* const grid = m_main_layout;
  // Row 0: how to connect (plus the traversal reset); row 1: who we are.
  grid->addWidget(new QLabel(tr("Connection Type:")), 0, 0);
  grid->addWidget(m_connection_type, 0, 1);
  grid->addWidget(m_reset_traversal_button, 0, 2);
  grid->addWidget(new QLabel(tr("Nickname:")), 1, 0);
  grid->addWidget(m_nickname_edit, 1, 1);
  // Then the tabs and the dialog buttons across the full width.
  grid->addWidget(m_tab_widget, 2, 0, 1, -1);
  grid->addWidget(m_button_box, 3, 0, 1, -1);

  m_tab_widget->addTab(connect_page, tr("Connect"));
  m_tab_widget->addTab(host_page, tr("Host"));

  setLayout(m_main_layout);
}

void NetPlaySetupDialog::ConnectWidgets()
{
  const auto save = &NetPlaySetupDialog::SaveSettings;

  connect(m_connection_type, &QComboBox::currentIndexChanged, this,
          &NetPlaySetupDialog::OnConnectionTypeChanged);
  connect(m_nickname_edit, &QLineEdit::textChanged, this, save);

  // "Connect" tab
  connect(m_ip_edit, &QLineEdit::textChanged, this, save);
  connect(m_connect_port_box, &QSpinBox::valueChanged, this, save);

  // "Host" tab
  connect(m_host_port_box, &QSpinBox::valueChanged, this, save);
  connect(m_host_games, &QListWidget::currentRowChanged, this, [this](int row) {
    Settings::GetQSettings().setValue(KEY_HOST_GAME, m_host_games->item(row)->text());
  });
  connect(m_host_games, &QListWidget::itemDoubleClicked, this, &NetPlaySetupDialog::accept);

  connect(m_host_force_port_check, &QCheckBox::toggled, this,
          [this](bool on) { m_host_force_port_box->setEnabled(on); });
  connect(m_host_chunked_upload_limit_check, &QCheckBox::toggled, this, [this](bool on) {
    m_host_chunked_upload_limit_box->setEnabled(on);
    SaveSettings();
  });
  connect(m_host_chunked_upload_limit_box, &QSpinBox::valueChanged, this, save);

  connect(m_host_server_browser, &QCheckBox::toggled, this, save);
  connect(m_host_server_name, &QLineEdit::textChanged, this, save);
  connect(m_host_server_password, &QLineEdit::textChanged, this, save);
  connect(m_host_server_region, &QComboBox::currentIndexChanged, this, save);

#ifdef USE_UPNP
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  connect(m_host_upnp, &QCheckBox::checkStateChanged, this, save);
#else
  connect(m_host_upnp, &QCheckBox::stateChanged, this, save);
#endif
#endif

  // Both action buttons go through accept(), which looks at the current tab.
  connect(m_connect_button, &QPushButton::clicked, this, &QDialog::accept);
  connect(m_host_button, &QPushButton::clicked, this, &QDialog::accept);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_reset_traversal_button, &QPushButton::clicked, this,
          &NetPlaySetupDialog::ResetTraversalHost);

  // Hooked up after the save above, so the config is written before the fields change state.
  connect(m_host_server_browser, &QCheckBox::toggled, this, [this](bool on) {
    m_host_server_region->setEnabled(on);
    m_host_server_name->setEnabled(on);
    m_host_server_password->setEnabled(on);
  });
}

void NetPlaySetupDialog::SaveSettings()
{
  // Batch the writes so listeners hear about them once.
  Config::ConfigChangeCallbackGuard batch;

  Config::SetBaseOrCurrent(Config::NETPLAY_NICKNAME, m_nickname_edit->text().toStdString());

  // The address field holds an IP for direct connections and a host code for traversal.
  const auto& address_setting = m_connection_type->currentIndex() == DIRECT_CONNECTION ?
                                    Config::NETPLAY_ADDRESS :
                                    Config::NETPLAY_HOST_CODE;
  Config::SetBaseOrCurrent(address_setting, m_ip_edit->text().toStdString());

  Config::SetBaseOrCurrent(Config::NETPLAY_CONNECT_PORT,
                           static_cast<u16>(m_connect_port_box->value()));
  Config::SetBaseOrCurrent(Config::NETPLAY_HOST_PORT, static_cast<u16>(m_host_port_box->value()));
#ifdef USE_UPNP
  Config::SetBaseOrCurrent(Config::NETPLAY_USE_UPNP, m_host_upnp->isChecked());
#endif

  // An unticked "force listen port" keeps whatever was stored before.
  if (m_host_force_port_check->isChecked())
  {
    Config::SetBaseOrCurrent(Config::NETPLAY_LISTEN_PORT,
                             static_cast<u16>(m_host_force_port_box->value()));
  }

  Config::SetBaseOrCurrent(Config::NETPLAY_ENABLE_CHUNKED_UPLOAD_LIMIT,
                           m_host_chunked_upload_limit_check->isChecked());
  Config::SetBaseOrCurrent(Config::NETPLAY_CHUNKED_UPLOAD_LIMIT,
                           static_cast<u32>(m_host_chunked_upload_limit_box->value()));

  Config::SetBaseOrCurrent(Config::NETPLAY_USE_INDEX, m_host_server_browser->isChecked());
  Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_REGION,
                           m_host_server_region->currentData().toString().toStdString());
  Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_NAME, m_host_server_name->text().toStdString());
  Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_PASSWORD,
                           m_host_server_password->text().toStdString());
}

void NetPlaySetupDialog::OnConnectionTypeChanged(int index)
{
  const bool direct = index == DIRECT_CONNECTION;

  // Ports only apply to direct connections; the forced listen port and the traversal reset
  // only to traversal.
  m_connect_port_box->setHidden(!direct);
  m_connect_port_label->setHidden(!direct);
  m_host_port_label->setHidden(!direct);
  m_host_port_box->setHidden(!direct);
#ifdef USE_UPNP
  m_host_upnp->setHidden(!direct);
#endif
  m_host_force_port_check->setHidden(direct);
  m_host_force_port_box->setHidden(direct);
  m_reset_traversal_button->setHidden(direct);

  const std::string address =
      direct ? Config::Get(Config::NETPLAY_ADDRESS) : Config::Get(Config::NETPLAY_HOST_CODE);

  m_ip_label->setText(direct ? tr("IP Address:") : tr("Host Code:"));
  m_ip_edit->setText(QString::fromStdString(address));

  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE,
                           std::string(direct ? "direct" : "traversal"));
}

void NetPlaySetupDialog::show()
{
  PopulateGameList();
  QDialog::show();
}

bool NetPlaySetupDialog::CheckHostRequirements()
{
  const char* problem = nullptr;
  if (m_host_games->selectedItems().empty())
    problem = QT_TR_NOOP("You must select a game to host!");
  else if (m_host_server_browser->isChecked() && m_host_server_name->text().isEmpty())
    problem = QT_TR_NOOP("You must provide a name for your session!");
  else if (m_host_server_browser->isChecked() &&
           m_host_server_region->currentData().toString().isEmpty())
    problem = QT_TR_NOOP("You must provide a region for your session!");

  if (problem == nullptr)
    return true;

  ModalMessageBox::critical(this, tr("Error"), tr(problem));
  return false;
}

void NetPlaySetupDialog::accept()
{
  SaveSettings();

  if (m_tab_widget->currentIndex() == CONNECT_TAB)
  {
    emit Join();
    return;
  }

  if (!CheckHostRequirements())
    return;

  const QListWidgetItem* const chosen = m_host_games->selectedItems()[0];
  emit Host(*chosen->data(Qt::UserRole).value<GamePtr>());
}

void NetPlaySetupDialog::PopulateGameList()
{
  // Refilling must not overwrite the remembered game through currentRowChanged.
  const QSignalBlocker quiet(m_host_games);

  m_host_games->clear();
  const int game_count = m_game_list_model.rowCount(QModelIndex());
  for (int row = 0; row < game_count; ++row)
  {
    GamePtr game = m_game_list_model.GetGameFile(row);
    auto* const entry =
        new QListWidgetItem(QString::fromStdString(m_game_list_model.GetNetPlayName(*game)));
    entry->setData(Qt::UserRole, QVariant::fromValue(std::move(game)));
    m_host_games->addItem(entry);
  }

  m_host_games->sortItems();

  const QString remembered = Settings::GetQSettings().value(KEY_HOST_GAME, QString{}).toString();
  const auto matches = m_host_games->findItems(remembered, Qt::MatchFlag::MatchExactly);
  if (!matches.isEmpty())
    m_host_games->setCurrentItem(matches.first());
}

void NetPlaySetupDialog::ResetTraversalHost()
{
  const std::string default_server = Config::NETPLAY_TRAVERSAL_SERVER.GetDefaultValue();
  const u16 default_port = Config::NETPLAY_TRAVERSAL_PORT.GetDefaultValue();

  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_SERVER, default_server);
  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_PORT, default_port);

  ModalMessageBox::information(
      this, tr("Reset Traversal Server"),
      tr("Reset Traversal Server to %1:%2")
          .arg(QString::fromStdString(default_server), QString::number(default_port)));
}
