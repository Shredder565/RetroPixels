// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/ControllerState.h"

bool ControllerState::operator==(const ControllerState& other) const
{
	return buttons == other.buttons;
}
