// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Wiimote.h"

#include <optional>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/USB/Bluetooth/WiimoteDevice.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/InputConfig.h"

namespace
{
// Current source of every slot, as last read from the config.
std::array<std::atomic<WiimoteSource>, MAX_BBMOTES> s_slot_sources;

std::optional<Config::ConfigChangedCallbackID> s_source_watch;

InputConfig s_emulated_config(WIIMOTE_INI_NAME, _trans("Wii Remote"), "Wiimote", "Wiimote");

WiimoteEmu::Wiimote* EmulatedRemote(int slot)
{
  return static_cast<WiimoteEmu::Wiimote*>(s_emulated_config.GetController(slot));
}

WiimoteSource CurrentSource(unsigned int slot)
{
  return s_slot_sources[slot];
}

void ApplySource(unsigned int slot, WiimoteSource wanted)
{
  if (s_slot_sources[slot].exchange(wanted) == wanted)
    return;

  WiimoteReal::HandleWiimoteSourceChange(slot);

  const Core::CPUThreadGuard guard(Core::System::GetInstance());
  WiimoteCommon::UpdateSource(slot);
}

void ReadSourcesFromConfig()
{
  for (int slot = 0; slot < MAX_BBMOTES; ++slot)
  {
    ApplySource(static_cast<unsigned int>(slot),
                Config::Get(Config::GetInfoForWiimoteSource(slot)));
  }
}
}  // namespace

namespace WiimoteCommon
{
void UpdateSource(unsigned int index)
{
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (!bluetooth)
    return;

  bluetooth->AccessWiimoteByIndex(index)->SetSource(GetHIDWiimoteSource(index));
}

HIDWiimote* GetHIDWiimoteSource(unsigned int index)
{
  switch (CurrentSource(index))
  {
  case WiimoteSource::Emulated:
    return EmulatedRemote(static_cast<int>(index));
  case WiimoteSource::Real:
    return WiimoteReal::g_wiimotes[index].get();
  default:
    return nullptr;
  }
}
}  // namespace WiimoteCommon

namespace Wiimote
{
InputConfig* GetConfig()
{
  return &s_emulated_config;
}

ControllerEmu::ControlGroup* GetWiimoteGroup(int number, WiimoteEmu::WiimoteGroup group)
{
  return EmulatedRemote(number)->GetWiimoteGroup(group);
}

ControllerEmu::ControlGroup* GetNunchukGroup(int number, WiimoteEmu::NunchukGroup group)
{
  return EmulatedRemote(number)->GetNunchukGroup(group);
}

ControllerEmu::ControlGroup* GetClassicGroup(int number, WiimoteEmu::ClassicGroup group)
{
  return EmulatedRemote(number)->GetClassicGroup(group);
}

ControllerEmu::ControlGroup* GetGuitarGroup(int number, WiimoteEmu::GuitarGroup group)
{
  return EmulatedRemote(number)->GetGuitarGroup(group);
}

ControllerEmu::ControlGroup* GetDrumsGroup(int number, WiimoteEmu::DrumsGroup group)
{
  return EmulatedRemote(number)->GetDrumsGroup(group);
}

ControllerEmu::ControlGroup* GetTurntableGroup(int number, WiimoteEmu::TurntableGroup group)
{
  return EmulatedRemote(number)->GetTurntableGroup(group);
}

ControllerEmu::ControlGroup* GetUDrawTabletGroup(int number, WiimoteEmu::UDrawTabletGroup group)
{
  return EmulatedRemote(number)->GetUDrawTabletGroup(group);
}

ControllerEmu::ControlGroup* GetDrawsomeTabletGroup(int number,
                                                    WiimoteEmu::DrawsomeTabletGroup group)
{
  return EmulatedRemote(number)->GetDrawsomeTabletGroup(group);
}

ControllerEmu::ControlGroup* GetTaTaConGroup(int number, WiimoteEmu::TaTaConGroup group)
{
  return EmulatedRemote(number)->GetTaTaConGroup(group);
}

ControllerEmu::ControlGroup* GetShinkansenGroup(int number, WiimoteEmu::ShinkansenGroup group)
{
  return EmulatedRemote(number)->GetShinkansenGroup(group);
}

void Initialize(InitializeMode init_mode)
{
  if (s_emulated_config.ControllersNeedToBeCreated())
  {
    for (unsigned int slot = WIIMOTE_CHAN_0; slot < MAX_BBMOTES; ++slot)
      s_emulated_config.CreateController<WiimoteEmu::Wiimote>(slot);
  }

  s_emulated_config.RegisterHotplugCallback();

  LoadConfig();

  if (!s_source_watch.has_value())
    s_source_watch = Config::AddConfigChangedCallback(ReadSourcesFromConfig);
  ReadSourcesFromConfig();

  WiimoteReal::Initialize(init_mode);

  // A running movie needs the remotes set up the way it recorded them.
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsMovieActive())
    movie.ChangeWiiPads();
}

void Shutdown()
{
  s_emulated_config.UnregisterHotplugCallback();
  s_emulated_config.ClearControllers();

  WiimoteReal::Stop();

  if (s_source_watch.has_value())
  {
    Config::RemoveConfigChangedCallback(*s_source_watch);
    s_source_watch.reset();
  }
}

void ResetAllWiimotes()
{
  for (int slot = WIIMOTE_CHAN_0; slot < MAX_BBMOTES; ++slot)
    EmulatedRemote(slot)->Reset();
}

void LoadConfig()
{
  s_emulated_config.LoadConfig();
}

void GenerateDynamicInputTextures()
{
  s_emulated_config.GenerateControllerTextures();
}

void Resume()
{
  WiimoteReal::Resume();
}

void Pause()
{
  WiimoteReal::Pause();
}

void DoState(PointerWrap& p)
{
  for (int slot = 0; slot < MAX_BBMOTES; ++slot)
  {
    const auto index = static_cast<unsigned int>(slot);
    const WiimoteSource current = CurrentSource(index);

    // One byte with the slot's source, then the full state if the slot is emulated.
    u8 saved = static_cast<u8>(current);
    p.Do(saved);
    const auto saved_source = static_cast<WiimoteSource>(saved);

    if (saved_source == WiimoteSource::Emulated)
      EmulatedRemote(slot)->DoState(p);

    // After loading, reconnect real remotes and every slot whose source differs from the state.
    if (p.IsReadMode() && (current == WiimoteSource::Real || current != saved_source))
      WiimoteCommon::UpdateSource(index);
  }
}
}  // namespace Wiimote
