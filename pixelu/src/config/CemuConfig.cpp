// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "config/CemuConfig.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/Account/Account.h"

// settings.xml layout (all under <content>):
//   general values, <GamePaths>, <GameCache>, <GraphicPack>, <Graphic>
//   (with <Overlay> and <Notification>), <Audio>, <Account>,
//   <AccountService>, <Debug>, <Input><DSUC/>, <EmulatedUsbDevices>
//
// The type of each default value passed to XMLConfigParser::get() selects
// how the text is parsed, and the type passed to set() selects how it is
// written, so these types are part of the file format as well.

namespace
{
	// iterate over all children of parent that are named childName
	template<typename TFunc>
	void forEachChild(XMLConfigParser& parent, const char* childName, TFunc&& func)
	{
		for (XMLConfigParser child = parent.get(childName); child.valid(); child = parent.get(childName, child))
			func(child);
	}

	// converts a UTF-8 setting to a wide string; on failure the target keeps its value
	void readWide(std::wstring& target, const char* utf8, const char* what)
	{
		try
		{
			target = boost::nowide::widen(utf8);
		}
		catch (const std::exception&)
		{
			cemuLog_log(LogType::Force, "config load error: can't load {}: {}", what, utf8);
		}
	}

	bool isSelectableService(NetworkService service)
	{
		return service == NetworkService::Offline || service == NetworkService::Nintendo ||
			   service == NetworkService::Pretendo || service == NetworkService::Custom;
	}

	void loadGamePaths(CemuConfig& cfg, XMLConfigParser& root)
	{
		cfg.game_paths.clear();
		XMLConfigParser list = root.get("GamePaths");
		forEachChild(list, "Entry", [&](XMLConfigParser& entry) {
			const std::string path = entry.value("");
			if (path.empty())
				return;
			try
			{
				cfg.game_paths.emplace_back(path);
			}
			catch (const std::exception&)
			{
				cemuLog_log(LogType::Force, "config load error: can't load game path: {}", path);
			}
		});
	}

	// caller holds game_cache_entries_mutex
	void loadGameCache(CemuConfig& cfg, XMLConfigParser& root)
	{
		cfg.game_cache_entries.clear();
		XMLConfigParser list = root.get("GameCache");
		forEachChild(list, "Entry", [&](XMLConfigParser& node) {
			const char* rpx = node.get("path", "");
			try
			{
				GameEntry game{};
				game.rpx_file = boost::nowide::widen(rpx);
				game.title_id = node.get<uint64>("title_id");
				game.legacy_name = boost::nowide::widen(node.get("name", ""));
				game.custom_name = node.get("custom_name", "");
				game.legacy_region = node.get("region", 0);
				// the update version has always been read back from "version"
				game.legacy_version = node.get("version", 0);
				game.legacy_update_version = node.get("version", 0);
				game.legacy_dlc_version = node.get("dlc_version", 0);
				game.legacy_time_played = node.get<uint64>("time_played");
				game.legacy_last_played = node.get<uint64>("last_played");
				game.favorite = node.get("favorite", false);
				cfg.game_cache_entries.emplace_back(game);
				if (game.title_id == 0)
					return;
				if (game.favorite)
					cfg.game_cache_favorites.emplace(game.title_id);
				else
					cfg.game_cache_favorites.erase(game.title_id);
			}
			catch (const std::exception&)
			{
				cemuLog_log(LogType::Force, "config load error: can't load game cache entry: {}", rpx);
			}
		});
	}

	void loadGraphicPacks(CemuConfig& cfg, XMLConfigParser& root)
	{
		auto& packs = cfg.graphic_pack_entries;
		packs.clear();
		XMLConfigParser list = root.get("GraphicPack");
		forEachChild(list, "Entry", [&](XMLConfigParser& node) {
			std::string file = node.get_attribute("filename", "");
			if (!file.empty())
			{
				// current layout: <Entry filename=".." disabled=".."><Preset><category/><preset/></Preset>...</Entry>
				const fs::path key = fs::path(file).lexically_normal();
				packs.try_emplace(key);
				if (node.get_attribute("disabled", false))
					packs[key].try_emplace("_disabled", "true");
				forEachChild(node, "Preset", [&](XMLConfigParser& preset) {
					const std::string category = preset.get("category", "");
					const std::string active = preset.get("preset", "");
					packs[key].try_emplace(category, active);
				});
				return;
			}
			// old layout: <Entry><filename/><category/><preset/></Entry>
			// (the preset is stored under the path as written, which is how
			// older versions behaved)
			file = node.get("filename", "");
			packs.try_emplace(fs::path(file).lexically_normal());
			const std::string category = node.get("category", "");
			const std::string active = node.get("preset", "");
			packs[file].try_emplace(category, active);
		});
	}

	void loadGraphics(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser gfx = root.get("Graphic");
		cfg.graphic_api = gfx.get("api", kDefaultGraphicsAPI);
		gfx.get("device", cfg.legacy_graphic_device_uuid);
		if (gfx.get("vkDevice").valid())
			gfx.get("vkDevice", cfg.vk_graphic_device_uuid);
		else
			cfg.vk_graphic_device_uuid = cfg.legacy_graphic_device_uuid;
		cfg.mtl_graphic_device_uuid = gfx.get("mtlDevice", 0);
		cfg.vsync = gfx.get("VSync", 0);
		cfg.overrideAppGammaPreference = gfx.get("OverrideAppGammaPreference", false);
		cfg.overrideGammaValue = gfx.get("OverrideGammaValue", 2.2f);
		if (cfg.overrideGammaValue < 0)
			cfg.overrideGammaValue = 2.2f;
		cfg.userDisplayGamma = gfx.get("UserDisplayGamma", 2.2f);
		if (cfg.userDisplayGamma < 0)
			cfg.userDisplayGamma = 2.2f;
		cfg.gx2drawdone_sync = gfx.get("GX2DrawdoneSync", true);
		cfg.upscale_filter = gfx.get("UpscaleFilter", kBicubicHermiteFilter);
		cfg.downscale_filter = gfx.get("DownscaleFilter", kLinearFilter);
		cfg.fullscreen_scaling = gfx.get("FullscreenScaling", kKeepAspectRatio);
		cfg.async_compile = gfx.get("AsyncCompile", cfg.async_compile);
		// named "VulkanAccurateBarriers" before the default became true
		cfg.vk_accurate_barriers = gfx.get("vkAccurateBarriers", true);

		auto& ov = cfg.overlay;
		auto& nt = cfg.notification;
		XMLConfigParser overlayNode = gfx.get("Overlay");
		if (overlayNode.valid())
		{
			ov.position = overlayNode.get("Position", ScreenPosition::kDisabled);
			ov.text_color = overlayNode.get("TextColor", 0xFFFFFFFF);
			ov.text_scale = overlayNode.get("TextScale", 100);
			ov.fps = overlayNode.get("FPS", true);
			ov.drawcalls = overlayNode.get("DrawCalls", false);
			ov.cpu_usage = overlayNode.get("CPUUsage", false);
			ov.cpu_per_core_usage = overlayNode.get("CPUPerCoreUsage", false);
			ov.ram_usage = overlayNode.get("RAMUsage", false);
			ov.vram_usage = overlayNode.get("VRAMUsage", false);
			ov.debug = overlayNode.get("Debug", false);
			nt.controller_profiles = overlayNode.get("ControllerProfiles", true);
			nt.controller_battery = overlayNode.get("ControllerBattery", true);
			nt.shader_compiling = overlayNode.get("ShaderCompiling", true);
		}
		else
		{
			// flat "Overlay*" values of old settings files
			ov.position = gfx.get("OverlayPosition", ScreenPosition::kDisabled);
			ov.text_color = gfx.get("OverlayTextColor", 0xFFFFFFFF);
			ov.fps = gfx.get("OverlayFPS", true);
			ov.drawcalls = gfx.get("OverlayDrawCalls", false);
			ov.cpu_usage = gfx.get("OverlayCPUUsage", false);
			ov.cpu_per_core_usage = gfx.get("OverlayCPUPerCoreUsage", false);
			ov.ram_usage = gfx.get("OverlayRAMUsage", false);
			nt.controller_profiles = gfx.get("OverlayControllerProfiles", true);
			nt.controller_battery = gfx.get("OverlayControllerBattery", true);
			nt.shader_compiling = gfx.get("ShaderCompiling", true);
		}

		XMLConfigParser notificationNode = gfx.get("Notification");
		if (notificationNode.valid())
		{
			nt.position = notificationNode.get("Position", ScreenPosition::kTopLeft);
			nt.text_color = notificationNode.get("TextColor", 0xFFFFFFFF);
			nt.text_scale = notificationNode.get("TextScale", 100);
			nt.controller_profiles = notificationNode.get("ControllerProfiles", true);
			nt.controller_battery = notificationNode.get("ControllerBattery", false);
			nt.shader_compiling = notificationNode.get("ShaderCompiling", true);
			nt.friends = notificationNode.get("FriendService", true);
		}
	}

	void loadAudio(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser audio = root.get("Audio");
		cfg.audio_api = audio.get("api", 0);
		cfg.audio_delay = audio.get("delay", 2);
		cfg.tv_channels = audio.get("TVChannels", kStereo);
		cfg.pad_channels = audio.get("PadChannels", kStereo);
		cfg.input_channels = audio.get("InputChannels", kMono);
		cfg.tv_volume = audio.get("TVVolume", 20);
		cfg.pad_volume = audio.get("PadVolume", 0);
		cfg.input_volume = audio.get("InputVolume", 20);
		cfg.portal_volume = audio.get("PortalVolume", 20);
		readWide(cfg.tv_device, audio.get("TVDevice", ""), "tv device");
		readWide(cfg.pad_device, audio.get("PadDevice", ""), "pad device");
		readWide(cfg.input_device, audio.get("InputDevice", ""), "input device");
		readWide(cfg.portal_device, audio.get("PortalDevice", ""), "input device");
	}

	void loadAccounts(CemuConfig& cfg, XMLConfigParser& root)
	{
		auto& acc = cfg.account;
		XMLConfigParser node = root.get("Account");
		acc.m_persistent_id = node.get("PersistentId", acc.m_persistent_id);
		// only read to migrate the old global online setting
		acc.legacy_online_enabled = node.get("OnlineEnabled", acc.legacy_online_enabled);
		acc.legacy_active_service = node.get("ActiveService", acc.legacy_active_service);

		acc.service_select.clear();
		XMLConfigParser services = root.get("AccountService");
		forEachChild(services, "SelectedService", [&](XMLConfigParser& entry) {
			const uint32 persistentId = entry.get_attribute<uint32>("PersistentId", 0);
			const auto service = static_cast<NetworkService>(entry.get_attribute<sint32>("Service", 0));
			if (persistentId >= Account::kMinPersistendId && isSelectableService(service))
				acc.service_select.emplace(persistentId, service);
		});
	}

	void loadDebugInputAndUsb(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser debug = root.get("Debug");
#if BOOST_OS_WINDOWS
		cfg.crash_dump = debug.get("CrashDumpWindows", cfg.crash_dump);
#elif BOOST_OS_UNIX
		cfg.crash_dump = debug.get("CrashDumpUnix", cfg.crash_dump);
#endif
		cfg.gdb_port = debug.get("GDBPort", 1337);

		XMLConfigParser dsu = root.get("Input").get("DSUC");
		cfg.dsu_client.host = dsu.get_attribute("host", cfg.dsu_client.host);
		cfg.dsu_client.port = dsu.get_attribute("port", cfg.dsu_client.port);

		auto& usb = cfg.emulated_usb_devices;
		XMLConfigParser usbNode = root.get("EmulatedUsbDevices");
		usb.emulate_skylander_portal = usbNode.get("EmulateSkylanderPortal", usb.emulate_skylander_portal);
		usb.emulate_infinity_base = usbNode.get("EmulateInfinityBase", usb.emulate_infinity_base);
		usb.emulate_dimensions_toypad = usbNode.get("EmulateDimensionsToypad", usb.emulate_dimensions_toypad);
	}

	void saveGameList(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser paths = root.set("GamePaths");
		for (const std::string& path : cfg.game_paths)
			paths.set("Entry", path.c_str());

		std::unique_lock lock(cfg.game_cache_entries_mutex);
		XMLConfigParser cache = root.set("GameCache");
		for (const GameEntry& game : cfg.game_cache_entries)
		{
			XMLConfigParser node = cache.set("Entry");
			node.set("title_id", (sint64)game.title_id);
			node.set("name", boost::nowide::narrow(game.legacy_name).c_str());
			node.set("custom_name", game.custom_name.c_str());
			node.set("region", (sint32)game.legacy_region);
			node.set("version", (sint32)game.legacy_update_version);
			node.set("dlc_version", (sint32)game.legacy_dlc_version);
			node.set("path", boost::nowide::narrow(game.rpx_file).c_str());
			node.set("time_played", game.legacy_time_played);
			node.set("last_played", game.legacy_last_played);
			node.set("favorite", game.favorite);
		}
	}

	void saveGraphicPacks(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser list = root.set("GraphicPack");
		for (const auto& [file, presets] : cfg.graphic_pack_entries)
		{
			XMLConfigParser node = list.set("Entry");
			node.set_attribute("filename", _pathToUtf8(file).c_str());
			for (const auto& [category, active] : presets)
			{
				if (boost::iequals(category, "_disabled"))
				{
					node.set_attribute("disabled", true);
					continue;
				}
				XMLConfigParser preset = node.set("Preset");
				if (!category.empty())
					preset.set("category", category.c_str());
				preset.set("preset", active.c_str());
			}
		}
	}

	void saveGraphics(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser gfx = root.set("Graphic");
		gfx.set("api", cfg.graphic_api);
		gfx.set("device", cfg.legacy_graphic_device_uuid);
		gfx.set("vkDevice", cfg.vk_graphic_device_uuid);
		gfx.set("mtlDevice", cfg.mtl_graphic_device_uuid);
		gfx.set("VSync", cfg.vsync);
		gfx.set("OverrideAppGammaPreference", cfg.overrideAppGammaPreference);
		gfx.set("OverrideGammaValue", cfg.overrideGammaValue);
		gfx.set("UserDisplayGamma", cfg.userDisplayGamma);
		gfx.set("GX2DrawdoneSync", cfg.gx2drawdone_sync);
		gfx.set("UpscaleFilter", cfg.upscale_filter);
		gfx.set("DownscaleFilter", cfg.downscale_filter);
		gfx.set("FullscreenScaling", cfg.fullscreen_scaling);
		gfx.set("AsyncCompile", cfg.async_compile.GetValue());
		gfx.set("vkAccurateBarriers", cfg.vk_accurate_barriers);

		const auto& ov = cfg.overlay;
		XMLConfigParser overlayNode = gfx.set("Overlay");
		overlayNode.set("Position", ov.position);
		overlayNode.set("TextColor", ov.text_color);
		overlayNode.set("TextScale", ov.text_scale);
		overlayNode.set("FPS", ov.fps);
		overlayNode.set("DrawCalls", ov.drawcalls);
		overlayNode.set("CPUUsage", ov.cpu_usage);
		overlayNode.set("CPUPerCoreUsage", ov.cpu_per_core_usage);
		overlayNode.set("RAMUsage", ov.ram_usage);
		overlayNode.set("VRAMUsage", ov.vram_usage);
		overlayNode.set("Debug", ov.debug);

		const auto& nt = cfg.notification;
		XMLConfigParser notificationNode = gfx.set("Notification");
		notificationNode.set("Position", nt.position);
		notificationNode.set("TextColor", nt.text_color);
		notificationNode.set("TextScale", nt.text_scale);
		notificationNode.set("ControllerProfiles", nt.controller_profiles);
		notificationNode.set("ControllerBattery", nt.controller_battery);
		notificationNode.set("ShaderCompiling", nt.shader_compiling);
		notificationNode.set("FriendService", nt.friends);
	}

	void saveAudio(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser audio = root.set("Audio");
		audio.set("api", cfg.audio_api);
		audio.set("delay", cfg.audio_delay);
		audio.set("TVChannels", cfg.tv_channels);
		audio.set("PadChannels", cfg.pad_channels);
		audio.set("InputChannels", cfg.input_channels);
		audio.set("TVVolume", cfg.tv_volume);
		audio.set("PadVolume", cfg.pad_volume);
		audio.set("InputVolume", cfg.input_volume);
		audio.set("PortalVolume", cfg.portal_volume);
		audio.set("TVDevice", boost::nowide::narrow(cfg.tv_device).c_str());
		audio.set("PadDevice", boost::nowide::narrow(cfg.pad_device).c_str());
		audio.set("InputDevice", boost::nowide::narrow(cfg.input_device).c_str());
		audio.set("PortalDevice", boost::nowide::narrow(cfg.portal_device).c_str());
	}

	void saveAccountsDebugInputAndUsb(CemuConfig& cfg, XMLConfigParser& root)
	{
		XMLConfigParser acc = root.set("Account");
		acc.set("PersistentId", cfg.account.m_persistent_id.GetValue());
		acc.set("OnlineEnabled", cfg.account.legacy_online_enabled.GetValue());
		acc.set("ActiveService", cfg.account.legacy_active_service.GetValue());

		XMLConfigParser services = root.set("AccountService");
		for (const auto& [persistentId, service] : cfg.account.service_select)
		{
			XMLConfigParser entry = services.set("SelectedService");
			entry.set_attribute("PersistentId", persistentId);
			entry.set_attribute("Service", static_cast<sint32>(service));
		}

		XMLConfigParser debug = root.set("Debug");
#if BOOST_OS_WINDOWS
		debug.set("CrashDumpWindows", cfg.crash_dump.GetValue());
#elif BOOST_OS_UNIX
		debug.set("CrashDumpUnix", cfg.crash_dump.GetValue());
#endif
		debug.set("GDBPort", cfg.gdb_port);

		XMLConfigParser dsu = root.set("Input").set("DSUC");
		dsu.set_attribute("host", cfg.dsu_client.host);
		dsu.set_attribute("port", cfg.dsu_client.port);

		const auto& usb = cfg.emulated_usb_devices;
		XMLConfigParser usbNode = root.set("EmulatedUsbDevices");
		usbNode.set("EmulateSkylanderPortal", usb.emulate_skylander_portal.GetValue());
		usbNode.set("EmulateInfinityBase", usb.emulate_infinity_base.GetValue());
		usbNode.set("EmulateDimensionsToypad", usb.emulate_dimensions_toypad.GetValue());
	}
}

void CemuConfig::SetMLCPath(fs::path path, bool save)
{
	mlc_path.SetValue(_pathToUtf8(path));
	if (save)
		GetConfigHandle().Save();
	Account::RefreshAccounts();
}

XMLConfigParser CemuConfig::Load(XMLConfigParser& parser)
{
	if (XMLConfigParser content = parser.get("content"); content.valid())
		parser = content;

	log_flag = parser.get("logflag", log_flag.GetInitValue());
	cemuLog_setActiveLoggingFlags(GetConfig().log_flag.GetValue());
	advanced_ppc_logging = parser.get("advanced_ppc_logging", advanced_ppc_logging.GetInitValue());
	const char* mlc = parser.get("mlc_path", "");
	mlc_path = mlc;
	permanent_storage = parser.get("permanent_storage", permanent_storage);
	proxy_server = parser.get("proxy_server", "");
	disable_screensaver = parser.get("disable_screensaver", disable_screensaver);
	play_boot_sound = parser.get("play_boot_sound", play_boot_sound);
	console_language = parser.get("console_language", console_language.GetInitValue());

	loadGamePaths(*this, parser);
	{
		std::unique_lock lock(game_cache_entries_mutex);
		loadGameCache(*this, parser);
	}
	loadGraphicPacks(*this, parser);
	loadGraphics(*this, parser);
	loadAudio(*this, parser);
	loadAccounts(*this, parser);
	loadDebugInputAndUsb(*this, parser);
	return parser;
}

XMLConfigParser CemuConfig::Save(XMLConfigParser& parser)
{
	XMLConfigParser content = parser.set("content");
	content.set("logflag", log_flag.GetValue());
	content.set("advanced_ppc_logging", advanced_ppc_logging.GetValue());
	content.set("mlc_path", mlc_path.GetValue().c_str());
	content.set<bool>("permanent_storage", permanent_storage);
	content.set("proxy_server", proxy_server.GetValue().c_str());
	content.set<bool>("disable_screensaver", disable_screensaver);
	content.set<bool>("play_boot_sound", play_boot_sound);
	content.set("console_language", console_language.GetValue());

	saveGameList(*this, content);
	saveGraphicPacks(*this, content);
	saveGraphics(*this, content);
	saveAudio(*this, content);
	saveAccountsDebugInputAndUsb(*this, content);
	return content;
}

GameEntry* CemuConfig::GetGameEntryByTitleId(uint64 titleId)
{
	auto it = std::find_if(game_cache_entries.begin(), game_cache_entries.end(), [titleId](const GameEntry& e) { return e.title_id == titleId; });
	return it == game_cache_entries.end() ? nullptr : &*it;
}

GameEntry* CemuConfig::CreateGameEntry(uint64 titleId)
{
	GameEntry& entry = game_cache_entries.emplace_back();
	entry.title_id = titleId;
	return &entry;
}

bool CemuConfig::IsGameListFavorite(uint64 titleId)
{
	std::unique_lock lock(game_cache_entries_mutex);
	return game_cache_favorites.contains(titleId);
}

void CemuConfig::SetGameListFavorite(uint64 titleId, bool isFavorite)
{
	std::unique_lock lock(game_cache_entries_mutex);
	GameEntry* entry = GetGameEntryByTitleId(titleId);
	if (!entry)
		entry = CreateGameEntry(titleId);
	entry->favorite = isFavorite;
	if (isFavorite)
		game_cache_favorites.emplace(titleId);
	else
		game_cache_favorites.erase(titleId);
}

bool CemuConfig::GetGameListCustomName(uint64 titleId, std::string& customName)
{
	std::unique_lock lock(game_cache_entries_mutex);
	const GameEntry* entry = GetGameEntryByTitleId(titleId);
	if (!entry || entry->custom_name.empty())
		return false;
	customName = entry->custom_name;
	return true;
}

void CemuConfig::SetGameListCustomName(uint64 titleId, std::string customName)
{
	std::unique_lock lock(game_cache_entries_mutex);
	GameEntry* entry = GetGameEntryByTitleId(titleId);
	if (!entry)
	{
		if (customName.empty())
			return; // nothing to store
		entry = CreateGameEntry(titleId);
	}
	entry->custom_name = std::move(customName);
}

NetworkService CemuConfig::GetAccountNetworkService(uint32 persistentId)
{
	if (auto it = account.service_select.find(persistentId); it != account.service_select.end())
	{
		const NetworkService service = it->second;
		if (!isSelectableService(service))
			return NetworkService::Offline;
		if (service == NetworkService::Custom && !NetworkConfig::XMLExists())
			return NetworkService::Offline; // no network_services.xml for the custom service
		return service;
	}
	// no per-account choice: derive it from the old global setting, whose
	// service index did not include "Offline"
	if (!account.legacy_online_enabled)
		return NetworkService::Offline;
	return static_cast<NetworkService>((sint32)((uint32)account.legacy_active_service.GetValue() + 1));
}

void CemuConfig::SetAccountSelectedService(uint32 persistentId, NetworkService serviceIndex)
{
	account.service_select[persistentId] = serviceIndex;
}
