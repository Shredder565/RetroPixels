// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Game controllers through SDL3's gamepad API. SDL runs on its own event thread
// (on macOS the app's main loop pumps the events instead); that thread also turns the
// gamepads' accelerometer and gyro events into motion samples.

#include <SDL3/SDL_joystick.h>
#include "input/motion/MotionHandler.h"
#include "input/api/ControllerProvider.h"

inline bool operator==(const SDL_GUID& a, const SDL_GUID& b)
{
	return memcmp(&a, &b, sizeof(SDL_GUID)) == 0;
}

class SDLControllerProvider : public ControllerProviderBase
{
	friend class SDLController;
public:
	SDLControllerProvider();
	~SDLControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::SDLController;
	InputAPI::Type api() const override { return kAPIType; }

	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	// position in SDL's gamepad list of the guid_index-th gamepad with this GUID, or -1
	int get_index(size_t guid_index, const SDL_GUID& guid) const;

	// latest fused motion of a gamepad
	MotionSample motion_sample(SDL_JoystickID diid);

#if BOOST_OS_MACOS
	// SDL has to run on the main thread here; the app calls these itself
	static void InitSDL();
	static void ShutdownSDL();
	static void PumpSDLEvents();
#endif

private:
	void event_thread();
	static void HandleSDLEvent(union SDL_Event& event);
#if !BOOST_OS_MACOS
	static void InitSDL();
	static void ShutdownSDL();
#endif

	// SDL is process wide, so is everything here; the first provider starts the event
	// thread and the last one stops it
	inline static std::atomic_int s_initCount{ 0 };
	inline static std::shared_mutex s_mutex;
	inline static std::atomic_bool s_running = false;
	inline static std::thread s_thread;

	// sensor readings waiting to be fused (timestamps in ns)
	struct SensorPair
	{
		uint64 accelTime{};
		uint64 gyroTime{};
		uint64 fusedTime{};
		bool haveAccel{};
		bool haveGyro{};
		glm::vec3 accel{}; // in g
		glm::vec3 gyro{};  // in rad/s
	};

	struct MotionState
	{
		WiiUMotionHandler handler;
		MotionSample data;
		SensorPair pending;
	};

	inline static std::unordered_map<SDL_JoystickID, MotionState> s_motion_states{};
};
