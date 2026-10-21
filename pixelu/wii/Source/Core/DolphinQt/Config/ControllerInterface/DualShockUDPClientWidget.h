// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QStringList>
#include <QWidget>

class QCheckBox;
class QListWidget;
class QPushButton;

// The DSU Client tab: the enable switch and the server list.
class DualShockUDPClientWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit DualShockUDPClientWidget();

signals:
  // The server list was reloaded after a change.
  void ConfigChanged();

private:
  void ReloadServers();
  void RefreshButtons();
  void EditServer(bool existing);
  void RemoveSelectedServer();

  QCheckBox* m_servers_enabled;
  QListWidget* m_server_list;
  QPushButton* m_add_server;
  QPushButton* m_edit_server;
  QPushButton* m_remove_server;
};
