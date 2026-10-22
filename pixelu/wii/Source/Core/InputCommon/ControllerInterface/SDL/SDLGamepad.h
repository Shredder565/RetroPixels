// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_haptic.h>

#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ciface::SDL
{
// One SDL joystick, with its gamepad mapping where SDL has one. Mapped controls get gamepad
// names ("Button S", "Left X+"...); the joystick's raw buttons, axes and hats that the mapping
// does not cover are added under their numbered names, and haptic effects become outputs.
class Gamepad : public Core::Device
{
public:
  // State of one touchpad (first finger only), in -1..1 for the position.
  struct Touchpad
  {
    float x = 0.f;
    float y = 0.f;
    float pressure = 0.f;
  };

  // Rumble levels sent with SDL_RumbleGamepad and SDL_RumbleGamepadTriggers.
  struct RumbleLevels
  {
    Uint16 low_frequency = 0;
    Uint16 high_frequency = 0;
    Uint16 left_trigger = 0;
    Uint16 right_trigger = 0;
  };

  // `gamepad` may be null for joysticks SDL has no mapping for.
  Gamepad(SDL_Gamepad* gamepad, SDL_Joystick* joystick);
  ~Gamepad() override;

  std::string GetName() const override;
  std::string GetSource() const override;
  SDL_JoystickID GetSDLInstanceID() const;
  Core::DeviceRemoval UpdateInput() override;

  void SendRumble();
  void SendTriggerRumble();

private:
  // The raw joystick controls covered by the gamepad mapping.
  struct CoveredControls;

  void AddMappedControls(CoveredControls* covered);
  void AddRawControls(const CoveredControls& covered);
  void AddHaptics();
  bool ReadBattery();

  RumbleLevels m_rumble;

  SDL_Gamepad* const m_gamepad;
  std::string m_name;
  SDL_Joystick* const m_joystick;
  SDL_Haptic* m_haptic = nullptr;
  ControlState m_battery = 0;
  std::vector<Touchpad> m_touchpads;
};

}  // namespace ciface::SDL
