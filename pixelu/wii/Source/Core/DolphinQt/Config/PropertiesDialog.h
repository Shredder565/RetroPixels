// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "DolphinQt/Config/SettingsWindow.h"

namespace UICommon
{
class GameFile;
}

// The per-game Properties window opened from the game list.
class PropertiesDialog final : public StackedSettingsWindow
{
  Q_OBJECT
public:
  explicit PropertiesDialog(QWidget* parent, const UICommon::GameFile& game);
  const std::string& GetFilePath() const { return m_filepath; }

signals:
  void OpenGeneralSettings();
  void OpenGraphicsSettings();
#ifdef USE_RETRO_ACHIEVEMENTS
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

private:
  const std::string m_filepath;
};
