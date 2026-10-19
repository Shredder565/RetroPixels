// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/InputAPI.h"
#include "input/api/Controller.h"
#include "input/emulated/EmulatedController.h"

// Creates controllers and providers from what a profile stores about them.
class ControllerFactory
{
public:
	// a host controller from its API and its uuid string (the format depends on the API);
	// throws std::invalid_argument for a uuid it can't use or an API that isn't built in
	static ControllerPtr CreateController(InputAPI::Type api, std::string_view uuid, std::string_view display_name);
	static EmulatedControllerPtr CreateEmulatedController(size_t player_index, EmulatedController::Type type);
	static ControllerProviderPtr CreateControllerProvider(InputAPI::Type api, const ControllerProviderSettings& settings);
};
