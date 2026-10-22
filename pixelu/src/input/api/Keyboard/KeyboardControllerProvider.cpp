// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Keyboard/KeyboardControllerProvider.h"
#include "input/api/Keyboard/KeyboardController.h"

std::vector<std::shared_ptr<ControllerBase>> KeyboardControllerProvider::get_controllers()
{
	return { std::make_shared<KeyboardController>() };
}
