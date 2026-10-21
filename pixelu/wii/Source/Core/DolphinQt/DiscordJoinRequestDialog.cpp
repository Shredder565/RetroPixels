// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_DISCORD_PRESENCE

#include "DolphinQt/DiscordJoinRequestDialog.h"

#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>

#include <discord_rpc.h>
#include <fmt/format.h>

#include "Common/HttpRequest.h"

namespace
{
// The user's Discord avatar, or a null pixmap if there is none or it can't be fetched.
QPixmap FetchAvatar(const std::string& user_id, const std::string& avatar)
{
  QPixmap picture;
  if (avatar.empty())
    return picture;

  Common::HttpRequest http;
  const auto reply =
      http.Get(fmt::format("https://cdn.discordapp.com/avatars/{}/{}.png", user_id, avatar));
  if (reply.has_value())
    picture.loadFromData(reply->data(), static_cast<uint>(reply->size()), "png");
  return picture;
}
}  // namespace

DiscordJoinRequestDialog::DiscordJoinRequestDialog(QWidget* parent, const std::string& id,
                                                   const std::string& discord_tag,
                                                   const std::string& avatar)
    : QDialog(parent), m_user_id(id), m_expires_at(std::time(nullptr) + s_max_lifetime_seconds)
{
  setWindowTitle(tr("Request to Join Your Party"));

  BuildLayout(discord_tag, FetchAvatar(id, avatar));

  connect(m_invite, &QPushButton::clicked, [this] { Answer(DISCORD_REPLY_YES); });
  connect(m_decline, &QPushButton::clicked, [this] { Answer(DISCORD_REPLY_NO); });
  connect(m_ignore, &QPushButton::clicked, [this] { Answer(DISCORD_REPLY_IGNORE); });
  // Closing the window any other way counts as ignoring the request.
  connect(this, &QDialog::rejected, [this] { Answer(DISCORD_REPLY_IGNORE); });
}

std::time_t DiscordJoinRequestDialog::GetCloseTimestamp() const
{
  return m_expires_at;
}

void DiscordJoinRequestDialog::BuildLayout(const std::string& discord_tag, const QPixmap& avatar)
{
  m_layout = new QGridLayout;

  m_invite = new QPushButton(tr("✔ Invite"));
  m_decline = new QPushButton(tr("✖ Decline"));
  m_ignore = new QPushButton(tr("Ignore"));

  auto* const message =
      new QLabel(tr("%1\nwants to join your party.").arg(QString::fromStdString(discord_tag)));
  message->setAlignment(Qt::AlignCenter);

  // Picture on top (if any), the message below it, then a row of three buttons.
  if (!avatar.isNull())
  {
    auto* const picture = new QLabel();
    picture->setPixmap(avatar);
    m_layout->addWidget(picture, 1, 0, 1, 3, Qt::AlignHCenter);
  }
  m_layout->addWidget(message, 2, 0, 3, 3, Qt::AlignHCenter);

  int column = 0;
  for (QPushButton* const button : {m_invite, m_decline, m_ignore})
    m_layout->addWidget(button, 8, column++);

  setLayout(m_layout);
}

void DiscordJoinRequestDialog::Answer(int reply)
{
  Discord_Respond(m_user_id.c_str(), reply);
  close();
}

#endif
