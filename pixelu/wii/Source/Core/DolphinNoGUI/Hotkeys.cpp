// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinNoGUI/Hotkeys.h"

#include <cmath>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/Thread.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

namespace
{
constexpr int POLL_MS = 5;
// Polls a held frame-step key waits before it starts repeating.
constexpr int FRAME_STEP_REPEAT_AFTER = 40;
constexpr u16 VOLUME_STEP = 3;

bool Pressed(int id)
{
  return HotkeyManagerEmu::IsPressed(id, false);
}

bool Held(int id)
{
  return HotkeyManagerEmu::IsPressed(id, true);
}

// Runs `job` on the main loop's thread, which owns the session.
template <typename F>
void OnHost(F&& job)
{
  Core::QueueHostJob(std::forward<F>(job));
}

bool Flip(const Config::Info<bool>& setting)
{
  const bool value = !Config::Get(setting);
  Config::SetCurrent(setting, value);
  return value;
}

void ShowVolume()
{
  if (Config::Get(Config::MAIN_AUDIO_MUTED))
    OSD::AddMessage("Volume: muted");
  else
    OSD::AddMessage(fmt::format("Volume: {}%", Config::Get(Config::MAIN_AUDIO_VOLUME)));
}

void ChangeSpeed(double by)
{
  double speed = Config::Get(Config::MAIN_EMULATION_SPEED) + by;
  // 0 means "no limit", so lowering stops at 10%; near 100% lands on exactly 100%.
  if (speed < 0.1)
    speed = by < 0 ? 0.1 : speed;
  if (std::abs(speed - 1.0) < 0.05)
    speed = 1.0;
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, static_cast<float>(speed));
  OSD::AddMessage(fmt::format("Speed limit: {}%", std::lround(speed * 100)));
}

void ChangeResolution(int by)
{
  const int scale = Config::Get(Config::GFX_EFB_SCALE) + by;
  if (scale < EFB_SCALE_AUTO_INTEGRAL)
    return;
  Config::SetCurrent(Config::GFX_EFB_SCALE, scale);
  if (scale == EFB_SCALE_AUTO_INTEGRAL)
    OSD::AddMessage("Internal resolution: auto");
  else if (scale == 1)
    OSD::AddMessage("Internal resolution: native");
  else
    OSD::AddMessage(fmt::format("Internal resolution: {}x", scale));
}

void NextAspect()
{
  static constexpr const char* NAMES[] = {"Auto", "Force 16:9", "Force 4:3", "Stretch"};
  const int next = (static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)) + 1) & 3;
  Config::SetCurrent(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(next));
  OSD::AddMessage(NAMES[next]);
}

// While held, the speed limit is off (and the sound muted, if the settings ask for that). The
// mute lives in the CurrentRun layer so that letting go does not undo a mute the player chose.
void Throttle(Core::System& system)
{
  const bool held = Held(HK_TOGGLE_THROTTLE);
  Core::SetIsThrottlerTempDisabled(held);
  const bool muted = Config::Get(Config::MAIN_AUDIO_MUTED);
  if (held && !muted && Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT))
  {
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
    AudioCommon::UpdateSoundStream(system);
  }
  else if (!held && muted &&
           Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) ==
               Config::LayerType::CurrentRun)
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
    AudioCommon::UpdateSoundStream(system);
  }
}

void ToggleRemote(int index)
{
  OnHost([index](Core::System& system) {
    if (!system.IsWii())
      return;
    const Core::CPUThreadGuard guard(system);
    if (const auto bluetooth = WiiUtils::GetBluetoothEmuDevice())
    {
      auto* const remote = bluetooth->AccessWiimoteByIndex(static_cast<std::size_t>(index));
      remote->Activate(!remote->IsConnected());
    }
  });
}
}  // namespace

Hotkeys::Hotkeys(Actions actions) : m_actions(std::move(actions))
{
  HotkeyManagerEmu::Enable(true);
  m_thread = std::thread([this] { Run(); });
}

Hotkeys::~Hotkeys()
{
  m_quit = true;
  if (m_thread.joinable())
    m_thread.join();
}

void Hotkeys::Run()
{
  Common::SetCurrentThreadName("Hotkeys");
  while (!m_quit)
  {
    Common::SleepCurrentThread(POLL_MS);
    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    g_controller_interface.UpdateInput();
    if (!HotkeyManagerEmu::IsEnabled())
      continue;

    Core::System& system = Core::System::GetInstance();
    if (Core::IsUninitialized(system) || Core::GetState(system) == Core::State::Stopping)
      continue;

    // Sample through the focus gate when hotkeys need the window focused, then open the gate
    // again for the rest of this thread.
    Core::UpdateInputGate(Config::Get(Config::MAIN_FOCUSED_HOTKEYS));
    HotkeyManagerEmu::GetStatus(false);
    ControlReference::SetInputGate(true);
    Poll();
  }
}

void Hotkeys::SelectSlot(int slot)
{
  m_slot = slot;
  OSD::AddMessage(
      fmt::format("Slot {}: {}", slot, State::GetInfoStringOfSlot(static_cast<u32>(slot), false)),
      2500);
}

void Hotkeys::Poll()
{
  Core::System& system = Core::System::GetInstance();

  if (Pressed(HK_STOP) || Pressed(HK_EXIT))
  {
    if (m_actions.stop)
      m_actions.stop();
    return;
  }
  if (Pressed(HK_FULLSCREEN) && m_actions.toggle_fullscreen)
  {
    m_actions.toggle_fullscreen();
    // One long press must not flip the window back and forth.
    Common::SleepCurrentThread(100);
  }

  if (Pressed(HK_PLAY_PAUSE))
  {
    OnHost([](Core::System& s) {
      Core::SetState(s, Core::GetState(s) == Core::State::Paused ? Core::State::Running :
                                                                     Core::State::Paused);
    });
  }
  if (Pressed(HK_RESET))
    OnHost([](Core::System& s) { s.GetProcessorInterface().ResetButton_Tap(); });
  if (Pressed(HK_SCREENSHOT))
    Core::SaveScreenShot();

  // Frame step: one frame per press; held, it repeats after a short wait.
  if (Held(HK_FRAME_ADVANCE))
  {
    if (m_frame_step_hold == 0 || m_frame_step_hold >= FRAME_STEP_REPEAT_AFTER)
      OnHost([](Core::System& s) { Core::DoFrameStep(s); });
    if (m_frame_step_hold < FRAME_STEP_REPEAT_AFTER)
      ++m_frame_step_hold;
  }
  else
  {
    m_frame_step_hold = 0;
  }

  if (Pressed(HK_VOLUME_DOWN))
  {
    AudioCommon::DecreaseVolume(system, VOLUME_STEP);
    ShowVolume();
  }
  if (Pressed(HK_VOLUME_UP))
  {
    AudioCommon::IncreaseVolume(system, VOLUME_STEP);
    ShowVolume();
  }
  if (Pressed(HK_VOLUME_TOGGLE_MUTE))
  {
    AudioCommon::ToggleMuteVolume(system);
    ShowVolume();
  }

  if (Pressed(HK_INCREASE_IR))
    ChangeResolution(1);
  if (Pressed(HK_DECREASE_IR))
    ChangeResolution(-1);
  if (Pressed(HK_TOGGLE_AR))
    NextAspect();
  if (Pressed(HK_TOGGLE_CROP_TO_ASPECT_RATIO))
    Flip(Config::GFX_CROP_TO_ASPECT_RATIO);
  if (Pressed(HK_TOGGLE_FOG))
    OSD::AddMessage(Flip(Config::GFX_DISABLE_FOG) ? "Fog: off" : "Fog: on");
  if (Pressed(HK_TOGGLE_TEXTURES))
    Flip(Config::GFX_HIRES_TEXTURES);

  Throttle(system);
  if (Pressed(HK_DECREASE_EMULATION_SPEED))
    ChangeSpeed(-0.1);
  if (Pressed(HK_INCREASE_EMULATION_SPEED))
    ChangeSpeed(0.1);

  static constexpr int REMOTES[] = {HK_WIIMOTE1_CONNECT, HK_WIIMOTE2_CONNECT, HK_WIIMOTE3_CONNECT,
                                    HK_WIIMOTE4_CONNECT, HK_BALANCEBOARD_CONNECT};
  for (int i = 0; i < 5; ++i)
  {
    if (Pressed(REMOTES[i]))
      ToggleRemote(i);
  }

  // Save states: the numbered slots, the selected slot, and undo.
  const int slots = static_cast<int>(State::NUM_STATES);
  for (int slot = 1; slot <= slots; ++slot)
  {
    const int offset = slot - 1;
    if (Pressed(HK_LOAD_STATE_SLOT_1 + offset))
      OnHost([slot](Core::System& s) { State::Load(s, static_cast<u32>(slot)); });
    if (Pressed(HK_SAVE_STATE_SLOT_1 + offset))
      OnHost([slot](Core::System& s) { State::Save(s, static_cast<u32>(slot)); });
    if (Pressed(HK_LOAD_LAST_STATE_1 + offset))
      OnHost([slot](Core::System& s) { State::LoadLastSaved(s, slot); });
    if (Pressed(HK_SELECT_STATE_SLOT_1 + offset))
      SelectSlot(slot);
  }
  if (Pressed(HK_INCREMENT_SELECTED_STATE_SLOT))
    SelectSlot(m_slot % slots + 1);
  if (Pressed(HK_DECREMENT_SELECTED_STATE_SLOT))
    SelectSlot(m_slot == 1 ? slots : m_slot - 1);
  const int selected = m_slot;
  if (Pressed(HK_SAVE_STATE_SLOT_SELECTED))
    OnHost([selected](Core::System& s) { State::Save(s, static_cast<u32>(selected)); });
  if (Pressed(HK_LOAD_STATE_SLOT_SELECTED))
    OnHost([selected](Core::System& s) { State::Load(s, static_cast<u32>(selected)); });
  if (Pressed(HK_SAVE_FIRST_STATE))
    OnHost([](Core::System& s) { State::SaveFirstSaved(s); });
  if (Pressed(HK_UNDO_LOAD_STATE))
    OnHost([](Core::System& s) { State::UndoLoadState(s); });
  if (Pressed(HK_UNDO_SAVE_STATE))
    OnHost([](Core::System& s) { State::UndoSaveState(s); });
}
