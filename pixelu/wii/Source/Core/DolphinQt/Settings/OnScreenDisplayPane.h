// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include <QWidget>

#include "Common/Config/Config.h"

class ConfigBool;
class QGridLayout;
class QGroupBox;

// On-screen display settings page. (No Q_OBJECT: its translations use the QWidget context.)
class OnScreenDisplayPane final : public QWidget
{
public:
  explicit OnScreenDisplayPane(QWidget* parent = nullptr);

private:
  ConfigBool* AddToggle(QGridLayout* grid, int row, int column, const QString& label,
                        const Config::Info<bool>& setting, const QString& description);

  QGroupBox* BuildGeneral();
  QGroupBox* BuildPerformance();
  QGroupBox* BuildMovie();
  QGroupBox* BuildNetPlay();
  QGroupBox* BuildDebug();

  // Options that only matter while the movie window is shown.
  ConfigBool* m_movie_window = nullptr;
  std::array<ConfigBool*, 5> m_movie_details{};
};
