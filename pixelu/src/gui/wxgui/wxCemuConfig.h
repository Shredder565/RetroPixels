// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/XMLConfig.h"
#include "util/math/vector2.h"

#include "wxgui.h"

// Settings that belong to the wxWidgets GUI only. They live in their own section of settings.xml,
// next to the emulator settings of CemuConfig.

// game list column widths used when settings.xml has none
namespace DefaultColumnSize
{
	enum : uint32
	{
		name = 500u,
		version = 60u,
		dlc = 50u,
		game_time = 140u,
		game_started = 160u,
		region = 80u,
		title_id = 160u
	};
};

// Windows only: light, dark or follow the system
enum class MSWThemeOption : int
{
	kAuto = 0,
	kLight = 1,
	kDark = 2,
};
ENABLE_ENUM_ITERATORS(MSWThemeOption, MSWThemeOption::kAuto, MSWThemeOption::kDark);

// a keyboard shortcut packed into 16 bits: wxWidgets key code plus modifier flags
typedef union
{
	struct
	{
		uint16 key : 13;
		uint16 alt : 1;
		uint16 ctrl : 1;
		uint16 shift : 1;
	};
	uint16 raw;
} uKeyboardHotkey;

// controller button index of a hotkey, -1 for none
typedef sint16 ControllerHotkey_t;

// one hotkey: a keyboard shortcut and/or a controller button.
// In settings.xml it is written as "<keyboard raw> <controller button>".
struct sHotkeyCfg
{
	static constexpr uint8 keyboardNone{WXK_NONE};
	static constexpr sint8 controllerNone{-1};

	uKeyboardHotkey keyboard{keyboardNone};
	ControllerHotkey_t controller{controllerNone};

	sHotkeyCfg(const uKeyboardHotkey& keyboard = {keyboardNone}, const ControllerHotkey_t& controller = {controllerNone})
		: keyboard(keyboard), controller(controller) {};

	// from the settings.xml text
	sHotkeyCfg(const char* xml_values)
	{
		std::istringstream text(xml_values);
		text >> keyboard.raw;
		text >> controller;
	}
};

template<>
struct fmt::formatter<sHotkeyCfg> : formatter<string_view>
{
	template<typename FormatContext>
	auto format(const sHotkeyCfg c, FormatContext& ctx) const
	{
		const std::string text = fmt::format("{} {}", c.keyboard.raw, c.controller);
		return formatter<string_view>::format(text, ctx);
	}
};

struct wxCemuConfig
{
	// general
	ConfigValue<sint32> language{wxLANGUAGE_DEFAULT};
	ConfigValue<int> msw_theme{static_cast<int>(MSWThemeOption::kAuto)};
	ConfigValue<bool> use_discord_presence{true};
	ConfigValue<bool> fullscreen{false};
	ConfigValue<bool> fullscreen_menubar{false};
	ConfigValue<bool> feral_gamemode{false};

	// most recent first, at most kMaxRecentEntries each
	static constexpr size_t kMaxRecentEntries = 15;
	std::vector<std::string> recent_launch_files;
	std::vector<std::string> recent_nfc_files;

	// main window geometry (-1 = not saved yet)
	Vector2i window_position{-1, -1};
	Vector2i window_size{-1, -1};
	ConfigValue<bool> window_maximized;

	// GamePad window
	ConfigValue<bool> pad_open;
	Vector2i pad_position{-1, -1};
	Vector2i pad_size{-1, -1};
	ConfigValue<bool> pad_maximized;

	ConfigValue<bool> check_update{true};
	ConfigValue<bool> receive_untested_updates{false};
	ConfigValue<bool> save_screenshot{true};

	// one-time notices
	ConfigValue<bool> did_show_vulkan_warning{false};
	ConfigValue<bool> did_show_graphic_pack_download{false}; // unused, kept so older versions find it (it marked the Getting Started dialog as seen)
	ConfigValue<bool> did_show_macos_disclaimer{false};		 // unused

	// game list
	ConfigValue<bool> show_icon_column{true};
	int game_list_style = 0;
	std::string game_list_column_order;
	struct
	{
		uint32 name = DefaultColumnSize::name;
		uint32 version = DefaultColumnSize::version;
		uint32 dlc = DefaultColumnSize::dlc;
		uint32 game_time = DefaultColumnSize::game_time;
		uint32 game_started = DefaultColumnSize::game_started;
		uint32 region = DefaultColumnSize::region;
		uint32 title_id = 0;
	} column_width{};

	struct
	{
		sHotkeyCfg modifiers;
		sHotkeyCfg toggleFullscreen;
		sHotkeyCfg toggleFullscreenAlt;
		sHotkeyCfg exitFullscreen;
		sHotkeyCfg takeScreenshot;
		sHotkeyCfg toggleFastForward;
		sHotkeyCfg exitApplication;
#ifdef CEMU_DEBUG_ASSERT
		sHotkeyCfg endEmulation;
#endif
	} hotkeys{};

	// put file first in its list (removing an older entry of it), dropping the oldest beyond the limit
	void AddRecentlyLaunchedFile(std::string_view file);
	void AddRecentNfcFile(std::string_view file);

	void Load(XMLConfigParser& parser);
	void Save(XMLConfigParser& parser);
};

typedef XMLChildConfig<wxCemuConfig, &wxCemuConfig::Load, &wxCemuConfig::Save> XMLWxCemuConfig_t;

extern XMLWxCemuConfig_t g_wxConfig;

inline XMLWxCemuConfig_t& GetWxGuiConfigHandle() { return g_wxConfig; }
inline wxCemuConfig& GetWxGUIConfig() { return g_wxConfig.Data(); }
