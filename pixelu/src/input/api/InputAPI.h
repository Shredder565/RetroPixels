// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// The host input APIs PixelU can read controllers from. The names are what
// controller profiles store in their <api> element, so they must not change.

#include "util/helpers/helpers.h"

namespace InputAPI
{
	enum Type
	{
		Keyboard,
		SDLController,
		XInput,
		DirectInput,
		DSUClient,
		GameCube,
		Wiimote,

		// reserved, not implemented
		WGIGamepad,
		WGIRawController,

		MAX
	};

	constexpr std::string_view to_string(Type type)
	{
		switch (type)
		{
		case Keyboard: return "Keyboard";
		case SDLController: return "SDLController";
		case XInput: return "XInput";
		case DirectInput: return "DirectInput";
		case DSUClient: return "DSUController";
		case GameCube: return "GameCube";
		case Wiimote: return "Wiimote";
		case WGIGamepad: return "WGIGamepad";
		case WGIRawController: return "WGIRawController";
		default: break;
		}
		throw std::runtime_error(fmt::format("unknown input api: {}", stdx::to_underlying(type)));
	}

	constexpr Type from_string(std::string_view str)
	{
		// the WGI entries can't be loaded from a profile
		constexpr Type kLoadable[] = { Keyboard, DirectInput, XInput, Wiimote, GameCube, DSUClient, SDLController };
		for (const Type type : kLoadable)
		{
			if (str == to_string(type))
				return type;
		}
		if (str == "DSU") // name used by old profiles
			return DSUClient;
		throw std::runtime_error(fmt::format("unknown input api: {}", str));
	}
}
