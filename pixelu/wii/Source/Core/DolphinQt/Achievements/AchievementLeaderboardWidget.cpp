// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementLeaderboardWidget.h"

#include <array>
#include <mutex>

#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

#include "Common/CommonTypes.h"
#include "Core/AchievementManager.h"

#include "DolphinQt/QtUtils/ClearLayoutRecursively.h"

namespace
{
// Each leaderboard row shows this many entries, in grid columns 1 to 4.
constexpr size_t SHOWN_ENTRIES = 4;

QFrame* Rule()
{
  auto* rule = new QFrame();
  rule->setFrameShape(QFrame::HLine);
  return rule;
}

QLabel* WrappingLabel(const char* utf8)
{
  auto* label = new QLabel(QString::fromUtf8(utf8));
  label->setWordWrap(true);
  label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  return label;
}
}  // namespace

AchievementLeaderboardWidget::AchievementLeaderboardWidget(QWidget* parent) : QWidget(parent)
{
  m_common_box = new QGroupBox();
  m_common_layout = new QGridLayout();
  m_common_box->setLayout(m_common_layout);

  auto* outer = new QVBoxLayout;
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setAlignment(Qt::AlignTop);
  outer->addWidget(m_common_box);
  outer->setSizeConstraint(QLayout::SetFixedSize);
  setLayout(outer);
}

void AchievementLeaderboardWidget::BuildGrid()
{
  rc_client_leaderboard_list_t* list = rc_client_create_leaderboard_list(
      AchievementManager::GetInstance().GetClient(), RC_CLIENT_LEADERBOARD_LIST_GROUPING_TRACKING);

  // Every bucket heading and every leaderboard takes two grid rows: a rule above, then content.
  int row = 0;
  for (u32 b = 0; b < list->num_buckets; ++b)
  {
    const rc_client_leaderboard_bucket_t& bucket = list->buckets[b];
    m_common_layout->addWidget(new QLabel(tr(bucket.label)), row, 0);
    row += 2;

    for (u32 l = 0; l < bucket.num_leaderboards; ++l)
    {
      const rc_client_leaderboard_t* board = bucket.leaderboards[l];
      m_leaderboard_order[board->id] = row;

      auto* about = new QVBoxLayout();
      about->addWidget(WrappingLabel(board->title));
      about->addWidget(WrappingLabel(board->description));
      m_common_layout->addWidget(Rule(), row - 1, 0);
      m_common_layout->addLayout(about, row, 0);

      // Placeholders until the entries arrive.
      for (int column = 1; column <= static_cast<int>(SHOWN_ENTRIES); ++column)
      {
        auto* entry = new QVBoxLayout();
        for (int line = 0; line < 3; ++line)
          entry->addWidget(new QLabel(QStringLiteral("---")));
        m_common_layout->addWidget(Rule(), row - 1, column);
        m_common_layout->addLayout(entry, row, column);
      }
      row += 2;
    }
  }

  rc_client_destroy_leaderboard_list(list);
}

void AchievementLeaderboardWidget::UpdateData(bool clean_all)
{
  if (clean_all)
  {
    ClearLayoutRecursively(m_common_layout);
    if (!AchievementManager::GetInstance().IsGameLoaded())
      return;
    BuildGrid();
  }

  for (const auto& [id, row] : m_leaderboard_order)
    UpdateRow(id);
}

void AchievementLeaderboardWidget::UpdateData(
    const std::set<AchievementManager::AchievementId>& update_ids)
{
  for (const auto& [id, row] : m_leaderboard_order)
  {
    if (update_ids.contains(id))
      UpdateRow(id);
  }
}

void AchievementLeaderboardWidget::UpdateRow(AchievementManager::AchievementId leaderboard_id)
{
  const auto known = m_leaderboard_order.find(leaderboard_id);
  if (known == m_leaderboard_order.end())
    return;
  const int row = known->second;

  const AchievementManager::LeaderboardStatus* status;
  {
    auto& manager = AchievementManager::GetInstance();
    std::lock_guard guard{manager.GetLock()};
    status = manager.GetLeaderboardInfo(leaderboard_id);
  }
  if (!status)
    return;

  // Which ranks to show. Normally first place, then the player with the entries just above and
  // below. Players without a score or in the top three see the top four; the last-placed player
  // appears in the fourth column under the two above them. Ranks with no entry keep their
  // placeholder.
  std::array<u32, SHOWN_ENTRIES> ranks{1, 2, 3, 4};
  if (status->player_index > SHOWN_ENTRIES - 1)
  {
    // One more if there is someone below the player.
    const u32 below = static_cast<u32>(status->entries.count(status->player_index + 1));
    for (u32 column = 1; column < SHOWN_ENTRIES; ++column)
      ranks[column] = status->player_index - 3 + below + column;
  }

  for (size_t column = 0; column < SHOWN_ENTRIES; ++column)
  {
    const auto entry = status->entries.find(ranks[column]);
    if (entry == status->entries.end())
      continue;

    auto* cell = new QVBoxLayout();
    cell->addWidget(new QLabel(tr("Rank %1").arg(entry->second.rank)));
    cell->addWidget(new QLabel(QString::fromStdString(entry->second.username)));
    cell->addWidget(new QLabel(QString::fromUtf8(entry->second.score.data())));

    const int grid_column = static_cast<int>(column) + 1;
    QLayoutItem* previous = m_common_layout->itemAtPosition(row, grid_column);
    m_common_layout->removeItem(previous);
    ClearLayoutRecursively(static_cast<QLayout*>(previous));
    m_common_layout->addLayout(cell, row, grid_column);
  }
}

#endif  // USE_RETRO_ACHIEVEMENTS
