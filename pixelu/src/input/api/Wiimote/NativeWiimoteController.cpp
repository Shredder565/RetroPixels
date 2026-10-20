// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Wiimote/NativeWiimoteController.h"

#include "input/api/Wiimote/WiimoteControllerProvider.h"

#include <pugixml.hpp>

namespace
{
	struct NamedButton
	{
		uint64 id;
		std::string_view name;
	};

	constexpr NamedButton kNames[] = {
		{ kWiimoteButton_A, "A" },
		{ kWiimoteButton_B, "B" },
		{ kWiimoteButton_One, "1" },
		{ kWiimoteButton_Two, "2" },
		{ kWiimoteButton_Plus, "+" },
		{ kWiimoteButton_Minus, "-" },
		{ kWiimoteButton_Home, "HOME" },
		{ kWiimoteButton_Up, "UP" },
		{ kWiimoteButton_Down, "DOWN" },
		{ kWiimoteButton_Left, "LEFT" },
		{ kWiimoteButton_Right, "RIGHT" },

		{ kWiimoteButton_C, "C" },
		{ kWiimoteButton_Z, "Z" },

		{ kHighestWiimote + kClassicButton_A, "A" },
		{ kHighestWiimote + kClassicButton_B, "B" },
		{ kHighestWiimote + kClassicButton_Y, "Y" },
		{ kHighestWiimote + kClassicButton_X, "X" },
		{ kHighestWiimote + kClassicButton_Plus, "+" },
		{ kHighestWiimote + kClassicButton_Minus, "-" },
		{ kHighestWiimote + kClassicButton_Home, "HOME" },
		{ kHighestWiimote + kClassicButton_Up, "UP" },
		{ kHighestWiimote + kClassicButton_Down, "DOWN" },
		{ kHighestWiimote + kClassicButton_Left, "LEFT" },
		{ kHighestWiimote + kClassicButton_Right, "RIGHT" },
		{ kHighestWiimote + kClassicButton_L, "L" },
		{ kHighestWiimote + kClassicButton_R, "R" },
		{ kHighestWiimote + kClassicButton_ZL, "ZL" },
		{ kHighestWiimote + kClassicButton_ZR, "ZR" },
	};

	void SetBits(ControllerState& out, uint64 bits, uint32 first_id)
	{
		for (uint32 bit = 0; bit < 64 && (bits >> bit) != 0; ++bit)
		{
			if ((bits >> bit) & 1)
				out.buttons.SetButtonState(first_id + bit, true);
		}
	}
}

NativeWiimoteController::NativeWiimoteController(size_t index)
	: base_type(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_index(index)
{
	m_settings.motion = true; // a remote always has an accelerometer
}

void NativeWiimoteController::save(pugi::xml_node& node)
{
	base_type::save(node);
	auto delay = node.append_child("packet_delay");
	delay.append_child(pugi::node_pcdata).set_value(fmt::format("{}", m_packet_delay).c_str());
}

void NativeWiimoteController::load(const pugi::xml_node& node)
{
	base_type::load(node);
	if (auto delay = node.child("packet_delay"))
		m_packet_delay = ConvertString<uint32>(delay.child_value());
}

bool NativeWiimoteController::connect()
{
	if (is_connected())
		return true;

	// a remote that was never seen: let the provider pick up new devices first
	if (!m_provider->is_registered_device(m_index))
		m_provider->get_controllers();

	if (!m_provider->is_connected(m_index))
		return false;

	m_provider->set_packet_delay(m_index, m_packet_delay);
	m_provider->set_led(m_index, m_player_index);
	return true;
}

bool NativeWiimoteController::is_connected()
{
	if (!m_provider->is_connected(m_index))
		return false;
	m_provider->set_packet_delay(m_index, m_packet_delay);
	return true;
}

void NativeWiimoteController::set_player_index(size_t player_index)
{
	m_player_index = player_index;
	m_provider->set_led(m_index, player_index);
}

NativeWiimoteController::Extension NativeWiimoteController::get_extension() const
{
	const auto state = m_provider->get_state(m_index);
	switch (state.m_extension.index())
	{
	case 1: return Nunchuck; // NunchuckData
	case 2: return Classic;  // ClassicData
	default: return None;
	}
}

bool NativeWiimoteController::is_mpls_attached() const
{
	return m_provider->get_state(m_index).m_motion_plus.has_value();
}

bool NativeWiimoteController::has_position()
{
	const auto state = m_provider->get_state(m_index);
	for (const IRDot& dot : state.ir_camera.dots)
	{
		if (dot.visible)
			return true;
	}
	return false;
}

glm::vec2 NativeWiimoteController::get_position()
{
	return m_provider->get_state(m_index).ir_camera.position;
}

glm::vec2 NativeWiimoteController::get_prev_position()
{
	return m_provider->get_state(m_index).ir_camera.m_prev_position;
}

PositionVisibility NativeWiimoteController::GetPositionVisibility()
{
	return m_provider->get_state(m_index).ir_camera.m_positionVisibility;
}

bool NativeWiimoteController::has_low_battery()
{
	return (m_provider->get_state(m_index).flags & kBatteryEmpty) != 0;
}

void NativeWiimoteController::start_rumble()
{
	// the remote's motor is on or off, so only full strength turns it on
	if (m_settings.rumble >= 1.0f)
		m_provider->set_rumble(m_index, true);
}

void NativeWiimoteController::stop_rumble()
{
	m_provider->set_rumble(m_index, false);
}

MotionSample NativeWiimoteController::get_motion_sample()
{
	return m_provider->get_state(m_index).motion_sample;
}

MotionSample NativeWiimoteController::get_nunchuck_motion_sample() const
{
	const auto state = m_provider->get_state(m_index);
	if (const auto* nunchuk = std::get_if<NunchuckData>(&state.m_extension))
		return nunchuk->motion_sample;
	return {};
}

std::string NativeWiimoteController::get_button_name(uint64 button) const
{
	for (const auto& entry : kNames)
	{
		if (entry.id == button)
			return std::string{ entry.name };
	}
	return base_type::get_button_name(button);
}

uint32 NativeWiimoteController::get_packet_delay()
{
	m_packet_delay = m_provider->get_packet_delay(m_index);
	return m_packet_delay;
}

void NativeWiimoteController::set_packet_delay(uint32 delay)
{
	m_packet_delay = delay;
	m_provider->set_packet_delay(m_index, delay);
}

ControllerState NativeWiimoteController::raw_state()
{
	ControllerState result{};
	if (!is_connected())
		return result;

	const auto state = m_provider->get_state(m_index);
	SetBits(result, state.buttons, 0);

	if (const auto* nunchuk = std::get_if<NunchuckData>(&state.m_extension))
	{
		if (nunchuk->c)
			result.buttons.SetButtonState(kWiimoteButton_C, true);
		if (nunchuk->z)
			result.buttons.SetButtonState(kWiimoteButton_Z, true);
		result.axis = nunchuk->axis;
	}
	else if (const auto* classic = std::get_if<ClassicData>(&state.m_extension))
	{
		// classic buttons sit above the remote's own ids, added to them
		SetBits(result, classic->buttons, kHighestWiimote);
		result.axis = classic->left_axis;
		result.rotation = classic->right_axis;
		result.trigger = classic->trigger;
	}
	return result;
}
