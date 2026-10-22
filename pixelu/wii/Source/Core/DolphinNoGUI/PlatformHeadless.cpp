// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinNoGUI/Platform.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "Core/Core.h"
#include "Core/System.h"

namespace
{
class HeadlessPlatform final : public Platform
{
public:
  WindowSystemInfo GetWindowSystemInfo() const override
  {
    // No display connection and no window: backends that need one fail to start, the Null and
    // Software backends run.
    return WindowSystemInfo{WindowSystemType::Headless, nullptr, nullptr, nullptr};
  }

  void MainLoop() override
  {
    Core::System& system = Core::System::GetInstance();
    bool stop_sent = false;

    while (true)
    {
      if (IsShutdownRequested() && !stop_sent)
      {
        // Core::Stop returns at once; the session winds down on its own threads, and the state
        // turns Uninitialized when it is over.
        Core::Stop(system);
        stop_sent = true;
      }

      // Also runs the jobs queued while stopping (run_during_stop).
      Core::HostDispatchJobs(system);

      // Over: stopped as asked, or ended on its own (the game failed to start, for example).
      if (Core::IsUninitialized(system))
        break;

      std::unique_lock lock(m_mutex);
      // Woken by new jobs; otherwise every few milliseconds to look at the state again.
      m_wake.wait_for(lock, std::chrono::milliseconds(10), [this] { return m_jobs_queued; });
      m_jobs_queued = false;
    }
  }

  void JobsQueued() override
  {
    {
      std::lock_guard lock(m_mutex);
      m_jobs_queued = true;
    }
    m_wake.notify_one();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_wake;
  bool m_jobs_queued = false;
};
}  // namespace

std::unique_ptr<Platform> Platform::CreateHeadless()
{
  return std::make_unique<HeadlessPlatform>();
}
