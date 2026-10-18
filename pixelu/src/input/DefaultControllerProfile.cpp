// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "input/DefaultControllerProfile.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "config/ActiveSettings.h"

#ifdef HAS_SDL
#include <SDL3/SDL.h>
#endif

namespace
{
	// keyboard: left stick WASD, D-pad arrow keys, right stick TFGH, face buttons IJKL laid out like the GamePad's
	// (keyboard button ids are the platform's raw key codes, as KeyboardController reports them)
	struct KeyBinding
	{
		VPADController::ButtonId mapping;
		uint32 key;
	};
#if BOOST_OS_WINDOWS
	constexpr uint32 kKeyLeft = 0x25, kKeyUp = 0x26, kKeyRight = 0x27, kKeyDown = 0x28; // VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN
	constexpr uint32 kKeyEnter = 0x0D, kKeyBackspace = 0x08, kKeyHome = 0x24; // VK_RETURN, VK_BACK, VK_HOME
	constexpr uint32 Letter(char c) { return (uint32)c; } // VK codes of letters are the upper case ASCII codes
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	constexpr uint32 kKeyLeft = 0xff51, kKeyUp = 0xff52, kKeyRight = 0xff53, kKeyDown = 0xff54; // GDK_KEY_Left ...
	constexpr uint32 kKeyEnter = 0xff0d, kKeyBackspace = 0xff08, kKeyHome = 0xff50; // GDK_KEY_Return, GDK_KEY_BackSpace, GDK_KEY_Home
	constexpr uint32 Letter(char c) { return (uint32)(c - 'A' + 'a'); } // GDK keyvals of letters are the lower case ASCII codes
#endif

#if BOOST_OS_WINDOWS || BOOST_OS_LINUX || BOOST_OS_BSD
	constexpr KeyBinding kKeyboardBindings[] =
	{
		{VPADController::kButtonId_A, Letter('L')},
		{VPADController::kButtonId_B, Letter('K')},
		{VPADController::kButtonId_X, Letter('I')},
		{VPADController::kButtonId_Y, Letter('J')},

		{VPADController::kButtonId_L, Letter('Q')},
		{VPADController::kButtonId_R, Letter('O')},
		{VPADController::kButtonId_ZL, Letter('E')},
		{VPADController::kButtonId_ZR, Letter('U')},

		{VPADController::kButtonId_Plus, kKeyEnter},
		{VPADController::kButtonId_Minus, kKeyBackspace},
		{VPADController::kButtonId_Home, kKeyHome},

		{VPADController::kButtonId_Up, kKeyUp},
		{VPADController::kButtonId_Down, kKeyDown},
		{VPADController::kButtonId_Left, kKeyLeft},
		{VPADController::kButtonId_Right, kKeyRight},

		{VPADController::kButtonId_StickL, Letter('C')},
		{VPADController::kButtonId_StickR, Letter('M')},

		{VPADController::kButtonId_StickL_Up, Letter('W')},
		{VPADController::kButtonId_StickL_Down, Letter('S')},
		{VPADController::kButtonId_StickL_Left, Letter('A')},
		{VPADController::kButtonId_StickL_Right, Letter('D')},

		{VPADController::kButtonId_StickR_Up, Letter('T')},
		{VPADController::kButtonId_StickR_Down, Letter('G')},
		{VPADController::kButtonId_StickR_Left, Letter('F')},
		{VPADController::kButtonId_StickR_Right, Letter('H')},
	};
#endif

	std::vector<ControllerPtr> ConnectedControllers(InputAPI::Type api)
	{
		auto& manager = InputManager::instance();
		if (!manager.is_api_available(api))
			return {};
		try
		{
			return manager.get_api_provider(api)->get_controllers();
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "Default controller profile: can't list {} controllers: {}", InputAPI::to_string(api), ex.what());
			return {};
		}
	}

	std::vector<ControllerPtr> ConnectedSDLControllers()
	{
#ifdef HAS_SDL
		if (!InputManager::instance().is_api_available(InputAPI::SDLController))
			return {};
		// SDL starts on its own thread and finds the connected pads shortly after: give it a moment (first start only)
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
		while (SDL_WasInit(SDL_INIT_GAMEPAD) == 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (SDL_WasInit(SDL_INIT_GAMEPAD) == 0)
			return {};
		const auto padDeadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(750));
		while (true)
		{
			auto controllers = ConnectedControllers(InputAPI::SDLController);
			if (!controllers.empty() || std::chrono::steady_clock::now() >= padDeadline)
				return controllers;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
#else
		return {};
#endif
	}

	// the game controller for player 1, or none
	ControllerPtr ChooseGamepad()
	{
		// Xbox pads (and pads that act like one): XInput by slot, so any such pad works, also one plugged in later
		if (auto xinput = ConnectedControllers(InputAPI::XInput); !xinput.empty())
			return ControllerFactory::CreateController(InputAPI::XInput, xinput.front()->uuid(), xinput.front()->display_name());
		// PlayStation, Switch and other pads: SDL (bound to that kind of pad, as Cemu's input settings do)
		if (auto sdl = ConnectedSDLControllers(); !sdl.empty())
			return ControllerFactory::CreateController(InputAPI::SDLController, sdl.front()->uuid(), sdl.front()->display_name());
		// nothing connected yet: the first XInput slot, which picks up an Xbox pad as soon as one is plugged in
		if (InputManager::instance().is_api_available(InputAPI::XInput))
			return ControllerFactory::CreateController(InputAPI::XInput, "0", "Controller 1");
		return {};
	}
}

void DefaultControllerProfile_CreateIfMissing()
{
	std::error_code ec;
	const fs::path profile = ActiveSettings::GetConfigPath("controllerProfiles/controller0.xml");
	const fs::path legacyProfile = ActiveSettings::GetConfigPath("controllerProfiles/controller0.txt");
	if (fs::exists(profile, ec) || ec || fs::exists(legacyProfile, ec) || ec)
		return; // the user's own profile (or we can't tell): leave it alone

	try
	{
		auto& manager = InputManager::instance();
		auto vpad = ControllerFactory::CreateEmulatedController(0, EmulatedController::Type::VPAD);

		if (const auto gamepad = ChooseGamepad())
		{
			vpad->add_controller(gamepad);
			vpad->set_default_mapping(gamepad); // Cemu's own layout for this API (A/B/X/Y, L/R, ZL/ZR on the triggers, sticks, D-pad, +/- on Start/Back)
			if (gamepad->api() == InputAPI::SDLController)
				vpad->set_mapping(VPADController::kButtonId_Home, gamepad, kButton5); // SDL_GAMEPAD_BUTTON_GUIDE (XInput doesn't report it)
			cemuLog_log(LogType::Force, "Default controller profile: GamePad on {} {} ({})", InputAPI::to_string(gamepad->api()), gamepad->uuid(), gamepad->display_name());
		}

#if BOOST_OS_WINDOWS || BOOST_OS_LINUX || BOOST_OS_BSD
		if (manager.is_api_available(InputAPI::Keyboard))
		{
			const auto keyboard = ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Keyboard");
			vpad->add_controller(keyboard);
			for (const auto& binding : kKeyboardBindings)
				vpad->add_mapping(binding.mapping, keyboard, binding.key);
		}
#endif

		if (vpad->get_controllers().empty())
			return;
		manager.set_controller(vpad);
		if (manager.save(0))
			cemuLog_log(LogType::Force, "Default controller profile written: {}", _pathToUtf8(profile));
		else
			cemuLog_log(LogType::Force, "Default controller profile: can't write {}", _pathToUtf8(profile));
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Default controller profile: {}", ex.what());
	}
}
