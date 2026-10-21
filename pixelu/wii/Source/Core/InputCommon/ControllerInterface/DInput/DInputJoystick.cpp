// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/DInput/DInputJoystick.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <set>
#include <span>
#include <unordered_set>

#include <fmt/format.h>

#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInput.h"
#include "InputCommon/ControllerInterface/DInput/XInputFilter.h"

namespace ciface::DInput
{
namespace
{
// Buffered events kept by DirectInput between updates.
constexpr DWORD EVENT_BUFFER_SIZE = 32;

// The data format's limits.
constexpr DWORD MAX_BUTTONS = 32;
constexpr DWORD MAX_HATS = 4;
// The LONG values (axes, sliders, then the hats) come before the button bytes.
constexpr DWORD LONG_VALUE_COUNT = DIJOFS_BUTTON(0) / sizeof(LONG);

struct GuidLess
{
  bool operator()(const GUID& a, const GUID& b) const { return std::memcmp(&a, &b, sizeof(a)) < 0; }
};

// Instances that have a Joystick, so they are not added twice.
std::set<GUID, GuidLess> s_instances_in_use;
std::mutex s_instances_mutex;

bool IsInUse(const GUID& instance)
{
  std::lock_guard lock(s_instances_mutex);
  return s_instances_in_use.contains(instance);
}

class ButtonInput final : public Core::Device::Input
{
public:
  ButtonInput(u8 index, const BYTE* state) : m_state(state), m_index(index) {}

  std::string GetName() const override { return fmt::format("Button {}", m_index); }
  ControlState GetState() const override { return ControlState(*m_state > 0); }

private:
  const BYTE* const m_state;
  const u8 m_index;
};

// One half of an axis or slider, from its centre to one end of its range.
class AxisInput final : public Core::Device::Input
{
public:
  AxisInput(u8 index, const LONG* value, LONG center, LONG extent)
      : m_value(value), m_center(center), m_extent(extent), m_index(index)
  {
  }

  std::string GetName() const override
  {
    const char sign = m_extent < 0 ? '-' : '+';
    // Six axes (X, Y, Z, then the rotations Xr, Yr, Zr), then sliders.
    if (m_index >= 6)
      return fmt::format("Slider {}{:c}", m_index - 6, sign);
    return fmt::format("Axis {:c}{}{:c}", 'X' + m_index % 3, m_index >= 3 ? "r" : "", sign);
  }

  ControlState GetState() const override { return ControlState(*m_value - m_center) / m_extent; }

private:
  const LONG* const m_value;
  const LONG m_center;
  const LONG m_extent;
  const u8 m_index;
};

// One direction (N, E, S, W) of a point-of-view hat. The hat value is an angle in hundredths
// of a degree, clockwise from north; diagonals count for both neighbouring directions.
class HatInput final : public Core::Device::Input
{
public:
  HatInput(u8 index, const DWORD* value, u8 direction)
      : m_value(value), m_index(index), m_direction(direction)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("Hat {} {:c}", m_index, "NESW"[m_direction]);
  }

  ControlState GetState() const override
  {
    const DWORD value = *m_value;
    // Centred is -1, or 0xffff in the low word for some drivers.
    if (LOWORD(value) == 0xffff)
      return 0;

    // Which of the eight 45 degree sectors the hat is in, counted from this direction.
    const int sector = (int(value / 4500) + 8 - 2 * m_direction) % 8;
    return sector == 7 || sector <= 1;
  }

private:
  const DWORD* const m_value;
  const u8 m_index;
  const u8 m_direction;
};

// Sets up a created device; on failure the device is released and nullptr returned.
LPDIRECTINPUTDEVICE8 PrepareDevice(LPDIRECTINPUTDEVICE8 device, HWND hwnd)
{
  if (FAILED(device->SetDataFormat(&c_dfDIJoystick)))
  {
    device->Release();
    return nullptr;
  }

  // Force feedback needs exclusive access, which needs the top-level window.
  const HRESULT exclusive =
      device->SetCooperativeLevel(GetAncestor(hwnd, GA_ROOT), DISCL_BACKGROUND | DISCL_EXCLUSIVE);
  if (SUCCEEDED(exclusive))
    return device;

  WARN_LOG_FMT(CONTROLLERINTERFACE,
               "DInput: Failed to acquire device exclusively. Force feedback will be "
               "unavailable.  {}",
               Common::HRWrap(exclusive));
  if (FAILED(device->SetCooperativeLevel(nullptr, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)))
  {
    device->Release();
    return nullptr;
  }
  return device;
}
}  // namespace

void InitJoystick(IDirectInput8* const idi8, HWND hwnd)
{
  std::list<DIDEVICEINSTANCE> attached;
  idi8->EnumDevices(DI8DEVCLASS_GAMECTRL, DIEnumDevicesCallback, &attached, DIEDFL_ATTACHEDONLY);

  const std::unordered_set<DWORD> xinput_products = GetXInputGUIDS();
  for (const DIDEVICEINSTANCE& instance : attached)
  {
    if (xinput_products.contains(instance.guidProduct.Data1))
      continue;
    if (IsInUse(instance.guidInstance))
      continue;

    // Failures here are not reported.
    LPDIRECTINPUTDEVICE8 created;
    if (FAILED(idi8->CreateDevice(instance.guidInstance, &created, nullptr)))
      continue;
    LPDIRECTINPUTDEVICE8 const device = PrepareDevice(created, hwnd);
    if (device == nullptr)
      continue;

    auto joystick = std::make_shared<Joystick>(device);
    // A joystick without inputs or outputs is dropped, and not remembered as in use: it may
    // get outputs once exclusive access works (e.g. once there is a window).
    if (joystick->Inputs().empty() && joystick->Outputs().empty())
      continue;
    if (!g_controller_interface.AddDevice(std::move(joystick)))
      continue;

    std::lock_guard lock(s_instances_mutex);
    s_instances_in_use.insert(instance.guidInstance);
  }
}

Joystick::Joystick(const LPDIRECTINPUTDEVICE8 device) : m_device(device)
{
  // The event buffer must be set up before reading the capabilities and acquiring. Without
  // it, whole states are read instead.
  DIPROPDWORD buffer_size;
  buffer_size.diph.dwSize = sizeof(DIPROPDWORD);
  buffer_size.diph.dwHeaderSize = sizeof(DIPROPHEADER);
  buffer_size.diph.dwObj = 0;
  buffer_size.diph.dwHow = DIPH_DEVICE;
  buffer_size.dwData = EVENT_BUFFER_SIZE;
  m_buffered = SUCCEEDED(m_device->SetProperty(DIPROP_BUFFERSIZE, &buffer_size.diph));

  m_device->Acquire();

  DIDEVCAPS caps;
  caps.dwSize = sizeof(caps);
  if (FAILED(m_device->GetCapabilities(&caps)))
    return;

  const DWORD button_count = std::min(MAX_BUTTONS, caps.dwButtons);
  for (u8 i = 0; i != button_count; ++i)
    AddInput(new ButtonInput(i, &m_state_in.rgbButtons[i]));

  const DWORD hat_count = std::min(MAX_HATS, caps.dwPOVs);
  for (u8 i = 0; i != hat_count; ++i)
  {
    for (u8 direction = 0; direction != 4; ++direction)
      AddInput(new HatInput(i, &m_state_in.rgdwPOV[i], direction));
  }

  // Each LONG slot of the state is tried as an axis: one that has a range is present. A
  // 16 bit range is asked for first; some devices need that, others keep their own, so the
  // range is read back either way.
  DIPROPRANGE range;
  range.diph.dwSize = sizeof(range);
  range.diph.dwHeaderSize = sizeof(range.diph);
  range.diph.dwHow = DIPH_BYOFFSET;
  for (unsigned int slot = 0; slot < LONG_VALUE_COUNT; ++slot)
  {
    range.diph.dwObj = static_cast<DWORD>(slot * sizeof(LONG));
    range.lMin = std::numeric_limits<s16>::min();
    range.lMax = std::numeric_limits<s16>::max();
    m_device->SetProperty(DIPROP_RANGE, &range.diph);
    if (FAILED(m_device->GetProperty(DIPROP_RANGE, &range.diph)))
      continue;

    const LONG center = (range.lMin + range.lMax) / 2;
    const LONG* const value = &m_state_in.lX + slot;
    AddFullAnalogSurfaceInputs(new AxisInput(u8(slot), value, center, range.lMin - center),
                               new AxisInput(u8(slot), value, center, range.lMax - center));
  }

  std::list<DIDEVICEOBJECTINSTANCE> axes;
  if (SUCCEEDED(m_device->EnumObjects(DIEnumDeviceObjectsCallback, &axes, DIDFT_AXIS)))
  {
    const auto actuators = std::ranges::count_if(axes, [](const DIDEVICEOBJECTINSTANCE& axis) {
      return (axis.dwFlags & DIDOI_FFACTUATOR) != 0;
    });
    InitForceFeedback(m_device, int(actuators));
  }

  // Hats start centred (-1).
  std::ranges::fill(m_state_in.rgdwPOV, DWORD(-1));
}

std::string Joystick::GetName() const
{
  return GetDeviceName(m_device);
}

std::string Joystick::GetSource() const
{
  return DINPUT_SOURCE_NAME;
}

bool Joystick::IsValid() const
{
  return SUCCEEDED(m_device->Acquire());
}

Joystick::~Joystick()
{
  DIDEVICEINSTANCE info{};
  info.dwSize = sizeof(info);
  if (FAILED(m_device->GetDeviceInfo(&info)))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "DInputJoystick: GetDeviceInfo failed.");
  }
  else
  {
    std::lock_guard lock(s_instances_mutex);
    s_instances_in_use.erase(info.guidInstance);
  }

  DeInitForceFeedback();

  m_device->Unacquire();
  m_device->Release();
}

void Joystick::ApplyBufferedEvents(const DIDEVICEOBJECTDATA* events, DWORD count)
{
  BYTE* const state = reinterpret_cast<BYTE*>(&m_state_in);
  for (const DIDEVICEOBJECTDATA& event : std::span(events, count))
  {
    // Offsets are those of the data format: LONG values first, then a byte per button.
    if (event.dwOfs < DIJOFS_BUTTON(0))
      std::memcpy(state + event.dwOfs, &event.dwData, sizeof(DWORD));
    else if (event.dwOfs < sizeof(m_state_in))
      state[event.dwOfs] = static_cast<BYTE>(event.dwData);
  }
}

Core::DeviceRemoval Joystick::UpdateInput()
{
  // Harmless for devices that don't need polling.
  m_device->Poll();

  HRESULT result;
  if (m_buffered)
  {
    DIDEVICEOBJECTDATA events[EVENT_BUFFER_SIZE];
    DWORD count = EVENT_BUFFER_SIZE;
    result = m_device->GetDeviceData(sizeof(*events), events, &count, 0);
    if (SUCCEEDED(result))
    {
      ApplyBufferedEvents(events, count);
      // Events were lost: read the whole state instead.
      if (result == DI_BUFFEROVERFLOW)
        result = m_device->GetDeviceState(sizeof(m_state_in), &m_state_in);
    }
  }
  else
  {
    result = m_device->GetDeviceState(sizeof(m_state_in), &m_state_in);
  }

  if (result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED)
    m_device->Acquire();

  return Core::DeviceRemoval::Keep;
}
}  // namespace ciface::DInput
