// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Owns the input backends (providers) and the emulated controllers of all player slots,
// reads and writes the controller profiles (controllerProfiles/*.xml) and keeps the
// mouse / touch state of the emulator's windows, which the GamePad touch screen uses.

#if BOOST_OS_WINDOWS
#include "input/api/DirectInput/DirectInputControllerProvider.h"
#include "input/api/XInput/XInputControllerProvider.h"
#endif

#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/WiimoteControllerProvider.h"
#endif

#ifdef HAS_SDL
#include "input/api/SDL/SDLControllerProvider.h"
#endif

#include "input/api/Keyboard/KeyboardControllerProvider.h"
#include "input/api/DSU/DSUControllerProvider.h"
#include "input/api/GameCube/GameCubeControllerProvider.h"

#include "input/emulated/VPADController.h"
#include "input/emulated/WPADController.h"

#include "util/helpers/Singleton.h"

class InputManager : public Singleton<InputManager>
{
	friend class Singleton<InputManager>;
	InputManager();
	~InputManager();

	friend class MainWindow;
	friend class PadViewFrame;

public:
	// player slots: up to 2 GamePads plus up to 7 padscore controllers, 8 players in all
	constexpr static size_t kMaxController = 8;
	constexpr static size_t kMaxVPADControllers = 2;
	constexpr static size_t kMaxWPADControllers = 7;

	// loads controller0..7
	void load() noexcept;
	// loads a player's profile, or the named profile into that player's slot; an old .txt
	// profile is converted to .xml first
	bool load(size_t player_index, std::string_view filename = {});

	// converts an old INI style .txt profile into an .xml next to it
	bool migrate_config(const fs::path& file_path);

	// saves controller0..7
	void save() noexcept;
	// saves a player's profile, or saves it under a profile name
	bool save(size_t player_index, std::string_view filename = {});

	// the slot was loaded from the running game's profile (and is not saved over)
	bool is_gameprofile_set(size_t player_index) const;

	void Shutdown();

	// puts the controller into its player's slot and returns what was there before; a new
	// controller without host controllers takes over those of the previous one
	EmulatedControllerPtr set_controller(EmulatedControllerPtr controller);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type, const std::shared_ptr<ControllerBase>& controller);

	EmulatedControllerPtr delete_controller(size_t player_index, bool delete_profile = false);

	EmulatedControllerPtr get_controller(size_t player_index) const;
	// by VPAD / WPAD channel, not by player
	std::shared_ptr<VPADController> get_vpad_controller(size_t index) const;
	std::shared_ptr<WPADController> get_wpad_controller(size_t index) const;
	// (GamePads, others) in use
	std::pair<size_t, size_t> get_controller_count() const;

	bool is_api_available(InputAPI::Type api) const { return !m_api_available[api].empty(); }

	ControllerProviderPtr get_api_provider(std::string_view api_name) const;
	ControllerProviderPtr get_api_provider(InputAPI::Type api) const;
	// the provider with these settings, created on first use
	ControllerProviderPtr get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings);

	const auto& get_api_providers() const
	{
		return m_api_available;
	}

	// loads the controller profiles the current game profile names
	void apply_game_profile();
	// reconnects every host controller after a device was added or removed
	void on_device_changed();

	// names of the user's saved profiles (not the per-player files)
	static std::vector<std::string> get_profiles();
	static bool is_valid_profilename(const std::string& name);

	// pointer state of one window, written by the GUI. A *_toggle flag remembers a press
	// that was already released again, so even a very short click is seen once.
	struct MouseInfo
	{
		mutable std::shared_mutex m_mutex;
		glm::ivec2 position{};
		bool left_down = false;
		bool right_down = false;

		bool left_down_toggle = false;
		bool right_down_toggle = false;
	} m_main_mouse{}, m_pad_mouse{}, m_main_touch{}, m_pad_touch{};
	glm::ivec2 get_mouse_position(bool pad_window) const;
	// position of a pressed pointer: main window mouse, main window touch, then the same on
	// the GamePad window; *is_pad tells which window it was
	std::optional<glm::ivec2> get_left_down_mouse_info(bool* is_pad);
	std::optional<glm::ivec2> get_right_down_mouse_info(bool* is_pad);

	std::atomic<float> m_mouse_wheel;

private:
	void update_thread();

	std::thread m_update_thread;
	std::atomic<bool> m_update_thread_shutdown{ false };

	// per API: the default provider first, then any created with other settings
	std::array<std::vector<ControllerProviderPtr>, InputAPI::MAX> m_api_available{};

	mutable std::shared_mutex m_mutex;
	std::array<EmulatedControllerPtr, kMaxVPADControllers> m_vpad;
	std::array<EmulatedControllerPtr, kMaxWPADControllers> m_wpad;

	std::array<bool, kMaxController> m_is_gameprofile_set{};

	template<std::derived_from<ControllerProviderBase> TProvider>
	void create_provider()
	{
		try
		{
			auto provider = std::make_shared<TProvider>();
			const auto api = provider->api();
			m_api_available[api] = { std::move(provider) };
		}
		catch (const std::exception& ex)
		{
			// a backend that can't start (missing DLL, no permission...) is simply not offered
			cemuLog_log(LogType::Force, ex.what());
		}
	}
};
