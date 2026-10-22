// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// CInputs: every control the emulator knows, for every game, plus the
// emulator's own UI keys. The Model 3 board and the drive boards read the
// public members; Poll() updates the ones the running game uses.

#ifndef INCLUDED_INPUTS_H
#define INCLUDED_INPUTS_H

#include "InputTypes.h"
#include "Types.h"
#include "Util/NewConfig.h"
#include <vector>
#include <memory>

class CInputSystem;
class CInput;
struct Game;

class CInputs
{
private:
  std::shared_ptr<CInputSystem> m_system;

  // In the order they are listed, configured and polled (an axis after its
  // direction inputs, the shifter and triggers after their buttons).
  std::vector<std::shared_ptr<CInput>> m_inputs;

  template <class T>
  std::shared_ptr<T> Keep(std::shared_ptr<T> input);

  std::shared_ptr<CSwitchInput> Switch(const char *id, const char *label, unsigned gameFlags, const char *defaultMapping = "NONE");
  std::shared_ptr<CAnalogInput> Analog(const char *id, const char *label, unsigned gameFlags);
  std::shared_ptr<CAxisInput> Axis(const char *id, const char *label, unsigned gameFlags,
    std::shared_ptr<CAnalogInput> neg, std::shared_ptr<CAnalogInput> pos, UINT16 minVal = 0x00, UINT16 offVal = 0x80, UINT16 maxVal = 0xFF);

  std::vector<std::shared_ptr<CInput>> InputsFor(uint32_t gameFlags);

  void PrintHeader(const char *fmt, ...);

  void PrintConfigureInputsHelp();

public:
  // Emulator UI
  std::shared_ptr<CSwitchInput> uiExit;
  std::shared_ptr<CSwitchInput> uiReset;
  std::shared_ptr<CSwitchInput> uiPause;
  std::shared_ptr<CSwitchInput> uiFullScreen;
  std::shared_ptr<CSwitchInput> uiSaveState;
  std::shared_ptr<CSwitchInput> uiChangeSlot;
  std::shared_ptr<CSwitchInput> uiLoadState;
  std::shared_ptr<CSwitchInput> uiMusicVolUp;
  std::shared_ptr<CSwitchInput> uiMusicVolDown;
  std::shared_ptr<CSwitchInput> uiSoundVolUp;
  std::shared_ptr<CSwitchInput> uiSoundVolDown;
  std::shared_ptr<CSwitchInput> uiClearNVRAM;
  std::shared_ptr<CSwitchInput> uiSelectCrosshairs;
  std::shared_ptr<CSwitchInput> uiToggleFrLimit;
  std::shared_ptr<CSwitchInput> uiDumpInpState;
  std::shared_ptr<CSwitchInput> uiDumpTimings;
  std::shared_ptr<CSwitchInput> uiScreenshot;

  // Every game: coins, start, test and service
  std::shared_ptr<CSwitchInput> coin[2];
  std::shared_ptr<CSwitchInput> start[2];
  std::shared_ptr<CSwitchInput> test[2];
  std::shared_ptr<CSwitchInput> service[2];

  // 4-way joysticks, players 1 and 2
  std::shared_ptr<CSwitchInput> up[2];
  std::shared_ptr<CSwitchInput> down[2];
  std::shared_ptr<CSwitchInput> left[2];
  std::shared_ptr<CSwitchInput> right[2];

  // Fighting games, players 1 and 2
  std::shared_ptr<CSwitchInput> punch[2];
  std::shared_ptr<CSwitchInput> kick[2];
  std::shared_ptr<CSwitchInput> guard[2];
  std::shared_ptr<CSwitchInput> escape[2];

  // Spikeout
  std::shared_ptr<CSwitchInput> shift;
  std::shared_ptr<CSwitchInput> beat;
  std::shared_ptr<CSwitchInput> charge;
  std::shared_ptr<CSwitchInput> jump;

  // Virtua Striker, players 1 and 2
  std::shared_ptr<CSwitchInput> shortPass[2];
  std::shared_ptr<CSwitchInput> longPass[2];
  std::shared_ptr<CSwitchInput> shoot[2];

  // Driving
  std::shared_ptr<CAxisInput> steering;
  std::shared_ptr<CAnalogInput> accelerator;
  std::shared_ptr<CAnalogInput> brake;

  // VR view buttons: red, blue, yellow, green
  std::shared_ptr<CSwitchInput> vr[4];

  std::shared_ptr<CSwitchInput> gearShiftUp;
  std::shared_ptr<CSwitchInput> gearShiftDown;

  std::shared_ptr<CGearShift4Input> gearShift4;

  // Rally
  std::shared_ptr<CSwitchInput> viewChange;
  std::shared_ptr<CSwitchInput> handBrake;

  // Harley-Davidson
  std::shared_ptr<CAnalogInput> rearBrake;
  std::shared_ptr<CSwitchInput> musicSelect;

  // Virtual On twin sticks, each direction mapped (1 = left stick, 2 = right)
  std::shared_ptr<CSwitchInput> twinJoyLeft1;
  std::shared_ptr<CSwitchInput> twinJoyLeft2;
  std::shared_ptr<CSwitchInput> twinJoyRight1;
  std::shared_ptr<CSwitchInput> twinJoyRight2;
  std::shared_ptr<CSwitchInput> twinJoyUp1;
  std::shared_ptr<CSwitchInput> twinJoyUp2;
  std::shared_ptr<CSwitchInput> twinJoyDown1;
  std::shared_ptr<CSwitchInput> twinJoyDown2;
  std::shared_ptr<CSwitchInput> twinJoyShot1;
  std::shared_ptr<CSwitchInput> twinJoyShot2;
  std::shared_ptr<CSwitchInput> twinJoyTurbo1;
  std::shared_ptr<CSwitchInput> twinJoyTurbo2;

  // Virtual On movement macros (for a single stick)
  std::shared_ptr<CSwitchInput> twinJoyTurnLeft;
  std::shared_ptr<CSwitchInput> twinJoyTurnRight;
  std::shared_ptr<CSwitchInput> twinJoyStrafeLeft;
  std::shared_ptr<CSwitchInput> twinJoyStrafeRight;
  std::shared_ptr<CSwitchInput> twinJoyForward;
  std::shared_ptr<CSwitchInput> twinJoyReverse;
  std::shared_ptr<CSwitchInput> twinJoyJump;
  std::shared_ptr<CSwitchInput> twinJoyCrouch;

  // Analog joystick (Star Wars Trilogy and others)
  std::shared_ptr<CAxisInput> analogJoyX;
  std::shared_ptr<CAxisInput> analogJoyY;
  std::shared_ptr<CSwitchInput> analogJoyTrigger1;
  std::shared_ptr<CSwitchInput> analogJoyTrigger2;
  std::shared_ptr<CSwitchInput> analogJoyEvent1;
  std::shared_ptr<CSwitchInput> analogJoyEvent2;

  // Light guns, players 1 and 2
  std::shared_ptr<CAxisInput> gunX[2];
  std::shared_ptr<CAxisInput> gunY[2];
  std::shared_ptr<CTriggerInput> trigger[2];

  // Analog guns, players 1 and 2
  std::shared_ptr<CAxisInput> analogGunX[2];
  std::shared_ptr<CAxisInput> analogGunY[2];
  std::shared_ptr<CSwitchInput> analogTriggerLeft[2];
  std::shared_ptr<CSwitchInput> analogTriggerRight[2];

  // Ski Champ
  std::shared_ptr<CAxisInput> skiX;
  std::shared_ptr<CAxisInput> skiY;
  std::shared_ptr<CSwitchInput> skiPollLeft;
  std::shared_ptr<CSwitchInput> skiPollRight;
  std::shared_ptr<CSwitchInput> skiSelect1;
  std::shared_ptr<CSwitchInput> skiSelect2;
  std::shared_ptr<CSwitchInput> skiSelect3;

  // Magical Truck Adventure
  std::shared_ptr<CAxisInput> magicalLever1;
  std::shared_ptr<CAxisInput> magicalLever2;
  std::shared_ptr<CSwitchInput> magicalPedal1;
  std::shared_ptr<CSwitchInput> magicalPedal2;

  // Sega Bass Fishing
  std::shared_ptr<CAxisInput> fishingRodX;
  std::shared_ptr<CAxisInput> fishingRodY;
  std::shared_ptr<CAnalogInput> fishingReel;
  std::shared_ptr<CAxisInput> fishingStickX;
  std::shared_ptr<CAxisInput> fishingStickY;
  std::shared_ptr<CSwitchInput> fishingCast;
  std::shared_ptr<CSwitchInput> fishingSelect;
  std::shared_ptr<CAnalogInput> fishingTension;

  CInputs(std::shared_ptr<CInputSystem> system);

  ~CInputs();

  unsigned Count();

  std::shared_ptr<CInput> operator[](const unsigned index);

  // By id or label (any case); null if there is none.
  std::shared_ptr<CInput> operator[](const char *idOrLabel);

  std::shared_ptr<CInputSystem> GetInputSystem();

  // Initializes the input system and then every input.
  bool Initialize();

  void LoadFromConfig(const Util::Config::Node &config);

  void StoreToConfig(Util::Config::Node *config);

  // Interactive mapping on the console for the game's inputs (all inputs
  // if the game has no name). False if the user left without saving.
  bool ConfigureInputs(const Game &game);

  // The same, after giving the input system the display geometry.
  bool ConfigureInputs(const Game &game, unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH);

  // The mappable inputs of the game (all of them if it has no name).
  std::vector<std::shared_ptr<CInput>> GetGameInputs(const Game& game);

  // Interactive joystick calibration on the console.
  void CalibrateJoysticks();

  void CalibrateJoystick(int joyNum);

  // Prints the mappings of the game's inputs (all if game is null).
  void PrintInputs(const Game *game);

  // Polls the input system, then the UI inputs and those of the game (all
  // if game is null). False if the input system says to quit.
  bool Poll(const Game *game, unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH);

  // Prints the current values (UI inputs and the game's).
  void DumpState(const Game *game);
};

#endif  // INCLUDED_INPUTS_H
