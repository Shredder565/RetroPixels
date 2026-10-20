// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxCemuConfig.h"
#include "Common/precompiled.h"
#include "config/CemuConfig.h"
#include "config/XMLConfig.h"
#include "util/helpers/helpers.h"
#include <wx/language.h>

XMLWxCemuConfig_t g_wxConfig(&GetConfigHandle);

namespace
{
	// file goes first, later copies of any entry are dropped, the list is cut to its maximum
	void PushRecent(std::vector<std::string>& list, std::string_view file)
	{
		std::vector<std::string> updated;
		updated.reserve(list.size() + 1);
		updated.emplace_back(file);
		for (auto& entry : list)
		{
			if (std::find(updated.begin(), updated.end(), entry) == updated.end())
				updated.emplace_back(std::move(entry));
		}
		if (updated.size() > wxCemuConfig::kMaxRecentEntries)
			updated.resize(wxCemuConfig::kMaxRecentEntries);
		list = std::move(updated);
	}

	// <section><Entry>path</Entry>...</section>, empty entries skipped
	void LoadPathList(XMLConfigParser& parent, const char* section, std::vector<std::string>& list, const char* what)
	{
		list.clear();
		auto node = parent.get(section);
		for (auto entry = node.get("Entry"); entry.valid(); entry = node.get("Entry", entry))
		{
			std::string path = entry.value("");
			if (path.empty())
				continue;
			try
			{
				list.emplace_back(std::move(path));
			}
			catch (const std::exception&)
			{
				cemuLog_log(LogType::Force, "settings.xml: skipped a recent {} entry that could not be loaded", what);
			}
		}
	}

	void SavePathList(XMLConfigParser& parent, const char* section, const std::vector<std::string>& list)
	{
		auto node = parent.set(section);
		for (const std::string& path : list)
			node.set("Entry", path.c_str());
	}

	void LoadXY(XMLConfigParser& parent, const char* name, Vector2i& value)
	{
		value.x = parent.get(name).get("x", -1);
		value.y = parent.get(name).get("y", -1);
	}

	void SaveXY(XMLConfigParser& parent, const char* name, const Vector2i& value)
	{
		auto node = parent.set(name);
		node.set<sint32>("x", value.x);
		node.set<sint32>("y", value.y);
	}

	// on/off settings stored directly under the root, in the order settings.xml
	// has always had them; a missing element loads as the declared default
	struct RootFlag
	{
		const char* element;
		ConfigValue<bool> wxCemuConfig::*value;
	};
	constexpr RootFlag kRootFlags[] = {
		{"use_discord_presence", &wxCemuConfig::use_discord_presence},
		{"fullscreen_menubar", &wxCemuConfig::fullscreen_menubar},
		{"feral_gamemode", &wxCemuConfig::feral_gamemode},
		{"check_update", &wxCemuConfig::check_update},
		{"receive_untested_updates", &wxCemuConfig::receive_untested_updates},
		{"save_screenshot", &wxCemuConfig::save_screenshot},
		{"vk_warning", &wxCemuConfig::did_show_vulkan_warning},
		{"gp_download", &wxCemuConfig::did_show_graphic_pack_download},
		{"macos_disclaimer", &wxCemuConfig::did_show_macos_disclaimer},
		{"fullscreen", &wxCemuConfig::fullscreen},
	};
}

void wxCemuConfig::AddRecentlyLaunchedFile(std::string_view file)
{
	PushRecent(recent_launch_files, file);
}

void wxCemuConfig::AddRecentNfcFile(std::string_view file)
{
	PushRecent(recent_nfc_files, file);
}

void wxCemuConfig::Load(XMLConfigParser& parser)
{
	// defaults come from each value's initial value
	language = parser.get<sint32>("language", wxLANGUAGE_DEFAULT);
	msw_theme = parser.get<sint32>("msw_theme", msw_theme);
	for (const RootFlag& flag : kRootFlags)
		this->*flag.value = parser.get(flag.element, this->*flag.value);

	LoadXY(parser, "window_position", window_position);
	LoadXY(parser, "window_size", window_size);
	window_maximized = parser.get("window_maximized", false);

	pad_open = parser.get("open_pad", false);
	LoadXY(parser, "pad_position", pad_position);
	LoadXY(parser, "pad_size", pad_size);
	pad_maximized = parser.get("pad_maximized", false);

	auto gameList = parser.get("GameList");
	game_list_style = gameList.get("style", 0);
	game_list_column_order = gameList.get("order", "");

	show_icon_column = parser.get("show_icon_column", true);

	auto width = [&gameList](const char* name, uint32 fallback) -> uint32 { return gameList.get(name, fallback); };
	column_width.name = width("name_width", DefaultColumnSize::name);
	column_width.version = width("version_width", DefaultColumnSize::version);
	column_width.dlc = width("dlc_width", DefaultColumnSize::dlc);
	column_width.game_time = width("game_time_width", DefaultColumnSize::game_time);
	column_width.game_started = width("game_started_width", DefaultColumnSize::game_started);
	column_width.region = width("region_width", DefaultColumnSize::region);
	column_width.title_id = width("title_id", DefaultColumnSize::title_id);

	LoadPathList(parser, "RecentLaunchFiles", recent_launch_files, "game");
	LoadPathList(parser, "RecentNFCFiles", recent_nfc_files, "NFC file");

	auto keys = parser.get("Hotkeys");
	auto shortcut = [](uint16 key, bool alt = false) {
		uKeyboardHotkey combo{};
		combo.key = key;
		combo.alt = alt ? 1 : 0;
		return sHotkeyCfg{combo};
	};
	hotkeys.modifiers = keys.get("modifiers", sHotkeyCfg{});
	hotkeys.exitFullscreen = keys.get("ExitFullscreen", shortcut(WXK_ESCAPE));
	hotkeys.toggleFullscreen = keys.get("ToggleFullscreen", shortcut(WXK_F11));
	hotkeys.toggleFullscreenAlt = keys.get("ToggleFullscreenAlt", shortcut(WXK_CONTROL_M, true)); // Alt+Enter
	hotkeys.takeScreenshot = keys.get("TakeScreenshot", shortcut(WXK_F12));
	hotkeys.toggleFastForward = keys.get("ToggleFastForward", sHotkeyCfg{});
	hotkeys.exitApplication = keys.get("ExitApplication", sHotkeyCfg{});
#ifdef CEMU_DEBUG_ASSERT
	hotkeys.endEmulation = keys.get("EndEmulation", shortcut(WXK_F5));
#endif
}

void wxCemuConfig::Save(XMLConfigParser& config)
{
	// the element order is the order settings.xml has always had
	config.set<sint32>("language", language);
	config.set<sint32>("msw_theme", msw_theme);
	for (const RootFlag& flag : kRootFlags)
		config.set<bool>(flag.element, this->*flag.value);

	SaveXY(config, "window_position", window_position);
	SaveXY(config, "window_size", window_size);
	config.set<bool>("window_maximized", window_maximized);

	config.set<bool>("open_pad", pad_open);
	SaveXY(config, "pad_position", pad_position);
	SaveXY(config, "pad_size", pad_size);
	config.set<bool>("pad_maximized", pad_maximized);
	config.set<bool>("show_icon_column", show_icon_column);

	auto gameList = config.set("GameList");
	gameList.set("style", game_list_style);
	gameList.set("order", game_list_column_order);
	gameList.set("name_width", column_width.name);
	gameList.set("version_width", column_width.version);
	gameList.set("dlc_width", column_width.dlc);
	gameList.set("game_time_width", column_width.game_time);
	gameList.set("game_started_width", column_width.game_started);
	gameList.set("region_width", column_width.region);
	gameList.set("title_id", column_width.title_id);

	SavePathList(config, "RecentLaunchFiles", recent_launch_files);
	SavePathList(config, "RecentNFCFiles", recent_nfc_files);

	auto keys = config.set("Hotkeys");
	keys.set("modifiers", hotkeys.modifiers);
	keys.set("ExitFullscreen", hotkeys.exitFullscreen);
	keys.set("ToggleFullscreen", hotkeys.toggleFullscreen);
	keys.set("ToggleFullscreenAlt", hotkeys.toggleFullscreenAlt);
	keys.set("TakeScreenshot", hotkeys.takeScreenshot);
	keys.set("ToggleFastForward", hotkeys.toggleFastForward);
	keys.set("ExitApplication", hotkeys.exitApplication);
}
