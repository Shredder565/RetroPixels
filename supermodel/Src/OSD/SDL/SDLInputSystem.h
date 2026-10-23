// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Keyboard, mouse and joystick input through SDL2, with force feedback
// through SDL's haptic API. Joysticks are read either as plain joysticks
// ("sdl") or through SDL's game controller mapping ("sdlgamepad", which
// gives every pad the same 6 axes, 4 d-pad directions and 16 buttons).
// SDL sees one keyboard and one mouse.

#ifndef INCLUDED_SDLINPUTSYSTEM_H
#define INCLUDED_SDLINPUTSYSTEM_H

#include "Types.h"
#include "Inputs/InputSource.h"
#include "Inputs/InputSystem.h"
#include "SDLIncludes.h"

#include <vector>

class CSDLInputSystem : public CInputSystem
{
public:
  CSDLInputSystem(const Util::Config::Node &config, bool useGameController);
  ~CSDLInputSystem();

  int GetNumKeyboards() const;
  int GetNumMice() const;
  int GetNumJoysticks() const;
  const KeyDetails *GetKeyDetails(int kbdNum);
  const MouseDetails *GetMouseDetails(int mseNum);
  const JoyDetails *GetJoyDetails(int joyNum);
  bool Poll();
  void SetMouseVisibility(bool visible);

protected:
  bool InitializeSystem();
  int GetKeyIndex(const char *keyName);
  const char *GetKeyName(int keyIndex);
  bool IsKeyPressed(int kbdNum, int keyIndex) const;
  int GetMouseAxisValue(int mseNum, int axisNum) const;
  int GetMouseWheelDir(int mseNum) const;
  bool IsMouseButPressed(int mseNum, int butNum) const;
  int GetJoyAxisValue(int joyNum, int axisNum) const;
  bool IsJoyPOVInDir(int joyNum, int povNum, int povDir) const;
  bool IsJoyButPressed(int joyNum, int butNum) const;
  bool ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd);

private:
  // One opened device.
  struct Device
  {
    SDL_Joystick *joystick = nullptr;
    SDL_GameController *pad = nullptr;   // in game controller mode
    SDL_Haptic *haptic = nullptr;
    int constant = -1;                   // effect ids, -1 if not made
    int sine = -1;
    int spring = -1;
    int friction = -1;
  };

  void OpenDevices();
  void CloseDevices();
  void SetUpEffects(Device &dev, JoyDetails &details, int index, int hapticAxes);

  // Hands an effect to the device's effect slot; false if the device
  // can't play that kind of effect.
  bool Play(int joyNum, int Device::*slot, SDL_HapticEffect &effect, bool run);
  void StopAll(int joyNum);

  const Util::Config::Node &m_config;
  const bool m_gameController;
  std::vector<Device> m_devices;
  std::vector<JoyDetails> m_details;

  const Uint8 *m_keys = nullptr;
  int m_mouseX = 0, m_mouseY = 0, m_mouseZ = 0;
  int m_wheel = 0;
  Uint32 m_mouseButtons = 0;
};

#endif // INCLUDED_SDLINPUTSYSTEM_H
