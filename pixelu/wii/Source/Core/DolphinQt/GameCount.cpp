// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/GameCount.h"

#include <QFontMetrics>
#include <QLayout>
#include <QStatusBar>

GameCount::GameCount(QWidget* const parent) : QStatusBar(parent)
{
  setSizeGripEnabled(false);

  // A small margin that scales with the font.
  const int margin = QFontMetrics(font()).height() / 5;
  layout()->setContentsMargins(margin, margin, margin, margin);
}

void GameCount::OnGameCountUpdated(const int total_games, const int visible_games)
{
  const int hidden = total_games - visible_games;
  if (hidden <= 0)
  {
    showMessage(tr("%1 game(s) in your collection").arg(total_games));
    return;
  }

  showMessage(tr("%1 game(s) in your collection (%2 visible, %3 filtered)")
                  .arg(total_games)
                  .arg(visible_games)
                  .arg(hidden));
}
