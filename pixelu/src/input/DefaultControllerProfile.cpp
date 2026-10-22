// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/DefaultControllerProfile.h"
#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "config/ActiveSettings.h"

#ifdef HAS_SDL
#include <SDL3/SDL.h>
#endif

namespace
{
	using Clock = std::chrono::steady_clock;
	using std::chrono::milliseconds;

	// ---- keyboard layout ----
	// Left stick on WASD, right stick on TFGH, D-pad on the arrow keys and the
	// face buttons on IJKL in the GamePad's diamond. The numbers are the raw key
	// codes KeyboardController reports on each platform.
#if BOOST_OS_WINDOWS
	// Windows virtual-key codes; letters use their upper case ASCII value
	constexpr uint32 KeyCode(char letter) { return (uint32)letter; }
	constexpr uint32 kArrowLeft = 0x25, kArrowUp = 0x26, kArrowRight = 0x27, kArrowDown = 0x28;
	constexpr uint32 kReturn = 0x0D, kBackspace = 0x08, kHomeKey = 0x24;
	#define PIXELU_DEFAULT_KEYBOARD 1
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	// GDK key values; letters use their lower case ASCII value
	constexpr uint32 KeyCode(char letter) { return (uint32)(letter + ('a' - 'A')); }
	constexpr uint32 kArrowLeft = 0xff51, kArrowUp = 0xff52, kArrowRight = 0xff53, kArrowDown = 0xff54;
	constexpr uint32 kReturn = 0xff0d, kBackspace = 0xff08, kHomeKey = 0xff50;
	#define PIXELU_DEFAULT_KEYBOARD 1
#endif

#ifdef PIXELU_DEFAULT_KEYBOARD
	struct KeyAssignment
	{
		VPADController::ButtonId button;
		uint32 keyCode;
	};

	constexpr KeyAssignment kKeys[] = {
		// face buttons
		{VPADController::kButtonId_A, KeyCode('L')},
		{VPADController::kButtonId_B, KeyCode('K')},
		{VPADController::kButtonId_X, KeyCode('I')},
		{VPADController::kButtonId_Y, KeyCode('J')},
		// shoulders and triggers
		{VPADController::kButtonId_L, KeyCode('Q')},
		{VPADController::kButtonId_R, KeyCode('O')},
		{VPADController::kButtonId_ZL, KeyCode('E')},
		{VPADController::kButtonId_ZR, KeyCode('U')},
		// system buttons
		{VPADController::kButtonId_Plus, kReturn},
		{VPADController::kButtonId_Minus, kBackspace},
		{VPADController::kButtonId_Home, kHomeKey},
		// D-pad
		{VPADController::kButtonId_Up, kArrowUp},
		{VPADController::kButtonId_Down, kArrowDown},
		{VPADController::kButtonId_Left, kArrowLeft},
		{VPADController::kButtonId_Right, kArrowRight},
		// stick clicks
		{VPADController::kButtonId_StickL, KeyCode('C')},
		{VPADController::kButtonId_StickR, KeyCode('M')},
		// left stick
		{VPADController::kButtonId_StickL_Up, KeyCode('W')},
		{VPADController::kButtonId_StickL_Down, KeyCode('S')},
		{VPADController::kButtonId_StickL_Left, KeyCode('A')},
		{VPADController::kButtonId_StickL_Right, KeyCode('D')},
		// right stick
		{VPADController::kButtonId_StickR_Up, KeyCode('T')},
		{VPADController::kButtonId_StickR_Down, KeyCode('G')},
		{VPADController::kButtonId_StickR_Left, KeyCode('F')},
		{VPADController::kButtonId_StickR_Right, KeyCode('H')},
	};
#endif

	// controllers an input API sees right now (none if the API is missing or fails)
	std::vector<ControllerPtr> ListControllers(InputAPI::Type api)
	{
		InputManager& input = InputManager::instance();
		if (!input.is_api_available(api))
			return {};
		try
		{
			return input.get_api_provider(api)->get_controllers();
		}
		catch (const std::exception& e)
		{
			cemuLog_log(LogType::Force, "Default controller profile: listing {} controllers failed: {}", InputAPI::to_string(api), e.what());
			return {};
		}
	}

	// SDL is started on another thread and reports pads a little after that, so
	// on this first start wait up to 2.5 s for SDL and then up to 0.75 s for a pad
	std::vector<ControllerPtr> ListSDLControllers()
	{
#ifdef HAS_SDL
		if (!InputManager::instance().is_api_available(InputAPI::SDLController))
			return {};
		const Clock::time_point giveUp = Clock::now() + milliseconds(2500);
		for (;;)
		{
			if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0)
				break;
			if (Clock::now() >= giveUp)
				return {};
			std::this_thread::sleep_for(milliseconds(20));
		}
		const Clock::time_point padGiveUp = std::min(giveUp, Clock::now() + milliseconds(750));
		for (;;)
		{
			std::vector<ControllerPtr> pads = ListControllers(InputAPI::SDLController);
			if (!pads.empty() || Clock::now() >= padGiveUp)
				return pads;
			std::this_thread::sleep_for(milliseconds(50));
		}
#else
		return {};
#endif
	}

	ControllerPtr MakeController(InputAPI::Type api, const ControllerPtr& found)
	{
		return ControllerFactory::CreateController(api, found->uuid(), found->display_name());
	}

	// player 1's game controller: a connected XInput pad, else a connected SDL
	// pad, else XInput slot 0 so an Xbox pad plugged in later works too
	ControllerPtr PickGamepad()
	{
		const std::vector<ControllerPtr> xinputPads = ListControllers(InputAPI::XInput);
		if (!xinputPads.empty())
			return MakeController(InputAPI::XInput, xinputPads.front());
		const std::vector<ControllerPtr> sdlPads = ListSDLControllers();
		if (!sdlPads.empty())
			return MakeController(InputAPI::SDLController, sdlPads.front());
		if (InputManager::instance().is_api_available(InputAPI::XInput))
			return ControllerFactory::CreateController(InputAPI::XInput, "0", "Controller 1");
		return {};
	}

	// true when a profile exists, or when that cannot be determined
	bool ProfileMayExist(const fs::path& file)
	{
		std::error_code ec;
		const bool exists = fs::exists(file, ec);
		return exists || ec;
	}
}

void DefaultControllerProfile_CreateIfMissing()
{
	const fs::path profileFile = ActiveSettings::GetConfigPath("controllerProfiles/controller0.xml");
	if (ProfileMayExist(profileFile) || ProfileMayExist(ActiveSettings::GetConfigPath("controllerProfiles/controller0.txt")))
		return;

	try
	{
		InputManager& input = InputManager::instance();
		auto gamepadEmulation = ControllerFactory::CreateEmulatedController(0, EmulatedController::Type::VPAD);

		if (ControllerPtr pad = PickGamepad())
		{
			gamepadEmulation->add_controller(pad);
			// the input API's standard layout (face buttons, shoulders, triggers, sticks, D-pad, Start/Back as +/-)
			gamepadEmulation->set_default_mapping(pad);
			// SDL reports the guide button (button 5), XInput does not
			if (pad->api() == InputAPI::SDLController)
				gamepadEmulation->set_mapping(VPADController::kButtonId_Home, pad, kButton5);
			cemuLog_log(LogType::Force, "Default controller profile: GamePad on {} {} ({})", InputAPI::to_string(pad->api()), pad->uuid(), pad->display_name());
		}

#ifdef PIXELU_DEFAULT_KEYBOARD
		if (input.is_api_available(InputAPI::Keyboard))
		{
			ControllerPtr keys = ControllerFactory::CreateController(InputAPI::Keyboard, "keyboard", "Keyboard");
			gamepadEmulation->add_controller(keys);
			for (const KeyAssignment& k : kKeys)
				gamepadEmulation->add_mapping(k.button, keys, k.keyCode);
		}
#endif

		if (gamepadEmulation->get_controllers().empty())
			return; // nothing to drive it with
		input.set_controller(gamepadEmulation);
		if (input.save(0))
			cemuLog_log(LogType::Force, "Default controller profile saved to {}", _pathToUtf8(profileFile));
		else
			cemuLog_log(LogType::Force, "Default controller profile: could not save {}", _pathToUtf8(profileFile));
	}
	catch (const std::exception& e)
	{
		cemuLog_log(LogType::Force, "Default controller profile: {}", e.what());
	}
}
