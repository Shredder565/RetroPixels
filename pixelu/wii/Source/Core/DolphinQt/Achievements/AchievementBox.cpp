// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementBox.h"

#include <cstring>
#include <mutex>
#include <string_view>

#include <QByteArray>
#include <QDateTime>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/AchievementManager.h"

#include "DolphinQt/QtUtils/FromStdString.h"

namespace
{
// rc_client keeps the measured progress text in a buffer of this size.
constexpr size_t PROGRESS_TEXT_SIZE = 24;

QLabel* WrappingLabel(const char* utf8)
{
  auto* label = new QLabel(QString::fromUtf8(utf8, static_cast<qsizetype>(std::strlen(utf8))));
  label->setWordWrap(true);
  label->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
  return label;
}
}  // namespace

AchievementBox::AchievementBox(QWidget* parent, const rc_client_achievement_t* achievement)
    : QGroupBox(parent), m_achievement(achievement)
{
  // Without a game there is nothing to show (and the achievement data is gone).
  if (!AchievementManager::GetInstance().IsGameLoaded())
    return;

  m_badge = new QLabel();
  QLabel* title = WrappingLabel(achievement->title);
  QLabel* description = WrappingLabel(achievement->description);
  auto* points = new QLabel(tr("%1 points").arg(achievement->points));
  m_status = new QLabel();

  // The bar keeps its room when hidden, so boxes line up whether or not they track progress.
  // The progress text sits on top of it.
  m_progress_bar = new QProgressBar();
  QSizePolicy keep_space = m_progress_bar->sizePolicy();
  keep_space.setRetainSizeWhenHidden(true);
  m_progress_bar->setSizePolicy(keep_space);
  m_progress_label = new QLabel();
  m_progress_label->setStyleSheet(QStringLiteral("background-color:transparent;"));
  m_progress_label->setAlignment(Qt::AlignCenter);

  // Badge on the left, centred vertically; the text on the right.
  auto* badge_column = new QVBoxLayout();
  badge_column->addSpacerItem(new QSpacerItem(0, 0));
  badge_column->addWidget(m_badge);
  badge_column->addSpacerItem(new QSpacerItem(0, 0));
  badge_column->setSizeConstraint(QLayout::SetFixedSize);
  badge_column->setAlignment(Qt::AlignCenter);

  auto* text_column = new QVBoxLayout();
  for (QWidget* line : {static_cast<QWidget*>(title), static_cast<QWidget*>(description),
                        static_cast<QWidget*>(points), static_cast<QWidget*>(m_status),
                        static_cast<QWidget*>(m_progress_bar)})
  {
    text_column->addWidget(line);
  }

  auto* over_bar = new QVBoxLayout(m_progress_bar);
  over_bar->setContentsMargins(0, 0, 0, 0);
  over_bar->addWidget(m_progress_label);

  auto* columns = new QHBoxLayout();
  columns->addLayout(badge_column);
  columns->addLayout(text_column);
  setLayout(columns);

  UpdateData();
}

void AchievementBox::UpdateData()
{
  {
    auto& manager = AchievementManager::GetInstance();
    std::lock_guard guard{manager.GetLock()};
    if (!manager.IsGameLoaded())
      return;

    const auto& badge = manager.GetAchievementBadge(m_achievement->id, !m_achievement->unlocked);

    // The frame shows how it was unlocked: gold for hardcore, blue for softcore.
    std::string_view frame = AchievementManager::GRAY;
    if (m_achievement->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE)
      frame = AchievementManager::GOLD;
    else if (m_achievement->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_SOFTCORE)
      frame = AchievementManager::BLUE;

    const QImage image(badge.data.data(), static_cast<int>(badge.width),
                       static_cast<int>(badge.height), QImage::Format_RGBA8888);
    m_badge->setPixmap(
        QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_badge->adjustSize();
    m_badge->setStyleSheet(
        QStringLiteral("border: 4px solid %1").arg(QtUtils::FromStdString(frame)));

    if (!m_achievement->unlocked)
    {
      m_status->setText(tr("Locked"));
    }
    else if (m_achievement->unlock_time == 0)
    {
      m_status->setText(tr("Unlocked"));
    }
    else
    {
      // i18n: %1 is a date/time.
      m_status->setText(
          tr("Unlocked at %1")
              .arg(QDateTime::fromSecsSinceEpoch(m_achievement->unlock_time).toString()));
    }
  }

  UpdateProgress();
}

void AchievementBox::UpdateProgress()
{
  auto& manager = AchievementManager::GetInstance();
  std::lock_guard guard{manager.GetLock()};
  if (!manager.IsGameLoaded())
    return;

  // Only measured achievements (those with a percentage) get the bar.
  if (!(m_achievement->measured_percent > 0.000))
  {
    m_progress_bar->setVisible(false);
    return;
  }

  m_progress_bar->setRange(0, 100);
  m_progress_bar->setValue(
      m_achievement->unlocked ? 100 : static_cast<int>(m_achievement->measured_percent));
  m_progress_bar->setTextVisible(false);
  const auto text_length =
      static_cast<qsizetype>(qstrnlen(m_achievement->measured_progress, PROGRESS_TEXT_SIZE));
  m_progress_label->setText(QString::fromUtf8(m_achievement->measured_progress, text_length));
  m_progress_label->setVisible(!m_achievement->unlocked);
  m_progress_bar->setVisible(true);
}

#endif  // USE_RETRO_ACHIEVEMENTS
