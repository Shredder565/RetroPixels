// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QStatusBar>

// The bar under the game list that says how many games there are.
class GameCount : public QStatusBar
{
  Q_OBJECT
public:
  explicit GameCount(QWidget* parent = nullptr);

  void OnGameCountUpdated(int total_games, int visible_games);
};
