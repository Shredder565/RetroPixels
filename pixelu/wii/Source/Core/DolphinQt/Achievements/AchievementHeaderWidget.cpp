// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementHeaderWidget.h"

#include <mutex>
#include <string_view>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QString>
#include <QVBoxLayout>

#include <rcheevos/include/rc_client.h>

#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"

#include "DolphinQt/QtUtils/FromStdString.h"

namespace
{
// Shows a badge image, scaled to 64x64, if there is one.
void ShowBadge(QLabel* icon, const AchievementManager::Badge& badge)
{
  if (badge.data.empty())
    return;
  const QImage image(badge.data.data(), static_cast<int>(badge.width),
                     static_cast<int>(badge.height), QImage::Format_RGBA8888);
  icon->setPixmap(
      QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

// Hides an icon and drops whatever it showed.
void ResetIcon(QLabel* icon)
{
  icon->setVisible(false);
  icon->clear();
  icon->setText({});
}

QString BorderStyle(std::string_view color)
{
  return QStringLiteral("border: 4px solid %1").arg(QtUtils::FromStdString(color));
}
}  // namespace

AchievementHeaderWidget::AchievementHeaderWidget(QWidget* parent) : QWidget(parent)
{
  m_user_icon = new QLabel();
  m_game_icon = new QLabel();
  m_name = new QLabel();
  m_points = new QLabel();
  m_game_progress = new QProgressBar();
  m_progress_label = new QLabel();
  m_rich_presence = new QLabel();

  for (QLabel* text : {m_name, m_points, m_rich_presence})
    text->setWordWrap(true);

  // The bar keeps its room when hidden; its "unlocked/total" text is drawn on top of it.
  QSizePolicy keep_space = m_game_progress->sizePolicy();
  keep_space.setRetainSizeWhenHidden(true);
  m_game_progress->setSizePolicy(keep_space);
  m_game_progress->setTextVisible(false);
  m_progress_label->setStyleSheet(QStringLiteral("background-color:transparent;"));
  m_progress_label->setAlignment(Qt::AlignCenter);

  auto* icons = new QVBoxLayout();
  icons->addWidget(m_user_icon);
  icons->addWidget(m_game_icon);

  auto* texts = new QVBoxLayout();
  texts->addWidget(m_name);
  texts->addWidget(m_points);
  texts->addWidget(m_game_progress);
  texts->addWidget(m_rich_presence);

  auto* over_bar = new QVBoxLayout(m_game_progress);
  over_bar->setContentsMargins(0, 0, 0, 0);
  over_bar->addWidget(m_progress_label);

  auto* columns = new QHBoxLayout();
  columns->addLayout(icons);
  columns->addLayout(texts);
  m_header_box = new QGroupBox();
  m_header_box->setLayout(columns);

  auto* outer = new QVBoxLayout();
  outer->addWidget(m_header_box);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setAlignment(Qt::AlignTop);
  setLayout(outer);
}

void AchievementHeaderWidget::UpdateData()
{
  auto& manager = AchievementManager::GetInstance();
  std::lock_guard guard{manager.GetLock()};

  if (!Config::Get(Config::RA_ENABLED) || !manager.HasAPIToken())
  {
    m_header_box->setVisible(false);
    return;
  }
  m_header_box->setVisible(true);

  const QString player = QtUtils::FromStdString(manager.GetPlayerDisplayName());
  const QString game = QtUtils::FromStdString(manager.GetGameDisplayName());
  const AchievementManager::Badge& player_badge = manager.GetPlayerBadge();
  const AchievementManager::Badge& game_badge = manager.GetGameBadge();

  ResetIcon(m_user_icon);
  ShowBadge(m_user_icon, player_badge);
  m_user_icon->adjustSize();
  m_user_icon->setStyleSheet(QStringLiteral("border: 4px solid transparent"));
  m_user_icon->setVisible(true);

  ResetIcon(m_game_icon);

  if (!manager.IsGameLoaded())
  {
    // Just the player and their score.
    m_name->setText(player);
    m_points->setText(tr("%1 points").arg(manager.GetPlayerScore()));
    m_game_progress->setVisible(false);
    m_progress_label->setVisible(false);
    m_rich_presence->setVisible(false);
    return;
  }

  rc_client_user_game_summary_t summary;
  rc_client_get_user_game_summary(manager.GetClient(), &summary);

  ShowBadge(m_game_icon, game_badge);
  m_game_icon->adjustSize();
  // A completed set gets a gold (hardcore) or blue (softcore) frame.
  std::string_view frame = AchievementManager::GRAY;
  if (summary.num_core_achievements == summary.num_unlocked_achievements)
    frame = manager.IsHardcoreModeActive() ? AchievementManager::GOLD : AchievementManager::BLUE;
  m_game_icon->setStyleSheet(BorderStyle(frame));
  m_game_icon->setVisible(true);

  m_name->setText(tr("%1 is playing %2").arg(player).arg(game));
  m_points->setText(tr("%1 has unlocked %2/%3 achievements worth %4/%5 points")
                        .arg(player)
                        .arg(summary.num_unlocked_achievements)
                        .arg(summary.num_core_achievements)
                        .arg(summary.points_unlocked)
                        .arg(summary.points_core));

  // A game without achievements shows an empty bar rather than a full one.
  const int total =
      summary.num_core_achievements == 0 ? 1 : static_cast<int>(summary.num_core_achievements);
  m_game_progress->setRange(0, total);
  m_game_progress->setVisible(true);
  m_game_progress->setValue(static_cast<int>(summary.num_unlocked_achievements));
  m_progress_label->setVisible(true);
  m_progress_label->setText(
      tr("%1/%2").arg(summary.num_unlocked_achievements).arg(summary.num_core_achievements));

  m_rich_presence->setText(QString::fromUtf8(manager.GetRichPresence().data()));
  m_rich_presence->setVisible(true);
}

#endif  // USE_RETRO_ACHIEVEMENTS
