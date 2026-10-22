// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <ctime>
#include <string>

#include <QDialog>

class QGridLayout;
class QPixmap;
class QPushButton;

// "<user> wants to join your party": invite, decline or ignore a Discord join request.
class DiscordJoinRequestDialog : public QDialog
{
  Q_OBJECT
public:
  explicit DiscordJoinRequestDialog(QWidget* parent, const std::string& id,
                                    const std::string& discord_tag, const std::string& avatar);
  std::time_t GetCloseTimestamp() const;

  static constexpr std::time_t s_max_lifetime_seconds = 30;

private:
  void BuildLayout(const std::string& discord_tag, const QPixmap& avatar);
  void Answer(int reply);

  QGridLayout* m_layout = nullptr;
  QPushButton* m_invite = nullptr;
  QPushButton* m_decline = nullptr;
  QPushButton* m_ignore = nullptr;

  const std::string m_user_id;
  const std::time_t m_expires_at;
};
