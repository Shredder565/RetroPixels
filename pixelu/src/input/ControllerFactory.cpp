// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/ControllerFactory.h"

#include "input/emulated/VPADController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/ClassicController.h"
#include "input/emulated/WiimoteController.h"

#include "input/api/Keyboard/KeyboardController.h"
#include "input/api/DSU/DSUController.h"
#include "input/api/GameCube/GameCubeController.h"

#if BOOST_OS_WINDOWS
#include "input/api/XInput/XInputController.h"
#include "input/api/DirectInput/DirectInputController.h"
#endif

#if HAS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

#ifdef HAS_SDL
#include "input/api/SDL/SDLController.h"
#endif

// uuid formats as stored in profiles:
//   Keyboard     anything ("Keyboard")
//   DirectInput  the device GUID in braces, or "0" for whichever DirectInput device comes first
//                (mouse2joystick setups ship profiles like that)
//   XInput       the user index 0..3
//   SDL          "<index among devices with this GUID>_<SDL GUID string>"
//   DSU          the server slot 0..3
//   GameCube     "<adapter>_<port>"
//   Wiimote      the device index

namespace
{
	// splits "a_b" at the first underscore
	[[maybe_unused]] std::pair<std::string_view, std::string_view> SplitUuid(std::string_view uuid, std::string_view what)
	{
		const size_t sep = uuid.find('_');
		if (sep == std::string_view::npos)
			throw std::invalid_argument(fmt::format("invalid {} uuid format: {}", what, uuid));
		return { uuid.substr(0, sep), uuid.substr(sep + 1) };
	}

#if HAS_DIRECTINPUT
	GUID DirectInputGuid(std::string_view uuid)
	{
		std::string text{ uuid };
		if (uuid == "0")
		{
			const auto devices = InputManager::instance().get_api_provider(InputAPI::DirectInput)->get_controllers();
			if (devices.empty())
				throw std::invalid_argument("can't apply non-uuid-specific directinput profile when no controllers are available");
			text = devices.front()->uuid();
		}
		GUID guid;
		if (!GUIDFromString(text.c_str(), guid))
			throw std::invalid_argument(fmt::format("invalid guid format: {}", uuid));
		return guid;
	}
#endif
}

ControllerPtr ControllerFactory::CreateController(InputAPI::Type api, std::string_view uuid, std::string_view display_name)
{
	switch (api)
	{
#if HAS_KEYBOARD
	case InputAPI::Keyboard:
		return std::make_shared<KeyboardController>();
#endif
#if HAS_DIRECTINPUT
	case InputAPI::DirectInput:
		return std::make_shared<DirectInputController>(DirectInputGuid(uuid));
#endif
#if HAS_XINPUT
	case InputAPI::XInput:
		return std::make_shared<XInputController>(ConvertString<uint32>(uuid));
#endif
#ifdef HAS_SDL
	case InputAPI::SDLController:
	{
		const auto [index, guid_text] = SplitUuid(uuid, "sdl");
		const auto guid_index = ConvertString<size_t>(index);
		const auto guid = SDL_StringToGUID(std::string{ guid_text }.c_str());
		if (display_name.empty())
			return std::make_shared<SDLController>(guid, guid_index);
		return std::make_shared<SDLController>(guid, guid_index, display_name);
	}
#endif
#if HAS_DSU
	case InputAPI::DSUClient:
		return std::make_shared<DSUController>(ConvertString<uint32>(uuid));
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
	case InputAPI::GameCube:
	{
		const auto [adapter, port] = SplitUuid(uuid, "gamecube");
		return std::make_shared<GameCubeController>(ConvertString<int>(adapter), ConvertString<int>(port));
	}
#endif
#if HAS_WIIMOTE
	case InputAPI::Wiimote:
		return std::make_shared<NativeWiimoteController>(ConvertString<uint32>(uuid));
#endif
	default:
		throw std::invalid_argument(fmt::format("unhandled controller api: {}", api));
	}
}

EmulatedControllerPtr ControllerFactory::CreateEmulatedController(size_t player_index, EmulatedController::Type type)
{
	switch (type)
	{
	case EmulatedController::Type::VPAD: return std::make_shared<VPADController>(player_index);
	case EmulatedController::Type::Pro: return std::make_shared<ProController>(player_index);
	case EmulatedController::Type::Classic: return std::make_shared<ClassicController>(player_index);
	case EmulatedController::Type::Wiimote: return std::make_shared<WiimoteController>(player_index);
	default: throw std::runtime_error(fmt::format("unknown emulated controller type: {}", type));
	}
}

ControllerProviderPtr ControllerFactory::CreateControllerProvider(InputAPI::Type api, const ControllerProviderSettings& settings)
{
	switch (api)
	{
#if HAS_KEYBOARD
	case InputAPI::Keyboard:
		return std::make_shared<KeyboardControllerProvider>();
#endif
#ifdef HAS_SDL
	case InputAPI::SDLController:
		return std::make_shared<SDLControllerProvider>();
#endif
#if HAS_XINPUT
	case InputAPI::XInput:
		return std::make_shared<XInputControllerProvider>();
#endif
#if HAS_DIRECTINPUT
	case InputAPI::DirectInput:
		return std::make_shared<DirectInputControllerProvider>();
#endif
#if HAS_DSU
	case InputAPI::DSUClient:
		// a DSU provider is per server address
		if (const auto* dsu = dynamic_cast<const DSUProviderSettings*>(&settings))
			return std::make_shared<DSUControllerProvider>(*dsu);
		cemuLog_log(LogType::Force, "failing to cast ControllerProviderSettings class to DSUControllerProvider");
		return std::make_shared<DSUControllerProvider>();
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
	case InputAPI::GameCube:
		return std::make_shared<GameCubeControllerProvider>();
#endif
#if HAS_WIIMOTE
	case InputAPI::Wiimote:
		return std::make_shared<WiimoteControllerProvider>();
#endif
	default:
		cemu_assert_debug(false);
		return {};
	}
}
