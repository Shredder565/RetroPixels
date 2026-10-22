// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// CInputSystem: the base of the platform input back ends (SDL, DirectInput).
// A back end reports raw key, mouse and joystick state; this class turns
// mapping strings into input sources over that state, keeps the per-device
// settings from Supermodel.ini, and runs the console tools that record a
// mapping or calibrate a joystick axis.
//
// Mapping strings (any case):
//   KEY_<name>, KEY<n>_<name>             a key (any keyboard, or keyboard n)
//   MOUSE_<part>, MOUSE<n>_<part>         XAXIS, YAXIS_INV, ZAXIS_POS, LEFT,
//                                         WHEEL_UP, LEFT_BUTTON, BUTTON4, ...
//   JOY_<part>, JOY<n>_<part>             XAXIS, RZAXIS_NEG, SLIDER1, POV1_UP,
//                                         BUTTON12, ...
//   <name>                                a bare key name (older ini files)
//   A,B   either   A+B   together   !A   not A   NONE   nothing
// Devices are numbered from 1; 0 or no number means any device.

#ifndef INCLUDED_INPUTSYSTEM_H
#define INCLUDED_INPUTSYSTEM_H

#include <cstdio>
#include <string>
#include <vector>
#include "MultiInputSource.h"
#include "Util/NewConfig.h"

class CInput;
class CInputSource;

#define MAX_NAME_LENGTH 255

// ReadMapping flags: which devices to read, and whether to report them as
// "any device" (KEY_A) rather than numbered (KEY2_A).
#define READ_KEYBOARD 1
#define READ_MOUSE 2
#define READ_JOYSTICK 4
#define READ_MERGE_KEYBOARD 8
#define READ_MERGE_MOUSE 16
#define READ_MERGE_JOYSTICK 32
#define READ_ALL (READ_KEYBOARD|READ_MOUSE|READ_JOYSTICK)
#define READ_MERGE (READ_MERGE_KEYBOARD|READ_MERGE_MOUSE|READ_MERGE_JOYSTICK)

// Device number meaning "any" (and, from GetNumXXX(), "the system cannot
// tell devices apart").
#define ANY_KEYBOARD -1
#define ANY_MOUSE -1
#define ANY_JOYSTICK -1

// Setting defaults
#define DEFAULT_DIGITAL_SENSITIVITY 25
#define DEFAULT_DIGITAL_DECAYSPEED 50
#define DEFAULT_MSE_DEADZONE 0
#define DEFAULT_JOY_AXISMINVAL -32768
#define DEFAULT_JOY_AXISOFFVAL 0
#define DEFAULT_JOY_AXISMAXVAL 32767
#define DEFAULT_JOY_DEADZONE 3
#define DEFAULT_JOY_SATURATION 100

#define NUM_MOUSE_PARTS ((int)MouseButtonX2 + 1)
#define NUM_MOUSE_AXES 3
#define NUM_MOUSE_BUTTONS 5
#define NUM_JOY_PARTS ((int)JoyButton31 + 1)
#define NUM_JOY_AXES 8
#define NUM_JOY_POVS 4
#define NUM_JOY_BUTTONS 32

// Axis numbers
#define AXIS_X 0
#define AXIS_Y 1
#define AXIS_Z 2
#define AXIS_RX 3
#define AXIS_RY 4
#define AXIS_RZ 5
#define AXIS_S1 6
#define AXIS_S2 7

// How an axis is used: whole, whole and reversed, or one half
#define AXIS_FULL 0
#define AXIS_INVERTED 1
#define AXIS_POS 2
#define AXIS_NEG 3

// POV hat directions
#define POV_UP 0
#define POV_DOWN 1
#define POV_LEFT 2
#define POV_RIGHT 3

// Mouse parts: three axes (each whole, reversed, positive half, negative
// half) and five buttons.
enum EMousePart
{
  MouseUnknown = -1,
  MouseXAxis = 0, MouseXAxisInv, MouseXAxisPos, MouseXAxisNeg,
  MouseYAxis, MouseYAxisInv, MouseYAxisPos, MouseYAxisNeg,
  MouseZAxis, MouseZAxisInv, MouseZAxisPos, MouseZAxisNeg,
  MouseButtonLeft, MouseButtonMiddle, MouseButtonRight, MouseButtonX1, MouseButtonX2
};

// Joystick parts: eight axes (four ways each), four POV hats (four
// directions each) and 32 buttons.
enum EJoyPart
{
  JoyUnknown = -1,
  JoyXAxis = 0, JoyXAxisInv, JoyXAxisPos, JoyXAxisNeg,
  JoyYAxis, JoyYAxisInv, JoyYAxisPos, JoyYAxisNeg,
  JoyZAxis, JoyZAxisInv, JoyZAxisPos, JoyZAxisNeg,
  JoyRXAxis, JoyRXAxisInv, JoyRXAxisPos, JoyRXAxisNeg,
  JoyRYAxis, JoyRYAxisInv, JoyRYAxisPos, JoyRYAxisNeg,
  JoyRZAxis, JoyRZAxisInv, JoyRZAxisPos, JoyRZAxisNeg,
  JoyS1Axis, JoyS1AxisInv, JoyS1AxisPos, JoyS1AxisNeg,
  JoyS2Axis, JoyS2AxisInv, JoyS2AxisPos, JoyS2AxisNeg,
  JoyPOV0Up, JoyPOV0Down, JoyPOV0Left, JoyPOV0Right,
  JoyPOV1Up, JoyPOV1Down, JoyPOV1Left, JoyPOV1Right,
  JoyPOV2Up, JoyPOV2Down, JoyPOV2Left, JoyPOV2Right,
  JoyPOV3Up, JoyPOV3Down, JoyPOV3Left, JoyPOV3Right,
  JoyButton0, JoyButton1, JoyButton2, JoyButton3, JoyButton4, JoyButton5, JoyButton6, JoyButton7,
  JoyButton8, JoyButton9, JoyButton10, JoyButton11, JoyButton12, JoyButton13, JoyButton14, JoyButton15,
  JoyButton16, JoyButton17, JoyButton18, JoyButton19, JoyButton20, JoyButton21, JoyButton22, JoyButton23,
  JoyButton24, JoyButton25, JoyButton26, JoyButton27, JoyButton28, JoyButton29, JoyButton30, JoyButton31
};

// Keyboard settings (InputKey[n]Sensitivity, InputKey[n]DecaySpeed): how fast
// a key held for an analog control ramps up (1-100 %) and falls back (1-200 %
// of the ramp-up speed).
struct KeySettings
{
  int kbdNum;
  unsigned sensitivity;
  unsigned decaySpeed;

  KeySettings()
    : kbdNum(ANY_KEYBOARD), sensitivity(DEFAULT_DIGITAL_SENSITIVITY), decaySpeed(DEFAULT_DIGITAL_DECAYSPEED)
  {}
};

// Mouse settings (InputMouse[n]<X|Y|Z>DeadZone): dead zone in the middle, as
// a percentage (0-99) of the display width/height (X, Y) or wheel range (Z).
struct MouseSettings
{
  int mseNum;
  unsigned deadZones[NUM_MOUSE_AXES];

  MouseSettings()
    : mseNum(ANY_MOUSE), deadZones{ DEFAULT_MSE_DEADZONE, DEFAULT_MSE_DEADZONE, 0 }
  {}
};

// Joystick settings (InputJoy[n]<axis><MinVal|OffVal|MaxVal|DeadZone|
// Saturation>): each axis's raw range and rest value, dead zone (0-99 %) and
// saturation (1-200 %) of each half.
struct JoySettings
{
  int joyNum;
  int axisMinVals[NUM_JOY_AXES];
  int axisOffVals[NUM_JOY_AXES];
  int axisMaxVals[NUM_JOY_AXES];
  unsigned deadZones[NUM_JOY_AXES];
  unsigned saturations[NUM_JOY_AXES];

  JoySettings()
    : joyNum(ANY_JOYSTICK)
  {
    for (int a = 0; a < NUM_JOY_AXES; a++)
    {
      axisMinVals[a] = DEFAULT_JOY_AXISMINVAL;
      axisOffVals[a] = DEFAULT_JOY_AXISOFFVAL;
      axisMaxVals[a] = DEFAULT_JOY_AXISMAXVAL;
      deadZones[a] = DEFAULT_JOY_DEADZONE;
      saturations[a] = DEFAULT_JOY_SATURATION;
    }
  }
};

struct KeyDetails
{
  char name[MAX_NAME_LENGTH + 1];
};

struct MouseDetails
{
  char name[MAX_NAME_LENGTH + 1];
  bool isAbsolute;  // reports positions (e.g. a light gun)
};

struct JoyDetails
{
  char name[MAX_NAME_LENGTH + 1];
  int numAxes;
  int numPOVs;
  int numButtons;
  bool hasFFeedback;
  bool hasAxis[NUM_JOY_AXES];
  char axisName[NUM_JOY_AXES][MAX_NAME_LENGTH + 1];
  bool axisHasFF[NUM_JOY_AXES];
};

class CInputSystem
{
public:
  // The sources over the back end's state (defined in InputSystem.cpp).
  class CKeyInputSource;
  class CMseAxisInputSource;
  class CMseButInputSource;
  class CJoyAxisInputSource;
  class CJoyPOVInputSource;
  class CJoyButInputSource;

private:
  // One device kind's cache of sources: index 0 for "any device", then one
  // per device; each holds a source per part (a reference is kept on each).
  typedef std::vector<std::vector<CInputSource *>> SourceCache;

  int m_numKbds;
  int m_numMice;
  int m_numJoys;
  SourceCache m_keySources;
  SourceCache m_mseSources;
  SourceCache m_joySources;
  bool m_haveCache;

  KeySettings m_defKeySettings;
  MouseSettings m_defMseSettings;
  JoySettings m_defJoySettings;
  std::vector<KeySettings*> m_keySettings;
  std::vector<MouseSettings*> m_mseSettings;
  std::vector<JoySettings*> m_joySettings;

  // A source that is never active (for parts a device does not have).
  CMultiInputSource *m_emptySource;

  // InputDigitalSensitivity, InputDigitalDecaySpeed: used for mouse and
  // joystick buttons and POV hats.
  unsigned m_digitalSensitivity;
  unsigned m_digitalDecaySpeed;

  void CreateSourceCache();
  void ClearSourceCache(bool deleteCache = false);

  // The cached source for a part of a device (devNum ANY_xxx: any device).
  CInputSource *GetKeySource(int kbdNum, int keyIndex);
  CInputSource *GetMouseSource(int mseNum, EMousePart msePart);
  CInputSource *GetJoySource(int joyNum, EJoyPart joyPart);
  template <class Make>
  CInputSource *CachedSource(SourceCache &cache, int numDevs, int devNum, int part, Make make);

  // ReadMapping's scan: appends each newly active source (and its mapping)
  // that is not in 'ignore'.
  void CheckAllSources(unsigned readFlags, bool fullAxisOnly, bool &mseCentered, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore);
  void CheckKeySources(int kbdNum, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore);
  void CheckMouseSources(int mseNum, bool fullAxisOnly, bool mseCentered, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore);
  void CheckJoySources(int joyNum, bool fullAxisOnly, std::vector<CInputSource*> &sources, std::string &mapping, std::vector<CInputSource*> &ignore);

  // Mapping parsing: "A,B" (isOr) and "A+B" lists, and single controls.
  CInputSource* ParseMultiSource(const std::string& str, bool fullAxisOnly, bool isOr);
  CInputSource* ParseSingleSource(std::string str);
  CInputSource* ParseKeyName(int kbdNum, const std::string &keyName);

  void PrintCommonSettings() const;
  void LoadCommonSettings(const Util::Config::Node &config);
  void StoreCommonSettings(Util::Config::Node *config);
  static void PrintKeySettings(int kbdNum, KeySettings *settings);
  KeySettings *LoadKeySettings(const Util::Config::Node &config, int kbdNum);
  void StoreKeySettings(Util::Config::Node *config, KeySettings *settings);
  static void PrintMouseSettings(int mseNum, MouseSettings *settings);
  MouseSettings *LoadMouseSettings(const Util::Config::Node &config, int mseNum);
  void StoreMouseSettings(Util::Config::Node *config, MouseSettings *settings);
  void PrintJoySettings(int joyNum, JoySettings *settings);
  JoySettings *LoadJoySettings(const Util::Config::Node &config, int joyNum);
  void StoreJoySettings(Util::Config::Node *config, JoySettings *settings);

  // Polls until 'source' is released (true) or the window closes (false).
  bool WaitForRelease(CInputSource *source);

protected:
  // Display area, for scaling mouse positions.
  unsigned m_dispX;
  unsigned m_dispY;
  unsigned m_dispW;
  unsigned m_dispH;

  bool m_grabMouse;

  CInputSystem(const char *systemName);

  // Mouse parts
  static bool IsAxis(EMousePart msePart);
  static bool IsFullAxis(EMousePart msePart);
  static bool GetAxisDetails(EMousePart msePart, int &axisNum, int &axisDir);
  static bool IsButton(EMousePart msePart);
  static int GetButtonNumber(EMousePart msePart);
  static EMousePart GetMouseAxis(int axisNum, int axisDir);
  static EMousePart GetMouseButton(int butNum);

  // Joystick parts
  static bool IsAxis(EJoyPart joyPart);
  static bool IsFullAxis(EJoyPart joyPart);
  static bool IsSliderAxis(EJoyPart joyPart);
  static bool GetAxisDetails(EJoyPart joyPart, int &axisNum, int &axisDir);
  static bool IsPOV(EJoyPart joyPart);
  static bool GetPOVDetails(EJoyPart joyPart, int &povNum, int &povDir);
  static bool IsButton(EJoyPart joyPart);
  static int GetButtonNumber(EJoyPart joyPart);
  static EJoyPart GetJoyAxis(int axisNum, int axisDir);
  static EJoyPart GetJoyPOV(int povNum, int povDir);
  static EJoyPart GetJoyButton(int butNum);

  //
  // What a back end provides
  //

  virtual bool InitializeSystem() = 0;

  // The back end's index for a key name (see the list in InputSystem.cpp),
  // or -1 if it has no such key.
  virtual int GetKeyIndex(const char *keyName) = 0;
  virtual const char *GetKeyName(int keyIndex) = 0;
  virtual bool IsKeyPressed(int kbdNum, int keyIndex) const = 0;

  // X and Y: position in the window's coordinates; Z (wheel): -100 to 100.
  virtual int GetMouseAxisValue(int mseNum, int axisNum) const = 0;
  // -1, 0 or 1: which way the wheel moved.
  virtual int GetMouseWheelDir(int mseNum) const = 0;
  virtual bool IsMouseButPressed(int mseNum, int butNum) const = 0;

  // Raw axis value (normally -32768 to 32767).
  virtual int GetJoyAxisValue(int joyNum, int axisNum) const = 0;
  virtual bool IsJoyPOVInDir(int joyNum, int povNum, int povDir) const = 0;
  virtual bool IsJoyButPressed(int joyNum, int butNum) const = 0;

  virtual bool ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd) = 0;

  //
  // What a back end may override
  //

  // ReadMapping only takes mouse X/Y movement after the pointer has been in
  // the middle half of the display.
  virtual bool ConfigMouseCentered();

  // "Any device" sources: by default the parts of every device, either one.
  virtual CInputSource *CreateAnyKeySource(int keyIndex);
  virtual CInputSource *CreateAnyMouseSource(EMousePart msePart);
  virtual CInputSource *CreateAnyJoySource(EJoyPart joyPart);

  virtual CInputSource *CreateKeySource(int kbdNum, int keyIndex);
  virtual CInputSource *CreateMouseSource(int mseNum, EMousePart msePart);
  virtual CInputSource *CreateJoySource(int joyNum, EJoyPart joyPart);

public:
  // "X-Axis", ..., "Slider2" ("" for a bad number).
  static const char *GetDefaultAxisName(int axisNum);

  const char *name;

  virtual ~CInputSystem();

  // Starts the back end; false if it cannot be used.
  bool Initialize();

  void SetDisplayGeom(unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH);

  // Number of devices: 0 for none, ANY_xxx if they cannot be told apart.
  virtual int GetNumKeyboards() const = 0;
  virtual int GetNumMice() const = 0;
  virtual int GetNumJoysticks() const = 0;

  // null if there is no such device
  virtual const KeyDetails *GetKeyDetails(int kbdNum) = 0;
  virtual const MouseDetails *GetMouseDetails(int mseNum) = 0;
  virtual const JoyDetails *GetJoyDetails(int joyNum) = 0;

  void ClearSettings();

  void PrintSettings();

  virtual void LoadFromConfig(const Util::Config::Node &config);

  virtual void StoreToConfig(Util::Config::Node *config);

  // The settings of a device (ANY_xxx: the shared ones). If it has none of
  // its own: null, or with useDefault the shared (else default) settings.
  KeySettings *GetKeySettings(int kbdNum, bool useDefault);
  MouseSettings *GetMouseSettings(int mseNum, bool useDefault);
  JoySettings *GetJoySettings(int joyNum, bool useDefault);

  // The source for a mapping string, or null if it cannot be used. With
  // fullAxisOnly only whole-axis controls are accepted (for axis inputs),
  // otherwise only the others.
  CInputSource* ParseSource(const char *mapping, bool fullAxisOnly = false);

  // Waits until the user operates some controls and lets go, and writes
  // their mapping (e.g. "KEY_ALT+KEY_P" or "JOY1_XAXIS") to buffer. False
  // if the escape mapping was used or the window closed.
  bool ReadMapping(char *buffer, unsigned bufSize, bool fullAxisOnly = false, unsigned readFlags = READ_ALL, const char *escapeMapping = "KEY_ESCAPE");

  // Reads the devices' current state; false if the program should quit.
  virtual bool Poll() = 0;

  virtual void GrabMouse();

  virtual void UngrabMouse();

  virtual void SetMouseVisibility(bool visible) = 0;

  virtual bool SendForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd);

  // Console tools: find which axis the user moves the most, and measure an
  // axis's range and rest position.
  bool DetectJoystickAxis(unsigned joyNum, unsigned &axisNum, const char *escapeMapping = "KEY_ESCAPE", const char *confirmMapping = "KEY_RETURN");

  bool CalibrateJoystickAxis(unsigned joyNum, unsigned axisNum, const char *escapeMapping = "KEY_ESCAPE", const char *confirmMapping = "KEY_RETURN");

  void PrintDevices();
};

#endif  // INCLUDED_INPUTSYSTEM_H
