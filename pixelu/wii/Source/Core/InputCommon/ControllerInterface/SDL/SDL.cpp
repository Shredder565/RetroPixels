// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/SDL/SDL.h"

#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <SDL3/SDL.h>

#include "Common/Event.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/SDL/SDLGamepad.h"

namespace ciface::SDL
{
namespace
{
// SDL's log categories, as shown in our log.
std::string_view CategoryLabel(int category)
{
  switch (category)
  {
  case SDL_LOG_CATEGORY_APPLICATION:
    return "app";
  case SDL_LOG_CATEGORY_ERROR:
    return "error";
  case SDL_LOG_CATEGORY_ASSERT:
    return "assert";
  case SDL_LOG_CATEGORY_SYSTEM:
    return "system";
  case SDL_LOG_CATEGORY_AUDIO:
    return "audio";
  case SDL_LOG_CATEGORY_VIDEO:
    return "video";
  case SDL_LOG_CATEGORY_RENDER:
    return "render";
  case SDL_LOG_CATEGORY_INPUT:
    return "input";
  case SDL_LOG_CATEGORY_TEST:
    return "test";
  case SDL_LOG_CATEGORY_GPU:
    return "gpu";
  default:
    return {};
  }
}

Common::Log::LogLevel LevelFor(SDL_LogPriority priority)
{
  switch (priority)
  {
  case SDL_LOG_PRIORITY_VERBOSE:
  case SDL_LOG_PRIORITY_DEBUG:
    return Common::Log::LogLevel::LDEBUG;
  case SDL_LOG_PRIORITY_INFO:
    return Common::Log::LogLevel::LINFO;
  case SDL_LOG_PRIORITY_WARN:
    return Common::Log::LogLevel::LWARNING;
  case SDL_LOG_PRIORITY_ERROR:
    return Common::Log::LogLevel::LERROR;
  default:
    return Common::Log::LogLevel::LNOTICE;
  }
}

void ForwardSDLLog(void*, int category, SDL_LogPriority priority, const char* message)
{
  const Common::Log::LogLevel level = LevelFor(priority);
  const std::string_view label = CategoryLabel(category);
  if (label.empty())
  {
    GENERIC_LOG_FMT(Common::Log::LogType::CONTROLLERINTERFACE, level, "unknown({}): {}", category,
                    message);
  }
  else
  {
    GENERIC_LOG_FMT(Common::Log::LogType::CONTROLLERINTERFACE, level, "{}: {}", label, message);
  }
}

// Gives an SDL hint setting its default value unless the user set it.
void DefaultHint(const Config::Info<std::string>& hint, const char* value)
{
  if (Config::Get(hint).empty())
    Config::SetBase(hint, value);
}

bool IsGameCubeAdapterInUse()
{
  for (int channel = 0; channel != SerialInterface::MAX_SI_CHANNELS; ++channel)
  {
    if (Config::Get(Config::GetInfoForSIDevice(channel)) == SerialInterface::SIDEVICE_WIIU_ADAPTER)
      return true;
  }
  return false;
}

class InputBackend final : public ciface::InputBackend
{
public:
  explicit InputBackend(ControllerInterface* controller_interface);
  ~InputBackend() override;
  void PopulateDevices() override;
  void UpdateInput(std::vector<std::weak_ptr<ciface::Core::Device>>& devices_to_remove) override;

private:
  void ApplyHints();
  void EventThread();
  bool StartSDL();
  void Post(Uint32 type);

  void AddJoystick(SDL_JoystickID instance_id);
  // Returns false once the stop event arrives.
  bool Handle(const SDL_Event& event);

  Common::Event m_started;
  Uint32 m_stop_event = 0;
  Uint32 m_populate_event = 0;
  std::thread m_thread;
};

InputBackend::InputBackend(ControllerInterface* controller_interface)
    : ciface::InputBackend(controller_interface)
{
  SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
  SDL_SetLogOutputFunction(ForwardSDLLog, nullptr);

  ApplyHints();

  m_thread = std::thread([this] { EventThread(); });
  m_started.Wait();
}

void InputBackend::ApplyHints()
{
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
  // Windows.Gaming.Input devices come from our own WGInput backend; SDL's would be duplicates.
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_WGI, "0");
  // The DualSense's light bar shows the player; its player LEDs stay off.
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
  // SDL's DirectInput support stays on: turning it off avoids rare shutdown hangs with some
  // 8BitDo pads, but breaks DualSense and DualShock 4 hotplug.
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_DIRECTINPUT, "1");
  // Spelled out so they show up in the configuration and are easy to change.
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
  DefaultHint(Config::MAIN_SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0");

  // A GameCube adapter in use by the SI devices is left to our own driver. SDL reads this hint
  // only when its joystick subsystem starts, so later configuration changes do not apply.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, IsGameCubeAdapterInUse() ? "0" : "1");

  // Every hint the user put in the SDL_Hints section.
  std::shared_ptr<Config::Layer> base_layer = Config::GetLayer(Config::LayerType::Base);
  const Config::Section& hints = base_layer->GetSection(Config::System::Main, "SDL_Hints");
  for (const auto& [location, value] : hints)
  {
    if (value)
      SDL_SetHint(location.key.c_str(), value->c_str());
  }
}

bool InputBackend::StartSDL()
{
  if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_GAMEPAD))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "SDL failed to initialize");
    return false;
  }

  const Uint32 first_event = SDL_RegisterEvents(2);
  if (first_event == static_cast<Uint32>(-1))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "SDL failed to register custom events");
    return false;
  }
  m_stop_event = first_event;
  m_populate_event = first_event + 1;

  // Handle the joysticks already present before the constructor returns. Otherwise their added
  // events (and a populate request) would be handled after ControllerInterface's first refresh
  // cleared the list, and the devices would show up twice. Adding them here is refused anyway,
  // as ControllerInterface is not initialised yet.
  SDL_Event event;
  while (SDL_PollEvent(&event))
  {
    if (!Handle(event))
      return false;
  }
  return true;
}

void InputBackend::EventThread()
{
  Common::SetCurrentThreadName("SDL Hotplug Thread");

  // SDL is shut down by the thread that started it, however the thread ends.
  Common::ScopeGuard quit_sdl([] { SDL_Quit(); });

  bool started;
  {
    Common::ScopeGuard release_constructor([this] { m_started.Set(); });
    started = StartSDL();
  }
  if (!started)
    return;

  SDL_Event event;
  while (SDL_WaitEvent(&event))
  {
    if (!Handle(event))
      return;
  }
}

InputBackend::~InputBackend()
{
  if (!m_thread.joinable())
    return;

  Post(m_stop_event);
  m_thread.join();
}

void InputBackend::Post(Uint32 type)
{
  SDL_Event event{type};
  SDL_PushEvent(&event);
}

void InputBackend::PopulateDevices()
{
  if (!m_thread.joinable())
    return;

  // Population happens on the event thread.
  Post(m_populate_event);
}

void InputBackend::UpdateInput(std::vector<std::weak_ptr<ciface::Core::Device>>&)
{
  SDL_UpdateGamepads();
}

void InputBackend::AddJoystick(SDL_JoystickID instance_id)
{
  SDL_Gamepad* const gamepad = SDL_OpenGamepad(instance_id);
  SDL_Joystick* const joystick = SDL_OpenJoystick(instance_id);
  if (joystick == nullptr)
    return;

  // Some non-controller HID devices (webcams among them) are opened as joysticks with absurd
  // numbers of controls; those are skipped.
  constexpr int MAX_CONTROLS = 255;
  if (SDL_GetNumJoystickButtons(joystick) > MAX_CONTROLS ||
      SDL_GetNumJoystickAxes(joystick) > MAX_CONTROLS ||
      SDL_GetNumJoystickHats(joystick) > MAX_CONTROLS ||
      SDL_GetNumJoystickBalls(joystick) > MAX_CONTROLS)
  {
    return;
  }

  auto device = std::make_shared<Gamepad>(gamepad, joystick);
  if (device->Inputs().empty() && device->Outputs().empty())
    return;

  GetControllerInterface().AddDevice(std::move(device));
}

bool InputBackend::Handle(const SDL_Event& event)
{
  if (event.type == SDL_EVENT_JOYSTICK_ADDED)
  {
    AddJoystick(event.jdevice.which);
    return true;
  }

  if (event.type == SDL_EVENT_JOYSTICK_REMOVED)
  {
    const SDL_JoystickID gone = event.jdevice.which;
    GetControllerInterface().RemoveDevice([gone](const Core::Device* device) {
      return device->GetSource() == "SDL" &&
             static_cast<const Gamepad*>(device)->GetSDLInstanceID() == gone;
    });
    return true;
  }

  if (event.type == m_populate_event)
  {
    GetControllerInterface().PlatformPopulateDevices([this] {
      int count = 0;
      SDL_JoystickID* const ids = SDL_GetJoysticks(&count);
      for (const SDL_JoystickID id : std::span(ids, static_cast<size_t>(count)))
        AddJoystick(id);
      SDL_free(ids);
    });
    return true;
  }

  return event.type != m_stop_event;
}
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

}  // namespace ciface::SDL
