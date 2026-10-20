// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/XInput/XInputController.h"

namespace
{
	// XInput's own recommendation is a bit lower, but worn sticks drift more than that
	constexpr float kDefaultDeadzone = 0.15f;

	// names of the wButtons bits; bits 10 and 11 have no button
	constexpr std::string_view kButtonNames[16] = {
		"DPAD-Up", "DPAD-Down", "DPAD-Left", "DPAD-Right",
		"Start", "Select", "L-Stick", "R-Stick",
		"L", "R", {}, {},
		"A", "B", "X", "Y",
	};

	// -32768..32767 onto -1..1, each half scaled by its own range so both ends reach 1
	float StickValue(SHORT v)
	{
		if (v > 0)
			return (float)v / 32767.0f;
		if (v < 0)
			return (float)v / 32768.0f;
		return 0.0f;
	}

	float TriggerValue(BYTE v)
	{
		return (float)v / 255.0f;
	}

	bool IsUsableBattery(const XINPUT_BATTERY_INFORMATION& battery)
	{
		return battery.BatteryType == BATTERY_TYPE_ALKALINE || battery.BatteryType == BATTERY_TYPE_NIMH;
	}
}

XInputController::XInputController(uint32 index)
	: Controller<XInputControllerProvider>(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_index(index)
{
	if (index >= XUSER_MAX_COUNT)
		throw std::runtime_error(fmt::format("invalid xinput index {} (must be smaller than {})", index, XUSER_MAX_COUNT));

	m_settings.axis.deadzone = kDefaultDeadzone;
	m_settings.rotation.deadzone = kDefaultDeadzone;
	m_settings.trigger.deadzone = kDefaultDeadzone;
}

bool XInputController::connect()
{
	if (m_connected)
		return true;

	m_has_battery = false;
	XINPUT_CAPABILITIES caps{};
	if (m_provider->m_XInputGetCapabilities(m_index, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_DEVICE_NOT_CONNECTED)
		return false;
	m_connected = true;

	m_has_rumble = caps.Vibration.wLeftMotorSpeed > 0 || caps.Vibration.wRightMotorSpeed > 0;

	XINPUT_BATTERY_INFORMATION battery{};
	if (m_provider->m_XInputGetBatteryInformation
		&& m_provider->m_XInputGetBatteryInformation(m_index, BATTERY_DEVTYPE_GAMEPAD, &battery) == ERROR_SUCCESS)
		m_has_battery = IsUsableBattery(battery);
	return true;
}

bool XInputController::is_connected()
{
	return m_connected;
}

void XInputController::start_rumble()
{
	const float strength = m_settings.rumble;
	if (!has_rumble() || strength <= 0)
		return;
	const WORD speed = static_cast<WORD>(strength * 65535.0f);
	XINPUT_VIBRATION vibration{ speed, speed };
	m_provider->m_XInputSetState(m_index, &vibration);
}

void XInputController::stop_rumble()
{
	if (!has_rumble())
		return;
	XINPUT_VIBRATION off{};
	m_provider->m_XInputSetState(m_index, &off);
}

bool XInputController::has_low_battery()
{
	if (!has_battery())
		return false;
	XINPUT_BATTERY_INFORMATION battery{};
	if (m_provider->m_XInputGetBatteryInformation(m_index, BATTERY_DEVTYPE_GAMEPAD, &battery) != ERROR_SUCCESS)
		return false;
	return IsUsableBattery(battery) && battery.BatteryLevel <= BATTERY_LEVEL_LOW;
}

std::string XInputController::get_button_name(uint64 button) const
{
	if (button < std::size(kButtonNames) && !kButtonNames[button].empty())
		return std::string{ kButtonNames[button] };
	return ControllerBase::get_button_name(button);
}

ControllerState XInputController::raw_state()
{
	ControllerState result{};
	if (!m_connected)
		return result;

	XINPUT_STATE input;
	if (m_provider->m_XInputGetState(m_index, &input) != ERROR_SUCCESS)
	{
		m_connected = false;
		return result;
	}

	const XINPUT_GAMEPAD& pad = input.Gamepad;
	for (uint32 bit = 0; bit < 16; ++bit)
	{
		if (pad.wButtons & (1u << bit))
			result.buttons.SetButtonState(bit, true);
	}
	result.axis = { StickValue(pad.sThumbLX), StickValue(pad.sThumbLY) };
	result.rotation = { StickValue(pad.sThumbRX), StickValue(pad.sThumbRY) };
	result.trigger = { TriggerValue(pad.bLeftTrigger), TriggerValue(pad.bRightTrigger) };
	return result;
}
