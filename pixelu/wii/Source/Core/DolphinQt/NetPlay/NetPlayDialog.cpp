// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/NetPlayDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStringList>
#include <QTableWidget>
#include <QTextBrowser>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <utility>

#ifdef HAS_LIBMGBA
#include <fmt/ranges.h>
#endif

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayCommon.h"
#include "Core/NetPlayProto.h"

#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlayServer.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"

#include "DolphinQt/NetPlay/ChunkedProgressDialog.h"
#include "DolphinQt/NetPlay/ClickBlurLabel.h"
#include "DolphinQt/NetPlay/GameDigestDialog.h"
#include "DolphinQt/NetPlay/GameListDialog.h"
#include "DolphinQt/NetPlay/PadMappingDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/GameCubePane.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"
#include "VideoCommon/NetPlayGolfUI.h"

namespace
{
using GamePtr = std::shared_ptr<const UICommon::GameFile>;

// QSettings keys.
const QString KEY_GEOMETRY = QStringLiteral("netplaydialog/geometry");
const QString KEY_SPLITTER = QStringLiteral("netplaydialog/splitter");
const QString KEY_HOST_GAME = QStringLiteral("netplay/hostgame");

// Player table columns; the first four shrink to their contents and the last one stretches.
constexpr int PLAYER_COLUMNS = 5;

// Config values for NETPLAY_NETWORK_MODE.
constexpr char MODE_FIXED_DELAY[] = "fixeddelay";
constexpr char MODE_HOST_INPUT_AUTHORITY[] = "hostinputauthority";
constexpr char MODE_GOLF[] = "golf";

// Formats the address the traversal server saw us at as "a.b.c.d:port". Both the IPv4 address
// and the port are stored in network byte order, so they are read byte by byte.
QString DescribeTraversalAddress(const Common::TraversalInetAddress& address)
{
  QString host;
  if (address.isIPV6)
  {
    host = QStringLiteral("IPv6-Not-Implemented");
  }
  else
  {
    std::array<u8, 4> octets{};
    std::memcpy(octets.data(), &address.address[0], octets.size());
    QStringList parts;
    for (const u8 octet : octets)
      parts.append(QString::number(octet));
    host = parts.join(QLatin1Char('.'));
  }

  std::array<u8, 2> port_bytes{};
  std::memcpy(port_bytes.data(), &address.port, port_bytes.size());
  const int port = (port_bytes[0] << 8) | port_bytes[1];

  return QStringLiteral("%1:%2").arg(host, QString::number(port));
}

// Short status text and tooltip for each way a player's copy of the game can match ours.
using StatusTexts = std::pair<QString, QString>;
const std::map<NetPlay::SyncIdentifierComparison, StatusTexts>& GameStatusTexts()
{
  using Cmp = NetPlay::SyncIdentifierComparison;
  static const std::map<Cmp, StatusTexts> texts{
      {Cmp::SameGame, {NetPlayDialog::tr("OK"), NetPlayDialog::tr("OK")}},
      {Cmp::DifferentHash,
       {NetPlayDialog::tr("Wrong hash"),
        NetPlayDialog::tr("Game file has a different hash; right-click it, select Properties, "
                          "switch to the Verify tab, and select Verify Integrity to check the "
                          "hash")}},
      {Cmp::DifferentDiscNumber,
       {NetPlayDialog::tr("Wrong disc number"),
        NetPlayDialog::tr("Game has a different disc number")}},
      {Cmp::DifferentRevision,
       {NetPlayDialog::tr("Wrong revision"), NetPlayDialog::tr("Game has a different revision")}},
      {Cmp::DifferentRegion,
       {NetPlayDialog::tr("Wrong region"), NetPlayDialog::tr("Game region does not match")}},
      {Cmp::DifferentGame,
       {NetPlayDialog::tr("Not found"), NetPlayDialog::tr("No matching game was found")}},
  };
  return texts;
}

// Adds a checkable entry to a menu, with an optional tooltip.
QAction* AddToggle(QMenu* menu, const QString& text, const QString& tooltip = {})
{
  QAction* const action = menu->addAction(text);
  if (!tooltip.isEmpty())
    action->setToolTip(tooltip);
  action->setCheckable(true);
  return action;
}

QActionGroup* MakeExclusiveGroup(QObject* owner, std::initializer_list<QAction*> actions)
{
  auto* const group = new QActionGroup(owner);
  group->setExclusive(true);
  for (QAction* const action : actions)
    group->addAction(action);
  return group;
}
}  // namespace

NetPlayDialog::NetPlayDialog(const GameListModel& games, StartGameCallback on_start, QWidget* owner)
    : QDialog(owner), m_game_list_model(games), m_start_game_callback(std::move(on_start))
{
  setWindowTitle(tr("NetPlay"));
  setWindowIcon(Resources::GetAppIcon());

  // The helper dialogs are owned by this window and reused for every request.
  m_pad_mapping = new PadMappingDialog(this);
  m_game_digest_dialog = new GameDigestDialog(this);
  m_chunked_progress_dialog = new ChunkedProgressDialog(this);

  ResetExternalIP();

  // The chat and player panes must exist before the main layout places them in the splitter.
  CreateChatLayout();
  CreatePlayersLayout();
  CreateMainLayout();

  LoadSettings();
  ConnectWidgets();

  const QSettings& stored = Settings::Instance().GetQSettings();
  restoreGeometry(stored.value(KEY_GEOMETRY).toByteArray());
  m_splitter->restoreState(stored.value(KEY_SPLITTER).toByteArray());
}

NetPlayDialog::~NetPlayDialog()
{
  auto& settings = Settings::Instance().GetQSettings();
  settings.setValue(KEY_GEOMETRY, saveGeometry());
  settings.setValue(KEY_SPLITTER, m_splitter->saveState());
}

void NetPlayDialog::CreateDataMenu()
{
  m_data_menu = m_menu_bar->addMenu(tr("Data"));
  m_data_menu->setToolTipsVisible(true);

  // How save data travels with the session; exactly one of these three is active.
  m_savedata_none_action =
      AddToggle(m_data_menu, tr("No Save Data"),
                tr("Netplay will start without any save data, and any created save data will be "
                   "discarded at the end of the Netplay session."));
  m_savedata_load_only_action =
      AddToggle(m_data_menu, tr("Load Host's Save Data Only"),
                tr("Netplay will start using the Host's save data, but any save data created or "
                   "modified during the Netplay session will be discarded at the end of the "
                   "session."));
  m_savedata_load_and_write_action =
      AddToggle(m_data_menu, tr("Load and Write Host's Save Data"),
                tr("Netplay will start using the Host's save data, and any save data created or "
                   "modified during the Netplay session will remain in the Host's local saves."));
  m_savedata_style_group =
      MakeExclusiveGroup(this, {m_savedata_none_action, m_savedata_load_only_action,
                                m_savedata_load_and_write_action});

  m_data_menu->addSeparator();

  m_savedata_all_wii_saves_action =
      AddToggle(m_data_menu, tr("Use All Wii Save Data"),
                tr("If checked, all Wii saves will be used instead of only the save of the game "
                   "being started. Useful when switching games mid-session. Has no effect if No "
                   "Save Data is selected."));

  m_data_menu->addSeparator();

  m_sync_codes_action = AddToggle(m_data_menu, tr("Sync AR/Gecko Codes"));
  m_strict_settings_sync_action =
      AddToggle(m_data_menu, tr("Strict Settings Sync"),
                tr("This will sync additional graphics settings, and force everyone to the same "
                   "internal resolution.\nMay prevent desync in some games that use EFB reads. "
                   "Please ensure everyone uses the same video backend."));
}

void NetPlayDialog::CreateNetworkMenu()
{
  m_network_menu = m_menu_bar->addMenu(tr("Network"));
  m_network_menu->setToolTipsVisible(true);

  // The three input models are mutually exclusive.
  m_fixed_delay_action =
      AddToggle(m_network_menu, tr("Fair Input Delay"),
                tr("Each player sends their own inputs to the game, with equal buffer size for "
                   "all players, configured by the host.\nSuitable for competitive games where "
                   "fairness and minimal latency are most important."));
  m_host_input_authority_action =
      AddToggle(m_network_menu, tr("Host Input Authority"),
                tr("Host has control of sending all inputs to the game, as received from other "
                   "players, giving the host zero latency but increasing latency for "
                   "others.\nSuitable for casual games with 3+ players, possibly on unstable or "
                   "high latency connections."));
  m_golf_mode_action =
      AddToggle(m_network_menu, tr("Golf Mode"),
                tr("Identical to Host Input Authority, except the \"Host\" (who has zero latency) "
                   "can be switched at any time.\nSuitable for turn-based games with "
                   "timing-sensitive controls, such as golf."));
  m_network_mode_group = MakeExclusiveGroup(
      this, {m_fixed_delay_action, m_host_input_authority_action, m_golf_mode_action});
  m_fixed_delay_action->setChecked(true);
}

void NetPlayDialog::CreateChecksumMenu()
{
  m_game_digest_menu = m_menu_bar->addMenu(tr("Checksum"));

  m_game_digest_menu->addAction(tr("Current game"), this, [this] {
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(m_current_game_identifier);
  });
  m_game_digest_menu->addAction(tr("Other game..."), this, [this] {
    GameListDialog picker(m_game_list_model, this);
    if (picker.exec() != QDialog::Accepted)
      return;
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(
        picker.GetSelectedGame().GetSyncIdentifier());
  });
  m_game_digest_menu->addAction(tr("SD Card"), this, [] {
    Settings::Instance().GetNetPlayServer()->ComputeGameDigest(
        NetPlay::NetPlayClient::GetSDCardIdentifier());
  });
}

void NetPlayDialog::CreateOtherMenu()
{
  m_other_menu = m_menu_bar->addMenu(tr("Other"));
  m_record_input_action = AddToggle(m_other_menu, tr("Record Inputs"));
  m_golf_mode_overlay_action = AddToggle(m_other_menu, tr("Show Golf Mode Overlay"));
  m_hide_remote_gbas_action = AddToggle(m_other_menu, tr("Hide Remote GBAs"));
}

void NetPlayDialog::CreateMainLayout()
{
  m_menu_bar = new QMenuBar(this);
  m_main_layout = new QGridLayout;
  m_splitter = new QSplitter(Qt::Horizontal);

  // The game button doubles as the title line showing the current game.
  m_game_button = new QPushButton;
  m_start_button = new QPushButton(tr("Start"));
  m_buffer_label = new QLabel(tr("Buffer:"));
  m_buffer_size_box = new QSpinBox;
  m_buffer_size_box->setMaximum(NetPlay::MAX_TARGET_PAD_BUFFER_SIZE);
  m_quit_button = new QPushButton(tr("Quit"));

  CreateDataMenu();
  CreateNetworkMenu();
  CreateChecksumMenu();
  CreateOtherMenu();

  m_game_button->setDefault(false);
  m_game_button->setAutoDefault(false);

  // Defaults until LoadSettings() applies the stored choices.
  m_savedata_load_only_action->setChecked(true);
  m_sync_codes_action->setChecked(true);

  m_main_layout->setMenuBar(m_menu_bar);
  m_main_layout->addWidget(m_game_button, 0, 0, 1, -1);
  m_main_layout->addWidget(m_splitter, 1, 0, 1, -1);

  m_splitter->addWidget(m_chat_box);
  m_splitter->addWidget(m_players_box);

  // Bottom row: start, buffer size, then quit pushed to the far right.
  auto* const controls = new QGridLayout;
  controls->addWidget(m_start_button, 0, 0, Qt::AlignVCenter);
  controls->addWidget(m_buffer_label, 0, 1, Qt::AlignVCenter);
  controls->addWidget(m_buffer_size_box, 0, 2, Qt::AlignVCenter);
  controls->addWidget(m_quit_button, 0, 3, Qt::AlignVCenter | Qt::AlignRight);
  controls->setColumnStretch(3, 1000);

  m_main_layout->addLayout(controls, 2, 0, 1, -1, Qt::AlignRight);
  m_main_layout->setRowStretch(1, 1000);

  setLayout(m_main_layout);
}

void NetPlayDialog::CreateChatLayout()
{
  m_chat_box = new QGroupBox(tr("Chat"));
  m_chat_edit = new QTextBrowser;
  m_chat_type_edit = new QLineEdit;
  m_chat_send_button = new QPushButton(tr("Send"));

  // Starts disabled; typing something into the input line turns it on.
  m_chat_send_button->setEnabled(false);
  m_chat_send_button->setDefault(false);
  m_chat_send_button->setAutoDefault(false);

  m_chat_edit->setReadOnly(true);

  auto* const grid = new QGridLayout;
  grid->addWidget(m_chat_edit, 0, 0, 1, -1);
  grid->addWidget(m_chat_type_edit, 1, 0);
  grid->addWidget(m_chat_send_button, 1, 1);
  m_chat_box->setLayout(grid);
}

void NetPlayDialog::CreatePlayersLayout()
{
  m_players_box = new QGroupBox(tr("Players"));

  // Host code line: which address to show, the code itself, and a copy/retry button.
  m_room_box = new QComboBox;
  m_hostcode_label = new ClickBlurLabel;
  m_hostcode_action_button = new QPushButton(tr("Copy"));

  m_players_list = new QTableWidget;
  m_kick_button = new QPushButton(tr("Kick Player"));
  m_assign_ports_button = new QPushButton(tr("Assign Controller Ports"));

  m_players_list->setTabKeyNavigation(false);
  m_players_list->setColumnCount(PLAYER_COLUMNS);
  m_players_list->verticalHeader()->hide();
  m_players_list->setSelectionBehavior(QAbstractItemView::SelectRows);

  QHeaderView* const header = m_players_list->horizontalHeader();
  header->setStretchLastSection(true);
  header->setHighlightSections(false);
  for (int column = 0; column < PLAYER_COLUMNS - 1; ++column)
    header->setSectionResizeMode(column, QHeaderView::ResizeToContents);

  auto* const grid = new QGridLayout;
  grid->addWidget(m_room_box, 0, 0);
  grid->addWidget(m_hostcode_label, 0, 1);
  grid->addWidget(m_hostcode_action_button, 0, 2);
  grid->addWidget(m_players_list, 1, 0, 1, -1);
  grid->addWidget(m_kick_button, 2, 0, 1, -1);
  grid->addWidget(m_assign_ports_button, 3, 0, 1, -1);
  m_players_box->setLayout(grid);
}

void NetPlayDialog::OnAssignPorts()
{
  m_pad_mapping->exec();

  // The dialog's result is ignored: whatever it holds now is pushed to the server.
  Settings::Instance().GetNetPlayServer()->SetPadMapping(m_pad_mapping->GetGCPadArray());
  Settings::Instance().GetNetPlayServer()->SetGBAConfig(m_pad_mapping->GetGBAArray(), true);
  Settings::Instance().GetNetPlayServer()->SetWiimoteMapping(m_pad_mapping->GetWiimoteArray());
}

void NetPlayDialog::OnChooseGame()
{
  GameListDialog picker(m_game_list_model, this);
  if (picker.exec() != QDialog::Accepted)
    return;

  Settings& settings = Settings::Instance();
  const UICommon::GameFile& game = picker.GetSelectedGame();
  const std::string netplay_name = m_game_list_model.GetNetPlayName(game);

  settings.GetNetPlayServer()->ChangeGame(game.GetSyncIdentifier(), netplay_name);
  Settings::GetQSettings().setValue(KEY_HOST_GAME, QString::fromStdString(netplay_name));
}

void NetPlayDialog::OnEmulationStateChanged(Core::State state)
{
  if (!isVisible())
    return;

  const bool stopped = state == Core::State::Uninitialized;
  GameStatusChanged(!stopped);

  // A game that ends locally without the server asking for it tells everyone else to stop too.
  if ((stopped || state == Core::State::Stopping) && !m_got_stop_request)
    Settings::Instance().GetNetPlayClient()->RequestStopGame();

  if (stopped)
    DisplayMessage(tr("Stopped game"), "red");
}

void NetPlayDialog::ConnectWidgets()
{
  // Players
  connect(m_room_box, &QComboBox::currentIndexChanged, this, &NetPlayDialog::UpdateGUI);
  connect(m_hostcode_action_button, &QPushButton::clicked, this, [this] {
    // After a traversal failure the button doubles as "Retry".
    if (m_is_copy_button_retry)
      Common::g_TraversalClient->ReconnectToServer();
    else
      QApplication::clipboard()->setText(m_hostcode_label->text());
  });
  connect(m_players_list, &QTableWidget::itemSelectionChanged, this, [this] {
    // Row 0 is the host, who cannot be kicked.
    const int row = m_players_list->currentRow();
    const bool kickable = row > 0 && !m_players_list->currentItem()->data(Qt::UserRole).isNull();
    m_kick_button->setEnabled(kickable);
  });
  connect(m_kick_button, &QPushButton::clicked, this, [this] {
    const int pid = m_players_list->currentItem()->data(Qt::UserRole).toInt();
    Settings::Instance().GetNetPlayServer()->KickPlayer(static_cast<NetPlay::PlayerId>(pid));
  });
  connect(m_assign_ports_button, &QPushButton::clicked, this, &NetPlayDialog::OnAssignPorts);

  // Chat
  connect(m_chat_send_button, &QPushButton::clicked, this, &NetPlayDialog::OnChat);
  connect(m_chat_type_edit, &QLineEdit::returnPressed, this, &NetPlayDialog::OnChat);
  connect(m_chat_type_edit, &QLineEdit::textChanged, this,
          [this] { m_chat_send_button->setEnabled(!m_chat_type_edit->text().isEmpty()); });

  // Session controls
  connect(m_buffer_size_box, &QSpinBox::valueChanged, this, [this](int value) {
    if (value == m_buffer_size)
      return;

    // With host input authority every client picks its own buffer; otherwise the host sets one
    // for everybody.
    const auto net_client = Settings::Instance().GetNetPlayClient();
    const auto net_server = Settings::Instance().GetNetPlayServer();
    const auto size = static_cast<u32>(value);
    if (net_server && !m_host_input_authority)
      net_server->AdjustPadBufferSize(size);
    else
      net_client->AdjustPadBufferSize(size);
  });

  // Each mode entry asks for its own authority setting whenever it toggles (on or off).
  const auto request_authority = [this](bool wanted) {
    if (m_host_input_authority == wanted)
      return;
    if (const auto net_server = Settings::Instance().GetNetPlayServer())
      net_server->SetHostInputAuthority(wanted);
  };
  connect(m_host_input_authority_action, &QAction::toggled, this,
          [request_authority] { request_authority(true); });
  connect(m_golf_mode_action, &QAction::toggled, this,
          [request_authority] { request_authority(true); });
  connect(m_fixed_delay_action, &QAction::toggled, this,
          [request_authority] { request_authority(false); });

  connect(m_start_button, &QPushButton::clicked, this, &NetPlayDialog::OnStart);
  connect(m_quit_button, &QPushButton::clicked, this, &NetPlayDialog::reject);
  connect(m_game_button, &QPushButton::clicked, this, &NetPlayDialog::OnChooseGame);

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &NetPlayDialog::OnEmulationStateChanged);

  ConnectSettingsSaving();
}

void NetPlayDialog::ConnectSettingsSaving()
{
  // Any change to the host's session options is written to the config right away.
  connect(m_buffer_size_box, &QSpinBox::valueChanged, this, &NetPlayDialog::SaveSettings);
  for (QAction* const option :
       {m_savedata_none_action, m_savedata_load_only_action, m_savedata_load_and_write_action,
        m_savedata_all_wii_saves_action, m_sync_codes_action, m_record_input_action,
        m_strict_settings_sync_action, m_host_input_authority_action, m_golf_mode_action,
        m_golf_mode_overlay_action, m_fixed_delay_action, m_hide_remote_gbas_action})
  {
    connect(option, &QAction::toggled, this, &NetPlayDialog::SaveSettings);
  }
}

void NetPlayDialog::SendChatLine(const std::string& message)
{
  Settings::Instance().GetNetPlayClient()->SendChatMessage(message);

  const QString line = QStringLiteral("%1: %2").arg(QString::fromStdString(m_nickname),
                                                    QString::fromStdString(message));
  DisplayMessage(line, "");
}

void NetPlayDialog::OnChat()
{
  QueueOnObject(this, [this] {
    const std::string message = m_chat_type_edit->text().toStdString();
    if (message.empty())
      return;

    m_chat_type_edit->clear();
    SendChatLine(message);
  });
}

void NetPlayDialog::OnIndexAdded(bool success, const std::string error)
{
  if (success)
  {
    DisplayMessage(tr("Successfully added to the NetPlay index"), "green");
    return;
  }
  DisplayMessage(
      tr("Failed to add this session to the NetPlay index: %1").arg(QString::fromStdString(error)),
      "red");
}

void NetPlayDialog::OnIndexRefreshFailed(const std::string error)
{
  DisplayMessage(QString::fromStdString(error), "red");
}

void NetPlayDialog::OnStart()
{
  if (!Settings::Instance().GetNetPlayClient()->DoAllPlayersHaveGame())
  {
    const auto answer = ModalMessageBox::question(
        this, tr("Warning"), tr("Not all players have the game. Do you really want to start?"));
    if (answer == QMessageBox::No)
      return;
  }

  // An EFB scale of 0 means "auto", which follows each player's window size.
  if (m_strict_settings_sync_action->isChecked() && Config::Get(Config::GFX_EFB_SCALE) == 0)
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Auto internal resolution is not allowed in strict sync mode, "
                                 "as it depends on window size.\n\nPlease select a specific "
                                 "internal resolution."));
    return;
  }

  if (!FindGameFile(m_current_game_identifier))
  {
    PanicAlertFmtT("Selected game doesn't exist in game list!");
    return;
  }

  if (Settings::Instance().GetNetPlayServer()->RequestStartGame())
    SetOptionsEnabled(false);
}

void NetPlayDialog::reject()
{
  const auto answer = ModalMessageBox::question(this, tr("Confirmation"),
                                                tr("Are you sure you want to quit NetPlay?"));
  if (answer == QMessageBox::Yes)
    QDialog::reject();
}

void NetPlayDialog::show(std::string name, bool traversal)
{
  // A new session: forget the previous one's name, mode and counters.
  m_nickname = std::move(name);
  m_use_traversal = traversal;
  m_buffer_size = 0;
  m_old_player_count = 0;

  m_room_box->clear();
  m_chat_edit->clear();
  m_chat_type_edit->clear();

  const bool hosting = Settings::Instance().GetNetPlayServer() != nullptr;

  if (hosting)
  {
    // Entries: [Room ID,] External, then one per local interface (data = interface name).
    if (traversal)
      m_room_box->addItem(tr("Room ID"));
    m_room_box->addItem(tr("External"));

    for (const auto& iface_name : Settings::Instance().GetNetPlayServer()->GetInterfaceSet())
    {
      const QString iface_text = QString::fromStdString(iface_name);
      m_room_box->addItem(iface_name == "!local!" ? tr("Local") : iface_text, iface_text);
    }
  }

  // Everything that only the host can change is hidden from clients.
  m_data_menu->menuAction()->setVisible(hosting);
  m_network_menu->menuAction()->setVisible(hosting);
  m_game_digest_menu->menuAction()->setVisible(hosting);
#ifdef HAS_LIBMGBA
  m_hide_remote_gbas_action->setVisible(hosting);
#else
  m_hide_remote_gbas_action->setVisible(false);
#endif
  for (QWidget* const host_only :
       {static_cast<QWidget*>(m_start_button), static_cast<QWidget*>(m_kick_button),
        static_cast<QWidget*>(m_assign_ports_button), static_cast<QWidget*>(m_room_box),
        static_cast<QWidget*>(m_hostcode_label), static_cast<QWidget*>(m_hostcode_action_button)})
  {
    host_only->setHidden(!hosting);
  }
  m_game_button->setEnabled(hosting);
  m_kick_button->setEnabled(false);

  SetOptionsEnabled(true);

  QDialog::show();
  UpdateGUI();
}

void NetPlayDialog::ResetExternalIP()
{
  // Looked up the first time it is needed, not when the dialog opens.
  m_external_ip_address =
      Common::Lazy<std::string>([]() -> std::string { return NetPlay::GetExternalIPAddress(); });
}

void NetPlayDialog::UpdateDiscordPresence()
{
#ifdef USE_DISCORD_PRESENCE
  // Without a player count and a game name there is nothing sensible to show yet.
  if (m_player_count == 0 || m_current_game_name.empty())
    return;

  // The plain status, with no way for friends to join.
  const auto without_secret = [this] {
    Discord::UpdateDiscordPresence(m_player_count, Discord::SecretType::Empty, "",
                                   m_current_game_name);
  };

  if (Core::IsRunning(Core::System::GetInstance()) || !IsHosting())
    return without_secret();

  if (Common::g_TraversalClient)
  {
    const auto host_id = Common::g_TraversalClient->GetHostID();
    if (host_id[0] == '\0')
      return without_secret();

    Discord::UpdateDiscordPresence(m_player_count, Discord::SecretType::RoomID,
                                   std::string(host_id.begin(), host_id.end()),
                                   m_current_game_name);
    return;
  }

  if (m_external_ip_address->empty())
    return without_secret();

  const int port = Settings::Instance().GetNetPlayServer()->GetPort();
  Discord::UpdateDiscordPresence(m_player_count, Discord::SecretType::IPAddress,
                                 Discord::CreateSecretFromIPAddress(*m_external_ip_address, port),
                                 m_current_game_name);
#endif
}

void NetPlayDialog::FillPlayerTable(NetPlay::NetPlayClient& net_client,
                                    const std::vector<const NetPlay::Player*>& players)
{
  // Keep the same player selected across the rebuild.
  const QTableWidgetItem* const previous = m_players_list->currentItem();
  const int selected_pid = previous ? previous->data(Qt::UserRole).toInt() : -1;

  m_players_list->clear();
  m_players_list->setHorizontalHeaderLabels(
      {tr("Player"), tr("Game Status"), tr("Ping"), tr("Mapping"), tr("Revision")});
  m_players_list->setRowCount(m_player_count);

  const auto& status_texts = GameStatusTexts();

  for (int row = 0; row < m_player_count; ++row)
  {
    const NetPlay::Player* const player = players[static_cast<size_t>(row)];

    const auto status = status_texts.find(player->game_status);
    const StatusTexts& shown_status = status != status_texts.end() ?
                                          status->second :
                                          std::make_pair(QStringLiteral("?"), QStringLiteral("?"));
    const QString mapping = QString::fromStdString(
        NetPlay::GetPlayerMappingString(player->pid, net_client.GetPadMapping(),
                                        net_client.GetGBAConfig(), net_client.GetWiimoteMapping()));

    // Text and tooltip per column; the tooltip repeats the text except for the game status.
    const std::array<std::pair<QString, QString>, PLAYER_COLUMNS> columns{{
        {QString::fromStdString(player->name), {}},
        {shown_status.first, shown_status.second},
        {QStringLiteral("%1 ms").arg(player->ping), {}},
        {mapping, {}},
        {QString::fromStdString(player->revision), {}},
    }};

    for (int column = 0; column < PLAYER_COLUMNS; ++column)
    {
      const auto& [text, tooltip] = columns[static_cast<size_t>(column)];
      auto* const cell = new QTableWidgetItem(text);
      cell->setToolTip(column == 1 ? tooltip : cell->text());
      cell->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
      cell->setData(Qt::UserRole, static_cast<int>(player->pid));
      m_players_list->setItem(row, column, cell);
    }

    if (player->pid == selected_pid)
      m_players_list->selectRow(row);
  }
}

void NetPlayDialog::ShowCopyableHostCode()
{
  m_hostcode_action_button->setEnabled(true);
  m_hostcode_action_button->setText(tr("Copy"));
  m_is_copy_button_retry = false;
}

void NetPlayDialog::UpdateHostCode(NetPlay::NetPlayServer& net_server)
{
  // Indices past "Room ID" (traversal only) and "External" are local interfaces.
  const int first_interface = m_use_traversal ? 2 : 1;
  if (m_room_box->currentIndex() >= first_interface)
  {
    const std::string iface_name = m_room_box->currentData().toString().toStdString();
    m_hostcode_label->setText(QString::fromStdString(net_server.GetInterfaceHost(iface_name)));
    ShowCopyableHostCode();
    return;
  }

  if (!m_use_traversal)
  {
    // Direct connection: show the public address, if it could be determined.
    if (m_external_ip_address->empty())
    {
      m_hostcode_label->setText(tr("Unknown"));
      m_hostcode_action_button->setEnabled(false);
    }
    else
    {
      const int port = Settings::Instance().GetNetPlayServer()->GetPort();
      m_hostcode_label->setText(QStringLiteral("%1:%2").arg(
          QString::fromStdString(*m_external_ip_address), QString::number(port)));
      m_hostcode_action_button->setEnabled(true);
    }
    m_hostcode_action_button->setText(tr("Copy"));
    m_is_copy_button_retry = false;
    return;
  }

  switch (Common::g_TraversalClient->GetState())
  {
  case Common::TraversalClient::State::Connecting:
    m_hostcode_label->setText(tr("Connecting"));
    m_hostcode_action_button->setEnabled(false);
    m_hostcode_action_button->setText(tr("..."));
    break;
  case Common::TraversalClient::State::Connected:
    if (m_room_box->currentIndex() == 0)
    {
      // "Room ID": the code the traversal server assigned to us.
      const auto host_id = Common::g_TraversalClient->GetHostID();
      m_hostcode_label->setText(
          QString::fromStdString(std::string(host_id.begin(), host_id.end())));
    }
    else
    {
      // "External": the traversal server also tells us our public address and port.
      m_hostcode_label->setText(
          DescribeTraversalAddress(Common::g_TraversalClient->GetExternalAddress()));
    }
    ShowCopyableHostCode();
    break;
  case Common::TraversalClient::State::Failure:
    m_hostcode_label->setText(tr("Error"));
    m_hostcode_action_button->setText(tr("Retry"));
    m_hostcode_action_button->setEnabled(true);
    m_is_copy_button_retry = true;
    break;
  }
}

void NetPlayDialog::UpdateGUI()
{
  const auto net_client = Settings::Instance().GetNetPlayClient();
  const auto net_server = Settings::Instance().GetNetPlayServer();
  if (!net_client)
    return;

  const std::vector<const NetPlay::Player*> players = net_client->GetPlayers();
  const int count = static_cast<int>(players.size());

  // Flash the taskbar entry when somebody joins or leaves.
  if (count != m_player_count && m_player_count != 0)
    QApplication::alert(this);
  m_player_count = count;

  FillPlayerTable(*net_client, players);

  if (m_old_player_count != m_player_count)
  {
    UpdateDiscordPresence();
    m_old_player_count = m_player_count;
  }

  if (net_server)
    UpdateHostCode(*net_server);
}

// ---- NetPlayUI: session flow ----

void NetPlayDialog::BootGame(const std::string& path, std::unique_ptr<BootSessionData> session)
{
  // From here on a local stop has to be reported back to the server.
  m_got_stop_request = false;
  m_start_game_callback(path, std::move(session));
}

void NetPlayDialog::StopGame()
{
  // Only the first stop request of a game is forwarded.
  const bool already_requested = std::exchange(m_got_stop_request, true);
  if (!already_requested)
    emit Stop();
}

bool NetPlayDialog::IsHosting() const
{
  const auto net_server = Settings::Instance().GetNetPlayServer();
  return net_server != nullptr;
}

void NetPlayDialog::Update()
{
  // May be called from the network thread.
  QueueOnObject(this, &NetPlayDialog::UpdateGUI);
}

void NetPlayDialog::DisplayMessage(const QString& msg, const std::string& color,
                                   [[maybe_unused]] int duration)
{
  QueueOnObject(m_chat_edit, [this, color, msg] {
    m_chat_edit->append(QStringLiteral("<font color='%1'>%2</font>")
                            .arg(QString::fromStdString(color), msg.toHtmlEscaped()));
  });

  // Mirror the line into the in-game chat overlay while a game runs.
  const QColor tint(color.empty() ? QStringLiteral("white") : QString::fromStdString(color));
  if (Config::Get(Config::GFX_SHOW_NETPLAY_MESSAGES) &&
      Core::IsRunning(Core::System::GetInstance()))
  {
    g_netplay_chat_ui->AppendChat(msg.toStdString(), {static_cast<float>(tint.redF()),
                                                      static_cast<float>(tint.greenF()),
                                                      static_cast<float>(tint.blueF())});
  }
}

void NetPlayDialog::AppendChat(const std::string& msg)
{
  DisplayMessage(QString::fromStdString(msg), "");
  QApplication::alert(this);
}

void NetPlayDialog::OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                                    const std::string& netplay_name)
{
  const QString shown_name = QString::fromStdString(netplay_name);
  // The identifier is read when the queued call runs, as it always has been.
  QueueOnObject(this, [this, shown_name, netplay_name, &sync_identifier] {
    m_game_button->setText(shown_name);
    m_current_game_identifier = sync_identifier;
    m_current_game_name = netplay_name;
    UpdateDiscordPresence();
  });
  DisplayMessage(tr("Game changed to \"%1\"").arg(shown_name), "magenta");
}

void NetPlayDialog::OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config)
{
  const int slot = pad + 1;
  if (!config.has_rom)
  {
    DisplayMessage(tr("GBA%1 ROM disabled").arg(slot), "magenta");
    return;
  }
  DisplayMessage(
      tr("GBA%1 ROM changed to \"%2\"").arg(slot).arg(QString::fromStdString(config.title)),
      "magenta");
}

void NetPlayDialog::GameStatusChanged(bool running)
{
  QueueOnObject(this, [this, running] { SetOptionsEnabled(!running); });
}

void NetPlayDialog::SetOptionsEnabled(bool enabled)
{
  if (Settings::Instance().GetNetPlayServer())
  {
    m_start_button->setEnabled(enabled);
    m_game_button->setEnabled(enabled);
    for (QAction* const option :
         {m_savedata_none_action, m_savedata_load_only_action, m_savedata_load_and_write_action,
          m_savedata_all_wii_saves_action, m_sync_codes_action})
    {
      option->setEnabled(enabled);
    }
    m_assign_ports_button->setEnabled(enabled);
    for (QAction* const option : {m_strict_settings_sync_action, m_host_input_authority_action,
                                  m_golf_mode_action, m_fixed_delay_action})
    {
      option->setEnabled(enabled);
    }
  }

  // Clients may still choose to record their own inputs.
  m_record_input_action->setEnabled(enabled);
}

void NetPlayDialog::OnMsgStartGame()
{
  DisplayMessage(tr("Started game"), "green");

  g_netplay_chat_ui = std::make_unique<NetPlayChatUI>(
      [this](const std::string& message) { SendChatLine(message); });

  if (m_host_input_authority && Settings::Instance().GetNetPlayClient()->GetNetSettings().golf_mode)
    g_netplay_golf_ui = std::make_unique<NetPlayGolfUI>(Settings::Instance().GetNetPlayClient());

  QueueOnObject(this, [this] {
    if (const auto net_client = Settings::Instance().GetNetPlayClient())
    {
      if (const GamePtr game = FindGameFile(m_current_game_identifier))
        net_client->StartGame(game->GetFilePath());
      else
        PanicAlertFmtT("Selected game doesn't exist in game list!");
    }
    UpdateDiscordPresence();
  });
}

void NetPlayDialog::OnMsgStopGame()
{
  // The in-game overlays go away with the game (chat first, then golf).
  g_netplay_chat_ui = nullptr;
  g_netplay_golf_ui = nullptr;
  QueueOnObject(this, [this] { UpdateDiscordPresence(); });
}

void NetPlayDialog::OnMsgPowerButton()
{
  // Only meaningful while a game is running; the event itself is raised on the GUI thread.
  if (Core::IsRunning(Core::System::GetInstance()))
    QueueOnObject(this, [] { UICommon::TriggerSTMPowerEvent(); });
}

void NetPlayDialog::OnPlayerConnect(const std::string& name)
{
  const QString who = QString::fromStdString(name);
  DisplayMessage(tr("%1 has joined").arg(who), "darkcyan");
}

void NetPlayDialog::OnPlayerDisconnect(const std::string& name)
{
  const QString who = QString::fromStdString(name);
  DisplayMessage(tr("%1 has left").arg(who), "darkcyan");
}

void NetPlayDialog::OnPadBufferChanged(u32 buffer)
{
  QueueOnObject(this, [this, buffer] {
    const QSignalBlocker quiet(m_buffer_size_box);
    m_buffer_size_box->setValue(static_cast<int>(buffer));
  });

  // Under host input authority the number is an upper bound rather than a fixed delay.
  const QString text = m_host_input_authority ? tr("Max buffer size changed to %1").arg(buffer) :
                                                tr("Buffer size changed to %1").arg(buffer);
  DisplayMessage(text, "darkcyan");

  m_buffer_size = static_cast<int>(buffer);
}

void NetPlayDialog::OnHostInputAuthorityChanged(bool enabled)
{
  m_host_input_authority = enabled;
  DisplayMessage(enabled ? tr("Host input authority enabled") : tr("Host input authority disabled"),
                 "");

  QueueOnObject(this, [this, enabled] {
    const bool hosting = IsHosting();
    // The host edits the shared buffer only without authority; clients edit their own only with.
    const bool editable = hosting != enabled;

    if (hosting)
    {
      m_buffer_size_box->setEnabled(editable);
      m_buffer_label->setEnabled(editable);
      m_buffer_size_box->setHidden(false);
      m_buffer_label->setHidden(false);
    }
    else
    {
      m_buffer_size_box->setEnabled(true);
      m_buffer_label->setEnabled(true);
      m_buffer_size_box->setHidden(!editable);
      m_buffer_label->setHidden(!editable);
    }

    m_buffer_label->setText(enabled ? tr("Max Buffer:") : tr("Buffer:"));
    if (!enabled)
      return;

    const QSignalBlocker quiet(m_buffer_size_box);
    m_buffer_size_box->setValue(static_cast<int>(Config::Get(Config::NETPLAY_CLIENT_BUFFER_SIZE)));
  });
}

void NetPlayDialog::OnDesync(u32 frame_number, const std::string& name)
{
  const QString text = tr("Possible desync detected: %1 might have desynced at frame %2")
                           .arg(QString::fromStdString(name), QString::number(frame_number));
  // Kept on screen longer than ordinary messages.
  DisplayMessage(text, "red", OSD::Duration::VERY_LONG);
}

void NetPlayDialog::OnConnectionLost()
{
  DisplayMessage(tr("Lost connection to NetPlay server..."), "red");
}

void NetPlayDialog::OnConnectionError(const std::string& reason)
{
  QueueOnObject(this, [this, reason] {
    // The reason is itself a translatable message from the client.
    const QString detail = tr(reason.c_str());
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to connect to server: %1").arg(detail));
  });
}

void NetPlayDialog::OnTraversalError(Common::TraversalClient::FailureReason error)
{
  using Reason = Common::TraversalClient::FailureReason;
  QueueOnObject(this, [this, error] {
    switch (error)
    {
    case Reason::BadHost:
      ModalMessageBox::critical(this, tr("Traversal Error"), tr("Couldn't look up central server"));
      QDialog::reject();
      break;
    case Reason::VersionTooOld:
      ModalMessageBox::critical(this, tr("Traversal Error"),
                                tr("Dolphin is too old for traversal server"));
      QDialog::reject();
      break;
    case Reason::ServerForgotAboutUs:
    case Reason::SocketSendError:
    case Reason::ResendTimeout:
      // Recoverable: just refresh the host code line, which then offers "Retry".
      UpdateGUI();
      break;
    }
  });
}

void NetPlayDialog::OnTraversalStateChanged(Common::TraversalClient::State state)
{
  if (state == Common::TraversalClient::State::Connected ||
      state == Common::TraversalClient::State::Failure)
  {
    UpdateDiscordPresence();
  }
}

void NetPlayDialog::OnGameStartAborted()
{
  QueueOnObject(this, [this] { SetOptionsEnabled(true); });
}

void NetPlayDialog::OnGolferChanged(const bool golfing, const std::string& name)
{
  // In golf mode only the player who is not golfing can adjust the buffer.
  if (m_host_input_authority)
  {
    QueueOnObject(this, [this, editable = !golfing] {
      m_buffer_size_box->setEnabled(editable);
      m_buffer_label->setEnabled(editable);
    });
  }

  if (name.empty())
    return;
  DisplayMessage(tr("%1 is now golfing").arg(QString::fromStdString(name)), "");
}

void NetPlayDialog::OnTtlDetermined(u8 hops)
{
  DisplayMessage(tr("Using TTL %1 for probe packet").arg(QString::number(hops)), "");
}

bool NetPlayDialog::IsRecording()
{
  // Asked from the network thread, so the checkbox is read on the GUI thread.
  const std::optional<bool> recording = RunOnObject(m_record_input_action, &QAction::isChecked);
  return recording.value_or(false);
}

GamePtr NetPlayDialog::FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
                                    NetPlay::SyncIdentifierComparison* found)
{
  NetPlay::SyncIdentifierComparison scratch;
  NetPlay::SyncIdentifierComparison* const best = found ? found : &scratch;

  // Comparisons are ordered from best to worst, so the minimum is the closest match seen.
  *best = NetPlay::SyncIdentifierComparison::DifferentGame;

  const std::optional<GamePtr> match = RunOnObject(this, [this, &sync_identifier, best] {
    for (int row = 0; row < m_game_list_model.rowCount(QModelIndex()); ++row)
    {
      GamePtr candidate = m_game_list_model.GetGameFile(row);
      *best = std::min(*best, candidate->CompareSyncIdentifier(sync_identifier));
      if (*best == NetPlay::SyncIdentifierComparison::SameGame)
        return candidate;
    }
    return GamePtr(nullptr);
  });
  return match ? *match : nullptr;
}

std::string NetPlayDialog::FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                          int device_number)
{
#ifdef HAS_LIBMGBA
  const auto chosen = RunOnObject(this, [&, this] {
    std::string path;
    std::array<u8, 20> found_hash;
    std::string found_title;
    const auto matches = [&] { return found_hash == hash && found_title == title; };

    // First try the configured ROM of every GBA slot, starting with the requesting one.
    const auto first_slot = static_cast<size_t>(device_number);
    for (size_t slot = first_slot; slot < first_slot + 4; ++slot)
    {
      path = Config::Get(Config::MAIN_GBA_ROM_PATHS[slot % 4]);
      if (!path.empty() && HW::GBA::Core::GetRomInfo(path.c_str(), found_hash, found_title) &&
          matches())
      {
        return path;
      }
    }

    // Otherwise keep asking the user until a matching ROM is picked or the prompt is cancelled.
    for (path = GameCubePane::GetOpenGBARom(title); !path.empty();
         path = GameCubePane::GetOpenGBARom(title))
    {
      if (!HW::GBA::Core::GetRomInfo(path.c_str(), found_hash, found_title))
      {
        ModalMessageBox::critical(this, tr("Error"),
                                  tr("%1 is not a valid ROM").arg(QString::fromStdString(path)));
        continue;
      }
      if (matches())
        return path;

      ModalMessageBox::critical(
          this, tr("Error"),
          QString::fromStdString(Common::FmtFormatT(
              "Mismatched ROMs\nSelected: {0}\n- Title: {1}\n- Hash: {2:02X}\nExpected:\n- "
              "Title: {3}\n- Hash: {4:02X}",
              path, found_title, fmt::join(found_hash, ""), title, fmt::join(hash, ""))));
    }
    return std::string();
  });
  if (chosen)
    return *chosen;
#endif
  return {};
}

void NetPlayDialog::LoadSettings()
{
  const int buffer_size = static_cast<int>(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  const bool load_saves = Config::Get(Config::NETPLAY_SAVEDATA_LOAD);
  const bool write_saves = Config::Get(Config::NETPLAY_SAVEDATA_WRITE);
  const bool all_wii_saves = Config::Get(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII);
  const bool sync_codes = Config::Get(Config::NETPLAY_SYNC_CODES);
  const bool record_inputs = Config::Get(Config::NETPLAY_RECORD_INPUTS);
  const bool strict_sync = Config::Get(Config::NETPLAY_STRICT_SETTINGS_SYNC);
  const bool golf_overlay = Config::Get(Config::NETPLAY_GOLF_MODE_OVERLAY);
  const bool hide_remote_gbas = Config::Get(Config::NETPLAY_HIDE_REMOTE_GBAS);

  m_buffer_size_box->setValue(buffer_size);

  QAction* const save_style = !load_saves  ? m_savedata_none_action :
                              !write_saves ? m_savedata_load_only_action :
                                             m_savedata_load_and_write_action;
  save_style->setChecked(true);
  m_savedata_all_wii_saves_action->setChecked(all_wii_saves);

  m_sync_codes_action->setChecked(sync_codes);
  m_record_input_action->setChecked(record_inputs);
  m_strict_settings_sync_action->setChecked(strict_sync);
  m_golf_mode_overlay_action->setChecked(golf_overlay);
  m_hide_remote_gbas_action->setChecked(hide_remote_gbas);

  const std::string network_mode = Config::Get(Config::NETPLAY_NETWORK_MODE);
  QAction* mode_action = m_fixed_delay_action;
  if (network_mode == MODE_HOST_INPUT_AUTHORITY)
    mode_action = m_host_input_authority_action;
  else if (network_mode == MODE_GOLF)
    mode_action = m_golf_mode_action;
  else if (network_mode != MODE_FIXED_DELAY)
    WARN_LOG_FMT(NETPLAY, "Unknown network mode '{}', using 'fixeddelay'", network_mode);
  mode_action->setChecked(true);
}

void NetPlayDialog::SaveSettings()
{
  // One change notification for the whole batch.
  Config::ConfigChangeCallbackGuard batch;

  // The spin box edits the client buffer under host input authority, the shared one otherwise.
  const auto& buffer_setting =
      m_host_input_authority ? Config::NETPLAY_CLIENT_BUFFER_SIZE : Config::NETPLAY_BUFFER_SIZE;
  Config::SetBase(buffer_setting, static_cast<u32>(m_buffer_size_box->value()));

  const bool write_saves = m_savedata_load_and_write_action->isChecked();
  const bool load_saves = write_saves || m_savedata_load_only_action->isChecked();
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, load_saves);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, write_saves);

  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII,
                  m_savedata_all_wii_saves_action->isChecked());
  Config::SetBase(Config::NETPLAY_SYNC_CODES, m_sync_codes_action->isChecked());
  Config::SetBase(Config::NETPLAY_RECORD_INPUTS, m_record_input_action->isChecked());
  Config::SetBase(Config::NETPLAY_STRICT_SETTINGS_SYNC, m_strict_settings_sync_action->isChecked());
  Config::SetBase(Config::NETPLAY_GOLF_MODE_OVERLAY, m_golf_mode_overlay_action->isChecked());
  Config::SetBase(Config::NETPLAY_HIDE_REMOTE_GBAS, m_hide_remote_gbas_action->isChecked());

  // Stored as an empty string if, somehow, no mode is selected.
  std::string network_mode;
  if (m_fixed_delay_action->isChecked())
    network_mode = MODE_FIXED_DELAY;
  else if (m_host_input_authority_action->isChecked())
    network_mode = MODE_HOST_INPUT_AUTHORITY;
  else if (m_golf_mode_action->isChecked())
    network_mode = MODE_GOLF;
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, network_mode);
}

// ---- NetPlayUI: game digest and chunked transfers ----
// All of these arrive from the network thread and are replayed on the GUI thread.

void NetPlayDialog::ShowGameDigestDialog(const std::string& game_title)
{
  QueueOnObject(this, [this, caption = QString::fromStdString(game_title)] {
    // One digest at a time: the menu stays off until this run finishes or is aborted.
    m_game_digest_menu->setEnabled(false);
    if (m_game_digest_dialog->isVisible())
      m_game_digest_dialog->close();
    m_game_digest_dialog->show(caption);
  });
}

void NetPlayDialog::SetGameDigestProgress(int player_id, int percent)
{
  QueueOnObject(this, [this, player_id, percent] {
    // Late updates for a dialog that was closed meanwhile are dropped.
    if (!m_game_digest_dialog->isVisible())
      return;
    m_game_digest_dialog->SetProgress(player_id, percent);
  });
}

void NetPlayDialog::SetGameDigestResult(int player_id, const std::string& digest)
{
  QueueOnObject(this, [this, player_id, digest] {
    m_game_digest_dialog->SetResult(player_id, digest);
    m_game_digest_menu->setEnabled(true);
  });
}

void NetPlayDialog::AbortGameDigest()
{
  QueueOnObject(this, [this] {
    m_game_digest_dialog->close();
    // Allow starting another digest right away.
    m_game_digest_menu->setEnabled(true);
  });
}

void NetPlayDialog::ShowChunkedProgressDialog(const std::string& transfer_title, const u64 total,
                                              std::span<const int> pids)
{
  QueueOnObject(this, [this, transfer_title, total, pids] {
    // A transfer still on screen is finished off before the new one starts.
    if (m_chunked_progress_dialog->isVisible())
      m_chunked_progress_dialog->done(QDialog::Accepted);
    m_chunked_progress_dialog->show(QString::fromStdString(transfer_title), total, pids);
  });
}

void NetPlayDialog::HideChunkedProgressDialog()
{
  QueueOnObject(this, [this] { m_chunked_progress_dialog->done(QDialog::Accepted); });
}

void NetPlayDialog::SetChunkedProgress(const int player_id, const u64 bytes_done)
{
  QueueOnObject(this, [this, player_id, bytes_done] {
    if (!m_chunked_progress_dialog->isVisible())
      return;
    m_chunked_progress_dialog->SetProgress(player_id, bytes_done);
  });
}

void NetPlayDialog::SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder)
{
  if (const auto net_client = Settings::Instance().GetNetPlayClient())
    net_client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
}
