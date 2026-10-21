// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class WiimoteControllersWidget;

// The Controllers page of the settings window: GameCube ports, Wii Remotes
// and the options shared by both.
class ControllersPane final : public QWidget
{
  Q_OBJECT
public:
  ControllersPane();

private:
  WiimoteControllersWidget* m_wiimote_controllers;
};
