// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/DSU/DSUController.h"

namespace
{
	void CheckIndex(uint32 index)
	{
		if (index >= DSUControllerProvider::kMaxClients)
			throw std::runtime_error(fmt::format("max {} dsu controllers are supported! given index: {}", DSUControllerProvider::kMaxClients, index));
	}

	// the DS4 touchpad is 1920 x 942
	std::optional<glm::vec2> TouchPosition(const DataResponseData& data)
	{
		for (const TouchPoint* point : { &data.tpad1, &data.tpad2 })
		{
			if (point->active)
				return glm::vec2{ (float)point->x / 1920.0f, (float)point->y / 942.0f };
		}
		return std::nullopt;
	}

	bool IsTouched(const DataResponseData& data)
	{
		return data.tpad1.active || data.tpad2.active;
	}

	// 0..255 onto -1..1
	float StickValue(uint8 v)
	{
		return ((float)v / 255.0f) * 2.0f - 1.0f;
	}

	constexpr std::string_view kButtonNames[] = {
		"Share", "Stick L", "Stick R", "Options", "Up", "Right", "Down", "Left",
		"ZL", "ZR", "L", "R", "Triangle", "Circle", "Cross", "Square",
		"Touch",
	};
}

DSUController::DSUController(uint32 index)
	: Controller<DSUControllerProvider>(fmt::format("{}", index), fmt::format("Controller {}", index + 1)), m_index(index)
{
	CheckIndex(index);
}

DSUController::DSUController(uint32 index, const DSUProviderSettings& settings)
	: Controller<DSUControllerProvider>(fmt::format("{}", index), fmt::format("Controller {}", index + 1), settings), m_index(index)
{
	CheckIndex(index);
}

void DSUController::save(pugi::xml_node& node)
{
	Controller<DSUControllerProvider>::save(node);
	const auto& settings = m_provider->get_settings();
	node.append_child("ip").append_child(pugi::node_pcdata).set_value(settings.ip.c_str());
	node.append_child("port").append_child(pugi::node_pcdata).set_value(fmt::format("{}", settings.port).c_str());
}

void DSUController::load(const pugi::xml_node& node)
{
	Controller<DSUControllerProvider>::load(node);

	DSUProviderSettings settings;
	if (const auto value = node.child("ip"))
		settings.ip = value.child_value();
	if (const auto value = node.child("port"))
		settings.port = ConvertString<uint16>(value.child_value());

	// switch to the provider for that server (created if needed)
	update_provider(std::dynamic_pointer_cast<DSUControllerProvider>(InputManager::instance().get_api_provider(api(), settings)));
	connect();
}

bool DSUController::connect()
{
	if (is_connected())
		return true;
	m_provider->request_pad_data((uint8_t)m_index);
	return is_connected();
}

bool DSUController::is_connected()
{
	return m_provider->is_connected((uint8_t)m_index);
}

MotionSample DSUController::get_motion_sample()
{
	return m_provider->get_motion_sample((uint8_t)m_index);
}

bool DSUController::has_position()
{
	return IsTouched(m_provider->get_state((uint8_t)m_index).data);
}

glm::vec2 DSUController::get_position()
{
	return TouchPosition(m_provider->get_state((uint8_t)m_index).data).value_or(glm::vec2{});
}

glm::vec2 DSUController::get_prev_position()
{
	return TouchPosition(m_provider->get_prev_state((uint8_t)m_index).data).value_or(glm::vec2{});
}

// judged from the previous packet, as it always has been
PositionVisibility DSUController::GetPositionVisibility()
{
	return IsTouched(m_provider->get_prev_state((uint8_t)m_index).data) ? PositionVisibility::FULL : PositionVisibility::NONE;
}

std::string DSUController::get_button_name(uint64 button) const
{
	if (button < std::size(kButtonNames))
		return std::string{ kButtonNames[button] };
	return ControllerBase::get_button_name(button);
}

ControllerState DSUController::raw_state()
{
	ControllerState result{};
	if (!is_connected())
		return result;

	const auto slot = m_provider->get_state((uint8_t)m_index);
	if (slot.info.state != DsState::Connected) // no data from the server yet
		return result;

	const DataResponseData& data = slot.data;
	const uint16 buttons = (uint16)(data.state1 | (data.state2 << 8));
	for (uint32 bit = 0; bit < 16; ++bit)
	{
		if (buttons & (1u << bit))
			result.buttons.SetButtonState(bit, true);
	}
	if (data.touch)
		result.buttons.SetButtonState(kButton16, true);

	result.axis = { StickValue(data.lx), StickValue(data.ly) };
	result.rotation = { StickValue(data.rx), StickValue(data.ry) };
	result.trigger = { (float)data.l2 / 255.0f, (float)data.r2 / 255.0f };
	return result;
}
