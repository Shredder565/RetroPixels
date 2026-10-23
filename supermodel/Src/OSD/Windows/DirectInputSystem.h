// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Windows input. Keyboards and mice are read through DirectInput, which sees
// one shared keyboard and mouse, or through Raw Input, which tells devices
// apart (two light guns, for instance). Joysticks are read through
// DirectInput; with XInput turned on, Xbox-style pads are read through XInput
// instead, which reports the two triggers as separate axes.

#ifndef INCLUDED_DIRECTINPUTSYSTEM_H
#define INCLUDED_DIRECTINPUTSYSTEM_H

#include "Types.h"
#include "Inputs/Input.h"
#include "Inputs/InputSource.h"
#include "Inputs/InputSystem.h"

#include <SDL.h>

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <dinput.h>
#include <XInput.h>

#include <array>
#include <string>
#include <vector>

class CDirectInputSystem : public CInputSystem
{
public:
  // useRawInput: keyboards and mice through Raw Input (DirectInput when that
  // finds nothing). useXInput: Xbox-style pads through XInput.
  CDirectInputSystem(const Util::Config::Node &config, SDL_Window *window, bool useRawInput, bool useXInput);
  ~CDirectInputSystem() override;

  int GetNumKeyboards() const override;
  int GetNumMice() const override;
  int GetNumJoysticks() const override;
  const KeyDetails *GetKeyDetails(int kbdNum) override;
  const MouseDetails *GetMouseDetails(int mseNum) override;
  const JoyDetails *GetJoyDetails(int joyNum) override;

  bool Poll() override;
  void GrabMouse() override;
  void UngrabMouse() override;
  void SetMouseVisibility(bool visible) override;

protected:
  bool InitializeSystem() override;
  int GetKeyIndex(const char *keyName) override;
  const char *GetKeyName(int keyIndex) override;
  bool IsKeyPressed(int kbdNum, int keyIndex) const override;
  int GetMouseAxisValue(int mseNum, int axisNum) const override;
  int GetMouseWheelDir(int mseNum) const override;
  bool IsMouseButPressed(int mseNum, int butNum) const override;
  int GetJoyAxisValue(int joyNum, int axisNum) const override;
  bool IsJoyPOVInDir(int joyNum, int povNum, int povDir) const override;
  bool IsJoyButPressed(int joyNum, int butNum) const override;
  bool ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd) override;
  bool ConfigMouseCentered() override;
  CInputSource *CreateAnyMouseSource(EMousePart msePart) override;

private:
  // A mouse as the game sees it: position in window pixels, wheel position
  // (-100 to 100), wheel movement since the last poll, and buttons as bits
  // (0 left, 1 middle, 2 right, 3 and 4 the side buttons).
  struct MouseState
  {
    LONG x = 0;
    LONG y = 0;
    LONG z = 0;
    LONG wheelMoved = 0;
    int wheelDir = 0;
    unsigned buttons = 0;
  };

  struct RawKeyboard
  {
    HANDLE handle;
    std::array<bool, 256> down;  // by DirectInput scan code
  };

  struct RawMouse
  {
    HANDLE handle;
    MouseState state;
  };

  // One slot per force feedback effect kind (FFConstantForce .. FFVibrate).
  using AxisEffects = std::array<LPDIRECTINPUTEFFECT, 4>;

  struct Pad
  {
    JoyDetails details;
    DIJOYSTATE2 state;
    bool xinput = false;
    DWORD xinputSlot = 0;
    LPDIRECTINPUTDEVICE8 device = nullptr;
    std::array<AxisEffects, NUM_JOY_AXES> effects{};
    // XInput motor speeds asked for by each kind of effect
    WORD pushLeft = 0;
    WORD pushRight = 0;
    WORD shake = 0;
  };

  using XInputGetCapabilitiesFn = DWORD (WINAPI *)(DWORD, DWORD, XINPUT_CAPABILITIES *);
  using XInputGetStateFn = DWORD (WINAPI *)(DWORD, XINPUT_STATE *);
  using XInputSetStateFn = DWORD (WINAPI *)(DWORD, XINPUT_VIBRATION *);

  const Util::Config::Node &m_config;
  SDL_Window *m_window;
  HWND m_hwnd = nullptr;
  bool m_useRawInput;
  bool m_useXInput;
  bool m_comStarted = false;
  bool m_activated = false;
  DWORD m_screenW = 0;
  DWORD m_screenH = 0;

  // Raw Input
  std::vector<RawKeyboard> m_rawKeyboards;
  std::vector<RawMouse> m_rawMice;
  std::vector<KeyDetails> m_keyDetails;
  std::vector<MouseDetails> m_mouseDetails;
  MouseState m_allMice;
  std::vector<BYTE> m_rawBuffer;

  // DirectInput
  LPDIRECTINPUT8 m_di = nullptr;
  LPDIRECTINPUTDEVICE8 m_diKeyboard = nullptr;
  LPDIRECTINPUTDEVICE8 m_diMouse = nullptr;
  BYTE m_diKeys[256] = {};
  MouseState m_diMouseState;

  // Joysticks, in DirectInput's order
  std::vector<Pad> m_pads;

  XInputGetCapabilitiesFn m_xiGetCapabilities = nullptr;
  XInputGetStateFn m_xiGetState = nullptr;
  XInputSetStateFn m_xiSetState = nullptr;

  void LoadXInput();
  void OpenKeyboardsAndMice();
  bool OpenRawDevices();
  void OpenDirectInputKeyboardAndMouse();
  void OpenJoysticks();
  bool OpenDirectInputPad(const DIDEVICEINSTANCE &instance, int number, Pad &pad);
  void Activate();
  void RegisterRawInput(DWORD flags);
  void PollKeyboardsAndMice();
  void PollJoysticks();
  void CloseKeyboardsAndMice();
  void CloseJoysticks();
  void SyncMiceToCursor();
  void ReadRawInput(HRAWINPUT input);
  bool RumbleXInput(Pad &pad, int axisNum, const ForceFeedbackCmd &cmd);
  bool ForceDirectInput(Pad &pad, int axisNum, const ForceFeedbackCmd &cmd);
};

#endif // INCLUDED_DIRECTINPUTSYSTEM_H
