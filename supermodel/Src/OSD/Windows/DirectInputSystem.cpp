// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See DirectInputSystem.h. Every joystick ends up as a DIJOYSTATE2, so XInput
// pads are translated into one; Raw Input keyboards are tracked by
// DirectInput scan code, so the one key table serves both.

#include "DirectInputSystem.h"
#include "Supermodel.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include <wbemidl.h>
#include <oleauto.h>

#include <SDL_syswm.h>

namespace
{
  struct KeyName
  {
    const char *name;
    int scanCode;
  };

  // Supermodel's key names and the DirectInput scan codes they stand for.
  const KeyName kKeys[] =
  {
    { "BACKSPACE", DIK_BACK }, { "TAB", DIK_TAB }, { "RETURN", DIK_RETURN },
    { "PAUSE", DIK_PAUSE }, { "ESCAPE", DIK_ESCAPE }, { "SPACE", DIK_SPACE },
    { "QUOTE", DIK_APOSTROPHE }, { "LEFTPAREN", DIK_LBRACKET }, { "RIGHTPAREN", DIK_RBRACKET },
    { "COMMA", DIK_COMMA }, { "MINUS", DIK_MINUS }, { "PERIOD", DIK_PERIOD }, { "SLASH", DIK_SLASH },
    { "0", DIK_0 }, { "1", DIK_1 }, { "2", DIK_2 }, { "3", DIK_3 }, { "4", DIK_4 },
    { "5", DIK_5 }, { "6", DIK_6 }, { "7", DIK_7 }, { "8", DIK_8 }, { "9", DIK_9 },
    { "SEMICOLON", DIK_SEMICOLON }, { "LESS", DIK_OEM_102 }, { "EQUALS", DIK_EQUALS },
    { "BACKQUOTE", DIK_GRAVE },
    { "A", DIK_A }, { "B", DIK_B }, { "C", DIK_C }, { "D", DIK_D }, { "E", DIK_E },
    { "F", DIK_F }, { "G", DIK_G }, { "H", DIK_H }, { "I", DIK_I }, { "J", DIK_J },
    { "K", DIK_K }, { "L", DIK_L }, { "M", DIK_M }, { "N", DIK_N }, { "O", DIK_O },
    { "P", DIK_P }, { "Q", DIK_Q }, { "R", DIK_R }, { "S", DIK_S }, { "T", DIK_T },
    { "U", DIK_U }, { "V", DIK_V }, { "W", DIK_W }, { "X", DIK_X }, { "Y", DIK_Y },
    { "Z", DIK_Z }, { "DEL", DIK_DELETE },
    { "KEYPAD0", DIK_NUMPAD0 }, { "KEYPAD1", DIK_NUMPAD1 }, { "KEYPAD2", DIK_NUMPAD2 },
    { "KEYPAD3", DIK_NUMPAD3 }, { "KEYPAD4", DIK_NUMPAD4 }, { "KEYPAD5", DIK_NUMPAD5 },
    { "KEYPAD6", DIK_NUMPAD6 }, { "KEYPAD7", DIK_NUMPAD7 }, { "KEYPAD8", DIK_NUMPAD8 },
    { "KEYPAD9", DIK_NUMPAD9 }, { "KEYPADPERIOD", DIK_DECIMAL }, { "KEYPADDIVIDE", DIK_DIVIDE },
    { "KEYPADMULTIPLY", DIK_MULTIPLY }, { "KEYPADMINUS", DIK_SUBTRACT }, { "KEYPADPLUS", DIK_ADD },
    { "KEYPADENTER", DIK_NUMPADENTER }, { "KEYPADEQUALS", DIK_NUMPADEQUALS },
    { "UP", DIK_UP }, { "DOWN", DIK_DOWN }, { "RIGHT", DIK_RIGHT }, { "LEFT", DIK_LEFT },
    { "INSERT", DIK_INSERT }, { "HOME", DIK_HOME }, { "END", DIK_END },
    { "PGUP", DIK_PRIOR }, { "PGDN", DIK_NEXT },
    { "F1", DIK_F1 }, { "F2", DIK_F2 }, { "F3", DIK_F3 }, { "F4", DIK_F4 }, { "F5", DIK_F5 },
    { "F6", DIK_F6 }, { "F7", DIK_F7 }, { "F8", DIK_F8 }, { "F9", DIK_F9 }, { "F10", DIK_F10 },
    { "F11", DIK_F11 }, { "F12", DIK_F12 }, { "F13", DIK_F13 }, { "F14", DIK_F14 }, { "F15", DIK_F15 },
    { "NUMLOCK", DIK_NUMLOCK }, { "CAPSLOCK", DIK_CAPITAL }, { "SCROLLLOCK", DIK_SCROLL },
    { "RIGHTSHIFT", DIK_RSHIFT }, { "LEFTSHIFT", DIK_LSHIFT },
    { "RIGHTCTRL", DIK_RCONTROL }, { "LEFTCTRL", DIK_LCONTROL },
    { "RIGHTALT", DIK_RMENU }, { "LEFTALT", DIK_LMENU },
    { "RIGHTWINDOWS", DIK_RWIN }, { "LEFTWINDOWS", DIK_LWIN },
    { "PRINT", DIK_SYSRQ },
  };
  constexpr int kNumKeys = int(sizeof(kKeys) / sizeof(kKeys[0]));

  const char *const kXInputLibraries[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };

  // DirectInput force values run from -10000 to 10000; settings are percent.
  constexpr LONG kForceFull = DI_FFNOMINALMAX;
  constexpr LONG kForcePerPercent = DI_FFNOMINALMAX / 100;
  // XInput motor speeds run from 0 to 65535.
  constexpr unsigned kMotorFull = 65535;
  constexpr unsigned kMotorPerPercent = 65535 / 100;

  // Raw Input usages for keyboards and mice (generic desktop page)
  constexpr USHORT kDesktopPage = 0x01;
  constexpr USHORT kMouseUsage = 0x02;
  constexpr USHORT kKeyboardUsage = 0x06;

  template <typename T>
  void Release(T *&p)
  {
    if (p)
    {
      p->Release();
      p = nullptr;
    }
  }

  // A joystick state with nothing pressed and every POV hat centred.
  DIJOYSTATE2 RestingPad()
  {
    DIJOYSTATE2 state{};
    for (DWORD &pov : state.rgdwPOV)
      pov = DWORD(-1);
    return state;
  }

  HRESULT SetDwordProperty(LPDIRECTINPUTDEVICE8 device, REFGUID property, DWORD value)
  {
    DIPROPDWORD prop{};
    prop.diph.dwSize = sizeof(prop);
    prop.diph.dwHeaderSize = sizeof(prop.diph);
    prop.diph.dwHow = DIPH_DEVICE;
    prop.dwData = value;
    return device->SetProperty(property, &prop.diph);
  }

  // Polls a DirectInput device, getting it back first if it was lost.
  // False when it can't be had at the moment.
  bool Reacquire(LPDIRECTINPUTDEVICE8 device)
  {
    if (SUCCEEDED(device->Poll()))
      return true;
    HRESULT hr;
    do
      hr = device->Acquire();
    while (hr == DIERR_INPUTLOST);
    return !(hr == DIERR_OTHERAPPHASPRIO || hr == DIERR_INVALIDPARAM || hr == DIERR_NOTINITIALIZED);
  }

  // ---- Telling XInput pads apart --------------------------------------------

  // The vendor and product ids (vendor in the low word, as DirectInput puts
  // them in a product GUID's first field) of every XInput device present.
  // Windows' device ids for those contain "IG_", which only WMI shows.
  std::vector<DWORD> XInputProducts()
  {
    std::vector<DWORD> found;
    IWbemLocator *locator = nullptr;
    IWbemServices *services = nullptr;
    IEnumWbemClassObject *devices = nullptr;
    BSTR space = SysAllocString(L"\\\\.\\root\\cimv2");
    BSTR type = SysAllocString(L"Win32_PNPEntity");
    BSTR field = SysAllocString(L"DeviceID");

    bool ready = space && type && field
      && SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID *)&locator)) && locator
      && SUCCEEDED(locator->ConnectServer(space, nullptr, nullptr, 0, 0, nullptr, nullptr, &services)) && services;
    if (ready)
    {
      CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
      ready = SUCCEEDED(services->CreateInstanceEnum(type, 0, nullptr, &devices)) && devices;
    }
    while (ready)
    {
      IWbemClassObject *device = nullptr;
      ULONG got = 0;
      if (FAILED(devices->Next(10000, 1, &device, &got)) || got == 0)
        break;
      VARIANT id;
      VariantInit(&id);
      if (SUCCEEDED(device->Get(field, 0, &id, nullptr, nullptr)) && id.vt == VT_BSTR && id.bstrVal && wcsstr(id.bstrVal, L"IG_"))
      {
        unsigned vendor = 0, product = 0;
        const wchar_t *v = wcsstr(id.bstrVal, L"VID_");
        const wchar_t *p = wcsstr(id.bstrVal, L"PID_");
        if (v && swscanf(v, L"VID_%4X", &vendor) != 1)
          vendor = 0;
        if (p && swscanf(p, L"PID_%4X", &product) != 1)
          product = 0;
        found.push_back(MAKELONG(vendor, product));
      }
      VariantClear(&id);
      device->Release();
    }

    Release(devices);
    Release(services);
    Release(locator);
    SysFreeString(space);
    SysFreeString(type);
    SysFreeString(field);
    return found;
  }

  // ---- Device names from the registry ----------------------------------------

  bool ReadRegistryText(HKEY key, const char *value, std::string &text)
  {
    DWORD size = 0;
    if (RegQueryValueExA(key, value, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS)
      return false;
    std::vector<char> data(size + 1, 0);
    if (RegQueryValueExA(key, value, nullptr, nullptr, (LPBYTE)data.data(), &size) != ERROR_SUCCESS)
      return false;
    text = data.data();
    return true;
  }

  // DeviceDesc of a key under HKLM, if both are there.
  bool ReadDescription(HKEY parent, const std::string &path, std::string &text)
  {
    HKEY key;
    if (RegOpenKeyExA(parent, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
      return false;
    bool ok = ReadRegistryText(key, "DeviceDesc", text);
    RegCloseKey(key);
    return ok;
  }

  std::vector<std::string> SubKeys(HKEY key)
  {
    std::vector<std::string> names;
    char name[MAX_PATH];
    for (DWORD i = 0; ; i++)
    {
      DWORD length = MAX_PATH - 1;
      if (RegEnumKeyExA(key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
        break;
      names.emplace_back(name, length);
    }
    return names;
  }

  // HID devices often have no description of their own; the USB device they
  // sit on does, and names the HID instance through its ParentIdPrefix.
  bool ReadParentDescription(const std::string &instance, std::string &text)
  {
    HKEY usb;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usb) != ERROR_SUCCESS)
      return false;
    bool found = false;
    for (const std::string &product : SubKeys(usb))
    {
      HKEY productKey;
      if (RegOpenKeyExA(usb, product.c_str(), 0, KEY_READ, &productKey) != ERROR_SUCCESS)
        continue;
      for (const std::string &serial : SubKeys(productKey))
      {
        HKEY device;
        if (RegOpenKeyExA(productKey, serial.c_str(), 0, KEY_READ, &device) != ERROR_SUCCESS)
          continue;
        std::string prefix;
        found = ReadRegistryText(device, "ParentIdPrefix", prefix)
          && instance.compare(0, prefix.size(), prefix) == 0
          && ReadRegistryText(device, "DeviceDesc", text);
        RegCloseKey(device);
        if (found)
          break;
      }
      RegCloseKey(productKey);
      if (found)
        break;
    }
    RegCloseKey(usb);
    return found;
  }

  // A friendly name for a Raw Input device path such as
  // \\?\HID#VID_046D&PID_C52B#7&2a&0&0000#{class}: the DeviceDesc of
  // Enum\HID\VID_046D&PID_C52B\7&2a&0&0000, past its last ';'.
  bool DeviceName(const std::string &path, char *name)
  {
    if (path.compare(0, 4, "\\??\\") != 0 && path.compare(0, 4, "\\\\?\\") != 0)
      return false;
    std::string id = path.substr(4);
    std::string key = "SYSTEM\\CurrentControlSet\\Enum\\" + id;
    std::replace(key.begin(), key.end(), '#', '\\');
    size_t cut = key.rfind('\\');
    if (cut != std::string::npos)
      key.erase(cut);

    std::string text;
    if (!ReadDescription(HKEY_LOCAL_MACHINE, key, text))
    {
      HKEY probe;
      if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &probe) != ERROR_SUCCESS)
        return false;
      RegCloseKey(probe);
      cut = key.rfind('\\');
      if (id.find("HID") == std::string::npos || cut == std::string::npos || cut + 1 == key.size())
        return false;
      if (!ReadParentDescription(key.substr(cut + 1), text))
        return false;
    }

    size_t semicolon = text.rfind(';');
    size_t start = (semicolon == std::string::npos || semicolon + 1 == text.size()) ? 0 : semicolon + 1;
    std::snprintf(name, MAX_NAME_LENGTH, "%s", text.c_str() + start);
    return true;
  }

  // ---- Joystick axes and effects ---------------------------------------------

  struct AxisScan
  {
    JoyDetails *details;
    int sliders;
    bool unsure;  // an axis could not be placed with certainty
  };

  // Where each axis lives in a DIJOYSTATE2.
  const DWORD kAxisOffsets[NUM_JOY_AXES] =
  {
    offsetof(DIJOYSTATE2, lX), offsetof(DIJOYSTATE2, lY), offsetof(DIJOYSTATE2, lZ),
    offsetof(DIJOYSTATE2, lRx), offsetof(DIJOYSTATE2, lRy), offsetof(DIJOYSTATE2, lRz),
    offsetof(DIJOYSTATE2, rglSlider[0]), offsetof(DIJOYSTATE2, rglSlider[1])
  };

  BOOL CALLBACK NoteAxis(LPCDIDEVICEOBJECTINSTANCE object, LPVOID context)
  {
    auto *scan = static_cast<AxisScan *>(context);
    int axis = -1;
    const GUID &kind = object->guidType;
    if (kind == GUID_XAxis) axis = AXIS_X;
    else if (kind == GUID_YAxis) axis = AXIS_Y;
    else if (kind == GUID_ZAxis) axis = AXIS_Z;
    else if (kind == GUID_RxAxis) axis = AXIS_RX;
    else if (kind == GUID_RyAxis) axis = AXIS_RY;
    else if (kind == GUID_RzAxis) axis = AXIS_RZ;
    else if (kind == GUID_Slider)
    {
      // The first two sliders are S1 and S2; any more are left out
      int n = scan->sliders++;
      if (n > 1)
        return DIENUM_CONTINUE;
      axis = n == 0 ? AXIS_S1 : AXIS_S2;
    }
    else if (object->dwType & DIDFT_AXIS)
    {
      // An axis without a type (allowed): place it by the c_dfDIJoystick2
      // entry its instance number picks, among the six main axes
      scan->unsure = true;
      DWORD entry = DIDFT_GETINSTANCE(object->dwType);
      if (entry >= c_dfDIJoystick2.dwNumObjs)
        return DIENUM_CONTINUE;
      DWORD offset = c_dfDIJoystick2.rgodf[entry].dwOfs;
      for (int a = AXIS_X; a <= AXIS_RZ && axis < 0; a++)
      {
        if (kAxisOffsets[a] == offset)
          axis = a;
      }
      if (axis < 0)
        return DIENUM_CONTINUE;
    }
    else
      return DIENUM_CONTINUE;

    JoyDetails &details = *scan->details;
    if (details.hasAxis[axis])
      scan->unsure = true;
    details.hasAxis[axis] = true;
    details.axisHasFF[axis] = (object->dwFlags & DIDOI_FFACTUATOR) != 0;
    std::snprintf(details.axisName[axis], MAX_NAME_LENGTH + 1, "%s (%s)", CInputSystem::GetDefaultAxisName(axis), object->tszName);
    return DIENUM_CONTINUE;
  }

  BOOL CALLBACK NoteEffect(LPCDIEFFECTINFO effect, LPVOID context)
  {
    if (effect->dwEffType & (DIEFT_CONSTANTFORCE | DIEFT_PERIODIC | DIEFT_CONDITION))
      static_cast<JoyDetails *>(context)->hasFFeedback = true;
    return DIENUM_CONTINUE;
  }

  BOOL CALLBACK NoteDevice(LPCDIDEVICEINSTANCE instance, LPVOID context)
  {
    static_cast<std::vector<DIDEVICEINSTANCE> *>(context)->push_back(*instance);
    return DIENUM_CONTINUE;
  }

  // The parameters of each kind of DirectInput effect at a given strength
  // (an effect's own scale; 0 when it is first made).
  struct EffectParams
  {
    DICONSTANTFORCE push{};
    DICONDITION spring{};
    DIPERIODIC shake{};
    DWORD axis = 0;
    LONG direction = 0;
    DIEFFECT effect{};

    EffectParams(EForceFeedback kind, LONG strength)
    {
      effect.dwSize = sizeof(DIEFFECT);
      effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
      effect.cAxes = 1;
      effect.rglDirection = &direction;
      switch (kind)
      {
      case FFConstantForce:
        push.lMagnitude = strength;
        effect.cbTypeSpecificParams = sizeof(push);
        effect.lpvTypeSpecificParams = &push;
        break;
      case FFSelfCenter:
      case FFFriction:
        spring.lPositiveCoefficient = strength;
        spring.lNegativeCoefficient = strength;
        spring.dwPositiveSaturation = DI_FFNOMINALMAX;
        spring.dwNegativeSaturation = DI_FFNOMINALMAX;
        spring.lDeadBand = kind == FFSelfCenter ? LONG(0.05 * DI_FFNOMINALMAX) : 0;
        effect.cbTypeSpecificParams = sizeof(spring);
        effect.lpvTypeSpecificParams = &spring;
        break;
      default:  // FFVibrate: a 20 Hz sine
        shake.dwMagnitude = DWORD(strength);
        shake.dwPeriod = DWORD(0.05 * DI_SECONDS);
        effect.cbTypeSpecificParams = sizeof(shake);
        effect.lpvTypeSpecificParams = &shake;
        break;
      }
    }
  };

  // Makes (and starts, at zero strength) one effect on one axis.
  LPDIRECTINPUTEFFECT MakeEffect(LPDIRECTINPUTDEVICE8 device, int axisNum, EForceFeedback kind)
  {
    static const GUID *const kTypes[] = { &GUID_ConstantForce, &GUID_Spring, &GUID_Friction, &GUID_Sine };
    if (axisNum < 0 || axisNum >= NUM_JOY_AXES || kind < FFConstantForce || kind > FFVibrate)
      return nullptr;
    EffectParams params(kind, 0);
    params.axis = kAxisOffsets[axisNum];
    params.effect.rgdwAxes = &params.axis;
    params.effect.dwTriggerButton = DIEB_NOTRIGGER;
    params.effect.dwGain = DI_FFNOMINALMAX;
    params.effect.dwDuration = INFINITE;
    device->Acquire();
    LPDIRECTINPUTEFFECT made = nullptr;
    if (FAILED(device->CreateEffect(*kTypes[kind], &params.effect, &made, nullptr)) || !made)
      return nullptr;
    made->Start(1, 0);
    return made;
  }

  LONG Within(LONG value, LONG low, LONG high)
  {
    return std::max(low, std::min(value, high));
  }
}

// ---- Construction ------------------------------------------------------------

static const char *SystemName(bool rawInput, bool xInput)
{
  if (rawInput)
    return xInput ? "RawInput/XInput" : "RawInput/DirectInput";
  return xInput ? "Xinput" : "DirectInput";
}

CDirectInputSystem::CDirectInputSystem(const Util::Config::Node &config, SDL_Window *window, bool useRawInput, bool useXInput)
  : CInputSystem(SystemName(useRawInput, useXInput)),
    m_config(config),
    m_window(window),
    m_useRawInput(useRawInput),
    m_useXInput(useXInput)
{
}

CDirectInputSystem::~CDirectInputSystem()
{
  CloseKeyboardsAndMice();
  CloseJoysticks();
  if (m_di)
  {
    Release(m_di);
    if (m_comStarted)
      CoUninitialize();
  }
}

bool CDirectInputSystem::InitializeSystem()
{
  if (m_useRawInput)
  {
    // Absolute pointing devices report in whole-screen units
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode))
    {
      ErrorLog("Unable to read current display settings\n");
      return false;
    }
    m_screenW = mode.dmPelsWidth;
    m_screenH = mode.dmPelsHeight;
  }

  if (m_useXInput)
    LoadXInput();

  // DirectInput 8 through COM (no link-time dependence on dinput8.dll).
  // CoInitialize refuses when a managed host already chose another model;
  // that is fine to carry on with.
  HRESULT hr = CoInitialize(nullptr);
  if (SUCCEEDED(hr))
    m_comStarted = true;
  else if (hr != RPC_E_CHANGED_MODE)
  {
    ErrorLog("Unable to initialize COM (error %d).\n", hr);
    return false;
  }

  hr = CoCreateInstance(CLSID_DirectInput8, nullptr, CLSCTX_INPROC_SERVER, IID_IDirectInput8A, (LPVOID *)&m_di);
  if (SUCCEEDED(hr))
  {
    hr = m_di->Initialize(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION);
    if (FAILED(hr))
      Release(m_di);
  }
  if (FAILED(hr))
  {
    ErrorLog("Unable to initialize DirectInput API (error %d) - is DirectX 8 or later installed?\n", hr);
    m_di = nullptr;
    if (m_comStarted)
      CoUninitialize();
    m_comStarted = false;
    return false;
  }

  OpenKeyboardsAndMice();
  OpenJoysticks();
  return true;
}

void CDirectInputSystem::LoadXInput()
{
  HMODULE library = nullptr;
  for (const char *name : kXInputLibraries)
  {
    if ((library = LoadLibraryA(name)) != nullptr)
      break;
  }
  if (library)
  {
    m_xiGetCapabilities = (XInputGetCapabilitiesFn)(void *)GetProcAddress(library, "XInputGetCapabilities");
    m_xiGetState = (XInputGetStateFn)(void *)GetProcAddress(library, "XInputGetState");
    m_xiSetState = (XInputSetStateFn)(void *)GetProcAddress(library, "XInputSetState");
  }
  m_useXInput = m_xiGetCapabilities && m_xiGetState && m_xiSetState;
  if (!m_useXInput)
  {
    std::string tried;
    for (const char *name : kXInputLibraries)
      tried += (tried.empty() ? "" : ", ") + std::string(name);
    ErrorLog("XInput not found. Tried: %s.", tried.c_str());
    ErrorLog("Falling back on DirectInput.");
  }
}

// ---- Keyboards and mice ------------------------------------------------------

void CDirectInputSystem::OpenKeyboardsAndMice()
{
  if (m_useRawInput)
  {
    m_useRawInput = OpenRawDevices();
    if (m_useRawInput)
      return;
    m_rawKeyboards.clear();
    m_rawMice.clear();
    m_keyDetails.clear();
    m_mouseDetails.clear();
  }
  OpenDirectInputKeyboardAndMouse();
}

// Lists the Raw Input keyboards and mice. False when there isn't at least
// one of each (DirectInput is used then).
bool CDirectInputSystem::OpenRawDevices()
{
  UINT count = 0;
  std::vector<RAWINPUTDEVICELIST> list;
  if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) == 0 && count > 0)
  {
    list.resize(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1))
      list.clear();
    else
      list.resize(std::min<size_t>(count, list.size()));
  }
  if (list.empty())
  {
    ErrorLog("Unable to query RawInput API for attached devices (error %d) - switching to DirectInput.\n", GetLastError());
    return false;
  }

  // Newest devices come first in the list; number them oldest first
  for (auto it = list.rbegin(); it != list.rend(); ++it)
  {
    UINT length = 0;
    if (GetRawInputDeviceInfoA(it->hDevice, RIDI_DEVICENAME, nullptr, &length) != 0)
      continue;
    std::vector<char> path(length + 1, 0);
    if (GetRawInputDeviceInfoA(it->hDevice, RIDI_DEVICENAME, path.data(), &length) == UINT(-1))
      continue;
    std::string name(path.data());
    if (name.find("Root#RDP_") != std::string::npos)  // remote desktop's stand-ins
      continue;

    if (it->dwType == RIM_TYPEKEYBOARD)
    {
      RawKeyboard keyboard{ it->hDevice, {} };
      m_rawKeyboards.push_back(keyboard);
      KeyDetails details{};
      if (!DeviceName(name, details.name))
        std::strcpy(details.name, "Unknown Keyboard");
      m_keyDetails.push_back(details);
    }
    else if (it->dwType == RIM_TYPEMOUSE)
    {
      m_rawMice.push_back({ it->hDevice, MouseState() });
      MouseDetails details{};
      if (!DeviceName(name, details.name))
        std::strcpy(details.name, "Unknown Mouse");
      m_mouseDetails.push_back(details);
    }
  }
  DebugLog("RawInput - found %d keyboards and %d mice", int(m_rawKeyboards.size()), int(m_rawMice.size()));
  return !m_rawKeyboards.empty() && !m_rawMice.empty();
}

void CDirectInputSystem::OpenDirectInputKeyboardAndMouse()
{
  HRESULT hr = m_di->CreateDevice(GUID_SysKeyboard, &m_diKeyboard, nullptr);
  if (FAILED(hr))
  {
    ErrorLog("Unable to create DirectInput keyboard device (error %d) - key input will be unavailable.\n", hr);
    m_diKeyboard = nullptr;
  }
  else if (FAILED(hr = m_diKeyboard->SetDataFormat(&c_dfDIKeyboard)))
  {
    ErrorLog("Unable to set data format for DirectInput keyboard (error %d) - key input will be unavailable.\n", hr);
    Release(m_diKeyboard);
  }

  hr = m_di->CreateDevice(GUID_SysMouse, &m_diMouse, nullptr);
  if (FAILED(hr))
  {
    ErrorLog("Unable to create DirectInput mouse device (error %d) - mouse input will be unavailable.\n", hr);
    m_diMouse = nullptr;
    return;
  }
  if (FAILED(hr = m_diMouse->SetDataFormat(&c_dfDIMouse2)))
  {
    ErrorLog("Unable to set data format for DirectInput mouse (error %d) - mouse input will be unavailable.\n", hr);
    Release(m_diMouse);
    return;
  }
  if (FAILED(hr = SetDwordProperty(m_diMouse, DIPROP_AXISMODE, DIPROPAXISMODE_REL)))
  {
    ErrorLog("Unable to set axis mode for DirectInput mouse (error %d) - mouse input will be unavailable.\n", hr);
    Release(m_diMouse);
  }
}

void CDirectInputSystem::RegisterRawInput(DWORD flags)
{
  RAWINPUTDEVICE wanted[2] = {};
  wanted[0].usUsagePage = kDesktopPage;
  wanted[0].usUsage = kKeyboardUsage;
  wanted[1].usUsagePage = kDesktopPage;
  wanted[1].usUsage = kMouseUsage;
  for (RAWINPUTDEVICE &device : wanted)
  {
    device.dwFlags = flags;
    device.hwndTarget = m_hwnd;
  }
  if (!RegisterRawInputDevices(wanted, 2, sizeof(RAWINPUTDEVICE)) && flags != RIDEV_REMOVE)
    ErrorLog("Unable to register for keyboard and mouse input with RawInput API (error %d) - keyboard and mouse input will be unavailable.\n", GetLastError());
}

// (Re)attaches every device to the window, in the way the grab state asks:
// a grabbed session keeps input to itself while in front, otherwise input
// is read in the background too.
void CDirectInputSystem::Activate()
{
  SyncMiceToCursor();
  if (m_useRawInput)
    RegisterRawInput((m_grabMouse ? RIDEV_CAPTUREMOUSE : RIDEV_INPUTSINK) | RIDEV_NOLEGACY);
  else
  {
    DWORD level = (m_grabMouse ? DISCL_FOREGROUND : DISCL_BACKGROUND) | DISCL_NONEXCLUSIVE;
    for (LPDIRECTINPUTDEVICE8 device : { m_diKeyboard, m_diMouse })
    {
      if (device)
      {
        device->Unacquire();
        device->SetCooperativeLevel(m_hwnd, level);
        device->Acquire();
      }
    }
  }

  DWORD padLevel = m_grabMouse ? (DISCL_EXCLUSIVE | DISCL_FOREGROUND) : (DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
  for (Pad &pad : m_pads)
  {
    if (pad.device)
    {
      pad.device->Unacquire();
      pad.device->SetCooperativeLevel(m_hwnd, padLevel);
      pad.device->Acquire();
    }
  }
}

void CDirectInputSystem::PollKeyboardsAndMice()
{
  if (m_useRawInput)
  {
    // Raw Input arrives as window messages; only the wheel needs settling
    auto settle = [](MouseState &mouse)
    {
      mouse.wheelDir = mouse.wheelMoved == 0 ? 0 : (mouse.wheelMoved > 0 ? 1 : -1);
      mouse.wheelMoved = 0;
    };
    for (RawMouse &mouse : m_rawMice)
      settle(mouse.state);
    settle(m_allMice);
    return;
  }

  if (m_diKeyboard)
  {
    if (!Reacquire(m_diKeyboard))
      return;
    m_diKeyboard->GetDeviceState(sizeof(m_diKeys), m_diKeys);
  }

  if (m_diMouse)
  {
    if (!Reacquire(m_diMouse))
      return;
    DIMOUSESTATE2 now{};
    m_diMouse->GetDeviceState(sizeof(now), &now);
    MouseState &mouse = m_diMouseState;
    mouse.x = CInputSource::Clamp(mouse.x + now.lX, m_dispX, m_dispX + m_dispW);
    mouse.y = CInputSource::Clamp(mouse.y + now.lY, m_dispY, m_dispY + m_dispH);
    if (now.lZ != 0)
    {
      // One wheel notch (120) moves the wheel axis by 5
      LONG step = 5 * now.lZ / 120;
      mouse.z = CInputSource::Clamp(mouse.z + step, -100, 100);
      mouse.wheelDir = step > 0 ? 1 : -1;
    }
    else
      mouse.wheelDir = 0;
    // DirectInput numbers the buttons left, right, middle, side, side
    static const int kOrder[5] = { 0, 2, 1, 3, 4 };
    mouse.buttons = 0;
    for (int bit = 0; bit < 5; bit++)
    {
      if (now.rgbButtons[kOrder[bit]] & 0x80)
        mouse.buttons |= 1u << bit;
    }
  }
}

void CDirectInputSystem::CloseKeyboardsAndMice()
{
  if (m_useRawInput)
  {
    if (m_activated)
      RegisterRawInput(RIDEV_REMOVE);
    m_rawKeyboards.clear();
    m_rawMice.clear();
    m_keyDetails.clear();
    m_mouseDetails.clear();
  }
  for (LPDIRECTINPUTDEVICE8 *device : { &m_diKeyboard, &m_diMouse })
  {
    if (*device)
    {
      (*device)->Unacquire();
      Release(*device);
    }
  }
}

// Puts every mouse where the cursor is, so relative movement starts there.
void CDirectInputSystem::SyncMiceToCursor()
{
  POINT cursor;
  if (!GetCursorPos(&cursor) || !ScreenToClient(m_hwnd, &cursor))
    return;
  auto place = [&](MouseState &mouse)
  {
    mouse.x = cursor.x;
    mouse.y = cursor.y;
    mouse.z = 0;
  };
  if (m_useRawInput)
  {
    place(m_allMice);
    for (RawMouse &mouse : m_rawMice)
      place(mouse.state);
  }
  place(m_diMouseState);
}

void CDirectInputSystem::ReadRawInput(HRAWINPUT input)
{
  UINT size = 0;
  if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
    return;
  if (m_rawBuffer.size() < size)
    m_rawBuffer.resize(size);
  if (GetRawInputData(input, RID_INPUT, m_rawBuffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
    return;
  const RAWINPUT &event = *reinterpret_cast<const RAWINPUT *>(m_rawBuffer.data());

  if (event.header.dwType == RIM_TYPEKEYBOARD)
  {
    for (RawKeyboard &keyboard : m_rawKeyboards)
    {
      if (keyboard.handle != event.header.hDevice)
        continue;
      // DirectInput scan codes mark the E0-prefixed keys with bit 7
      const RAWKEYBOARD &key = event.data.keyboard;
      unsigned code = (key.MakeCode & 0x7F) | ((key.Flags & RI_KEY_E0) ? 0x80 : 0);
      if (code != 0xAA)  // part of the fake shift Windows sends around some keys
        keyboard.down[code] = (key.Flags & RI_KEY_BREAK) == 0;
      break;
    }
    return;
  }

  if (event.header.dwType != RIM_TYPEMOUSE)
    return;
  MouseState *mouse = nullptr;
  for (RawMouse &raw : m_rawMice)
  {
    if (raw.handle == event.header.hDevice)
    {
      mouse = &raw.state;
      break;
    }
  }
  if (!mouse)
    return;

  const RAWMOUSE &data = event.data.mouse;
  if (data.usFlags & MOUSE_MOVE_ABSOLUTE)
  {
    // 0..65535 across the screen, turned into window pixels
    POINT at;
    at.x = CInputSource::Scale(data.lLastX, 0, 0xFFFF, 0, int(m_screenW));
    at.y = CInputSource::Scale(data.lLastY, 0, 0xFFFF, 0, int(m_screenH));
    if (ScreenToClient(m_hwnd, &at))
    {
      mouse->x = at.x;
      mouse->y = at.y;
    }
    m_allMice.x = mouse->x;
    m_allMice.y = mouse->y;
  }
  else
  {
    for (MouseState *m : { mouse, &m_allMice })
    {
      m->x = CInputSource::Clamp(m->x + data.lLastX, m_dispX, m_dispX + m_dispW);
      m->y = CInputSource::Clamp(m->y + data.lLastY, m_dispY, m_dispY + m_dispH);
    }
  }

  USHORT flags = data.usButtonFlags;
  if (flags & RI_MOUSE_WHEEL)
  {
    LONG step = 5 * SHORT(data.usButtonData) / 120;
    for (MouseState *m : { mouse, &m_allMice })
    {
      m->z = CInputSource::Clamp(m->z + step, -100, 100);
      m->wheelMoved += step;
    }
  }

  static const USHORT kDown[5] = { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
  static const USHORT kUp[5] = { RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
  for (int bit = 0; bit < 5; bit++)
  {
    if (flags & kDown[bit])
      mouse->buttons |= 1u << bit;
    else if (flags & kUp[bit])
      mouse->buttons &= ~(1u << bit);
  }
  m_allMice.buttons = 0;
  for (const RawMouse &raw : m_rawMice)
    m_allMice.buttons |= raw.state.buttons;
}

// ---- Joysticks -----------------------------------------------------------------

void CDirectInputSystem::OpenJoysticks()
{
  std::vector<DIDEVICEINSTANCE> found;
  if (FAILED(m_di->EnumDevices(DI8DEVCLASS_GAMECTRL, NoteDevice, &found, DIEDFL_ATTACHEDONLY)))
    return;

  std::vector<DWORD> xinputProducts;
  if (m_useXInput && !found.empty())
    xinputProducts = XInputProducts();

  DWORD xinputCount = 0;
  int number = 0;
  for (const DIDEVICEINSTANCE &instance : found)
  {
    number++;
    Pad pad;
    pad.details = JoyDetails{};
    pad.state = RestingPad();

    bool isXInput = std::find(xinputProducts.begin(), xinputProducts.end(), instance.guidProduct.Data1) != xinputProducts.end();
    if (isXInput)
    {
      // An Xbox 360 style pad: two sticks and two triggers as six axes, a
      // d-pad and ten buttons. The motors stand in for force on the sticks.
      JoyDetails &d = pad.details;
      pad.xinput = true;
      pad.xinputSlot = xinputCount++;
      std::snprintf(d.name, sizeof(d.name), "Xbox 360 Controller %u (via XInput)", unsigned(pad.xinputSlot + 1));
      d.numAxes = 6;
      d.numPOVs = 1;
      d.numButtons = 10;
      d.hasFFeedback = true;
      for (int axis : { AXIS_X, AXIS_Y, AXIS_Z, AXIS_RX, AXIS_RY, AXIS_RZ })
        d.hasAxis[axis] = true;
      for (int axis : { AXIS_X, AXIS_Y, AXIS_RX, AXIS_RY })
        d.axisHasFF[axis] = true;
    }
    else if (!OpenDirectInputPad(instance, number, pad))
      continue;

    m_pads.push_back(pad);
  }
}

bool CDirectInputSystem::OpenDirectInputPad(const DIDEVICEINSTANCE &instance, int number, Pad &pad)
{
  LPDIRECTINPUTDEVICE8 device = nullptr;
  HRESULT hr = m_di->CreateDevice(instance.guidInstance, &device, nullptr);
  if (FAILED(hr))
  {
    ErrorLog("Unable to create DirectInput joystick device %d (error %d) - skipping joystick.\n", number, hr);
    return false;
  }
  auto fail = [&](const char *what) -> bool
  {
    ErrorLog("Unable to %s DirectInput joystick %d (error %d) - skipping joystick.\n", what, number, hr);
    device->Release();
    return false;
  };

  if (FAILED(hr = device->SetDataFormat(&c_dfDIJoystick2)))
    return fail("set data format for");
  DIDEVCAPS caps{};
  caps.dwSize = sizeof(caps);
  if (FAILED(hr = device->GetCapabilities(&caps)))
    return fail("query capabilities of");

  JoyDetails &d = pad.details;
  DIPROPSTRING label{};
  label.diph.dwSize = sizeof(label);
  label.diph.dwHeaderSize = sizeof(label.diph);
  label.diph.dwHow = DIPH_DEVICE;
  if (FAILED(hr = device->GetProperty(DIPROP_INSTANCENAME, &label.diph)))
    return fail("get name of");
  int chars = std::min<int>(MAX_NAME_LENGTH, int(wcslen(label.wsz)) + 1);
  WideCharToMultiByte(CP_ACP, 0, label.wsz, chars, d.name, chars, nullptr, nullptr);
  d.name[MAX_NAME_LENGTH - 1] = '\0';
  d.numPOVs = int(caps.dwPOVs);
  d.numButtons = int(caps.dwButtons);

  AxisScan scan{ &d, 0, false };
  if (FAILED(hr = device->EnumObjects(NoteAxis, &scan, DIDFT_ALL)))
    return fail("enumerate axes of");
  if (scan.unsure)
  {
    // Offer every axis rather than miss one
    for (int axis = 0; axis < NUM_JOY_AXES; axis++)
    {
      if (!d.hasAxis[axis])
      {
        d.hasAxis[axis] = true;
        d.axisHasFF[axis] = false;
        std::snprintf(d.axisName[axis], MAX_NAME_LENGTH + 1, "%s", GetDefaultAxisName(axis));
      }
    }
  }
  d.numAxes = int(std::count(std::begin(d.hasAxis), std::end(d.hasAxis), true));

  if ((caps.dwFlags & DIDC_FORCEFEEDBACK) && FAILED(hr = device->EnumEffects(NoteEffect, &d, DIEFT_ALL)))
    ErrorLog("Unable to enumerate effects of DirectInput joystick %d (error %d) - force feedback will be unavailable for joystick.\n", number, hr);

  if (d.numAxes > 0)
  {
    // Full 16-bit absolute axes, with dead zone and saturation left to the
    // input system
    DIPROPRANGE range{};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = -32768;
    range.lMax = 32767;
    if (FAILED(hr = device->SetProperty(DIPROP_RANGE, &range.diph)))
      return fail("set axis range of");
    if (FAILED(hr = SetDwordProperty(device, DIPROP_AXISMODE, DIPROPAXISMODE_ABS)))
      return fail("set axis mode of");
    if (FAILED(hr = SetDwordProperty(device, DIPROP_DEADZONE, 0)))
      return fail("set deadzone of");
    if (FAILED(hr = SetDwordProperty(device, DIPROP_SATURATION, 10000)))
      return fail("set saturation of");
    // The game drives centring itself through force feedback
    if (d.hasFFeedback && FAILED(hr = SetDwordProperty(device, DIPROP_AUTOCENTER, DIPROPAUTOCENTER_OFF)))
    {
      ErrorLog("Unable to unset auto-center of DirectInput joystick %d (error %d) - force feedback will be unavailable for joystick.\n", number, hr);
      d.hasFFeedback = false;
    }
  }

  pad.device = device;
  return true;
}

void CDirectInputSystem::PollJoysticks()
{
  for (Pad &pad : m_pads)
  {
    if (!pad.xinput)
    {
      if (Reacquire(pad.device))
        pad.device->GetDeviceState(sizeof(DIJOYSTATE2), &pad.state);
      else
        pad.state = RestingPad();
      continue;
    }

    XINPUT_STATE now{};
    if (m_xiGetState(pad.xinputSlot, &now) != ERROR_SUCCESS)
    {
      pad.state = RestingPad();
      continue;
    }
    const XINPUT_GAMEPAD &g = now.Gamepad;
    DIJOYSTATE2 &s = pad.state;
    s.lX = g.sThumbLX;
    s.lY = -LONG(g.sThumbLY);
    s.lZ = CInputSource::Scale(g.bLeftTrigger, 0, 255, 0, 32767);
    s.lRx = g.sThumbRX;
    s.lRy = -LONG(g.sThumbRY);
    s.lRz = CInputSource::Scale(g.bRightTrigger, 0, 255, 0, 32767);

    // The d-pad as a POV hat, in hundredths of a degree clockwise from up.
    // Up wins over down and left over right when both are held.
    WORD b = g.wButtons;
    int dx = (b & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : ((b & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0);
    int dy = (b & XINPUT_GAMEPAD_DPAD_UP) ? -1 : ((b & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0);
    static const DWORD kHat[3][3] =
    {
      { 31500, 0, 4500 },              // up-left, up, up-right
      { 27000, DWORD(-1), 9000 },      // left, centre, right
      { 22500, 18000, 13500 },         // down-left, down, down-right
    };
    s.rgdwPOV[0] = kHat[dy + 1][dx + 1];

    static const WORD kButtons[10] =
    {
      XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
      XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
      XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB
    };
    for (int i = 0; i < 10; i++)
      s.rgbButtons[i] = (g.wButtons & kButtons[i]) ? 1 : 0;
  }
}

void CDirectInputSystem::CloseJoysticks()
{
  for (Pad &pad : m_pads)
  {
    for (AxisEffects &effects : pad.effects)
    {
      for (LPDIRECTINPUTEFFECT &effect : effects)
        Release(effect);
    }
    if (pad.device)
    {
      pad.device->Unacquire();
      Release(pad.device);
    }
  }
  m_pads.clear();
}

// ---- Force feedback ------------------------------------------------------------

// XInput pads only have two rumble motors. A constant force runs the motor
// on the side it pushes towards (or both, with XInputStereoVibration); a
// vibration runs both. Springs and friction have no equivalent.
bool CDirectInputSystem::RumbleXInput(Pad &pad, int axisNum, const ForceFeedbackCmd &cmd)
{
  if (axisNum != AXIS_X && axisNum != AXIS_Y && axisNum != AXIS_RX && axisNum != AXIS_RY)
    return false;

  switch (cmd.id)
  {
  case FFStop:
    pad.pushLeft = pad.pushRight = pad.shake = 0;
    break;

  case FFConstantForce:
  {
    bool stereo = m_config["XInputStereoVibration"].ValueAs<bool>();
    unsigned most = m_config["XInputConstForceMax"].ValueAs<unsigned>();
    if (most == 0)
      return false;
    float size = cmd.force < 0.0f ? -cmd.force : cmd.force;
    float threshold = float(m_config["XInputConstForceThreshold"].ValueAs<unsigned>()) / 100.0f;
    WORD speed = WORD(size * float(most * kMotorPerPercent));
    if (size == 0.0f || size < threshold)
      pad.pushLeft = pad.pushRight = pad.shake = 0;
    else if (stereo)
      pad.shake = speed;
    else if (cmd.force < 0.0f)
    {
      pad.pushLeft = speed;
      pad.pushRight = 0;
    }
    else
    {
      pad.pushLeft = 0;
      pad.pushRight = speed;
    }
    break;
  }

  case FFVibrate:
  {
    unsigned most = m_config["XInputVibrateMax"].ValueAs<unsigned>();
    if (most == 0)
      return false;
    pad.shake = cmd.force == 0.0f ? 0 : WORD(cmd.force * float(most * kMotorPerPercent));
    break;
  }

  default:
    return false;
  }

  XINPUT_VIBRATION motors;
  motors.wLeftMotorSpeed = WORD(std::min<unsigned>(unsigned(pad.pushLeft) + pad.shake, kMotorFull));
  motors.wRightMotorSpeed = WORD(std::min<unsigned>(unsigned(pad.pushRight) + pad.shake, kMotorFull));
  return m_xiSetState(pad.xinputSlot, &motors) == ERROR_SUCCESS;
}

// One DirectInput effect per axis and kind, made on first use and then only
// given new strengths. The *Max settings scale each kind (0 turns it off).
bool CDirectInputSystem::ForceDirectInput(Pad &pad, int axisNum, const ForceFeedbackCmd &cmd)
{
  if (cmd.id == FFStop)
    return SUCCEEDED(pad.device->SendForceFeedbackCommand(DISFFC_STOPALL));
  if (axisNum < 0 || axisNum >= NUM_JOY_AXES || cmd.id < FFConstantForce || cmd.id > FFVibrate)
    return false;

  LONG strength;
  switch (cmd.id)
  {
  case FFConstantForce:
  {
    // DirectInput pushes the other way round from the game's sign
    bool right = cmd.force >= 0.0f;
    unsigned most = m_config[right ? "DirectInputConstForceRightMax" : "DirectInputConstForceLeftMax"].ValueAs<unsigned>();
    if (most == 0)
      return false;
    strength = LONG(-cmd.force * float(most * kForcePerPercent));
    strength = right ? std::max(strength, -kForceFull) : std::min(strength, kForceFull);
    break;
  }
  case FFSelfCenter:
  case FFFriction:
  {
    unsigned most = m_config[cmd.id == FFSelfCenter ? "DirectInputSelfCenterMax" : "DirectInputFrictionMax"].ValueAs<unsigned>();
    if (most == 0)
      return false;
    strength = Within(LONG(cmd.force * float(most * kForcePerPercent)), 0, kForceFull);
    break;
  }
  default:  // FFVibrate
  {
    unsigned most = m_config["DirectInputVibrateMax"].ValueAs<unsigned>();
    if (most == 0)
      return false;
    DWORD size = DWORD(cmd.force * float(most * kForcePerPercent));
    strength = LONG(std::min<DWORD>(size, DWORD(kForceFull)));
    break;
  }
  }

  AxisEffects &axisEffects = pad.effects[size_t(axisNum) % NUM_JOY_AXES];
  size_t kind = size_t(cmd.id);
  if (kind >= axisEffects.size())
    return false;
  LPDIRECTINPUTEFFECT &effect = axisEffects[kind];
  if (!effect && !(effect = MakeEffect(pad.device, axisNum, cmd.id)))
    return false;
  EffectParams params(cmd.id, strength);
  return SUCCEEDED(effect->SetParameters(&params.effect, DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS | DIEP_START));
}

bool CDirectInputSystem::ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd)
{
  if (joyNum < 0 || joyNum >= int(m_pads.size()))
    return false;
  Pad &pad = m_pads[joyNum];
  return pad.xinput ? RumbleXInput(pad, axisNum, ffCmd) : ForceDirectInput(pad, axisNum, ffCmd);
}

// ---- Reading inputs ------------------------------------------------------------

int CDirectInputSystem::GetKeyIndex(const char *keyName)
{
  for (int i = 0; i < kNumKeys; i++)
  {
    if (_stricmp(keyName, kKeys[i].name) == 0)
      return i;
  }
  return -1;
}

const char *CDirectInputSystem::GetKeyName(int keyIndex)
{
  return keyIndex >= 0 && keyIndex < kNumKeys ? kKeys[keyIndex].name : nullptr;
}

bool CDirectInputSystem::IsKeyPressed(int kbdNum, int keyIndex) const
{
  int code = kKeys[keyIndex].scanCode;
  if (m_useRawInput)
    return m_rawKeyboards[kbdNum].down[code];
  return (m_diKeys[code] & 0x80) != 0;
}

int CDirectInputSystem::GetMouseAxisValue(int mseNum, int axisNum) const
{
  const MouseState *mouse;
  if (m_useRawInput)
  {
    // The cursor is hidden with Raw Input, so the tracked positions are used
    mouse = mseNum == ANY_MOUSE ? &m_allMice : &m_rawMice[mseNum].state;
  }
  else
  {
    // With DirectInput, follow the cursor itself where possible
    POINT cursor;
    if ((axisNum == AXIS_X || axisNum == AXIS_Y) && GetCursorPos(&cursor) && ScreenToClient(m_hwnd, &cursor))
      return axisNum == AXIS_X ? cursor.x : cursor.y;
    mouse = &m_diMouseState;
  }
  switch (axisNum)
  {
  case AXIS_X: return mouse->x;
  case AXIS_Y: return mouse->y;
  case AXIS_Z: return mouse->z;
  default:     return 0;
  }
}

int CDirectInputSystem::GetMouseWheelDir(int mseNum) const
{
  if (m_useRawInput)
    return mseNum == ANY_MOUSE ? m_allMice.wheelDir : m_rawMice[mseNum].state.wheelDir;
  return m_diMouseState.wheelDir;
}

bool CDirectInputSystem::IsMouseButPressed(int mseNum, int butNum) const
{
  if (butNum < 0 || butNum >= 5)
    return false;
  unsigned buttons;
  if (m_useRawInput)
    buttons = mseNum == ANY_MOUSE ? m_allMice.buttons : m_rawMice[mseNum].state.buttons;
  else
    buttons = m_diMouseState.buttons;
  return (buttons & (1u << butNum)) != 0;
}

int CDirectInputSystem::GetJoyAxisValue(int joyNum, int axisNum) const
{
  if (axisNum < 0 || axisNum >= NUM_JOY_AXES)
    return 0;
  const BYTE *state = reinterpret_cast<const BYTE *>(&m_pads[joyNum].state);
  return int(*reinterpret_cast<const LONG *>(state + kAxisOffsets[axisNum]));
}

bool CDirectInputSystem::IsJoyPOVInDir(int joyNum, int povNum, int povDir) const
{
  // Each direction takes in the diagonals either side of it
  int degrees = int(m_pads[joyNum].state.rgdwPOV[povNum] / 100);
  auto any = [degrees](int a, int b, int c) { return degrees == a || degrees == b || degrees == c; };
  switch (povDir)
  {
  case POV_UP:    return any(315, 0, 45);
  case POV_RIGHT: return any(45, 90, 135);
  case POV_DOWN:  return any(135, 180, 225);
  case POV_LEFT:  return any(225, 270, 315);
  default:        return false;
  }
}

bool CDirectInputSystem::IsJoyButPressed(int joyNum, int butNum) const
{
  return m_pads[joyNum].state.rgbButtons[butNum] != 0;
}

bool CDirectInputSystem::ConfigMouseCentered()
{
  // Judged by the cursor, which is what the player sees while configuring
  POINT cursor;
  if (!GetCursorPos(&cursor) || !ScreenToClient(m_hwnd, &cursor))
    return false;
  LONG left = LONG(m_dispX + m_dispW / 4);
  LONG top = LONG(m_dispY + m_dispH / 4);
  if (cursor.x < left || cursor.x > left + LONG(m_dispW / 2) || cursor.y < top || cursor.y > top + LONG(m_dispH / 2))
    return false;
  SyncMiceToCursor();
  return true;
}

CInputSource *CDirectInputSystem::CreateAnyMouseSource(EMousePart msePart)
{
  // Raw Input keeps its own combined mouse
  if (m_useRawInput)
    return CreateMouseSource(ANY_MOUSE, msePart);
  return CInputSystem::CreateAnyMouseSource(msePart);
}

// ---- Devices -------------------------------------------------------------------

int CDirectInputSystem::GetNumKeyboards() const
{
  // DirectInput merges every keyboard into one
  return m_useRawInput ? int(m_rawKeyboards.size()) : ANY_KEYBOARD;
}

int CDirectInputSystem::GetNumMice() const
{
  return m_useRawInput ? int(m_rawMice.size()) : ANY_MOUSE;
}

int CDirectInputSystem::GetNumJoysticks() const
{
  return int(m_pads.size());
}

const KeyDetails *CDirectInputSystem::GetKeyDetails(int kbdNum)
{
  return m_useRawInput ? &m_keyDetails[kbdNum] : nullptr;
}

const MouseDetails *CDirectInputSystem::GetMouseDetails(int mseNum)
{
  return m_useRawInput ? &m_mouseDetails[mseNum] : nullptr;
}

const JoyDetails *CDirectInputSystem::GetJoyDetails(int joyNum)
{
  return &m_pads[joyNum].details;
}

// ---- Each frame ------------------------------------------------------------------

bool CDirectInputSystem::Poll()
{
  if (!m_activated)
  {
    // The devices attach to the SDL window, which exists by the first poll
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(m_window, &info))
      m_hwnd = info.info.win.window;
    Activate();
    m_activated = true;
  }

  // Empty the thread's message queue here rather than through SDL, which
  // would let Raw Input handles go stale before they are read; everything
  // still goes on to SDL's window procedure.
  MSG msg;
  while (PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE))
  {
    BOOL got = GetMessageA(&msg, nullptr, 0, 0);
    if (got == 0)
      return false;  // WM_QUIT
    if (got > 0)
    {
      TranslateMessage(&msg);
      if (m_useRawInput && msg.message == WM_INPUT)
        ReadRawInput((HRAWINPUT)msg.lParam);
      DispatchMessageA(&msg);
    }
  }

  SDL_Event event;
  while (SDL_PollEvent(&event))
  {
    if (event.type == SDL_QUIT)
      return false;
  }

  PollKeyboardsAndMice();
  PollJoysticks();
  return true;
}

void CDirectInputSystem::SetMouseVisibility(bool visible)
{
  // Raw Input mice never show the cursor while grabbed
  bool show = m_useRawInput ? (!m_grabMouse && visible) : visible;
  ShowCursor(show ? TRUE : FALSE);
}

void CDirectInputSystem::GrabMouse()
{
  CInputSystem::GrabMouse();
  if (m_useRawInput)
    SetMouseVisibility(false);
  if (m_activated)
    Activate();
}

void CDirectInputSystem::UngrabMouse()
{
  CInputSystem::UngrabMouse();
  if (m_activated)
    Activate();
}
