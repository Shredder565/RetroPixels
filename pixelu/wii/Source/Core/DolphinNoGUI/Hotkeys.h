// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <functional>
#include <thread>

// The emulation hotkeys of PixelU-Wii's own game window (Hotkeys.ini): stop, fullscreen, pause,
// reset, screenshots, save states, volume, speed, internal resolution, aspect ratio and the Wii
// Remote connect keys. A thread samples them every few milliseconds; anything that touches the
// running game is queued as a host job, so it runs on the main loop's thread.
class Hotkeys
{
public:
  struct Actions
  {
    // Called on the hotkey thread; must be safe from there (post to the window's thread).
    std::function<void()> toggle_fullscreen;
    std::function<void()> stop;
  };

  explicit Hotkeys(Actions actions);
  ~Hotkeys();

  Hotkeys(const Hotkeys&) = delete;
  Hotkeys& operator=(const Hotkeys&) = delete;

private:
  void Run();
  void Poll();
  void SelectSlot(int slot);

  Actions m_actions;
  std::atomic<bool> m_quit{false};
  int m_slot = 1;
  int m_frame_step_hold = 0;
  std::thread m_thread;
};
