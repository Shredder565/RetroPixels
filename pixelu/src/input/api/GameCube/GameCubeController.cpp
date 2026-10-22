// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/GameCube/GameCubeController.h"

#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)

namespace
{
	constexpr std::string_view kButtonNames[] = {
		"A", "B", "X", "Y", "Left", "Right", "Down", "Up", "Start", "Z", "Trigger R", "Trigger L",
	};

	float StickValue(uint8 v)
	{
		return ((float)v / 255.0f) * 2.0f - 1.0f;
	}
}

GameCubeController::GameCubeController(uint32 adapter, uint32 index)
	: Controller<GameCubeControllerProvider>(fmt::format("{}_{}", adapter, index), fmt::format("Controller {}", index + 1)),
	  m_adapter(adapter), m_index(index)
{
	if (adapter > 0) // tell the ports of several adapters apart
		m_display_name = fmt::format("Controller {} ({})", index + 1, adapter);

	// GameCube sticks don't reach the edge of the unit range
	m_settings.trigger.range = 1.07f;
	m_settings.axis.range = 1.20f; // main stick
	m_settings.rotation.range = 1.25f; // C stick
}

// connection and rumble are properties of the adapter port, kept by the provider
bool GameCubeController::has_rumble()
{
	return m_provider->has_rumble_connected(m_adapter);
}

bool GameCubeController::is_connected()
{
	return m_provider->is_connected(m_adapter);
}

void GameCubeController::start_rumble()
{
	if (m_settings.rumble > 0)
		m_provider->set_rumble_state(m_adapter, m_index, true);
}

void GameCubeController::stop_rumble()
{
	m_provider->set_rumble_state(m_adapter, m_index, false);
}

std::string GameCubeController::get_button_name(uint64 button) const
{
	if (button < std::size(kButtonNames))
		return std::string{ kButtonNames[button] };
	return ControllerBase::get_button_name(button);
}

ControllerState GameCubeController::raw_state()
{
	ControllerState result{};
	if (!is_connected())
		return result;

	const auto port = m_provider->get_state(m_adapter, m_index);
	if (!port.valid)
		return result;

	for (uint32 bit = 0; bit < std::size(kButtonNames); ++bit)
	{
		if (port.button & (1u << bit))
			result.buttons.SetButtonState(bit, true);
	}
	result.axis = { StickValue(port.lstick_x), StickValue(port.lstick_y) };
	result.rotation = { StickValue(port.rstick_x), StickValue(port.rstick_y) };
	result.trigger = { (float)port.lstick / 255.0f, (float)port.rstick / 255.0f };
	return result;
}

#endif
