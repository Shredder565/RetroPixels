// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Keys pressed while one of the emulator's windows has focus. Button ids are the
// window system's key codes; there are no analog axes.

#include "input/api/Keyboard/KeyboardControllerProvider.h"
#include "input/api/Controller.h"

class KeyboardController : public Controller<KeyboardControllerProvider>
{
public:
	KeyboardController();

	std::string_view api_name() const override
	{
		static_assert(to_string(InputAPI::Keyboard) == "Keyboard");
		return to_string(InputAPI::Keyboard);
	}
	InputAPI::Type api() const override { return InputAPI::Keyboard; }

	bool is_connected() override { return true; }
	bool has_axis() const override { return false; }

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;
};
