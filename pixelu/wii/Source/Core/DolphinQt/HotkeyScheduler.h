// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <thread>

#include <QObject>

#include "Common/CommonTypes.h"
#include "InputCommon/InputProfile.h"

namespace Core
{
class System;
}

// Polls the hotkey controller on a thread of its own (every 5 ms) and turns presses into signals
// for the GUI, or acts on them directly when no GUI is involved (graphics toggles, volume, ...).
// Signals are emitted from the polling thread, so receivers get them queued.
class HotkeyScheduler : public QObject
{
  Q_OBJECT
public:
  explicit HotkeyScheduler();
  ~HotkeyScheduler() override;

  void Start();
  void Stop();
signals:
  void Open();
  void EjectDisc();
  void ChangeDisc();

  void ExitHotkey();
  void UnlockCursor();
  void ActivateChat();
  void RequestGolfControl();
  void FullScreenHotkey();
  void StopHotkey();
  void ResetHotkey();
  void TogglePauseHotkey();
  void ScreenShotHotkey();
  void RefreshGameListHotkey();
  void SetStateSlotHotkey(int slot);
  void IncrementSelectedStateSlotHotkey();
  void DecrementSelectedStateSlotHotkey();
  void StateLoadSlotHotkey();
  void StateSaveSlotHotkey();
  void StateLoadSlot(int state);
  void StateSaveSlot(int state);
  void StateLoadLastSaved(int state);
  void StateSaveOldest();
  void StateLoadFile();
  void StateSaveFile();
  void StateLoadUndo();
  void StateSaveUndo();
  void StartRecording();
  void PlayRecording();
  void ExportRecording();
  void ToggleReadOnlyMode();
  void ConnectWiiRemote(int id);
#ifdef USE_RETRO_ACHIEVEMENTS
  void OpenAchievements();
#endif  // USE_RETRO_ACHIEVEMENTS

  void Step();
  void StepOver();
  void StepOut();
  void Skip();

  void ShowPC();
  void SetPC();

  void ToggleBreakpoint();
  void AddBreakpoint();

  void SkylandersPortalHotkey();
  void InfinityBaseHotkey();

private:
  using Signal = void (HotkeyScheduler::*)();
  struct SignalHotkey
  {
    int hotkey;
    Signal signal;
  };

  void PollLoop();

  // Emits the signal of every entry whose hotkey was just pressed, in table order.
  template <std::size_t N>
  void EmitPressed(const SignalHotkey (&table)[N]);

  // Hotkeys that only make sense with the core up. Returns false when no game is running, in
  // which case the rest of this poll is skipped.
  bool HandleCoreHotkeys(Core::System& system);
  void HandleSessionHotkeys(Core::System& system);
  void HandleWiiHotkeys();
  void HandleProfileHotkeys();
  void HandleDebuggerHotkeys();
  void HandleGBAHotkeys();
  // Hotkeys that are honoured even while the core is stopping.
  void HandleLateHotkeys();

  std::atomic<bool> m_quit{false};
  std::thread m_poller;

  InputProfile::ProfileCycler m_profiles;
};
