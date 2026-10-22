// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Keyboard/KeyboardController.h"
#include "WindowSystem.h"

KeyboardController::KeyboardController()
	: Controller<KeyboardControllerProvider>("keyboard", "Keyboard")
{
}

std::string KeyboardController::get_button_name(uint64 button) const
{
	return WindowSystem::GetKeyCodeName((uint32)button);
}

ControllerState KeyboardController::raw_state()
{
	ControllerState state{};
	auto& window = WindowSystem::GetWindowInfo();
	// keys typed into the debugger are not game input
	if (window.debugger_focused)
		return state;

	std::vector<uint32> held;
	window.iter_keystates([&held](const std::pair<const uint32, bool>& key)
	{
		if (key.second)
			held.push_back(key.first);
	});
	state.buttons.SetPressedButtons(held);
	return state;
}
