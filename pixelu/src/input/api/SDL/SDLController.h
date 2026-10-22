// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// One SDL gamepad. It is identified by its SDL GUID plus its position among the
// connected gamepads with the same GUID, so two identical pads keep their order.
// Button ids are SDL_GamepadButton values.

#include "input/api/Controller.h"
#include "input/api/SDL/SDLControllerProvider.h"

#include <SDL3/SDL_gamepad.h>

class SDLController : public Controller<SDLControllerProvider>
{
public:
	SDLController(const SDL_GUID& guid, size_t guid_index);
	SDLController(const SDL_GUID& guid, size_t guid_index, std::string_view display_name);

	~SDLController() override;

	std::string_view api_name() const override { return to_string(InputAPI::SDLController); } // "SDLController"
	InputAPI::Type api() const override { return InputAPI::SDLController; }

	bool is_connected() override;
	bool connect() override;

	bool has_motion() override { return m_has_gyro && m_has_accel; }
	bool has_rumble() override { return m_has_rumble; }

	void start_rumble() override;
	void stop_rumble() override;

	MotionSample get_motion_sample() override;

	std::string get_button_name(uint64 button) const override;
	const SDL_GUID& get_guid() const { return m_guid; }

	// Nintendo Switch pads, which get their own default layouts
	constexpr static SDL_GUID kLeftJoyCon{ 0x03, 0x00, 0x00, 0x00, 0x7e, 0x05, 0x00, 0x00, 0x06, 0x20, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00 };
	constexpr static SDL_GUID kRightJoyCon{ 0x03, 0x00, 0x00, 0x00, 0x7e, 0x05, 0x00, 0x00, 0x07, 0x20, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00 };
	constexpr static SDL_GUID kSwitchProController{ 0x03, 0x00, 0x00, 0x00, 0x7e, 0x05, 0x00, 0x00, 0x09, 0x20, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00 };

protected:
	ControllerState raw_state() override;

private:
	size_t m_guid_index;
	SDL_GUID m_guid;

	std::recursive_mutex m_controller_mutex;
	SDL_Gamepad* m_controller = nullptr; // open while connected
	SDL_JoystickID m_diid = -1;

	bool m_has_gyro = false;
	bool m_has_accel = false;
	bool m_has_rumble = false;

	// which buttons and axes this pad has (read once on connect)
	std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> m_buttons{};
	std::array<bool, SDL_GAMEPAD_AXIS_COUNT> m_axis{};
};
