// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;

// Settings for one port of the GameCube controller adapter (the Wii U / Switch USB adapter):
// rumble and DK Bongos simulation, plus a live adapter status and poll rate.
class GCPadWiiUConfigDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit GCPadWiiUConfigDialog(int port, QWidget* parent = nullptr);

private:
  void BuildLayout();
  void ReadConfig();
  void WriteConfig();
  void RefreshStatus();

  const int m_port;

  QLabel* m_status = nullptr;
  QLabel* m_poll_rate = nullptr;
  QCheckBox* m_rumble = nullptr;
  QCheckBox* m_bongos = nullptr;
};
