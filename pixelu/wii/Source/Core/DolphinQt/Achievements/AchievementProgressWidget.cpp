// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementProgressWidget.h"

#include <cstring>
#include <memory>

#include <QGroupBox>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

#include "Core/AchievementManager.h"

#include "DolphinQt/Achievements/AchievementBox.h"
#include "DolphinQt/QtUtils/ClearLayoutRecursively.h"

AchievementProgressWidget::AchievementProgressWidget(QWidget* parent) : QWidget(parent)
{
  m_common_box = new QGroupBox();
  m_common_layout = new QVBoxLayout();
  m_common_box->setLayout(m_common_layout);

  auto* outer = new QVBoxLayout;
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setAlignment(Qt::AlignTop);
  outer->addWidget(m_common_box);
  outer->setSizeConstraint(QLayout::SetFixedSize);
  setLayout(outer);
}

void AchievementProgressWidget::DetachEntries()
{
  while (QLayoutItem* item = m_common_layout->takeAt(0))
  {
    QWidget* entry = item->widget();
    m_common_layout->removeWidget(entry);
    // Only the headings are plain labels; boxes are owned by m_achievement_boxes.
    if (std::strcmp(entry->metaObject()->className(), "QLabel") == 0)
    {
      entry->deleteLater();
      delete item;
    }
  }
}

void AchievementProgressWidget::UpdateData(bool clean_all)
{
  if (clean_all)
  {
    m_achievement_boxes.clear();
    ClearLayoutRecursively(m_common_layout);
  }
  else
  {
    DetachEntries();
  }

  auto& manager = AchievementManager::GetInstance();
  if (!manager.IsGameLoaded())
    return;

  rc_client_achievement_list_t* list = rc_client_create_achievement_list(
      manager.GetClient(), RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
      RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  if (!list)
    return;

  for (u32 b = 0; b < list->num_buckets; ++b)
  {
    const rc_client_achievement_bucket_t& bucket = list->buckets[b];
    m_common_layout->addWidget(new QLabel(tr(bucket.label)));

    for (u32 a = 0; a < bucket.num_achievements; ++a)
    {
      const rc_client_achievement_t* achievement = bucket.achievements[a];

      // Reuse the box made for this achievement earlier, with its progress brought up to date.
      auto slot = m_achievement_boxes.lower_bound(achievement->id);
      if (slot != m_achievement_boxes.end() && slot->first == achievement->id)
      {
        slot->second->UpdateProgress();
      }
      else
      {
        slot = m_achievement_boxes.try_emplace(slot, achievement->id,
                                               std::make_shared<AchievementBox>(this, achievement));
      }
      m_common_layout->addWidget(slot->second.get());
    }
  }

  rc_client_destroy_achievement_list(list);
}

void AchievementProgressWidget::UpdateData(
    const std::set<AchievementManager::AchievementId>& update_ids)
{
  for (const auto& [id, box] : m_achievement_boxes)
  {
    if (update_ids.contains(id))
      box->UpdateData();
  }
}

#endif  // USE_RETRO_ACHIEVEMENTS
