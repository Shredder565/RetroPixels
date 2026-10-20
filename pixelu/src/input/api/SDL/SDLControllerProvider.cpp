// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/SDL/SDLControllerProvider.h"

#include "input/api/SDL/SDLController.h"

#include <SDL3/SDL.h>

namespace
{
	// SDL's joystick list must not change while we walk it
	struct JoystickListLock
	{
		JoystickListLock() { SDL_LockJoysticks(); }
		~JoystickListLock() { SDL_UnlockJoysticks(); }
		JoystickListLock(const JoystickListLock&) = delete;
		JoystickListLock& operator=(const JoystickListLock&) = delete;
	};

	// SDL_GetGamepads() result, freed with SDL_free
	struct GamepadList
	{
		int count = 0;
		SDL_JoystickID* ids = SDL_GetGamepads(&count);
		~GamepadList() { SDL_free(ids); }
		GamepadList() = default;
		GamepadList(const GamepadList&) = delete;
		GamepadList& operator=(const GamepadList&) = delete;
		std::span<const SDL_JoystickID> span() const { return ids ? std::span<const SDL_JoystickID>(ids, (size_t)count) : std::span<const SDL_JoystickID>{}; }
	};

	struct GuidKey
	{
		SDL_GUID guid;
		bool operator<(const GuidKey& o) const { return memcmp(&guid, &o.guid, sizeof(SDL_GUID)) < 0; }
	};

	constexpr float kStandardGravity = 9.81f; // SDL reports m/s^2
	// a gap this long (10 s) means the sensor stream restarted
	constexpr uint64 kSensorGapNs = 10'000'000'000ull;

	constexpr const char* kHintsOn[] = {
		SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
		SDL_HINT_JOYSTICK_ENHANCED_REPORTS,
		SDL_HINT_JOYSTICK_HIDAPI_PS4,
		SDL_HINT_JOYSTICK_HIDAPI_PS5,
		SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE,
		SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
		SDL_HINT_JOYSTICK_HIDAPI_SWITCH2,
		SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS,
		SDL_HINT_JOYSTICK_HIDAPI_STADIA,
		SDL_HINT_JOYSTICK_HIDAPI_STEAM,
		SDL_HINT_JOYSTICK_HIDAPI_LUNA,
	};
}

SDLControllerProvider::SDLControllerProvider()
{
#if !BOOST_OS_MACOS
	std::scoped_lock lock(s_mutex);
	if (s_initCount.fetch_add(1) == 0)
	{
		s_running = true;
		s_thread = std::thread(&SDLControllerProvider::event_thread, this);
	}
#endif
}

SDLControllerProvider::~SDLControllerProvider()
{
#if !BOOST_OS_MACOS
	bool last = false;
	{
		std::scoped_lock lock(s_mutex);
		if (s_initCount.fetch_sub(1) == 1)
		{
			cemu_assert_debug(s_running);
			s_running = false;
			last = true;
		}
	}
	if (!last)
		return;

	// the event thread may be blocked in SDL_WaitEvent
	SDL_Event quit;
	SDL_zero(quit);
	quit.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&quit);
	if (s_thread.joinable())
		s_thread.join();
#endif
}

std::vector<std::shared_ptr<ControllerBase>> SDLControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> result;
	std::map<GuidKey, size_t> seen; // how many gamepads of each GUID came before

	JoystickListLock lock;
	const GamepadList gamepads;
	for (const SDL_JoystickID id : gamepads.span())
	{
		const SDL_GUID guid = SDL_GetGamepadGUIDForID(id);
		const size_t guid_index = seen[GuidKey{ guid }]++;
		if (const char* name = SDL_GetGamepadNameForID(id))
			result.emplace_back(std::make_shared<SDLController>(guid, guid_index, name));
		else
			result.emplace_back(std::make_shared<SDLController>(guid, guid_index));
	}
	return result;
}

int SDLControllerProvider::get_index(size_t guid_index, const SDL_GUID& guid) const
{
	JoystickListLock lock;
	const GamepadList gamepads;
	const auto ids = gamepads.span();
	size_t matches = 0;
	for (size_t i = 0; i < ids.size(); ++i)
	{
		if (!(SDL_GetGamepadGUIDForID(ids[i]) == guid))
			continue;
		if (matches++ == guid_index)
			return (int)i;
	}
	return -1;
}

MotionSample SDLControllerProvider::motion_sample(SDL_JoystickID diid)
{
	std::shared_lock lock(s_mutex);
	const auto it = s_motion_states.find(diid);
	if (it == s_motion_states.end())
		return {};
	return it->second.data;
}

void SDLControllerProvider::InitSDL()
{
	for (const char* hint : kHintsOn)
		SDL_SetHint(hint, "1");

	if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC))
		throw std::runtime_error(fmt::format("couldn't initialize SDL: {}", SDL_GetError()));

	SDL_SetGamepadEventsEnabled(true);
	if (!SDL_GamepadEventsEnabled())
		cemuLog_log(LogType::Force, "Couldn't enable SDL gamecontroller event polling: {}", SDL_GetError());
}

void SDLControllerProvider::ShutdownSDL()
{
	SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC);
}

#if BOOST_OS_MACOS
void SDLControllerProvider::PumpSDLEvents()
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
		HandleSDLEvent(event);
}
#endif

void SDLControllerProvider::HandleSDLEvent(SDL_Event& event)
{
	switch (event.type)
	{
	case SDL_EVENT_QUIT:
	{
		std::scoped_lock lock(s_mutex);
		s_running = false;
		return;
	}
	case SDL_EVENT_GAMEPAD_ADDED:
	{
		std::scoped_lock lock(s_mutex);
		InputManager::instance().on_device_changed();
		return;
	}
	case SDL_EVENT_GAMEPAD_REMOVED:
	{
		std::scoped_lock lock(s_mutex);
		InputManager::instance().on_device_changed();
		s_motion_states.erase(event.gdevice.which);
		return;
	}
	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
		break;
	default:
		// buttons and axes are polled, not taken from events
		return;
	}

	const SDL_GamepadSensorEvent& sensor = event.gsensor;
	const uint64 time = sensor.timestamp;
	std::scoped_lock lock(s_mutex);
	MotionState& state = s_motion_states[sensor.which];
	SensorPair& pending = state.pending;

	// keeps one reading of each sensor; returns false to drop this event
	const auto accept = [&pending, time](uint64& last) -> bool
	{
		const uint64 elapsed = time - last;
		if (elapsed == 0)
			return false;
		last = time;
		if (elapsed >= kSensorGapNs)
		{
			pending.haveAccel = false;
			pending.haveGyro = false;
			return false;
		}
		return true;
	};

	if (sensor.sensor == SDL_SENSOR_ACCEL)
	{
		if (!accept(pending.accelTime))
			return;
		pending.accel = { -sensor.data[0] / kStandardGravity, -sensor.data[1] / kStandardGravity, -sensor.data[2] / kStandardGravity };
		pending.haveAccel = true;
	}
	if (sensor.sensor == SDL_SENSOR_GYRO)
	{
		if (!accept(pending.gyroTime))
			return;
		pending.gyro = { sensor.data[0], -sensor.data[1], -sensor.data[2] };
		pending.haveGyro = true;
	}

	if (!pending.haveAccel || !pending.haveGyro)
		return;

	// one fused step per accelerometer + gyro pair
	const uint64 newest = std::max(pending.gyroTime, pending.accelTime);
	if (newest > pending.fusedTime)
	{
		const float dt = std::min((float)(newest - pending.fusedTime) / 1000000000.0f, 1.0f);
		pending.fusedTime = newest;
		state.handler.processMotionSample(dt, pending.gyro.x, pending.gyro.y, pending.gyro.z, pending.accel.x, -pending.accel.y, -pending.accel.z);
		state.data = state.handler.getMotionSample();
	}
	pending.haveAccel = false;
	pending.haveGyro = false;
}

void SDLControllerProvider::event_thread()
{
#if BOOST_OS_MACOS
	cemu_assert(false);
#endif
	SetThreadName("SDL_events");
	InitSDL();
	while (s_running.load(std::memory_order_relaxed))
	{
		SDL_Event event{};
		SDL_WaitEvent(&event);
		HandleSDLEvent(event);
	}
	ShutdownSDL();
}
