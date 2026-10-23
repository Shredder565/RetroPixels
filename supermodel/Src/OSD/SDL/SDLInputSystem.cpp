// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "SDLInputSystem.h"

#include "Supermodel.h"
#include "Inputs/Input.h"

#include <cstring>
#include <vector>

namespace
{
  struct KeyName
  {
    const char *name;
    SDL_Scancode code;
  };

  // The key names Supermodel.ini uses, and their keys. (Num/Caps/Scroll
  // Lock are left out: SDL reports them unreliably.)
  const KeyName kKeys[] =
  {
    { "BACKSPACE", SDL_SCANCODE_BACKSPACE }, { "TAB", SDL_SCANCODE_TAB }, { "CLEAR", SDL_SCANCODE_CLEAR },
    { "RETURN", SDL_SCANCODE_RETURN }, { "PAUSE", SDL_SCANCODE_PAUSE }, { "ESCAPE", SDL_SCANCODE_ESCAPE },
    { "SPACE", SDL_SCANCODE_SPACE }, { "QUOTE", SDL_SCANCODE_APOSTROPHE },
    { "LEFTPAREN", SDL_SCANCODE_KP_LEFTPAREN }, { "RIGHTPAREN", SDL_SCANCODE_KP_RIGHTPAREN },
    { "COMMA", SDL_SCANCODE_COMMA }, { "MINUS", SDL_SCANCODE_MINUS }, { "PERIOD", SDL_SCANCODE_PERIOD },
    { "SLASH", SDL_SCANCODE_SLASH },
    { "0", SDL_SCANCODE_0 }, { "1", SDL_SCANCODE_1 }, { "2", SDL_SCANCODE_2 }, { "3", SDL_SCANCODE_3 },
    { "4", SDL_SCANCODE_4 }, { "5", SDL_SCANCODE_5 }, { "6", SDL_SCANCODE_6 }, { "7", SDL_SCANCODE_7 },
    { "8", SDL_SCANCODE_8 }, { "9", SDL_SCANCODE_9 },
    { "SEMICOLON", SDL_SCANCODE_SEMICOLON }, { "EQUALS", SDL_SCANCODE_EQUALS },
    { "LEFTBRACKET", SDL_SCANCODE_LEFTBRACKET }, { "BACKSLASH", SDL_SCANCODE_BACKSLASH },
    { "RIGHTBRACKET", SDL_SCANCODE_RIGHTBRACKET }, { "BACKQUOTE", SDL_SCANCODE_GRAVE },
    { "A", SDL_SCANCODE_A }, { "B", SDL_SCANCODE_B }, { "C", SDL_SCANCODE_C }, { "D", SDL_SCANCODE_D },
    { "E", SDL_SCANCODE_E }, { "F", SDL_SCANCODE_F }, { "G", SDL_SCANCODE_G }, { "H", SDL_SCANCODE_H },
    { "I", SDL_SCANCODE_I }, { "J", SDL_SCANCODE_J }, { "K", SDL_SCANCODE_K }, { "L", SDL_SCANCODE_L },
    { "M", SDL_SCANCODE_M }, { "N", SDL_SCANCODE_N }, { "O", SDL_SCANCODE_O }, { "P", SDL_SCANCODE_P },
    { "Q", SDL_SCANCODE_Q }, { "R", SDL_SCANCODE_R }, { "S", SDL_SCANCODE_S }, { "T", SDL_SCANCODE_T },
    { "U", SDL_SCANCODE_U }, { "V", SDL_SCANCODE_V }, { "W", SDL_SCANCODE_W }, { "X", SDL_SCANCODE_X },
    { "Y", SDL_SCANCODE_Y }, { "Z", SDL_SCANCODE_Z },
    { "DEL", SDL_SCANCODE_DELETE },
    { "KEYPAD0", SDL_SCANCODE_KP_0 }, { "KEYPAD1", SDL_SCANCODE_KP_1 }, { "KEYPAD2", SDL_SCANCODE_KP_2 },
    { "KEYPAD3", SDL_SCANCODE_KP_3 }, { "KEYPAD4", SDL_SCANCODE_KP_4 }, { "KEYPAD5", SDL_SCANCODE_KP_5 },
    { "KEYPAD6", SDL_SCANCODE_KP_6 }, { "KEYPAD7", SDL_SCANCODE_KP_7 }, { "KEYPAD8", SDL_SCANCODE_KP_8 },
    { "KEYPAD9", SDL_SCANCODE_KP_9 },
    { "KEYPADPERIOD", SDL_SCANCODE_KP_PERIOD }, { "KEYPADDIVIDE", SDL_SCANCODE_KP_DIVIDE },
    { "KEYPADMULTIPLY", SDL_SCANCODE_KP_MULTIPLY }, { "KEYPADMINUS", SDL_SCANCODE_KP_MINUS },
    { "KEYPADPLUS", SDL_SCANCODE_KP_PLUS }, { "KEYPADENTER", SDL_SCANCODE_KP_ENTER },
    { "KEYPADEQUALS", SDL_SCANCODE_KP_EQUALS },
    { "UP", SDL_SCANCODE_UP }, { "DOWN", SDL_SCANCODE_DOWN }, { "RIGHT", SDL_SCANCODE_RIGHT },
    { "LEFT", SDL_SCANCODE_LEFT }, { "INSERT", SDL_SCANCODE_INSERT }, { "HOME", SDL_SCANCODE_HOME },
    { "END", SDL_SCANCODE_END }, { "PGUP", SDL_SCANCODE_PAGEUP }, { "PGDN", SDL_SCANCODE_PAGEDOWN },
    { "F1", SDL_SCANCODE_F1 }, { "F2", SDL_SCANCODE_F2 }, { "F3", SDL_SCANCODE_F3 }, { "F4", SDL_SCANCODE_F4 },
    { "F5", SDL_SCANCODE_F5 }, { "F6", SDL_SCANCODE_F6 }, { "F7", SDL_SCANCODE_F7 }, { "F8", SDL_SCANCODE_F8 },
    { "F9", SDL_SCANCODE_F9 }, { "F10", SDL_SCANCODE_F10 }, { "F11", SDL_SCANCODE_F11 },
    { "F12", SDL_SCANCODE_F12 }, { "F13", SDL_SCANCODE_F13 }, { "F14", SDL_SCANCODE_F14 },
    { "F15", SDL_SCANCODE_F15 },
    { "RIGHTSHIFT", SDL_SCANCODE_RSHIFT }, { "LEFTSHIFT", SDL_SCANCODE_LSHIFT },
    { "RIGHTCTRL", SDL_SCANCODE_RCTRL }, { "LEFTCTRL", SDL_SCANCODE_LCTRL },
    { "RIGHTALT", SDL_SCANCODE_RALT }, { "LEFTALT", SDL_SCANCODE_LALT },
    { "RIGHTMETA", SDL_SCANCODE_RGUI }, { "LEFTMETA", SDL_SCANCODE_LGUI }, { "ALTGR", SDL_SCANCODE_MODE },
    { "HELP", SDL_SCANCODE_HELP }, { "SYSREQ", SDL_SCANCODE_SYSREQ }, { "MENU", SDL_SCANCODE_MENU },
    { "POWER", SDL_SCANCODE_POWER }, { "UNDO", SDL_SCANCODE_UNDO }
  };
  const int kNumKeys = int(sizeof(kKeys) / sizeof(kKeys[0]));

  // Game controller mode: Supermodel's buttons 0..16 and the d-pad.
  const SDL_GameControllerButton kPadButtons[] =
  {
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_PADDLE1, SDL_CONTROLLER_BUTTON_PADDLE2, SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_PADDLE3, SDL_CONTROLLER_BUTTON_PADDLE4, SDL_CONTROLLER_BUTTON_MISC1,
    SDL_CONTROLLER_BUTTON_TOUCHPAD
  };

  // ... and its axes, in Supermodel's order X, Y, Z, RX, RY, RZ.
  const SDL_GameControllerAxis kPadAxes[] =
  {
    SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, SDL_CONTROLLER_AXIS_TRIGGERRIGHT
  };

  // The effects, built the same way each time they are made, changed or
  // stopped. Every one uses a cartesian direction along X.
  SDL_HapticEffect Blank(Uint16 type)
  {
    SDL_HapticEffect e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.constant.direction.type = SDL_HAPTIC_CARTESIAN;   // same place in every kind
    return e;
  }

  SDL_HapticEffect Constant(Uint32 length, Sint16 level)
  {
    SDL_HapticEffect e = Blank(SDL_HAPTIC_CONSTANT);
    e.constant.length = length;
    e.constant.level = level;
    return e;
  }

  SDL_HapticEffect Sine(Uint32 length, Sint16 magnitude)
  {
    SDL_HapticEffect e = Blank(SDL_HAPTIC_SINE);
    e.periodic.length = length;
    e.periodic.period = 50;
    e.periodic.magnitude = magnitude;
    return e;
  }

  SDL_HapticEffect Condition(Uint16 type, Sint16 coefficient)
  {
    SDL_HapticEffect e = Blank(type);
    e.condition.length = SDL_HAPTIC_INFINITY;
    e.condition.left_sat[0] = 0xffff;
    e.condition.right_sat[0] = 0xffff;
    e.condition.left_coeff[0] = coefficient;
    e.condition.right_coeff[0] = coefficient;
    return e;
  }

  // Lengths used when the effects are made (and stopped).
  const Uint32 kConstantIdle = 30;
  const Uint32 kSineIdle = 500;

  // A 0..1 strength as an SDL level.
  int Level(float strength)
  {
    return int(strength * INT16_MAX);
  }
}

CSDLInputSystem::CSDLInputSystem(const Util::Config::Node &config, bool useGameController)
  : CInputSystem("SDL"),
    m_config(config),
    m_gameController(useGameController)
{
}

CSDLInputSystem::~CSDLInputSystem()
{
  CloseDevices();
}

bool CSDLInputSystem::InitializeSystem()
{
  const Uint32 subsystems = (m_gameController ? SDL_INIT_GAMECONTROLLER : SDL_INIT_JOYSTICK) | SDL_INIT_HAPTIC;
  if (SDL_InitSubSystem(subsystems) != 0)
  {
    ErrorLog("Unable to initialize SDL joystick subsystem (%s).\n", SDL_GetError());
    return false;
  }
  if (m_gameController)
    SDL_GameControllerEventState(SDL_ENABLE);
  else
    SDL_JoystickEventState(SDL_ENABLE);
  OpenDevices();
  return true;
}

void CSDLInputSystem::OpenDevices()
{
  // Keep reading joysticks when the window is in the background (several
  // linked copies on one machine).
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

  const int count = SDL_NumJoysticks();
  for (int i = 0; i < count; i++)
  {
    Device dev;
    if (m_gameController)
    {
      dev.pad = SDL_GameControllerOpen(i);
      dev.joystick = SDL_GameControllerGetJoystick(dev.pad);
    }
    else
      dev.joystick = SDL_JoystickOpen(i);
    if (!dev.joystick)
    {
      ErrorLog("Unable to open joystick device %d with SDL - skipping joystick.\n", i + 1);
      continue;
    }

    JoyDetails details;
    const char *name = m_gameController ? SDL_GameControllerName(dev.pad) : SDL_JoystickName(dev.joystick);
    strncpy(details.name, name ? name : "", MAX_NAME_LENGTH);
    details.name[MAX_NAME_LENGTH] = '\0';
    if (m_gameController)
    {
      details.numAxes = 6;
      details.numPOVs = 4;
      details.numButtons = 16;
    }
    else
    {
      details.numAxes = SDL_JoystickNumAxes(dev.joystick);
      details.numPOVs = SDL_JoystickNumHats(dev.joystick);
      details.numButtons = SDL_JoystickNumButtons(dev.joystick);
    }

    // Wheels and joysticks report force feedback axes; pads only rumble.
    int hapticAxes = 0;
    details.hasFFeedback = SDL_JoystickIsHaptic(dev.joystick) ? true : false;
    if (details.hasFFeedback)
    {
      dev.haptic = SDL_HapticOpenFromJoystick(dev.joystick);
      if (!dev.haptic)
      {
        ErrorLog("Unable to obtain haptic interface for joystick %s. Force feedback will be disabled for this joystick.", details.name);
        details.hasFFeedback = false;
      }
      else
      {
        hapticAxes = SDL_HapticNumAxes(dev.haptic);
#ifndef _WIN32
        // SDL on Linux always claims 2 axes: a device that can't even play
        // a constant force is a pad.
        SDL_HapticEffect probe = Constant(kConstantIdle, 0);
        if (!SDL_HapticEffectSupported(dev.haptic, &probe))
          hapticAxes = 0;
#endif
        DebugLog("joy num %d haptic num axe %d name : %s\n", i, hapticAxes, SDL_HapticName(i));
      }
    }

    // Which axes carry force feedback can't be told; all of them do.
    for (int a = 0; a < NUM_JOY_AXES; a++)
    {
      details.hasAxis[a] = details.numAxes > a;
      details.axisHasFF[a] = hapticAxes > 0 && details.hasAxis[a];
      strcpy(details.axisName[a], CInputSystem::GetDefaultAxisName(a));
    }

    if (details.hasFFeedback && dev.haptic)
      SetUpEffects(dev, details, i, hapticAxes);

    m_devices.push_back(dev);
    m_details.push_back(details);
  }
}

void CSDLInputSystem::SetUpEffects(Device &dev, JoyDetails &details, int index, int hapticAxes)
{
  if (hapticAxes == 0)
  {
    // A pad: rumble stands in for force feedback on the X and Y sticks.
    const bool ok = SDL_HapticRumbleInit(dev.haptic) >= 0;
    if (!ok)
      ErrorLog("Unable to create rumble effect for pad %s (pad id=%d). Rumble will not be applied.SDL_HapticRumbleInit failed : %s", details.name, index + 1, SDL_GetError());
    details.axisHasFF[AXIS_X] = ok;
    details.axisHasFF[AXIS_Y] = ok;
    return;
  }

  SDL_HapticSetAutocenter(dev.haptic, 0);
  SDL_HapticSetGain(dev.haptic, 100);
  const unsigned supported = unsigned(SDL_HapticQuery(dev.haptic));

  struct Kind
  {
    unsigned bit;
    int Device::*slot;
    SDL_HapticEffect effect;
    const char *what;
  };
  const Kind kinds[] =
  {
    { SDL_HAPTIC_CONSTANT, &Device::constant, Constant(kConstantIdle, 0), "constant force effect for joystick %s (joy id=%d). Constant force" },
    { SDL_HAPTIC_SINE, &Device::sine, Sine(kSineIdle, 0), "vibration effect for joystick %s (joy id=%d). Vibration" },
    { SDL_HAPTIC_SPRING, &Device::spring, Condition(SDL_HAPTIC_SPRING, 0), "spring force effect for joystick %s (joy id=%d). Spring force" },
    { SDL_HAPTIC_FRICTION, &Device::friction, Condition(SDL_HAPTIC_FRICTION, 0), "friction force effect for joystick %s (joy id=%d). Friction force" },
  };
  for (const Kind &k : kinds)
  {
    if (!(supported & k.bit))
      continue;
    SDL_HapticEffect e = k.effect;
    dev.*k.slot = SDL_HapticNewEffect(dev.haptic, &e);
    if (dev.*k.slot < 0)
    {
      char what[160];
      snprintf(what, sizeof(what), k.what, details.name, index + 1);
      ErrorLog("Unable to create %s will not be applied.", what);
    }
  }
}

void CSDLInputSystem::CloseDevices()
{
  for (size_t i = 0; i < m_devices.size(); i++)
  {
    Device &dev = m_devices[i];
    if (m_details[i].hasFFeedback)
    {
      for (int Device::*slot : { &Device::constant, &Device::sine, &Device::spring, &Device::friction })
      {
        if (dev.*slot >= 0)
          SDL_HapticDestroyEffect(dev.haptic, dev.*slot);
      }
      SDL_HapticClose(dev.haptic);
    }
    if (m_gameController)
      SDL_GameControllerClose(dev.pad);
    else
      SDL_JoystickClose(dev.joystick);
  }
  m_devices.clear();
  m_details.clear();
}

int CSDLInputSystem::GetKeyIndex(const char *keyName)
{
  for (int i = 0; i < kNumKeys; i++)
  {
    if (SDL_strcasecmp(keyName, kKeys[i].name) == 0)
      return i;
  }
  return -1;
}

const char *CSDLInputSystem::GetKeyName(int keyIndex)
{
  return keyIndex >= 0 && keyIndex < kNumKeys ? kKeys[keyIndex].name : nullptr;
}

bool CSDLInputSystem::IsKeyPressed(int, int keyIndex) const
{
  return m_keys[kKeys[keyIndex].code] != 0;
}

int CSDLInputSystem::GetMouseAxisValue(int, int axisNum) const
{
  switch (axisNum)
  {
  case AXIS_X: return m_mouseX;
  case AXIS_Y: return m_mouseY;
  case AXIS_Z: return m_mouseZ;
  default:     return 0;
  }
}

int CSDLInputSystem::GetMouseWheelDir(int) const
{
  return m_wheel;
}

bool CSDLInputSystem::IsMouseButPressed(int, int butNum) const
{
  // left, middle, right, X1, X2
  static const Uint32 masks[] = { SDL_BUTTON_LMASK, SDL_BUTTON_MMASK, SDL_BUTTON_RMASK, SDL_BUTTON_X1MASK, SDL_BUTTON_X2MASK };
  return butNum >= 0 && butNum < 5 && (m_mouseButtons & masks[butNum]) != 0;
}

int CSDLInputSystem::GetJoyAxisValue(int joyNum, int axisNum) const
{
  // -32768..32767
  const Device &dev = m_devices[joyNum];
  if (!m_gameController)
    return SDL_JoystickGetAxis(dev.joystick, axisNum);
  if (axisNum < 0 || axisNum >= 6)
    return 0;
  return SDL_GameControllerGetAxis(dev.pad, kPadAxes[axisNum]);
}

bool CSDLInputSystem::IsJoyPOVInDir(int joyNum, int povNum, int povDir) const
{
  const Device &dev = m_devices[joyNum];
  if (m_gameController)
  {
    switch (povDir)
    {
    case POV_UP:    return SDL_GameControllerGetButton(dev.pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    case POV_DOWN:  return SDL_GameControllerGetButton(dev.pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    case POV_LEFT:  return SDL_GameControllerGetButton(dev.pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    case POV_RIGHT: return SDL_GameControllerGetButton(dev.pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
    default:        return false;
    }
  }
  const Uint8 hat = SDL_JoystickGetHat(dev.joystick, povNum);
  switch (povDir)
  {
  case POV_UP:    return (hat & SDL_HAT_UP) != 0;
  case POV_DOWN:  return (hat & SDL_HAT_DOWN) != 0;
  case POV_LEFT:  return (hat & SDL_HAT_LEFT) != 0;
  case POV_RIGHT: return (hat & SDL_HAT_RIGHT) != 0;
  default:        return false;
  }
}

bool CSDLInputSystem::IsJoyButPressed(int joyNum, int butNum) const
{
  const Device &dev = m_devices[joyNum];
  if (!m_gameController)
    return SDL_JoystickGetButton(dev.joystick, butNum) != 0;
  const int n = int(sizeof(kPadButtons) / sizeof(kPadButtons[0]));
  return butNum >= 0 && butNum < n && SDL_GameControllerGetButton(dev.pad, kPadButtons[butNum]) != 0;
}

bool CSDLInputSystem::Play(int joyNum, int Device::*slot, SDL_HapticEffect &effect, bool run)
{
  Device &dev = m_devices[joyNum];
  if (!SDL_HapticEffectSupported(dev.haptic, &effect))
    return false;
  SDL_HapticUpdateEffect(dev.haptic, dev.*slot, &effect);
  if (run)
    SDL_HapticRunEffect(dev.haptic, dev.*slot, 1);
  return true;
}

void CSDLInputSystem::StopAll(int joyNum)
{
  SDL_Haptic *haptic = m_devices[joyNum].haptic;
  SDL_HapticEffect constant = Constant(kConstantIdle, 0);
  if (!Play(joyNum, &Device::constant, constant, false))
    SDL_HapticRumbleStop(haptic);
  SDL_HapticEffect sine = Sine(kSineIdle, 0);
  if (!Play(joyNum, &Device::sine, sine, false))
    SDL_HapticRumbleStop(haptic);
  SDL_HapticEffect spring = Condition(SDL_HAPTIC_SPRING, 0);
  Play(joyNum, &Device::spring, spring, false);
  SDL_HapticEffect friction = Condition(SDL_HAPTIC_FRICTION, 0);
  Play(joyNum, &Device::friction, friction, false);
}

bool CSDLInputSystem::ProcessForceFeedbackCmd(int joyNum, int, ForceFeedbackCmd ffCmd)
{
  SDL_Haptic *haptic = m_devices[joyNum].haptic;

  // Each kind's strength is scaled by its ...Max setting (percent); 0 turns
  // it off.
  auto percent = [this](const char *key) { return m_config[key].ValueAs<unsigned>(); };

  switch (ffCmd.id)
  {
  case FFStop:
    StopAll(joyNum);
    break;

  case FFConstantForce:
  {
    const unsigned max = percent("SDLConstForceMax");
    if (max == 0)
      return false;
    if (ffCmd.force == 0.0f)
    {
      SDL_HapticEffect e = Constant(kConstantIdle, 0);
      if (!Play(joyNum, &Device::constant, e, false))
        SDL_HapticRumbleStop(haptic);
      break;
    }
    // positive pushes left (-1), negative right (+1)
    const int dir = ffCmd.force > 0.0f ? -1 : 1;
    const float force = (ffCmd.force > 0.0f ? ffCmd.force : -ffCmd.force) * (max / 100.0f);
    SDL_HapticEffect e = Constant(SDL_HAPTIC_INFINITY, Sint16(int(dir * (force * INT16_MAX))));
    if (!Play(joyNum, &Device::constant, e, true))
    {
      // a pad: rumble instead, when the force is above the threshold
      const float threshold = float(percent("SDLConstForceThreshold")) / 100.0f;
      if (force != 0.0f && force > threshold)
        SDL_HapticRumblePlay(haptic, force, 200);
      else
        SDL_HapticRumbleStop(haptic);
    }
    break;
  }

  case FFSelfCenter:
  case FFFriction:
  {
    const bool spring = ffCmd.id == FFSelfCenter;
    const unsigned max = percent(spring ? "SDLSelfCenterMax" : "SDLFrictionMax");
    if (max == 0)
      return false;
    SDL_HapticEffect e = Condition(spring ? SDL_HAPTIC_SPRING : SDL_HAPTIC_FRICTION, Sint16(Level(ffCmd.force * (max / 100.0f))));
    Play(joyNum, spring ? &Device::spring : &Device::friction, e, true);
    break;
  }

  case FFVibrate:
  {
    const unsigned max = percent("SDLVibrateMax");
    if (max == 0)
      return false;
    const float strength = ffCmd.force * (max / 100.0f);
    SDL_HapticEffect e = Sine(SDL_HAPTIC_INFINITY, Sint16(Level(strength)));
    if (!Play(joyNum, &Device::sine, e, true))
    {
      if (strength != 0.0f)
        SDL_HapticRumblePlay(haptic, strength, 0);
      else
        SDL_HapticRumbleStop(haptic);
    }
    break;
  }
  }
  return true;
}

int CSDLInputSystem::GetNumKeyboards() const
{
  return ANY_KEYBOARD;    // SDL can't tell keyboards apart
}

int CSDLInputSystem::GetNumMice() const
{
  return ANY_MOUSE;       // nor mice
}

int CSDLInputSystem::GetNumJoysticks() const
{
  return int(m_devices.size());
}

const KeyDetails *CSDLInputSystem::GetKeyDetails(int)
{
  return nullptr;
}

const MouseDetails *CSDLInputSystem::GetMouseDetails(int)
{
  return nullptr;
}

const JoyDetails *CSDLInputSystem::GetJoyDetails(int joyNum)
{
  return &m_details[joyNum];
}

bool CSDLInputSystem::Poll()
{
  m_wheel = 0;
  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    if (e.type == SDL_QUIT)
      return false;
    if (e.type == SDL_MOUSEWHEEL)
    {
      // Read through the mouse button view of the event, as this input
      // system always has; that 'y' sits on the wheel event's direction
      // field, so a turn only registers (as up) when SDL reports the
      // wheel flipped.
      const int y = e.button.y;
      if (y != 0)
      {
        m_mouseZ += y > 0 ? 5 : -5;
        m_wheel = y > 0 ? 1 : -1;
      }
    }
  }
  m_keys = SDL_GetKeyboardState(nullptr);
  m_mouseButtons = SDL_GetMouseState(&m_mouseX, &m_mouseY);
  return true;
}

void CSDLInputSystem::SetMouseVisibility(bool visible)
{
  SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}
