// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/HotkeyScheduler.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <fmt/format.h>

#include <QApplication>
#include <QCoreApplication>

#include "AudioCommon/AudioCommon.h"

#include "Common/Config/Config.h"
#include "Common/Thread.h"

#include "Core/AchievementManager.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/IOS.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#ifdef HAS_LIBMGBA
#include "DolphinQt/GBAWidget.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#endif
#include "DolphinQt/Settings.h"

#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
// The post-processing shader that implements anaglyph 3D.
constexpr const char* ANAGLYPH_SHADER = "dubois";

constexpr int POLL_INTERVAL_MS = 5;
// Pause after a fullscreen toggle, so one long press does not flip the window back and forth.
constexpr int FULLSCREEN_COOLDOWN_MS = 100;

bool Pressed(int hotkey)
{
  return HotkeyManagerEmu::IsPressed(hotkey, false);
}

bool Held(int hotkey)
{
  return HotkeyManagerEmu::IsPressed(hotkey, true);
}

// Flips a boolean setting for this session and returns the new value.
bool Toggle(const Config::Info<bool>& setting)
{
  const bool flipped = !Config::Get(setting);
  Config::SetCurrent(setting, flipped);
  return flipped;
}

// Frame advance. Tapping the hotkey steps one frame; holding it steps once, waits a while and
// then keeps stepping, one frame every `pace` polls (adjustable with the speed hotkeys).
class FrameAdvance
{
public:
  void Poll()
  {
    if (Pressed(HK_FRAME_ADVANCE_INCREASE_SPEED))
    {
      m_pace = std::max(m_pace - 1, 0);
    }
    else if (Pressed(HK_FRAME_ADVANCE_DECREASE_SPEED))
    {
      m_pace = std::min(m_pace + 1, SLOWEST_PACE);
    }
    else if (Pressed(HK_FRAME_ADVANCE_RESET_SPEED))
    {
      m_pace = 1;
    }
    else if (Held(HK_FRAME_ADVANCE))
    {
      WhileHeld();
    }
    else if (m_held_polls != 0)
    {
      Release();
    }
  }

private:
  static constexpr int SLOWEST_PACE = 60;
  // Polls the hotkey has to be held before stepping repeats.
  static constexpr int REPEAT_DELAY = 30;

  void WhileHeld()
  {
    if (m_stepped && m_polls_since_step < m_pace)
      ++m_polls_since_step;

    const bool may_step = m_held_polls == 0 || m_held_polls == REPEAT_DELAY;
    if (may_step && !m_stepped)
    {
      // Every step after the first one is part of a continuous run.
      if (m_held_polls != 0)
        Settings::Instance().SetIsContinuouslyFrameStepping(true);
      Core::QueueHostJob([](Core::System& system) { Core::DoFrameStep(system); });
      m_stepped = true;
    }

    if (m_held_polls < REPEAT_DELAY)
    {
      // Still in the initial delay: the step above was the only one.
      ++m_held_polls;
      m_stepped = false;
      return;
    }

    // Repeating: allow the next step once enough polls have passed.
    if (m_stepped && m_polls_since_step >= m_pace)
    {
      m_stepped = false;
      m_polls_since_step = 0;
    }
  }

  void Release()
  {
    m_held_polls = 0;
    m_stepped = false;
    m_polls_since_step = 0;

    auto& settings = Settings::Instance();
    settings.SetIsContinuouslyFrameStepping(false);
    // The debugger skipped its updates during the run; let it catch up.
    emit settings.EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  }

  int m_pace = 1;
  int m_polls_since_step = 0;
  int m_held_polls = 0;
  bool m_stepped = false;
};

// Shared by every scheduler (and kept across Start/Stop), like the hotkey state itself.
FrameAdvance s_frame_advance;

void ShowVolume()
{
  const std::string level = Config::Get(Config::MAIN_AUDIO_MUTED) ?
                                std::string("Muted") :
                                fmt::format("{}%", Config::Get(Config::MAIN_AUDIO_VOLUME));
  OSD::AddMessage("Volume: " + level);
}

void ShowInternalResolution(int scale)
{
  if (scale == EFB_SCALE_AUTO_INTEGRAL)
    OSD::AddMessage("Internal Resolution: Auto (integral)");
  else if (scale == 1)
    OSD::AddMessage("Internal Resolution: Native");
  else
    OSD::AddMessage(fmt::format("Internal Resolution: {}x", scale));
}

const char* AspectModeLabel(AspectMode mode)
{
  switch (mode)
  {
  case AspectMode::Stretch:
    return "Stretch";
  case AspectMode::ForceStandard:
    return "Force 4:3";
  case AspectMode::ForceWide:
    return "Force 16:9";
  case AspectMode::Custom:
    return "Custom";
  case AspectMode::CustomStretch:
    return "Custom (Stretch)";
  case AspectMode::Raw:
    return "Raw (Square Pixels)";
  default:
    return "Auto";
  }
}

void ShowXFBCopyMode()
{
  // (The missing space before "to" is how the message has always read.)
  const char* const immediate = Config::Get(Config::GFX_HACK_IMMEDIATE_XFB) ? " (Immediate)" : "";
  const char* const target =
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM) ? "to Texture" : "to RAM";
  OSD::AddMessage(fmt::format("Copy XFB: {}{}", immediate, target));
}

void ShowSpeedLimit()
{
  const float speed = Config::Get(Config::MAIN_EMULATION_SPEED);

  // Hardcore mode does not allow slowing the game down, so a lower limit is not announced.
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    const bool full_speed_or_unlimited = speed >= 1.0f || speed <= 0.0f;
    if (!full_speed_or_unlimited)
      return;
  }

  if (speed <= 0)
    OSD::AddMessage("Speed Limit: Unlimited");
  else
    OSD::AddMessage(fmt::format("Speed Limit: {}%", std::lround(speed * 100.f)));
}

// Moves the speed limit by `step`; values close to 100% snap to exactly 100%. Lowering stops
// short of zero (which would mean unlimited).
void ChangeSpeedLimit(double step)
{
  double speed = Config::Get(Config::MAIN_EMULATION_SPEED) + step;
  if (step > 0 || speed > 0)
  {
    if (speed >= 0.95 && speed <= 1.05)
      speed = 1.0;
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, static_cast<float>(speed));
  }
  ShowSpeedLimit();
}

// Side-by-side and top-and-bottom: toggles between the mode and no 3D. Turning the mode on
// drops the anaglyph shader, which would otherwise be applied on top.
void ToggleStereoMode(StereoMode mode)
{
  if (Config::Get(Config::GFX_STEREO_MODE) == mode)
  {
    Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Off);
    return;
  }

  if (Config::Get(Config::GFX_ENHANCE_POST_SHADER) == ANAGLYPH_SHADER)
    Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");
  Config::SetCurrent(Config::GFX_STEREO_MODE, mode);
}

// Anaglyph 3D is drawn by a post-processing shader, which comes and goes with the mode.
void ToggleAnaglyph()
{
  const bool turn_on = Config::Get(Config::GFX_STEREO_MODE) != StereoMode::Anaglyph;
  Config::SetCurrent(Config::GFX_STEREO_MODE, turn_on ? StereoMode::Anaglyph : StereoMode::Off);
  Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, turn_on ? ANAGLYPH_SHADER : "");
}

// The throttle hotkey disables the speed limit while held, and (if configured) mutes the sound
// for that time. The mute goes into the CurrentRun layer so releasing the key can tell it apart
// from a mute the user chose.
void HandleThrottleHotkey(Core::System& system)
{
  const bool held = Held(HK_TOGGLE_THROTTLE);
  Core::SetIsThrottlerTempDisabled(held);

  const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED);
  if (held)
  {
    if (muted || !Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT))
      return;
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
  }
  else
  {
    if (!muted || Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) !=
                      Config::LayerType::CurrentRun)
    {
      return;
    }
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
  }
  AudioCommon::UpdateSoundStream(system);
}
}  // namespace

HotkeyScheduler::HotkeyScheduler()
{
  HotkeyManagerEmu::Enable(true);
}

HotkeyScheduler::~HotkeyScheduler()
{
  Stop();
}

void HotkeyScheduler::Start()
{
  m_quit = false;
  m_poller = std::thread([this] { PollLoop(); });
}

void HotkeyScheduler::Stop()
{
  m_quit = true;
  if (m_poller.joinable())
    m_poller.join();
}

template <std::size_t N>
void HotkeyScheduler::EmitPressed(const SignalHotkey (&table)[N])
{
  for (const SignalHotkey& entry : table)
  {
    if (Pressed(entry.hotkey))
      emit(this->*entry.signal)();
  }
}

void HotkeyScheduler::PollLoop()
{
  Common::SetCurrentThreadName("HotkeyScheduler");

  while (!m_quit)
  {
    Common::SleepCurrentThread(POLL_INTERVAL_MS);

    // Free Look reads its own controller; the host channel feeds the hotkeys and the GUI.
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::FreeLook);
    g_controller_interface.UpdateInput();
    FreeLook::UpdateInput();
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    g_controller_interface.UpdateInput();

    if (!HotkeyManagerEmu::IsEnabled())
      continue;

    Core::System& system = Core::System::GetInstance();
    if (Core::GetState(system) != Core::State::Stopping && !HandleCoreHotkeys(system))
      continue;

    HandleLateHotkeys();
  }
}

bool HotkeyScheduler::HandleCoreHotkeys(Core::System& system)
{
  // Read the hotkeys through the focus gate (when hotkeys need focus), then open the gate again
  // for everything else on this thread, such as the controller configuration's input display.
  Core::UpdateInputGate(Config::Get(Config::MAIN_FOCUSED_HOTKEYS));
  HotkeyManagerEmu::GetStatus(false);
  ControlReference::SetInputGate(true);
  HotkeyManagerEmu::GetStatus(true);

  static constexpr SignalHotkey ALWAYS[] = {
      {HK_OPEN, &HotkeyScheduler::Open},
      {HK_REFRESH_LIST, &HotkeyScheduler::RefreshGameListHotkey},
      {HK_START_RECORDING, &HotkeyScheduler::StartRecording},
      {HK_EXIT, &HotkeyScheduler::ExitHotkey},
#ifdef USE_RETRO_ACHIEVEMENTS
      {HK_OPEN_ACHIEVEMENTS, &HotkeyScheduler::OpenAchievements},
#endif
  };
  EmitPressed(ALWAYS);

  if (Core::IsUninitialized(system))
  {
    // A recording can only be started from the game list.
    if (Pressed(HK_PLAY_RECORDING))
      emit PlayRecording();
    return false;
  }

  HandleSessionHotkeys(system);
  return true;
}

void HotkeyScheduler::HandleSessionHotkeys(Core::System& system)
{
  static constexpr SignalHotkey DISC[] = {
      {HK_EJECT_DISC, &HotkeyScheduler::EjectDisc},
      {HK_CHANGE_DISC, &HotkeyScheduler::ChangeDisc},
  };
  EmitPressed(DISC);

  if (Pressed(HK_FULLSCREEN))
  {
    emit FullScreenHotkey();
    Common::SleepCurrentThread(FULLSCREEN_COOLDOWN_MS);
  }

  static constexpr SignalHotkey EMULATION[] = {
      {HK_PLAY_PAUSE, &HotkeyScheduler::TogglePauseHotkey},
      {HK_STOP, &HotkeyScheduler::StopHotkey},
      {HK_RESET, &HotkeyScheduler::ResetHotkey},
  };
  EmitPressed(EMULATION);

  s_frame_advance.Poll();

  static constexpr SignalHotkey WINDOW[] = {
      {HK_SCREENSHOT, &HotkeyScheduler::ScreenShotHotkey},
      {HK_UNLOCK_CURSOR, &HotkeyScheduler::UnlockCursor},
  };
  EmitPressed(WINDOW);

  if (Held(HK_CENTER_MOUSE))
    g_controller_interface.SetMouseCenteringRequested(true);

  auto& settings = Settings::Instance();

  static constexpr SignalHotkey NETPLAY_AND_MOVIES[] = {
      {HK_ACTIVATE_CHAT, &HotkeyScheduler::ActivateChat},
      {HK_REQUEST_GOLF_CONTROL, &HotkeyScheduler::RequestGolfControl},
      {HK_EXPORT_RECORDING, &HotkeyScheduler::ExportRecording},
      {HK_READ_ONLY_MODE, &HotkeyScheduler::ToggleReadOnlyMode},
  };
  EmitPressed(NETPLAY_AND_MOVIES);

  // A passed-through Bluetooth adapter gets the sync button as long as the hotkey is held.
  if (const auto real_bluetooth = WiiUtils::GetBluetoothRealDevice())
    real_bluetooth->UpdateSyncButtonState(Held(HK_TRIGGER_SYNC_BUTTON));

  if (Config::IsDebuggingEnabled())
    HandleDebuggerHotkeys();

  if (Core::System::GetInstance().IsWii())
    HandleWiiHotkeys();

  HandleProfileHotkeys();

  // Volume
  if (Pressed(HK_VOLUME_DOWN))
  {
    settings.DecreaseVolume(3);
    ShowVolume();
  }
  if (Pressed(HK_VOLUME_UP))
  {
    settings.IncreaseVolume(3);
    ShowVolume();
  }
  if (Pressed(HK_VOLUME_TOGGLE_MUTE))
  {
    AudioCommon::ToggleMuteVolume(system);
    ShowVolume();
  }

  // Internal resolution; both hotkeys start from the scale as it was before this poll.
  const int scale = Config::Get(Config::GFX_EFB_SCALE);
  if (Pressed(HK_INCREASE_IR))
  {
    Config::SetCurrent(Config::GFX_EFB_SCALE, scale + 1);
    ShowInternalResolution(scale + 1);
  }
  if (Pressed(HK_DECREASE_IR) && scale > EFB_SCALE_AUTO_INTEGRAL)
  {
    Config::SetCurrent(Config::GFX_EFB_SCALE, scale - 1);
    ShowInternalResolution(scale - 1);
  }

  if (Pressed(HK_TOGGLE_CROP_TO_ASPECT_RATIO))
    Toggle(Config::GFX_CROP_TO_ASPECT_RATIO);

  if (Pressed(HK_TOGGLE_CROP_CUSTOM))
    Toggle(Config::GFX_CROP_CUSTOM);

  if (Pressed(HK_TOGGLE_AR))
  {
    // Cycles through the first four modes.
    const auto next = static_cast<AspectMode>(
        (static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)) + 1) & 3);
    Config::SetCurrent(Config::GFX_ASPECT_RATIO, next);
    OSD::AddMessage(AspectModeLabel(next));
  }

  if (Pressed(HK_TOGGLE_SKIP_EFB_ACCESS))
  {
    const bool skip = Toggle(Config::GFX_HACK_EFB_ACCESS_ENABLE);
    OSD::AddMessage(fmt::format("{} EFB Access from CPU", skip ? "Skip" : "Don't skip"));
  }

  if (Pressed(HK_TOGGLE_EFBCOPIES))
  {
    const bool to_texture = Toggle(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
    OSD::AddMessage(fmt::format("Copy EFB: {}", to_texture ? "to Texture" : "to RAM"));
  }

  if (Pressed(HK_TOGGLE_XFBCOPIES))
  {
    Toggle(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
    ShowXFBCopyMode();
  }
  if (Pressed(HK_TOGGLE_IMMEDIATE_XFB))
  {
    Toggle(Config::GFX_HACK_IMMEDIATE_XFB);
    ShowXFBCopyMode();
  }

  if (Pressed(HK_TOGGLE_FOG))
  {
    // The setting is "disable fog"; the message reports the new value of that setting.
    const bool fog_disabled = Toggle(Config::GFX_DISABLE_FOG);
    OSD::AddMessage(fmt::format("Fog: {}", fog_disabled ? "Enabled" : "Disabled"));
  }

  if (Pressed(HK_TOGGLE_DUMPTEXTURES))
  {
    const bool dumping = Toggle(Config::GFX_DUMP_TEXTURES);
    OSD::AddMessage(fmt::format("Texture Dumping {}", dumping ?
                                                          "enabled. This will reduce performance." :
                                                          "disabled."),
                    OSD::Duration::NORMAL);
  }

  if (Pressed(HK_TOGGLE_TEXTURES))
    Toggle(Config::GFX_HIRES_TEXTURES);

  HandleThrottleHotkey(system);

  if (Pressed(HK_DECREASE_EMULATION_SPEED))
    ChangeSpeedLimit(-0.1);
  if (Pressed(HK_INCREASE_EMULATION_SPEED))
    ChangeSpeedLimit(0.1);

  static constexpr SignalHotkey DEVICES_AND_SLOTS[] = {
      {HK_SKYLANDERS_PORTAL, &HotkeyScheduler::SkylandersPortalHotkey},
      {HK_INFINITY_BASE, &HotkeyScheduler::InfinityBaseHotkey},
      {HK_SAVE_STATE_SLOT_SELECTED, &HotkeyScheduler::StateSaveSlotHotkey},
      {HK_LOAD_STATE_SLOT_SELECTED, &HotkeyScheduler::StateLoadSlotHotkey},
      {HK_INCREMENT_SELECTED_STATE_SLOT, &HotkeyScheduler::IncrementSelectedStateSlotHotkey},
      {HK_DECREMENT_SELECTED_STATE_SLOT, &HotkeyScheduler::DecrementSelectedStateSlotHotkey},
  };
  EmitPressed(DEVICES_AND_SLOTS);

  if (Pressed(HK_TOGGLE_STEREO_SIDE_BY_SIDE))
    ToggleStereoMode(StereoMode::SideBySide);
  if (Pressed(HK_TOGGLE_STEREO_TOP_AND_BOTTOM))
    ToggleStereoMode(StereoMode::TopAndBottom);
  if (Pressed(HK_TOGGLE_STEREO_ANAGLYPH))
    ToggleAnaglyph();

  HandleGBAHotkeys();
}

void HotkeyScheduler::HandleWiiHotkeys()
{
  // Remotes 1-4, then the balance board.
  static constexpr int CONNECT_HOTKEYS[] = {HK_WIIMOTE1_CONNECT, HK_WIIMOTE2_CONNECT,
                                            HK_WIIMOTE3_CONNECT, HK_WIIMOTE4_CONNECT,
                                            HK_BALANCEBOARD_CONNECT};
  int remote = 0;
  for (const int hotkey : CONNECT_HOTKEYS)
  {
    if (Pressed(hotkey))
      emit ConnectWiiRemote(remote);
    ++remote;
  }

  auto& settings = Settings::Instance();

  if (Pressed(HK_TOGGLE_SD_CARD))
    settings.SetSDCardInserted(!settings.IsSDCardInserted());

  if (Pressed(HK_TOGGLE_USB_KEYBOARD))
    settings.SetUSBKeyboardConnected(!settings.IsUSBKeyboardConnected());

  if (Pressed(HK_TOGGLE_WII_SPEAK_MUTE))
  {
    const bool mute = !settings.IsWiiSpeakMuted();
    settings.SetWiiSpeakMuted(mute);
    OSD::AddMessage(mute ? "Wii Speak muted" : "Wii Speak unmuted");
  }
}

void HotkeyScheduler::HandleProfileHotkeys()
{
  struct ProfileHotkeys
  {
    int previous;
    int next;
    int remote;
    bool game_profiles;
  };
  static constexpr ProfileHotkeys PROFILE_HOTKEYS[] = {
      {HK_PREV_WIIMOTE_PROFILE_1, HK_NEXT_WIIMOTE_PROFILE_1, 0, false},
      {HK_PREV_WIIMOTE_PROFILE_2, HK_NEXT_WIIMOTE_PROFILE_2, 1, false},
      {HK_PREV_WIIMOTE_PROFILE_3, HK_NEXT_WIIMOTE_PROFILE_3, 2, false},
      {HK_PREV_WIIMOTE_PROFILE_4, HK_NEXT_WIIMOTE_PROFILE_4, 3, false},
      {HK_PREV_GAME_WIIMOTE_PROFILE_1, HK_NEXT_GAME_WIIMOTE_PROFILE_1, 0, true},
      {HK_PREV_GAME_WIIMOTE_PROFILE_2, HK_NEXT_GAME_WIIMOTE_PROFILE_2, 1, true},
      {HK_PREV_GAME_WIIMOTE_PROFILE_3, HK_NEXT_GAME_WIIMOTE_PROFILE_3, 2, true},
      {HK_PREV_GAME_WIIMOTE_PROFILE_4, HK_NEXT_GAME_WIIMOTE_PROFILE_4, 3, true},
  };

  // "Previous" wins when both are pressed; "next" is then not looked at in this poll.
  for (const ProfileHotkeys& entry : PROFILE_HOTKEYS)
  {
    if (Pressed(entry.previous))
    {
      if (entry.game_profiles)
        m_profiles.PreviousWiimoteProfileForGame(entry.remote);
      else
        m_profiles.PreviousWiimoteProfile(entry.remote);
    }
    else if (Pressed(entry.next))
    {
      if (entry.game_profiles)
        m_profiles.NextWiimoteProfileForGame(entry.remote);
      else
        m_profiles.NextWiimoteProfile(entry.remote);
    }
  }
}

void HotkeyScheduler::HandleDebuggerHotkeys()
{
  // HK_SET_PC is reported as Skip, which is what the code view does for it.
  static constexpr SignalHotkey DEBUGGER[] = {
      {HK_STEP, &HotkeyScheduler::Step},
      {HK_STEP_OVER, &HotkeyScheduler::StepOver},
      {HK_STEP_OUT, &HotkeyScheduler::StepOut},
      {HK_SKIP, &HotkeyScheduler::Skip},
      {HK_SHOW_PC, &HotkeyScheduler::ShowPC},
      {HK_SET_PC, &HotkeyScheduler::Skip},
      {HK_BP_TOGGLE, &HotkeyScheduler::ToggleBreakpoint},
      {HK_BP_ADD, &HotkeyScheduler::AddBreakpoint},
  };
  EmitPressed(DEBUGGER);
}

void HotkeyScheduler::HandleGBAHotkeys()
{
#ifdef HAS_LIBMGBA
  // GBA hotkeys go to whichever GBA window is active, on the GUI thread.
  GBAWidget* const window = qobject_cast<GBAWidget*>(QApplication::activeWindow());
  if (window == nullptr)
    return;

  using Command = void (*)(GBAWidget*);
  struct GBAHotkey
  {
    int hotkey;
    Command command;
  };
  static constexpr GBAHotkey GBA_HOTKEYS[] = {
      {HK_GBA_LOAD, [](GBAWidget* w) { w->LoadROM(); }},
      {HK_GBA_UNLOAD, [](GBAWidget* w) { w->UnloadROM(); }},
      {HK_GBA_RESET, [](GBAWidget* w) { w->ResetCore(); }},
      {HK_GBA_VOLUME_DOWN, [](GBAWidget* w) { w->VolumeDown(); }},
      {HK_GBA_VOLUME_UP, [](GBAWidget* w) { w->VolumeUp(); }},
      {HK_GBA_TOGGLE_MUTE, [](GBAWidget* w) { w->ToggleMute(); }},
      {HK_GBA_1X, [](GBAWidget* w) { w->Resize(1); }},
      {HK_GBA_2X, [](GBAWidget* w) { w->Resize(2); }},
      {HK_GBA_3X, [](GBAWidget* w) { w->Resize(3); }},
      {HK_GBA_4X, [](GBAWidget* w) { w->Resize(4); }},
  };

  for (const GBAHotkey& entry : GBA_HOTKEYS)
  {
    if (Pressed(entry.hotkey))
      QueueOnObject(window, [window, command = entry.command] { command(window); });
  }
#endif
}

void HotkeyScheduler::HandleLateHotkeys()
{
  // 3D depth and convergence change for as long as their hotkeys are held. Both directions start
  // from the value before this poll.
  const float depth = Config::Get(Config::GFX_STEREO_DEPTH);
  if (Held(HK_DECREASE_DEPTH))
    Config::SetCurrent(Config::GFX_STEREO_DEPTH, std::max(depth - 1, 0.0f));
  if (Held(HK_INCREASE_DEPTH))
    Config::SetCurrent(Config::GFX_STEREO_DEPTH,
                       std::min(depth + 1, Config::GFX_STEREO_DEPTH_MAXIMUM));

  const float convergence = Config::Get(Config::GFX_STEREO_CONVERGENCE);
  if (Held(HK_DECREASE_CONVERGENCE))
    Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE, std::max(convergence - 5, 0.0f));
  if (Held(HK_INCREASE_CONVERGENCE))
    Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE,
                       std::min(convergence + 5, Config::GFX_STEREO_CONVERGENCE_MAXIMUM));

  if (Pressed(HK_FREELOOK_TOGGLE))
  {
    const bool enabled = Toggle(Config::FREE_LOOK_ENABLED);
    if (AchievementManager::GetInstance().IsHardcoreModeActive())
      OSD::AddMessage("Free Look is Disabled in Hardcore Mode");
    else
      OSD::AddMessage(fmt::format("Free Look: {}", enabled ? "Enabled" : "Disabled"));
  }

  // Numbered save state slots, 1 to NUM_STATES.
  for (int slot = 1; slot <= static_cast<int>(State::NUM_STATES); ++slot)
  {
    const int offset = slot - 1;
    if (Pressed(HK_LOAD_STATE_SLOT_1 + offset))
      emit StateLoadSlot(slot);
    if (Pressed(HK_SAVE_STATE_SLOT_1 + offset))
      emit StateSaveSlot(slot);
    if (Pressed(HK_LOAD_LAST_STATE_1 + offset))
      emit StateLoadLastSaved(slot);
    if (Pressed(HK_SELECT_STATE_SLOT_1 + offset))
      emit SetStateSlotHotkey(slot);
  }

  static constexpr SignalHotkey STATE_FILES[] = {
      {HK_SAVE_FIRST_STATE, &HotkeyScheduler::StateSaveOldest},
      {HK_UNDO_LOAD_STATE, &HotkeyScheduler::StateLoadUndo},
      {HK_UNDO_SAVE_STATE, &HotkeyScheduler::StateSaveUndo},
      {HK_LOAD_STATE_FILE, &HotkeyScheduler::StateLoadFile},
      {HK_SAVE_STATE_FILE, &HotkeyScheduler::StateSaveFile},
  };
  EmitPressed(STATE_FILES);
}
