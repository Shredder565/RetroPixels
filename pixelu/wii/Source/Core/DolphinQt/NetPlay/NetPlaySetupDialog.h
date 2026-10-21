// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "DolphinQt/GameList/GameListModel.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QGridLayout;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace UICommon
{
class GameFile;
}

// The dialog shown before a NetPlay session: a "Connect" tab for joining someone else's game and
// a "Host" tab for starting one. Every edit is written back to the NetPlay config immediately.
class NetPlaySetupDialog : public QDialog
{
  Q_OBJECT
public:
  explicit NetPlaySetupDialog(const GameListModel& game_list_model, QWidget* parent);

  void accept() override;
  void show();

signals:
  bool Join();
  bool Host(const UICommon::GameFile& game);

private:
  void CreateMainLayout();
  QWidget* CreateConnectTab();
  QWidget* CreateHostTab();
  void LoadSettings();
  void ConnectWidgets();
  void PopulateGameList();
  void ResetTraversalHost();

  void SaveSettings();

  void OnConnectionTypeChanged(int index);
  bool CheckHostRequirements();

  // Shared by both tabs
  QDialogButtonBox* m_button_box = nullptr;
  QComboBox* m_connection_type = nullptr;
  QLineEdit* m_nickname_edit = nullptr;
  QGridLayout* m_main_layout = nullptr;
  QTabWidget* m_tab_widget = nullptr;
  QPushButton* m_reset_traversal_button = nullptr;

  // "Connect" tab
  QLabel* m_ip_label = nullptr;
  QLineEdit* m_ip_edit = nullptr;
  QLabel* m_connect_port_label = nullptr;
  QSpinBox* m_connect_port_box = nullptr;
  QPushButton* m_connect_button = nullptr;

  // "Host" tab
  QLabel* m_host_port_label = nullptr;
  QSpinBox* m_host_port_box = nullptr;
  QListWidget* m_host_games = nullptr;
  QPushButton* m_host_button = nullptr;
  QCheckBox* m_host_force_port_check = nullptr;
  QSpinBox* m_host_force_port_box = nullptr;
  QCheckBox* m_host_chunked_upload_limit_check = nullptr;
  QSpinBox* m_host_chunked_upload_limit_box = nullptr;
  QCheckBox* m_host_server_browser = nullptr;
  QLineEdit* m_host_server_name = nullptr;
  QLineEdit* m_host_server_password = nullptr;
  QComboBox* m_host_server_region = nullptr;

#ifdef USE_UPNP
  QCheckBox* m_host_upnp = nullptr;
#endif

  const GameListModel& m_game_list_model;
};
