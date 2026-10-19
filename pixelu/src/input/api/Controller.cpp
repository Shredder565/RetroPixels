// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Controller.h"
#include "config/CemuConfig.h"
#include "WindowSystem.h"

namespace
{
	// distance between a positive direction id and its negative partner
	constexpr uint32 kNegativeOffset = kAxisXN - kAxisXP;

	// a stick that is pushed far enough in a direction also holds that direction's button
	void AddDirectionButtons(ControllerButtonState& buttons, const glm::vec2& value, uint32 positiveX)
	{
		const float components[2] = { value.x, value.y };
		for (uint32 i = 0; i < 2; ++i)
		{
			const uint32 positive = positiveX + i;
			if (components[i] < -ControllerState::kAxisThreshold)
				buttons.SetButtonState(positive + kNegativeOffset, true);
			else if (components[i] > ControllerState::kAxisThreshold)
				buttons.SetButtonState(positive, true);
		}
	}

	// maps v from [-1, rest] or [rest, 1] onto [-1, 0] or [0, 1]
	float Recentre(float v, float rest, float top)
	{
		if (v < rest)
			return (v - rest) / (top + rest);
		return (v - rest) / (top - rest);
	}
}

ControllerBase::ControllerBase(std::string_view uuid, std::string_view display_name)
	: m_uuid{ uuid }, m_display_name{ display_name }
{
}

const ControllerState& ControllerBase::calibrate()
{
	m_default_state = raw_state();
	m_is_calibrated = is_connected();
	return m_default_state;
}

const ControllerState& ControllerBase::update_state()
{
	if (!m_is_calibrated)
		calibrate();

	ControllerState state = raw_state();
	// buttons that were already held at calibration time are stuck or always on
	state.buttons.UnsetButtons(m_default_state.buttons);

	const Settings settings = get_settings();
	apply_axis_setting(state.axis, m_default_state.axis, settings.axis);
	apply_axis_setting(state.rotation, m_default_state.rotation, settings.rotation);
	apply_axis_setting(state.trigger, m_default_state.trigger, settings.trigger);

	AddDirectionButtons(state.buttons, state.axis, kAxisXP);
	AddDirectionButtons(state.buttons, state.rotation, kRotationXP);
	AddDirectionButtons(state.buttons, state.trigger, kTriggerXP);

	// hotkeys and the input settings window look at every state change
	WindowSystem::CaptureInput(state, m_last_state);

	m_last_state = std::move(state);
	return m_last_state;
}

void ControllerBase::apply_axis_setting(glm::vec2& axis, const glm::vec2& default_value, const AxisSetting& setting) const
{
	// a hair above 1 so a fully pushed stick doesn't land exactly on the edge
	constexpr float kTop = 1.0f + ControllerState::kMinAxisValue;

	if (!(setting.deadzone < 1.0f))
	{
		axis = { 0, 0 };
		return;
	}

	axis.x = Recentre(axis.x, default_value.x, kTop);
	axis.y = Recentre(axis.y, default_value.y, kTop);

	if (!(length(axis) >= setting.deadzone))
	{
		axis = { 0, 0 };
		return;
	}

	// scaled radial dead zone: keep the direction, map [deadzone, 1] onto [0, 1]
	axis *= setting.range;
	const float magnitude = length(axis);
	if (magnitude > 0)
	{
		axis = normalize(axis);
		axis *= (magnitude - setting.deadzone) / (kTop - setting.deadzone);
		if (length(axis) > 1.0f)
			axis = normalize(axis);
	}

	if (axis.x != 0 || axis.y != 0)
	{
		if (std::abs(axis.x) < ControllerState::kMinAxisValue)
			axis.x = ControllerState::kMinAxisValue;
		if (std::abs(axis.y) < ControllerState::kMinAxisValue)
			axis.y = ControllerState::kMinAxisValue;
	}
}

bool ControllerBase::operator==(const ControllerBase& c) const
{
	return api() == c.api() && uuid() == c.uuid();
}

float ControllerBase::get_axis_value(uint64 button) const
{
	if (!m_last_state.buttons.GetButtonState(button))
		return 0;
	// plain buttons, and every button of a device without analog axes, are all or nothing
	if (button <= kButtonNoneAxisMAX || !has_axis())
		return 1.0f;

	const ControllerState& s = m_last_state;
	switch (button)
	{
	case kAxisXP: case kAxisXN: return std::abs(s.axis.x);
	case kAxisYP: case kAxisYN: return std::abs(s.axis.y);
	case kRotationXP: case kRotationXN: return std::abs(s.rotation.x);
	case kRotationYP: case kRotationYN: return std::abs(s.rotation.y);
	case kTriggerXP: case kTriggerXN: return std::abs(s.trigger.x);
	case kTriggerYP: case kTriggerYN: return std::abs(s.trigger.y);
	default: return 0;
	}
}

std::string ControllerBase::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kButtonZL: return "ZL";
	case kButtonZR: return "ZR";
	case kButtonUp: return "DPAD-Up";
	case kButtonDown: return "DPAD-Down";
	case kButtonLeft: return "DPAD-Left";
	case kButtonRight: return "DPAD-Right";
	case kAxisXP: return "X-Axis+";
	case kAxisYP: return "Y-Axis+";
	case kAxisXN: return "X-Axis-";
	case kAxisYN: return "Y-Axis-";
	case kRotationXP: return "X-Rotation+";
	case kRotationYP: return "Y-Rotation+";
	case kRotationXN: return "X-Rotation-";
	case kRotationYN: return "Y-Rotation-";
	case kTriggerXP: return "X-Trigger+";
	case kTriggerYP: return "Y-Trigger+";
	case kTriggerXN: return "X-Trigger-";
	case kTriggerYN: return "y-Trigger-"; // lower case y as it always was (profiles and screenshots show it)
	default: return fmt::format("Button {}", button);
	}
}

ControllerBase::Settings ControllerBase::get_settings() const
{
	std::scoped_lock lock(m_settings_mutex);
	return m_settings;
}

void ControllerBase::set_settings(const Settings& settings)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings = settings;
}

void ControllerBase::set_axis_settings(const AxisSetting& settings)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings.axis = settings;
}

void ControllerBase::set_rotation_settings(const AxisSetting& settings)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings.rotation = settings;
}

void ControllerBase::set_trigger_settings(const AxisSetting& settings)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings.trigger = settings;
}

void ControllerBase::set_rumble(float rumble)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings.rumble = rumble;
}

void ControllerBase::set_use_motion(bool state)
{
	std::scoped_lock lock(m_settings_mutex);
	m_settings.motion = state;
}
