// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <memory>
#include <string_view>

#include "Common/WindowSystemInfo.h"

// What dolphin-emu-nogui runs the emulator in: where the picture goes (no window at all for the
// headless platform) and what drives the host thread while a game runs.
class Platform
{
public:
  virtual ~Platform() = default;

  // Prepares the platform (opens its window, for platforms that have one). False on failure.
  virtual bool Init() { return true; }

  // What the video backend and the input code get.
  virtual WindowSystemInfo GetWindowSystemInfo() const = 0;

  // Runs the host thread until emulation has ended: dispatches the core's host jobs and stops
  // emulation once RequestShutdown() was called.
  virtual void MainLoop() = 0;

  // Asks MainLoop() to stop emulation and return. Safe from any thread and from a signal handler.
  void RequestShutdown() { m_shutdown_requested.store(true, std::memory_order_relaxed); }
  bool IsShutdownRequested() const { return m_shutdown_requested.load(std::memory_order_relaxed); }

  // Called by Host_Message(WMUserJobDispatch); wakes MainLoop() early. Any thread.
  virtual void JobsQueued() {}

  // Called by Host_UpdateTitle.
  virtual void SetTitle(std::string_view title) {}

  // The headless platform: nothing on screen, for the Null video backend, the software renderer's
  // frame dumps, tests and servers.
  static std::unique_ptr<Platform> CreateHeadless();

private:
  std::atomic<bool> m_shutdown_requested{false};
};
