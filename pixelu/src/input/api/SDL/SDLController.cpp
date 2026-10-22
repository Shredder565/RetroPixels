// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/SDL/SDLController.h"

#include "input/api/SDL/SDLControllerProvider.h"

namespace
{
	// "<guid_index>_<guid>"
	std::string MakeUuid(const SDL_GUID& guid, size_t guid_index)
	{
		char text[64];
		SDL_GUIDToString(guid, text, sizeof(text));
		return fmt::format("{}_{}", guid_index, text);
	}

	// SDL axes are -32768..32767, the triggers 0..32767
	float AxisValue(SDL_Gamepad* pad, SDL_GamepadAxis axis)
	{
		return (float)SDL_GetGamepadAxis(pad, axis) / 32767.0f;
	}
}

SDLController::SDLController(const SDL_GUID& guid, size_t guid_index)
	: SDLController(guid, guid_index, fmt::format("Controller {}", guid_index + 1))
{
}

SDLController::SDLController(const SDL_GUID& guid, size_t guid_index, std::string_view display_name)
	: Controller<SDLControllerProvider>(MakeUuid(guid, guid_index), display_name), m_guid_index(guid_index), m_guid(guid)
{
}

SDLController::~SDLController()
{
	if (m_controller)
	{
		SDL_CloseGamepad(m_controller);
		m_controller = nullptr;
	}
}

bool SDLController::is_connected()
{
	std::scoped_lock lock(m_controller_mutex);
	if (!m_controller)
		return false;
	if (SDL_GamepadConnected(m_controller))
		return true;
	// unplugged: let go of it so connect() can open it again
	SDL_CloseGamepad(m_controller);
	m_controller = nullptr;
	return false;
}

bool SDLController::connect()
{
	if (is_connected())
		return true;

	m_has_rumble = false;
	const int index = m_provider->get_index(m_guid_index, m_guid);

	std::scoped_lock lock(m_controller_mutex);
	{
		int count = 0;
		SDL_JoystickID* ids = SDL_GetGamepads(&count);
		const bool found = ids && index >= 0 && index < count;
		if (found)
			m_diid = ids[index];
		SDL_free(ids);
		if (!found)
			return false;
	}

	m_controller = SDL_OpenGamepad(m_diid);
	if (!m_controller)
		return false;

	if (const char* name = SDL_GetGamepadName(m_controller))
		m_display_name = name;

	for (size_t i = 0; i < m_buttons.size(); ++i)
		m_buttons[i] = SDL_GamepadHasButton(m_controller, (SDL_GamepadButton)i);
	for (size_t i = 0; i < m_axis.size(); ++i)
		m_axis[i] = SDL_GamepadHasAxis(m_controller, (SDL_GamepadAxis)i);

	if (SDL_GamepadHasSensor(m_controller, SDL_SENSOR_ACCEL))
		m_has_accel = SDL_SetGamepadSensorEnabled(m_controller, SDL_SENSOR_ACCEL, true);
	if (SDL_GamepadHasSensor(m_controller, SDL_SENSOR_GYRO))
		m_has_gyro = SDL_SetGamepadSensorEnabled(m_controller, SDL_SENSOR_GYRO, true);
	// a zero strength rumble tells whether the pad can rumble at all
	m_has_rumble = SDL_RumbleGamepad(m_controller, 0, 0, 0);
	return true;
}

void SDLController::start_rumble()
{
	std::scoped_lock lock(m_controller_mutex);
	if (!is_connected() || !m_has_rumble)
		return;
	const float strength = m_settings.rumble;
	if (strength <= 0)
		return;
	const Uint16 motor = (Uint16)(strength * 0xFFFF);
	SDL_RumbleGamepad(m_controller, motor, motor, 5000); // stop_rumble() normally ends it earlier
}

void SDLController::stop_rumble()
{
	std::scoped_lock lock(m_controller_mutex);
	if (!is_connected() || !m_has_rumble)
		return;
	SDL_RumbleGamepad(m_controller, 0, 0, 0);
}

MotionSample SDLController::get_motion_sample()
{
	if (!is_connected() || !has_motion())
		return {};
	return m_provider->motion_sample(m_diid);
}

std::string SDLController::get_button_name(uint64 button) const
{
	if (const char* name = SDL_GetGamepadStringForButton((SDL_GamepadButton)button))
		return name;
	return ControllerBase::get_button_name(button);
}

ControllerState SDLController::raw_state()
{
	ControllerState state{};
	std::scoped_lock lock(m_controller_mutex);
	if (!is_connected())
		return state;

	for (size_t i = 0; i < m_buttons.size(); ++i)
	{
		if (m_buttons[i] && SDL_GetGamepadButton(m_controller, (SDL_GamepadButton)i))
			state.buttons.SetButtonState((uint32)i, true);
	}

	const auto read = [this](SDL_GamepadAxis axis, float& out)
	{
		if (m_axis[axis])
			out = AxisValue(m_controller, axis);
	};
	read(SDL_GAMEPAD_AXIS_LEFTX, state.axis.x);
	read(SDL_GAMEPAD_AXIS_LEFTY, state.axis.y);
	read(SDL_GAMEPAD_AXIS_RIGHTX, state.rotation.x);
	read(SDL_GAMEPAD_AXIS_RIGHTY, state.rotation.y);
	read(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, state.trigger.x);
	read(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, state.trigger.y);
	return state;
}
