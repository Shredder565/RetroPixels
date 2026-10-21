// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

#include "Common/CommonTypes.h"
#include "Core/AchievementManager.h"

class AchievementBox;
class QCheckBox;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

struct rc_api_achievement_definition_t;

// The Progress tab: the game's achievements in rc_client's progress buckets, one box each.
class AchievementProgressWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit AchievementProgressWidget(QWidget* parent);
  // clean_all throws the boxes away and rebuilds them; otherwise existing boxes are reused.
  void UpdateData(bool clean_all);
  void UpdateData(const std::set<AchievementManager::AchievementId>& update_ids);

private:
  // Takes everything out of the list, deleting the bucket headings but keeping the boxes.
  void DetachEntries();

  QGroupBox* m_common_box;
  QVBoxLayout* m_common_layout;
  std::map<AchievementManager::AchievementId, std::shared_ptr<AchievementBox>> m_achievement_boxes;
};

#endif  // USE_RETRO_ACHIEVEMENTS
