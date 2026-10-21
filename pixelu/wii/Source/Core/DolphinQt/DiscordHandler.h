// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <list>
#include <mutex>
#include <thread>

#include <QObject>

#include "Common/Event.h"
#include "Common/Flag.h"

#include "UICommon/DiscordPresence.h"

class DiscordJoinRequestDialog;

// Pumps Discord's callbacks on a thread of its own and turns them into Qt signals and
// join request dialogs.
class DiscordHandler : public QObject, public Discord::Handler
{
  Q_OBJECT
#ifdef USE_DISCORD_PRESENCE
public:
  explicit DiscordHandler(QWidget* parent);
  ~DiscordHandler() override;

  void Start();
  void Stop();
  void DiscordJoin() override;
  void DiscordJoinRequest(const char* id, const std::string& discord_tag,
                          const char* avatar) override;
  void ShowNewJoinRequest(const std::string& id, const std::string& discord_tag,
                          const std::string& avatar);
#endif

signals:
  void Join();
  void JoinRequest(const std::string id, const std::string discord_tag, const std::string avatar);

#ifdef USE_DISCORD_PRESENCE
private:
  void PollLoop();
  void CloseExpiredRequests();

  QWidget* m_parent;
  Common::Flag m_quit;
  Common::Event m_wake;
  std::thread m_poller;
  // Open join requests, newest first. Guarded by m_requests_lock.
  std::list<DiscordJoinRequestDialog> m_requests;
  std::mutex m_requests_lock;
#endif
};
