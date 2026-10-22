// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#ifdef USE_RETRO_ACHIEVEMENTS
class HardcoreWarningWidget;
#endif  // USE_RETRO_ACHIEVEMENTS
class QDialogButtonBox;

// Dialog wrapping FreeLookWidget with a Close button.
class FreeLookWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit FreeLookWindow(QWidget* parent);

#ifdef USE_RETRO_ACHIEVEMENTS
signals:
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS

private:
#ifdef USE_RETRO_ACHIEVEMENTS
  HardcoreWarningWidget* m_hc_warning;
#endif  // USE_RETRO_ACHIEVEMENTS
  QDialogButtonBox* m_button_box;
};
