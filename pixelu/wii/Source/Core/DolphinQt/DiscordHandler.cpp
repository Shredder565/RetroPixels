// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_DISCORD_PRESENCE

#include "DolphinQt/DiscordHandler.h"

#include <chrono>
#include <ctime>

#include <QApplication>

#include "Common/Thread.h"

#include "UICommon/DiscordPresence.h"

#include "DolphinQt/DiscordJoinRequestDialog.h"
#include "DolphinQt/QtUtils/RunOnObject.h"

namespace
{
// How often the poller looks for Discord callbacks and expired requests.
constexpr auto POLL_INTERVAL = std::chrono::seconds{2};
}  // namespace

DiscordHandler::DiscordHandler(QWidget* parent) : QObject{parent}, m_parent{parent}
{
  // Join requests arrive on the poller thread; the dialog is made on the GUI thread.
  connect(this, &DiscordHandler::JoinRequest, this, &DiscordHandler::ShowNewJoinRequest);
}

DiscordHandler::~DiscordHandler()
{
  Stop();
}

void DiscordHandler::Start()
{
  m_quit.Set(false);
  m_poller = std::thread([this] { PollLoop(); });
}

void DiscordHandler::Stop()
{
  if (!m_poller.joinable())
    return;

  m_quit.Set(true);
  m_wake.Set();
  m_poller.join();
}

void DiscordHandler::DiscordJoinRequest(const char* id, const std::string& discord_tag,
                                        const char* avatar)
{
  emit JoinRequest(id, discord_tag, avatar);
}

void DiscordHandler::DiscordJoin()
{
  emit Join();
}

void DiscordHandler::ShowNewJoinRequest(const std::string& id, const std::string& discord_tag,
                                        const std::string& avatar)
{
  std::lock_guard lock(m_requests_lock);

  DiscordJoinRequestDialog& dialog = m_requests.emplace_front(m_parent, id, discord_tag, avatar);
  dialog.show();
  dialog.raise();
  dialog.activateWindow();

  // Flash the taskbar entry for as long as the request stays open.
  QApplication::alert(nullptr, DiscordJoinRequestDialog::s_max_lifetime_seconds * 1000);
}

void DiscordHandler::CloseExpiredRequests()
{
  std::lock_guard lock(m_requests_lock);

  auto request = m_requests.begin();
  while (request != m_requests.end())
  {
    if (std::time(nullptr) < request->GetCloseTimestamp())
    {
      ++request;
      continue;
    }

    // Dialogs belong to the GUI thread, so closing and destroying happen there; this thread
    // waits for it before going on.
    RunOnObject(m_parent, [this, &request] {
      request->close();
      request = m_requests.erase(request);
      return nullptr;
    });
  }
}

void DiscordHandler::PollLoop()
{
  Common::SetCurrentThreadName("DiscordHandler");

  while (!m_quit.IsSet())
  {
    Discord::CallPendingCallbacks();
    CloseExpiredRequests();
    m_wake.WaitFor(POLL_INTERVAL);
  }
}

#endif
