// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <QDialog>
#include <QMenuBar>

#include "Common/Lazy.h"
#include "Core/NetPlayClient.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "DolphinQt/NetPlay/ClickBlurLabel.h"
#include "VideoCommon/OnScreenDisplay.h"

class BootSessionData;
class ChunkedProgressDialog;
class GameDigestDialog;
class PadMappingDialog;
class QCheckBox;
class QComboBox;
class QGridLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTableWidget;
class QTextEdit;

namespace Core
{
enum class State;
}

namespace NetPlay
{
class NetPlayServer;
}

// The lobby window of a NetPlay session: chat on the left, the player list and host code on the
// right, and (for the host) the session options in the menu bar. It is also the NetPlayUI that
// the NetPlay client reports to; those callbacks may arrive on the network thread, so anything
// touching widgets is queued onto the GUI thread.
class NetPlayDialog : public QDialog, public NetPlay::NetPlayUI
{
  Q_OBJECT
public:
  using StartGameCallback = std::function<void(const std::string& path,
                                               std::unique_ptr<BootSessionData> boot_session_data)>;

  explicit NetPlayDialog(const GameListModel& game_list_model,
                         StartGameCallback start_game_callback, QWidget* parent = nullptr);
  ~NetPlayDialog() override;

  void show(std::string nickname, bool use_traversal);
  void reject() override;

  // NetPlayUI methods
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override;
  void StopGame() override;
  bool IsHosting() const override;

  void Update() override;
  void AppendChat(const std::string& msg) override;

  void OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                       const std::string& netplay_name) override;
  void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) override;
  void OnMsgStartGame() override;
  void OnMsgStopGame() override;
  void OnMsgPowerButton() override;
  void OnPlayerConnect(const std::string& player) override;
  void OnPlayerDisconnect(const std::string& player) override;
  void OnPadBufferChanged(u32 buffer) override;
  void OnHostInputAuthorityChanged(bool enabled) override;
  void OnDesync(u32 frame, const std::string& player) override;
  void OnConnectionLost() override;
  void OnConnectionError(const std::string& message) override;
  void OnTraversalError(Common::TraversalClient::FailureReason error) override;
  void OnTraversalStateChanged(Common::TraversalClient::State state) override;
  void OnGameStartAborted() override;
  void OnGolferChanged(bool is_golfer, const std::string& golfer_name) override;
  void OnTtlDetermined(u8 ttl) override;

  void OnIndexAdded(bool success, const std::string error) override;
  void OnIndexRefreshFailed(const std::string error) override;

  bool IsRecording() override;
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found = nullptr) override;
  std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                             int device_number) override;

  void LoadSettings();
  void SaveSettings();

  void ShowGameDigestDialog(const std::string& title) override;
  void SetGameDigestProgress(int pid, int progress) override;
  void SetGameDigestResult(int pid, const std::string& result) override;
  void AbortGameDigest() override;

  void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                 std::span<const int> players) override;
  void HideChunkedProgressDialog() override;
  void SetChunkedProgress(int pid, u64 progress) override;

  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override;

signals:
  void Stop();

private:
  void CreateChatLayout();
  void CreatePlayersLayout();
  void CreateMainLayout();
  void CreateDataMenu();
  void CreateNetworkMenu();
  void CreateChecksumMenu();
  void CreateOtherMenu();
  void ConnectWidgets();
  void ConnectSettingsSaving();
  void OnChat();
  void OnStart();
  void OnEmulationStateChanged(Core::State state);
  void OnChooseGame();
  void OnAssignPorts();
  void DisplayMessage(const QString& msg, const std::string& color,
                      int duration = OSD::Duration::NORMAL);
  void ResetExternalIP();
  void UpdateDiscordPresence();
  void UpdateGUI();
  void FillPlayerTable(NetPlay::NetPlayClient& net_client,
                       const std::vector<const NetPlay::Player*>& players);
  void UpdateHostCode(NetPlay::NetPlayServer& net_server);
  void ShowCopyableHostCode();
  void GameStatusChanged(bool running);
  void SetOptionsEnabled(bool enabled);

  // Sends a line of chat and echoes it locally. (Not called SendMessage, which is a Win32 macro.)
  void SendChatLine(const std::string& message);

  // Chat
  QGroupBox* m_chat_box = nullptr;
  QTextEdit* m_chat_edit = nullptr;
  QLineEdit* m_chat_type_edit = nullptr;
  QPushButton* m_chat_send_button = nullptr;

  // Players
  QGroupBox* m_players_box = nullptr;
  QComboBox* m_room_box = nullptr;
  ClickBlurLabel* m_hostcode_label = nullptr;
  QPushButton* m_hostcode_action_button = nullptr;
  QTableWidget* m_players_list = nullptr;
  QPushButton* m_kick_button = nullptr;
  QPushButton* m_assign_ports_button = nullptr;

  // Menus and session controls
  QMenuBar* m_menu_bar = nullptr;
  QMenu* m_data_menu = nullptr;
  QMenu* m_network_menu = nullptr;
  QMenu* m_game_digest_menu = nullptr;
  QMenu* m_other_menu = nullptr;
  QPushButton* m_game_button = nullptr;
  QPushButton* m_start_button = nullptr;
  QLabel* m_buffer_label = nullptr;
  QSpinBox* m_buffer_size_box = nullptr;

  QActionGroup* m_savedata_style_group = nullptr;
  QAction* m_savedata_none_action = nullptr;
  QAction* m_savedata_load_only_action = nullptr;
  QAction* m_savedata_load_and_write_action = nullptr;
  QAction* m_savedata_all_wii_saves_action = nullptr;

  QAction* m_sync_codes_action = nullptr;
  QAction* m_record_input_action = nullptr;
  QAction* m_strict_settings_sync_action = nullptr;
  QAction* m_host_input_authority_action = nullptr;
  QAction* m_golf_mode_action = nullptr;
  QAction* m_golf_mode_overlay_action = nullptr;
  QAction* m_fixed_delay_action = nullptr;
  QAction* m_hide_remote_gbas_action = nullptr;
  QPushButton* m_quit_button = nullptr;
  QSplitter* m_splitter = nullptr;
  QActionGroup* m_network_mode_group = nullptr;

  QGridLayout* m_main_layout = nullptr;
  GameDigestDialog* m_game_digest_dialog = nullptr;
  ChunkedProgressDialog* m_chunked_progress_dialog = nullptr;
  PadMappingDialog* m_pad_mapping = nullptr;
  NetPlay::SyncIdentifier m_current_game_identifier;
  std::string m_current_game_name;
  Common::Lazy<std::string> m_external_ip_address;
  std::string m_nickname;
  const GameListModel& m_game_list_model;
  bool m_use_traversal = false;
  bool m_is_copy_button_retry = false;
  bool m_got_stop_request = true;
  int m_buffer_size = 0;
  int m_player_count = 0;
  int m_old_player_count = 0;
  bool m_host_input_authority = false;

  StartGameCallback m_start_game_callback;
};
