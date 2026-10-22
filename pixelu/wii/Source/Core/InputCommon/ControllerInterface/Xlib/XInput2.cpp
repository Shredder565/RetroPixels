// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/Xlib/XInput2.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Host.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

// Last: these headers define macros (None, Bool, Status, Success...) that must not reach the
// headers above.
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>

namespace ciface::XInput2
{
namespace
{
constexpr std::string_view SOURCE_NAME = "XInput2";

// The XI2 version asked for. 2.1 is the least that delivers raw events to a client that has not
// grabbed the device, which the mouse movement and wheel inputs need; 2.0 servers still get keys,
// buttons and the cursor.
constexpr int WANTED_XI_MAJOR = 2;
constexpr int WANTED_XI_MINOR = 2;

// X11 buttons 1-32 are mappable as "Click 1".."Click 32" (1 left, 2 middle, 3 right, 4-7 the
// wheels, 8 and 9 back and forward). Past the ninth they are kept out of the lists, as hardly
// any mouse has them, but configurations naming them still work.
constexpr int BUTTON_COUNT = 32;
constexpr int LISTED_BUTTONS = 9;
constexpr int WHEEL_UP_BUTTON = 4;
constexpr int WHEEL_DOWN_BUTTON = 5;

// Movement per update that counts as a full "Axis" input: 8 device units (unaccelerated mouse
// counts, as DirectInput reports them on Windows) for X and Y, one wheel notch for Z.
constexpr double MOUSE_FULL_MOVEMENT = 8;
constexpr double WHEEL_FULL_MOVEMENT = 1;

// After this long without an update the movement is dropped, as it would otherwise come out as
// one huge jump.
constexpr auto STALE_MOUSE_TIME = std::chrono::milliseconds(250);

using Clock = std::chrono::steady_clock;
using Motion = Common::TVec3<double>;
using RelativeMotion = RelativeInputState<Motion>;

// ---------------------------------------------------------------------------------------------
// X errors. Xlib reports them to one handler per process, whose default prints the error and
// exits. Requests about the render window fail with BadWindow once the window is gone, which must
// not end the program, so errors on this backend's own connections are recorded and every other
// error goes on to the handler that was installed before (the frontend's, or Xlib's default).

std::mutex s_error_mutex;
XErrorHandler s_next_error_handler = nullptr;
bool s_error_handler_installed = false;
// The backend's connections and where to record their last error code.
std::vector<std::pair<Display*, int*>> s_trapped_displays;

int OnXError(Display* display, XErrorEvent* error)
{
  XErrorHandler next;
  {
    std::lock_guard lock(s_error_mutex);
    for (const auto& [trapped, code] : s_trapped_displays)
    {
      if (trapped == display)
      {
        *code = error->error_code;
        return 0;
      }
    }
    next = s_next_error_handler;
  }
  return next != nullptr ? next(display, error) : 0;
}

void TrapErrors(Display* display, int* code)
{
  std::lock_guard lock(s_error_mutex);
  if (!s_error_handler_installed)
  {
    // Kept for the rest of the run: a handler installed after this one may have saved it.
    s_next_error_handler = XSetErrorHandler(OnXError);
    s_error_handler_installed = true;
  }
  s_trapped_displays.emplace_back(display, code);
}

void UntrapErrors(Display* display)
{
  std::lock_guard lock(s_error_mutex);
  std::erase_if(s_trapped_displays,
                [display](const auto& entry) { return entry.first == display; });
}

// ---------------------------------------------------------------------------------------------
// The state of one seat (a master pointer and its master keyboard), filled by the connection.

struct Seat
{
  int pointer_id = 0;
  int keyboard_id = 0;
  std::string name;
  // The keyboard XQueryKeymap reports (the client pointer's pair) is read with it on every
  // update; the keys of other master keyboards are followed through their raw key events.
  bool keymap_keyboard = false;

  // One bit per keycode, in XQueryKeymap's layout.
  std::array<u8, 32> keys{};
  // Bit n-1 for button n.
  u32 buttons = 0;
  // Cursor position in the render window, -1..1 from edge to edge (times the input scale).
  Common::TVec2<double> cursor{};
  // Movement since the previous update: X and Y in device units, Z in wheel notches (up/forward
  // positive), and its smoothed value that the "Axis" inputs read.
  Motion pending{};
  Motion smoothed{};
  RelativeMotion relative;
  Clock::time_point last_update = Clock::now();
};

// What the raw events of one slave device need: its master, and how its valuators report.
struct Slave
{
  int master = 0;
  // Valuator number -> whether it reports absolute positions (tablets, touch screens, some
  // virtual machine mice) rather than movement.
  std::map<int, bool> absolute;
  // Last position of each absolute valuator, to turn positions into movement.
  std::map<int, double> last_position;
  // Vertical scroll valuators (smooth scrolling, XI 2.1) -> their increment for one notch.
  std::map<int, double> vertical_scroll;
};

// One X connection of the backend's own, shared by the devices it made. Xlib connections are not
// to be used from several threads at once, so everything goes through the mutex (device updates
// are already serialised by ControllerInterface; window changes come from the UI thread).
class Connection
{
public:
  static std::shared_ptr<Connection> Open(const WindowSystemInfo& wsi);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  std::span<const std::unique_ptr<Seat>> Seats() const { return m_seats; }
  Display* GetDisplay() const { return m_display; }

  void SetWindow(Window window);
  void Update(Seat& seat);

private:
  Connection(Display* display, int opcode, int xi_minor);

  bool ReadSeats();
  void ReadSlaves();
  void SelectEvents();
  void ReadEvents();
  void OnRawEvent(const XIRawEvent& event);
  Seat* SeatOfDevice(int deviceid, bool pointer);
  void ReadPointer(Seat& seat);

  std::mutex m_mutex;
  Display* const m_display;
  const int m_opcode;
  const int m_xi_minor;
  const Window m_root;
  // Error code of the last failed request (0 when none since it was cleared).
  int m_error = 0;

  Window m_window = 0;
  int m_window_width = 1;
  int m_window_height = 1;

  std::vector<std::unique_ptr<Seat>> m_seats;
  std::map<int, Slave> m_slaves;
};

std::shared_ptr<Connection> Connection::Open(const WindowSystemInfo& wsi)
{
  // The frontend's display (it may have been given another one than $DISPLAY).
  const char* display_name = nullptr;
  if (wsi.display_connection != nullptr)
    display_name = DisplayString(static_cast<Display*>(wsi.display_connection));

  Display* const display = XOpenDisplay(display_name);
  if (display == nullptr)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "XInput2: cannot open X display {}",
                 display_name != nullptr ? display_name : "(DISPLAY not set)");
    return nullptr;
  }

  int opcode = 0;
  int first_event = 0;
  int first_error = 0;
  int major = WANTED_XI_MAJOR;
  int minor = WANTED_XI_MINOR;
  if (!XQueryExtension(display, "XInputExtension", &opcode, &first_event, &first_error) ||
      XIQueryVersion(display, &major, &minor) != Success || major < 2)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "XInput2: the X server has no X Input Extension 2");
    XCloseDisplay(display);
    return nullptr;
  }
  if (minor < 1)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE,
                 "XInput2: X server has XI 2.{} only; mouse movement and wheel inputs stay at "
                 "rest",
                 minor);
  }

  std::shared_ptr<Connection> connection(new Connection(display, opcode, minor));
  if (!connection->ReadSeats())
    return nullptr;
  connection->ReadSlaves();
  connection->SelectEvents();
  connection->SetWindow(reinterpret_cast<Window>(wsi.render_window));
  return connection;
}

Connection::Connection(Display* display, int opcode, int xi_minor)
    : m_display(display), m_opcode(opcode), m_xi_minor(xi_minor), m_root(DefaultRootWindow(display))
{
  TrapErrors(m_display, &m_error);
}

Connection::~Connection()
{
  XCloseDisplay(m_display);
  UntrapErrors(m_display);
}

bool Connection::ReadSeats()
{
  int client_pointer = 0;
  XIGetClientPointer(m_display, 0, &client_pointer);

  int count = 0;
  XIDeviceInfo* const devices = XIQueryDevice(m_display, XIAllMasterDevices, &count);
  if (devices == nullptr)
    return false;

  for (const XIDeviceInfo& device : std::span(devices, static_cast<size_t>(count)))
  {
    if (device.use != XIMasterPointer)
      continue;
    auto seat = std::make_unique<Seat>();
    seat->pointer_id = device.deviceid;
    seat->keyboard_id = device.attachment;
    seat->name = device.name != nullptr ? device.name : "Pointer";
    seat->keymap_keyboard = device.deviceid == client_pointer;
    m_seats.push_back(std::move(seat));
  }
  XIFreeDeviceInfo(devices);

  // The client pointer's seat (the desktop's own) first.
  std::ranges::stable_partition(m_seats, [](const auto& seat) { return seat->keymap_keyboard; });
  return !m_seats.empty();
}

void Connection::ReadSlaves()
{
  int count = 0;
  XIDeviceInfo* const devices = XIQueryDevice(m_display, XIAllDevices, &count);
  if (devices == nullptr)
    return;

  std::map<int, Slave> slaves;
  for (const XIDeviceInfo& device : std::span(devices, static_cast<size_t>(count)))
  {
    if (device.use != XISlavePointer && device.use != XISlaveKeyboard)
      continue;
    Slave& slave = slaves[device.deviceid];
    slave.master = device.attachment;

    // Keep positions already seen, so a device change in the middle of a movement adds no jump.
    if (const auto old = m_slaves.find(device.deviceid); old != m_slaves.end())
      slave.last_position = old->second.last_position;

    for (XIAnyClassInfo* const any : std::span(device.classes, size_t(device.num_classes)))
    {
      if (any->type == XIValuatorClass)
      {
        const auto* const valuator = reinterpret_cast<const XIValuatorClassInfo*>(any);
        slave.absolute[valuator->number] = valuator->mode == XIModeAbsolute;
      }
      else if (any->type == XIScrollClass)
      {
        const auto* const scroll = reinterpret_cast<const XIScrollClassInfo*>(any);
        if (scroll->scroll_type == XIScrollTypeVertical && scroll->increment != 0)
          slave.vertical_scroll[scroll->number] = scroll->increment;
      }
    }
  }
  XIFreeDeviceInfo(devices);
  m_slaves = std::move(slaves);
}

void Connection::SelectEvents()
{
  std::array<unsigned char, XIMaskLen(XI_LASTEVENT)> raw_bits{};
  std::array<unsigned char, XIMaskLen(XI_LASTEVENT)> change_bits{};
  if (m_xi_minor >= 1)
  {
    for (const int type : {XI_RawKeyPress, XI_RawKeyRelease, XI_RawButtonPress, XI_RawMotion})
      XISetMask(raw_bits.data(), type);
  }
  XISetMask(change_bits.data(), XI_HierarchyChanged);
  XISetMask(change_bits.data(), XI_DeviceChanged);

  std::array<XIEventMask, 2> masks{};
  masks[0].deviceid = XIAllMasterDevices;
  masks[0].mask_len = int(raw_bits.size());
  masks[0].mask = raw_bits.data();
  masks[1].deviceid = XIAllDevices;
  masks[1].mask_len = int(change_bits.size());
  masks[1].mask = change_bits.data();
  XISelectEvents(m_display, m_root, masks.data(), int(masks.size()));
  XFlush(m_display);
}

void Connection::SetWindow(Window window)
{
  std::lock_guard lock(m_mutex);

  if (m_window != 0 && m_window != window)
    XSelectInput(m_display, m_window, NoEventMask);

  m_window = window;
  m_window_width = 1;
  m_window_height = 1;
  if (window == 0)
    return;

  // Its size is followed through ConfigureNotify from here on.
  XSelectInput(m_display, window, StructureNotifyMask);
  XWindowAttributes attributes{};
  m_error = 0;
  if (!XGetWindowAttributes(m_display, window, &attributes) || m_error != 0)
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "XInput2: render window {:#x} is not usable", window);
    m_window = 0;
    return;
  }
  m_window_width = std::max(attributes.width, 1);
  m_window_height = std::max(attributes.height, 1);
}

Seat* Connection::SeatOfDevice(int deviceid, bool pointer)
{
  for (int id : {deviceid, -1})
  {
    if (id == -1)
    {
      // A slave: its master's seat.
      const auto slave = m_slaves.find(deviceid);
      if (slave == m_slaves.end())
        return nullptr;
      id = slave->second.master;
    }
    for (const std::unique_ptr<Seat>& seat : m_seats)
    {
      if ((pointer ? seat->pointer_id : seat->keyboard_id) == id)
        return seat.get();
    }
  }
  return nullptr;
}

void Connection::OnRawEvent(const XIRawEvent& event)
{
  const bool pointer = event.evtype == XI_RawButtonPress || event.evtype == XI_RawMotion;
  Seat* const seat = SeatOfDevice(event.deviceid, pointer);
  if (seat == nullptr)
    return;

  const auto slave_it = m_slaves.find(event.sourceid);
  Slave* const slave = slave_it != m_slaves.end() ? &slave_it->second : nullptr;

  switch (event.evtype)
  {
  case XI_RawKeyPress:
  case XI_RawKeyRelease:
  {
    if (seat->keymap_keyboard || event.detail < 0 || event.detail > 255)
      return;
    const u8 bit = u8(1u << (event.detail % 8));
    u8& byte = seat->keys[size_t(event.detail / 8)];
    byte = event.evtype == XI_RawKeyPress ? (byte | bit) : (byte & ~bit);
    return;
  }

  case XI_RawButtonPress:
  {
    // Wheel clicks, unless the device scrolls smoothly: then the server also sends emulated
    // clicks for the same movement, which the valuators below already count.
    const bool smooth = slave != nullptr && !slave->vertical_scroll.empty();
    if (smooth && (event.flags & XIPointerEmulated) != 0)
      return;
    if (event.detail == WHEEL_UP_BUTTON)
      seat->pending.z += 1;
    else if (event.detail == WHEEL_DOWN_BUTTON)
      seat->pending.z -= 1;
    return;
  }

  case XI_RawMotion:
  {
    const double* raw_value = event.raw_values;
    const int bits = event.valuators.mask_len * 8;
    for (int number = 0; number < bits; ++number)
    {
      if (!XIMaskIsSet(event.valuators.mask, number))
        continue;
      double value = *raw_value++;

      if (slave != nullptr && slave->absolute.contains(number) && slave->absolute[number])
      {
        const auto last = slave->last_position.find(number);
        const double previous = last != slave->last_position.end() ? last->second : value;
        slave->last_position[number] = value;
        value -= previous;
      }

      if (number == 0)
      {
        seat->pending.x += value;
      }
      else if (number == 1)
      {
        seat->pending.y += value;
      }
      else if (slave != nullptr)
      {
        const auto scroll = slave->vertical_scroll.find(number);
        // Positive increments scroll down; Z is positive upwards.
        if (scroll != slave->vertical_scroll.end())
          seat->pending.z -= value / scroll->second;
      }
    }
    return;
  }

  default:
    return;
  }
}

void Connection::ReadEvents()
{
  while (XPending(m_display) > 0)
  {
    XEvent event;
    XNextEvent(m_display, &event);

    if (event.type == ConfigureNotify && event.xconfigure.window == m_window)
    {
      m_window_width = std::max(event.xconfigure.width, 1);
      m_window_height = std::max(event.xconfigure.height, 1);
      continue;
    }
    if (event.type == DestroyNotify && event.xdestroywindow.window == m_window)
    {
      m_window = 0;
      continue;
    }

    XGenericEventCookie& cookie = event.xcookie;
    if (cookie.type != GenericEvent || cookie.extension != m_opcode ||
        !XGetEventData(m_display, &cookie))
    {
      continue;
    }

    switch (cookie.evtype)
    {
    case XI_HierarchyChanged:
    case XI_DeviceChanged:
      ReadSlaves();
      break;
    case XI_RawKeyPress:
    case XI_RawKeyRelease:
    case XI_RawButtonPress:
    case XI_RawMotion:
      OnRawEvent(*static_cast<const XIRawEvent*>(cookie.data));
      break;
    default:
      break;
    }
    XFreeEventData(m_display, &cookie);
  }
}

void Connection::ReadPointer(Seat& seat)
{
  const Window window = m_window != 0 ? m_window : m_root;
  int width = m_window_width;
  int height = m_window_height;
  if (m_window == 0)
  {
    width = std::max(DisplayWidth(m_display, DefaultScreen(m_display)), 1);
    height = std::max(DisplayHeight(m_display, DefaultScreen(m_display)), 1);
  }

  Window root_return = 0;
  Window child_return = 0;
  double root_x = 0;
  double root_y = 0;
  double x = 0;
  double y = 0;
  XIButtonState buttons{};
  XIModifierState modifiers{};
  XIGroupState group{};
  m_error = 0;
  const bool same_screen =
      XIQueryPointer(m_display, seat.pointer_id, window, &root_return, &child_return, &root_x,
                     &root_y, &x, &y, &buttons, &modifiers, &group);
  if (m_error != 0)
  {
    // The window went away (BadWindow); the cursor is measured on the root window until the
    // frontend names a new one.
    if (window == m_window)
      m_window = 0;
    return;
  }

  u32 pressed = 0;
  for (int button = 1; button <= BUTTON_COUNT && button < buttons.mask_len * 8; ++button)
  {
    if (XIMaskIsSet(buttons.mask, button))
      pressed |= 1u << (button - 1);
  }
  seat.buttons = pressed;
  XFree(buttons.mask);

  if (!same_screen)
    return;

  if (g_controller_interface.IsMouseCenteringRequested() &&
      (Host_RendererHasFocus() || Host_TASInputHasFocus()))
  {
    x = width / 2;
    y = height / 2;
    XIWarpPointer(m_display, seat.pointer_id, 0, window, 0, 0, 0, 0, x, y);
    XFlush(m_display);
    g_controller_interface.SetMouseCenteringRequested(false);
  }
  else if (Host_TASInputHasFocus())
  {
    // Clicks in a focused TAS input window must not move the game's pointer.
    return;
  }

  const Common::Vec2 scale = g_controller_interface.GetWindowInputScale();
  seat.cursor.x = (x / width * 2 - 1) * scale.x;
  seat.cursor.y = (y / height * 2 - 1) * scale.y;
}

void Connection::Update(Seat& seat)
{
  std::lock_guard lock(m_mutex);

  ReadEvents();

  const Clock::time_point now = Clock::now();
  if (now - seat.last_update > STALE_MOUSE_TIME)
  {
    seat.pending = {};
    seat.smoothed = {};
    seat.relative = {};
  }
  seat.last_update = now;

  seat.relative.Move(seat.pending);
  seat.relative.Update();
  // Averaged with the previous update's value; raw movement is too jumpy to use as it is.
  seat.smoothed.x = (seat.smoothed.x + seat.pending.x) / 2;
  seat.smoothed.y = (seat.smoothed.y + seat.pending.y) / 2;
  seat.smoothed.z = (seat.smoothed.z + seat.pending.z) / 2;
  seat.pending = {};

  if (seat.keymap_keyboard)
    XQueryKeymap(m_display, reinterpret_cast<char*>(seat.keys.data()));

  ReadPointer(seat);
}

// ---------------------------------------------------------------------------------------------
// Inputs.

// A key, by the name of its keysym. Keys that share a name (both Enter keys on some layouts, for
// example) are one input pressed by any of them.
class Key final : public Core::Device::Input
{
public:
  Key(std::string name, const std::array<u8, 32>* keys) : m_name(std::move(name)), m_keys(keys) {}

  void AddKeycode(int keycode) { m_keycodes.push_back(u8(keycode)); }
  void AddAlias(std::string alias) { m_aliases.push_back(std::move(alias)); }

  std::string GetName() const override { return m_name; }
  bool IsMatchingName(std::string_view name) const override
  {
    return name == m_name || std::ranges::find(m_aliases, name) != m_aliases.end();
  }
  ControlState GetState() const override
  {
    return std::ranges::any_of(m_keycodes, [this](u8 keycode) {
      return ((*m_keys)[keycode / 8] >> (keycode % 8) & 1) != 0;
    });
  }

private:
  const std::string m_name;
  const std::array<u8, 32>* const m_keys;
  std::vector<u8> m_keycodes;
  std::vector<std::string> m_aliases;
};

class Button final : public Core::Device::Input
{
public:
  Button(int number, const u32* buttons) : m_buttons(buttons), m_number(number) {}

  std::string GetName() const override { return fmt::format("Click {}", m_number); }
  bool IsHidden() const override { return m_number > LISTED_BUTTONS; }
  ControlState GetState() const override { return (*m_buttons >> (m_number - 1) & 1) != 0; }

private:
  const u32* const m_buttons;
  const int m_number;
};

// One direction of the cursor position, from the centre of the render window.
class CursorAxis final : public Core::Device::Input
{
public:
  CursorAxis(int index, bool positive, const Common::TVec2<double>* cursor)
      : m_cursor(cursor), m_index(index), m_positive(positive)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("Cursor {}{}", char('X' + m_index), m_positive ? '+' : '-');
  }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override
  {
    const double value = m_index == 0 ? m_cursor->x : m_cursor->y;
    return m_positive ? value : -value;
  }

private:
  const Common::TVec2<double>* const m_cursor;
  const int m_index;
  const bool m_positive;
};

// One direction of the smoothed movement per update (Z: the wheel).
class MouseAxis final : public Core::Device::Input
{
public:
  MouseAxis(int index, bool positive, const Motion* motion)
      : m_motion(motion), m_index(index), m_positive(positive)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("Axis {}{}", char('X' + m_index), m_positive ? '+' : '-');
  }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override
  {
    const double full = m_index == 2 ? WHEEL_FULL_MOVEMENT : MOUSE_FULL_MOVEMENT;
    const double value = m_motion->data[size_t(m_index)] / full;
    return m_positive ? value : -value;
  }

private:
  const Motion* const m_motion;
  const int m_index;
  const bool m_positive;
};

// One direction of the movement since the previous update of the reading input channel.
class RelativeMouseAxis final : public Core::Device::RelativeInput
{
public:
  RelativeMouseAxis(int index, bool positive, const RelativeMotion* motion)
      : m_motion(motion), m_index(index), m_positive(positive)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("RelativeMouse {}{}", char('X' + m_index), m_positive ? '+' : '-');
  }
  ControlState GetState() const override
  {
    const double value = m_motion->GetValue().data[size_t(m_index)];
    return m_positive ? value : -value;
  }

private:
  const RelativeMotion* const m_motion;
  const int m_index;
  const bool m_positive;
};

// The name of a key as configurations write it: its keysym without modifiers, in the first
// group that has one, through XKeysymToString ("Return", "space", "KP_Add", "F1"...), with the
// letters a-z in capitals ("A" for the A key).
std::string KeycodeName(Display* display, int keycode)
{
  KeySym keysym = NoSymbol;
  for (int group = 0; group < 4 && keysym == NoSymbol; ++group)
    keysym = XkbKeycodeToKeysym(display, KeyCode(keycode), group, 0);
  if (keysym == NoSymbol)
    return {};
  if (keysym >= XK_a && keysym <= XK_z)
    keysym -= XK_a - XK_A;
  const char* const name = XKeysymToString(keysym);
  return name != nullptr ? name : std::string();
}

// The keypad digits and decimal point are named by their keysym without Num Lock ("KP_End" for
// 1); the name with Num Lock ("KP_1"), which the default hotkeys use, is accepted as well.
std::string KeypadNumLockName(Display* display, int keycode)
{
  const KeySym keysym = XkbKeycodeToKeysym(display, KeyCode(keycode), 0, 1);
  if ((keysym < XK_KP_0 || keysym > XK_KP_9) && keysym != XK_KP_Decimal)
    return {};
  const char* const name = XKeysymToString(keysym);
  return name != nullptr ? name : std::string();
}

class KeyboardMouse final : public Core::Device
{
public:
  KeyboardMouse(std::shared_ptr<Connection> connection, Seat* seat, bool first);

  std::string GetName() const override { return m_seat->name; }
  std::string GetSource() const override { return std::string(SOURCE_NAME); }
  // The desktop's own seat is the default device of new mappings.
  int GetSortPriority() const override
  {
    return m_first ? DEFAULT_DEVICE_SORT_PRIORITY : DEFAULT_DEVICE_SORT_PRIORITY - 1;
  }
  bool IsVirtualDevice() const override { return true; }

  Core::DeviceRemoval UpdateInput() override
  {
    m_connection->Update(*m_seat);
    return Core::DeviceRemoval::Keep;
  }

private:
  const std::shared_ptr<Connection> m_connection;
  Seat* const m_seat;
  const bool m_first;
};

KeyboardMouse::KeyboardMouse(std::shared_ptr<Connection> connection, Seat* seat, bool first)
    : m_connection(std::move(connection)), m_seat(seat), m_first(first)
{
  Display* const display = m_connection->GetDisplay();

  int min_keycode = 0;
  int max_keycode = 0;
  XDisplayKeycodes(display, &min_keycode, &max_keycode);
  std::map<std::string, Key*> keys;
  for (int keycode = std::max(min_keycode, 8); keycode <= std::min(max_keycode, 255); ++keycode)
  {
    std::string name = KeycodeName(display, keycode);
    if (name.empty())
      continue;
    Key*& key = keys[name];
    if (key == nullptr)
    {
      key = new Key(std::move(name), &m_seat->keys);
      AddInput(key);
    }
    key->AddKeycode(keycode);
    std::string alias = KeypadNumLockName(display, keycode);
    if (!alias.empty() && alias != key->GetName() && !keys.contains(alias))
      key->AddAlias(std::move(alias));
  }

  // Either-side modifiers, as on the other systems.
  AddCombinedInput("Alt", {"Alt_L", "Alt_R"});
  AddCombinedInput("Shift", {"Shift_L", "Shift_R"});
  AddCombinedInput("Ctrl", {"Control_L", "Control_R"});

  for (int button = 1; button <= BUTTON_COUNT; ++button)
    AddInput(new Button(button, &m_seat->buttons));

  for (int index = 0; index != 2; ++index)
  {
    AddInput(new CursorAxis(index, false, &m_seat->cursor));
    AddInput(new CursorAxis(index, true, &m_seat->cursor));
  }
  for (int index = 0; index != 3; ++index)
  {
    AddInput(new MouseAxis(index, false, &m_seat->smoothed));
    AddInput(new MouseAxis(index, true, &m_seat->smoothed));
  }
  for (int index = 0; index != 3; ++index)
  {
    AddInput(new RelativeMouseAxis(index, false, &m_seat->relative));
    AddInput(new RelativeMouseAxis(index, true, &m_seat->relative));
  }
}

// ---------------------------------------------------------------------------------------------

class InputBackend final : public ciface::InputBackend
{
public:
  using ciface::InputBackend::InputBackend;

  void PopulateDevices() override;
  void HandleWindowChange() override;

private:
  void AddDevices();
  void RemoveDevices();

  // The connection of the current devices; it lives as long as they do.
  std::weak_ptr<Connection> m_connection;
};

void InputBackend::AddDevices()
{
  const WindowSystemInfo wsi = GetControllerInterface().GetWindowSystemInfo();
  if (wsi.type != WindowSystemType::X11)
    return;

  const std::shared_ptr<Connection> connection = Connection::Open(wsi);
  if (!connection)
    return;
  m_connection = connection;

  bool first = true;
  for (const std::unique_ptr<Seat>& seat : connection->Seats())
  {
    GetControllerInterface().AddDevice(
        std::make_shared<KeyboardMouse>(connection, seat.get(), first));
    first = false;
  }
}

void InputBackend::RemoveDevices()
{
  GetControllerInterface().RemoveDevice(
      [](const Core::Device* device) { return device->GetSource() == SOURCE_NAME; });
}

void InputBackend::PopulateDevices()
{
  // ControllerInterface has dropped every device before this.
  GetControllerInterface().PlatformPopulateDevices([this] { AddDevices(); });
}

void InputBackend::HandleWindowChange()
{
  const WindowSystemInfo wsi = GetControllerInterface().GetWindowSystemInfo();
  const std::shared_ptr<Connection> connection = m_connection.lock();
  if (connection && wsi.type == WindowSystemType::X11)
  {
    // Same display: only the window the cursor is measured in changes.
    connection->SetWindow(reinterpret_cast<Window>(wsi.render_window));
    return;
  }

  GetControllerInterface().PlatformPopulateDevices([this] {
    RemoveDevices();
    AddDevices();
  });
}
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::XInput2
