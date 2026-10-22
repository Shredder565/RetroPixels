// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// One port of a GameCube controller adapter. Button ids are the bits of the adapter's
// button word.

#include "input/api/Controller.h"
#include "input/api/GameCube/GameCubeControllerProvider.h"

#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)

class GameCubeController : public Controller<GameCubeControllerProvider>
{
public:
	GameCubeController(uint32 adapter, uint32 index);

	std::string_view api_name() const override { return to_string(InputAPI::GameCube); } // "GameCube"
	InputAPI::Type api() const override { return InputAPI::GameCube; }

	bool is_connected() override;
	bool has_rumble() override;
	void start_rumble() override;
	void stop_rumble() override;

	std::string get_button_name(uint64 button) const override;

protected:
	ControllerState raw_state() override;

	uint32 m_adapter;
	uint32 m_index;
};

#endif
