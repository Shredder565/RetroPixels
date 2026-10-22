// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QGroupBox;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;

namespace Core
{
class System;
}

// Dock listing the emulated sockets (Wii IOS or Triforce) and SSL contexts with what the host
// knows about them, plus the network dump and certificate options.
class NetworkWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit NetworkWidget(QWidget* parent = nullptr);
  ~NetworkWidget() override;

protected:
  void closeEvent(QCloseEvent*) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildLayout();
  void WireUp();
  QGroupBox* BuildSocketBox();
  QGroupBox* BuildSslBox();
  QGroupBox* BuildDumpBox();
  QGroupBox* BuildSecurityBox();

  void Refresh();
  void FillWiiTables(Core::System& system);
  void FillTriforceTable();
  void DumpFormatPicked(int index);

  QTableWidget* m_sockets = nullptr;
  QTableWidget* m_ssl_contexts = nullptr;
  QComboBox* m_dump_format = nullptr;
  QCheckBox* m_dump_ssl_read = nullptr;
  QCheckBox* m_dump_ssl_write = nullptr;
  QCheckBox* m_dump_root_ca = nullptr;
  QCheckBox* m_dump_peer_cert = nullptr;
  QCheckBox* m_verify_certs = nullptr;
  QCheckBox* m_dump_bba = nullptr;
  QPushButton* m_open_folder = nullptr;
};
