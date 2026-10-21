// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// pixelu-wii-input-check: checks PixelU-Wii's Linux input end to end on a running X server
// (Xvfb in CI). It opens a window, starts the controller interface as the frontends do, and
//  - presses keys and mouse buttons and moves the mouse with xdotool, then reads them back
//    from the XInput2 keyboard-and-mouse device and through the default hotkeys;
//  - plugs in an SDL virtual gamepad, maps GameCube controller 1 with the same GCPadNew.ini
//    lines RetroPixels writes for its "Set up Xbox controller" button (Device =
//    XInput/0/Gamepad, XInput input names), and reads the emulated controller and its rumble.
// No game or system file is used. Exit status 0 when every check passes, 1 otherwise.
//
//     pixelu-wii-input-check <empty user folder>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/WindowSystemInfo.h"
#include "Core/HW/GCPad.h"
#include "Core/Host.h"
#include "Core/HotkeyManager.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCPadStatus.h"
#include "UICommon/UICommon.h"

#include <X11/Xlib.h>

namespace
{
int s_failures = 0;
std::atomic<Uint16> s_rumble_low = 0;
std::atomic<Uint16> s_rumble_high = 0;

void Check(bool ok, const std::string& what)
{
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok)
    ++s_failures;
}

void Run(const std::string& command)
{
  const std::string line = "xdotool " + command;
  if (std::system(line.c_str()) != 0)
    Check(false, "ran " + line);
  // Let the server deliver the events.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
}

// Updates the inputs a few times (raw events may take a moment to arrive).
void Pump()
{
  for (int i = 0; i != 3; ++i)
  {
    g_controller_interface.UpdateInput();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool WaitFor(const std::function<bool()>& condition)
{
  for (int i = 0; i != 300; ++i)
  {
    g_controller_interface.UpdateInput();
    if (condition())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

ControlState Read(const std::shared_ptr<ciface::Core::Device>& device, const char* input)
{
  ciface::Core::Device::Input* const found = device->FindInput(input);
  if (found == nullptr)
  {
    Check(false, fmt::format("{} has an input named \"{}\"", device->GetQualifiedName(), input));
    return -100;
  }
  return found->GetState();
}

void CheckKeyboardMouse(Window window)
{
  const std::shared_ptr<ciface::Core::Device> device =
      g_controller_interface.FindDevice({"XInput2", 0, "Virtual core pointer"});
  Check(device != nullptr, "device XInput2/0/Virtual core pointer exists");
  if (!device)
    return;
  Check(g_controller_interface.GetDefaultDeviceString() == "XInput2/0/Virtual core pointer",
        "it is the default device (" + g_controller_interface.GetDefaultDeviceString() + ")");

  // The names the default mappings and Dolphin-style configurations use.
  for (const char* name : {"A",
                           "Z",
                           "1",
                           "Return",
                           "Escape",
                           "Tab",
                           "space",
                           "Up",
                           "Left",
                           "F1",
                           "F10",
                           "KP_Add",
                           "KP_Subtract",
                           "KP_1",
                           "KP_End",
                           "Shift_L",
                           "Control_L",
                           "Alt_L",
                           "Shift",
                           "Ctrl",
                           "Alt",
                           "Click 1",
                           "Click 3",
                           "Click 32",
                           "Cursor X+",
                           "Cursor Y-",
                           "Axis X+",
                           "Axis Z-",
                           "RelativeMouse X+",
                           "RelativeMouse Y-"})
  {
    Check(device->FindInput(name) != nullptr, fmt::format("input \"{}\"", name));
  }

  Pump();
  Check(Read(device, "A") == 0, "A at rest");
  Run("keydown a");
  Pump();
  Check(Read(device, "A") == 1, "A pressed");
  Run("keyup a");
  Pump();
  Check(Read(device, "A") == 0, "A released");

  Run("keydown Shift_R");
  Pump();
  Check(Read(device, "Shift") == 1 && Read(device, "Shift_R") == 1, "right Shift counts as Shift");
  Run("keyup Shift_R");

  // Cursor: the middle of the window is 0, three quarters across is +0.5.
  const unsigned long id = window;
  Run(fmt::format("mousemove --window {} 160 120", id));
  Pump();
  Check(std::abs(Read(device, "Cursor X+")) < 0.02 && std::abs(Read(device, "Cursor Y+")) < 0.02,
        fmt::format("cursor in the middle reads 0 ({:.3f}, {:.3f})", Read(device, "Cursor X+"),
                    Read(device, "Cursor Y+")));
  Run(fmt::format("mousemove --window {} 240 60", id));
  Pump();
  Check(std::abs(Read(device, "Cursor X+") - 0.5) < 0.02 &&
            std::abs(Read(device, "Cursor Y-") - 0.5) < 0.02,
        fmt::format("cursor at (3/4, 1/4) reads X+ 0.5, Y- 0.5 ({:.3f}, {:.3f})",
                    Read(device, "Cursor X+"), Read(device, "Cursor Y-")));

  Run("mousedown 1");
  Pump();
  Check(Read(device, "Click 1") == 1, "left button");
  Run("mouseup 1");
  Run("mousedown 3");
  Pump();
  Check(Read(device, "Click 1") == 0 && Read(device, "Click 3") == 1, "right button alone");
  Run("mouseup 3");

  // Movement: read in the same update the events arrive in.
  Pump();
  Run("mousemove_relative -- 24 0");
  g_controller_interface.UpdateInput();
  const ControlState axis = Read(device, "Axis X+");
  const ControlState relative = Read(device, "RelativeMouse X+");
  Check(axis > 0.5 && relative > 1, fmt::format("mouse moved right: Axis X+ {:.2f}, "
                                                "RelativeMouse X+ {:.2f}",
                                                axis, relative));
  Pump();
  Run("click 4");
  g_controller_interface.UpdateInput();
  Check(Read(device, "Axis Z+") > 0.2,
        fmt::format("wheel up: Axis Z+ {:.2f}", Read(device, "Axis Z+")));
  Pump();
  Check(Read(device, "Axis Z+") < 0.2 && Read(device, "Axis X+") < 0.2, "movement settles");

  // The default hotkeys (on this device): Escape stops, Alt+Return goes full screen.
  HotkeyManagerEmu::Enable(true);
  Run("keydown Escape");
  Pump();
  HotkeyManagerEmu::GetStatus(false);
  Check(HotkeyManagerEmu::IsPressed(HK_STOP, true), "hotkey Stop (Escape)");
  Run("keyup Escape");
  Run("keydown alt+Return");
  Pump();
  HotkeyManagerEmu::GetStatus(false);
  Check(HotkeyManagerEmu::IsPressed(HK_FULLSCREEN, true), "hotkey Full screen (Alt+Return)");
  Run("keyup alt+Return");
}

bool OnRumble(void*, Uint16 low, Uint16 high)
{
  s_rumble_low = low;
  s_rumble_high = high;
  return true;
}

void CheckGamepad()
{
  for (int i = 0; i != 300 && SDL_WasInit(SDL_INIT_JOYSTICK) == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  Check(SDL_WasInit(SDL_INIT_JOYSTICK) != 0, "SDL joystick subsystem started");

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
  desc.name = "Virtual test gamepad";
  desc.Rumble = OnRumble;
  const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
  Check(id != 0, fmt::format("SDL virtual gamepad attached ({})", SDL_GetError()));
  SDL_Joystick* const joystick = SDL_OpenJoystick(id);
  if (joystick == nullptr)
    return;

  std::shared_ptr<ciface::Core::Device> pad;
  Check(WaitFor([&] {
          pad = g_controller_interface.FindDevice({"XInput", 0, "Gamepad"});
          return pad != nullptr;
        }),
        "XInput/0/Gamepad finds the SDL gamepad");
  if (!pad)
    return;
  std::printf("     (it is %s)\n", pad->GetQualifiedName().c_str());
  Check(g_controller_interface.HasConnectedDevice({"XInput", 0, "Gamepad"}),
        "XInput/0/Gamepad counts as connected");
  Check(g_controller_interface.FindDevice({"XInput", 1, "Gamepad"}) == nullptr,
        "XInput/1/Gamepad is not (only one gamepad)");

  // Every input and output name RetroPixels' mappings use.
  for (const char* name :
       {"Button A",   "Button B",   "Button X", "Button Y", "Start",     "Back",
        "Shoulder L", "Shoulder R", "Thumb L",  "Thumb R",  "Trigger L", "Trigger R",
        "Left X+",    "Left X-",    "Left Y+",  "Left Y-",  "Right X+",  "Right X-",
        "Right Y+",   "Right Y-",   "Pad N",    "Pad S",    "Pad W",     "Pad E"})
  {
    Check(pad->FindInput(name) != nullptr, fmt::format("gamepad input \"{}\"", name));
  }
  for (const char* name : {"Motor L", "Motor R"})
    Check(pad->FindOutput(name) != nullptr, fmt::format("gamepad output \"{}\"", name));

  // The emulated GameCube controller 1, through the INI lines RetroPixels writes.
  Pad::LoadConfig();
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  WaitFor([] { return true; });
  GCPadStatus status = Pad::GetStatus(0);
  Check(status.isConnected && (status.button & PAD_BUTTON_A) == 0 && status.stickX == 0x80,
        fmt::format("GameCube pad 1 at rest (buttons {:04x}, stick x {:02x})", status.button,
                    status.stickX));

  SDL_SetJoystickVirtualButton(joystick, SDL_GAMEPAD_BUTTON_SOUTH, true);
  SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MAX);
  SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
  Check(WaitFor([&] {
          status = Pad::GetStatus(0);
          return (status.button & PAD_BUTTON_A) != 0 && status.stickX > 0xf0 &&
                 status.triggerLeft > 0xf0;
        }),
        fmt::format("pad A, main stick right, L pressed (buttons {:04x}, stick x {:02x}, L {:02x})",
                    status.button, status.stickX, status.triggerLeft));

  Pad::Rumble(0, 1.0);
  Check(
      WaitFor([] { return s_rumble_low > 30000 && s_rumble_high > 30000; }),
      fmt::format("rumble reaches the gamepad ({} {})", s_rumble_low.load(), s_rumble_high.load()));
  Pad::Rumble(0, 0.0);
  ControllerInterface::SetCurrentInputChannel(ciface::InputChannel::Host);

  SDL_CloseJoystick(joystick);
  SDL_DetachVirtualJoystick(id);
  Check(WaitFor([] { return !g_controller_interface.FindDevice({"XInput", 0, "Gamepad"}); }),
        "unplugged gamepad is gone");
}

// RetroPixels' GCPadMap (internal/dolphin/config.go), as written into GCPadNew.ini.
void WriteRetroPixelsPadConfig()
{
  Common::IniFile ini;
  Common::IniFile::Section* const pad = ini.GetOrCreateSection("GCPad1");
  const std::vector<std::pair<const char*, const char*>> lines = {
      {"Device", "XInput/0/Gamepad"},
      {"Buttons/A", "`Button A`"},
      {"Buttons/B", "`Button X`"},
      {"Buttons/X", "`Button B`"},
      {"Buttons/Y", "`Button Y`"},
      {"Buttons/Z", "`Shoulder R`"},
      {"Buttons/Start", "Start"},
      {"Main Stick/Up", "`Left Y+`"},
      {"Main Stick/Down", "`Left Y-`"},
      {"Main Stick/Left", "`Left X-`"},
      {"Main Stick/Right", "`Left X+`"},
      {"C-Stick/Up", "`Right Y+`"},
      {"C-Stick/Down", "`Right Y-`"},
      {"C-Stick/Left", "`Right X-`"},
      {"C-Stick/Right", "`Right X+`"},
      {"Triggers/L", "`Trigger L`"},
      {"Triggers/R", "`Trigger R`"},
      {"Triggers/L-Analog", "`Trigger L`"},
      {"Triggers/R-Analog", "`Trigger R`"},
      {"D-Pad/Up", "`Pad N`"},
      {"D-Pad/Down", "`Pad S`"},
      {"D-Pad/Left", "`Pad W`"},
      {"D-Pad/Right", "`Pad E`"},
      {"Rumble/Motor", "`Motor L`|`Motor R`"},
  };
  for (const auto& [key, value] : lines)
    pad->Set(key, std::string(value));
  ini.Save(File::GetUserPath(D_CONFIG_IDX) + "GCPadNew.ini");
}
}  // namespace

// The frontend calls the core makes; the window counts as focused.
std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}
bool Host_UIBlocksControllerState()
{
  return false;
}
bool Host_RendererHasFocus()
{
  return true;
}
bool Host_RendererHasFullFocus()
{
  return true;
}
bool Host_RendererIsFullscreen()
{
  return false;
}
bool Host_TASInputHasFocus()
{
  return false;
}
void Host_Message(HostMessageID)
{
}
void Host_PPCSymbolsChanged()
{
}
void Host_PPCBreakpointsChanged()
{
}
void Host_RequestRenderWindowSize(int, int)
{
}
void Host_UpdateDisasmDialog()
{
}
void Host_JitCacheInvalidation()
{
}
void Host_JitProfileDataWiped()
{
}
void Host_UpdateTitle(const std::string&)
{
}
void Host_YieldToUI()
{
}
void Host_TitleChanged()
{
}
void Host_UpdateDiscordClientID(const std::string&)
{
}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   const int64_t, const int64_t, const int, const int)
{
  return false;
}
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::fprintf(stderr, "usage: %s <empty user folder>\n", argv[0]);
    return 2;
  }

  Display* const display = XOpenDisplay(nullptr);
  if (display == nullptr)
  {
    std::fprintf(stderr, "no X display (DISPLAY not set?)\n");
    return 2;
  }
  const Window window =
      XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 320, 240, 0, 0, 0);
  XMapWindow(display, window);
  XSync(display, False);

  UICommon::SetUserDirectory(argv[1]);
  UICommon::CreateDirectories();
  UICommon::Init();
  WriteRetroPixelsPadConfig();

  const WindowSystemInfo wsi(WindowSystemType::X11, display, reinterpret_cast<void*>(window),
                             reinterpret_cast<void*>(window));
  UICommon::InitControllers(wsi);

  std::printf("devices:\n");
  for (const std::string& name : g_controller_interface.GetAllDeviceStrings())
    std::printf("     %s\n", name.c_str());

  CheckKeyboardMouse(window);
  CheckGamepad();

  // The window changing (as on docking) keeps the device and measures in the new window.
  const Window other =
      XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 100, 100, 0, 0, 0);
  XMapWindow(display, other);
  XSync(display, False);
  g_controller_interface.ChangeWindow(reinterpret_cast<void*>(other));
  Run(fmt::format("mousemove --window {} 75 50", other));
  Pump();
  const auto device = g_controller_interface.FindDevice({"XInput2", 0, "Virtual core pointer"});
  Check(device && std::abs(Read(device, "Cursor X+") - 0.5) < 0.03,
        "after a window change the cursor is measured in the new window");
  // ... and a window that disappears is no error.
  XDestroyWindow(display, other);
  XSync(display, False);
  Pump();
  Check(true, "render window destroyed under the input backend");

  UICommon::ShutdownControllers();
  UICommon::Shutdown();
  XCloseDisplay(display);

  std::printf("%s: %d failure(s)\n", s_failures == 0 ? "passed" : "FAILED", s_failures);
  return s_failures == 0 ? 0 : 1;
}
