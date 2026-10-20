// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "ConfigValue.h"
#include "XMLConfig.h"

// The main settings file (settings.xml). The element names written by
// CemuConfig::Save and read by CemuConfig::Load are a file format that
// existing installations depend on; see CemuConfig.cpp.

enum class NetworkService;

// one game list cache entry (<GameCache><Entry>)
struct GameEntry
{
	GameEntry() = default;

	std::wstring rpx_file;
	std::wstring legacy_name;
	std::string product_code;
	std::string company_code;

	std::string custom_name;

	sint32 icon = -1;
	sint32 icon_small = -1;
	sint32 legacy_version = 0;
	sint32 legacy_update_version = -1;
	sint32 legacy_dlc_version = -1;
	uint64 title_id = 0;
	uint32 legacy_region = 0;

	std::wstring save_folder;
	std::wstring update_folder;
	std::wstring dlc_folder;

	uint64 legacy_time_played = 0;
	uint64 legacy_last_played = 0;

	bool favorite = false;

	bool is_update = false;
};

struct GraphicPackEntry
{
	GraphicPackEntry(std::wstring_view filename, std::string_view category, std::string_view preset)
		: filename(filename)
	{
		presets[std::string(category)] = preset;
	}

	GraphicPackEntry(std::wstring_view filename, std::string_view preset)
		: filename(filename)
	{
		presets[""] = preset;
	}

	GraphicPackEntry(std::wstring filename) : filename(std::move(filename)) {}

	struct Preset
	{
		std::string name;
		std::string category;
	};
	std::unordered_map<std::string, std::string> presets; // category -> active preset

	std::wstring filename;
	bool enabled = true;
};

// values are stored in settings.xml, do not renumber
enum GraphicAPI
{
	kOpenGL = 0,
	kVulkan,
	kMetal,
	COUNT
};

#if defined(ENABLE_VULKAN)
constexpr GraphicAPI kDefaultGraphicsAPI = kVulkan;
#elif defined(ENABLE_OPENGL)
constexpr GraphicAPI kDefaultGraphicsAPI = kOpenGL;
#endif

enum AudioChannels
{
	kMono = 0,
	kStereo,
	kSurround,
};

enum UpscalingFilter
{
	kLinearFilter,
	kBicubicFilter,
	kBicubicHermiteFilter,
	kNearestNeighborFilter,
};

enum FullscreenScaling
{
	kKeepAspectRatio,
	kStretch,
};

enum class ScreenPosition
{
	kDisabled = 0,
	kTopLeft,
	kTopCenter,
	kTopRight,
	kBottomLeft,
	kBottomCenter,
	kBottomRight,
};

enum class PrecompiledShaderOption
{
	Auto,
	Enable,
	Disable,
};
ENABLE_ENUM_ITERATORS(PrecompiledShaderOption, PrecompiledShaderOption::Auto, PrecompiledShaderOption::Disable);

enum class AccurateShaderMulOption
{
	False = 0, // plain IEEE multiplication
	True = 1   // emulate the GPU's non-IEEE MUL (0 * anything = 0)
};
ENABLE_ENUM_ITERATORS(AccurateShaderMulOption, AccurateShaderMulOption::False, AccurateShaderMulOption::True);

enum class PositionInvariance
{
	Auto,
	False,
	True,
};
ENABLE_ENUM_ITERATORS(PositionInvariance, PositionInvariance::False, PositionInvariance::True);

enum class CPUMode
{
	SinglecoreInterpreter = 0,
	SinglecoreRecompiler = 1,
	DualcoreRecompiler = 2, // no longer offered, treated as multi-core
	MulticoreRecompiler = 3,
	Auto = 4,
};
ENABLE_ENUM_ITERATORS(CPUMode, CPUMode::SinglecoreInterpreter, CPUMode::Auto);

// CPU mode values of older settings files
enum class CPUModeLegacy
{
	SinglecoreInterpreter = 0,
	SinglecoreRecompiler = 1,
	DualcoreRecompiler = 2,
	TriplecoreRecompiler = 3,
	Auto = 4
};
ENABLE_ENUM_ITERATORS(CPUModeLegacy, CPUModeLegacy::SinglecoreInterpreter, CPUModeLegacy::Auto);

enum class CafeConsoleRegion
{
	JPN = 0x1,
	USA = 0x2,
	EUR = 0x4,
	AUS_DEPR = 0x8,
	CHN = 0x10,
	KOR = 0x20,
	TWN = 0x40,
	Auto = 0xFF,
};
ENABLE_BITMASK_OPERATORS(CafeConsoleRegion);

enum class CafeConsoleLanguage
{
	JA = 0,
	EN = 1,
	FR = 2,
	DE = 3,
	IT = 4,
	ES = 5,
	ZH = 6,
	KO = 7,
	NL = 8,
	PT = 9,
	RU = 10,
	TW = 11,
};
ENABLE_ENUM_ITERATORS(CafeConsoleLanguage, CafeConsoleLanguage::JA, CafeConsoleLanguage::TW);

#if BOOST_OS_WINDOWS
enum class CrashDump
{
	Disabled,
	Lite,
	Full
};
ENABLE_ENUM_ITERATORS(CrashDump, CrashDump::Disabled, CrashDump::Full);
#elif BOOST_OS_UNIX
enum class CrashDump
{
	Disabled,
	Enabled
};
ENABLE_ENUM_ITERATORS(CrashDump, CrashDump::Disabled, CrashDump::Enabled);
#endif

// display names of the option enums (used by fmt and the settings UI)
namespace config_names
{
	constexpr const char* name(PrecompiledShaderOption v)
	{
		switch (v)
		{
		case PrecompiledShaderOption::Auto: return "auto";
		case PrecompiledShaderOption::Enable: return "true";
		case PrecompiledShaderOption::Disable: return "false";
		}
		return "unknown";
	}

	constexpr const char* name(AccurateShaderMulOption v)
	{
		switch (v)
		{
		case AccurateShaderMulOption::False: return "false";
		case AccurateShaderMulOption::True: return "true";
		}
		return "unknown";
	}

	constexpr const char* name(PositionInvariance v)
	{
		switch (v)
		{
		case PositionInvariance::Auto: return "auto";
		case PositionInvariance::False: return "false";
		case PositionInvariance::True: return "true";
		}
		return "unknown";
	}

	constexpr const char* name(CPUMode v)
	{
		switch (v)
		{
		case CPUMode::SinglecoreInterpreter: return "Single-core interpreter";
		case CPUMode::SinglecoreRecompiler: return "Single-core recompiler";
		case CPUMode::DualcoreRecompiler: return "Dual-core recompiler";
		case CPUMode::MulticoreRecompiler: return "Multi-core recompiler";
		case CPUMode::Auto: return "Auto";
		}
		return "unknown";
	}

	constexpr const char* name(CPUModeLegacy v)
	{
		switch (v)
		{
		case CPUModeLegacy::SinglecoreInterpreter: return "Singlecore-Interpreter";
		case CPUModeLegacy::SinglecoreRecompiler: return "Singlecore-Recompiler";
		case CPUModeLegacy::DualcoreRecompiler: return "Dualcore-Recompiler";
		case CPUModeLegacy::TriplecoreRecompiler: return "Triplecore-Recompiler";
		case CPUModeLegacy::Auto: return "Auto";
		}
		return "unknown";
	}

	// region names are translated by the UI
	constexpr const char* name(CafeConsoleRegion v)
	{
		switch (v)
		{
		case CafeConsoleRegion::JPN: return TR_NOOP("Japan");
		case CafeConsoleRegion::USA: return TR_NOOP("USA");
		case CafeConsoleRegion::EUR: return TR_NOOP("Europe");
		case CafeConsoleRegion::AUS_DEPR: return TR_NOOP("Australia");
		case CafeConsoleRegion::CHN: return TR_NOOP("China");
		case CafeConsoleRegion::KOR: return TR_NOOP("Korea");
		case CafeConsoleRegion::TWN: return TR_NOOP("Taiwan");
		case CafeConsoleRegion::Auto: return TR_NOOP("Auto");
		default: return TR_NOOP("many"); // a combination of regions
		}
	}

	constexpr const char* name(CafeConsoleLanguage v)
	{
		switch (v)
		{
		case CafeConsoleLanguage::JA: return "Japanese";
		case CafeConsoleLanguage::EN: return "English";
		case CafeConsoleLanguage::FR: return "French";
		case CafeConsoleLanguage::DE: return "German";
		case CafeConsoleLanguage::IT: return "Italian";
		case CafeConsoleLanguage::ES: return "Spanish";
		case CafeConsoleLanguage::ZH: return "Chinese";
		case CafeConsoleLanguage::KO: return "Korean";
		case CafeConsoleLanguage::NL: return "Dutch";
		case CafeConsoleLanguage::PT: return "Portugese";
		case CafeConsoleLanguage::RU: return "Russian";
		case CafeConsoleLanguage::TW: return "Taiwanese";
		}
		return "unknown";
	}

	constexpr const char* name(CrashDump v)
	{
		switch (v)
		{
		case CrashDump::Disabled: return "Disabled";
#if BOOST_OS_WINDOWS
		case CrashDump::Lite: return "Lite";
		case CrashDump::Full: return "Full";
#elif BOOST_OS_UNIX
		case CrashDump::Enabled: return "Enabled";
#endif
		}
		return "unknown";
	}

	// fmt formatter that prints config_names::name(value)
	template<typename TEnum>
	struct NameFormatter : fmt::formatter<fmt::string_view>
	{
		template<typename FormatContext>
		auto format(const TEnum v, FormatContext& ctx) const
		{
			return fmt::formatter<fmt::string_view>::format(fmt::string_view(name(v)), ctx);
		}
	};
}

template<> struct fmt::formatter<PrecompiledShaderOption> : config_names::NameFormatter<PrecompiledShaderOption> {};
template<> struct fmt::formatter<AccurateShaderMulOption> : config_names::NameFormatter<AccurateShaderMulOption> {};
template<> struct fmt::formatter<PositionInvariance> : config_names::NameFormatter<PositionInvariance> {};
template<> struct fmt::formatter<CPUMode> : config_names::NameFormatter<CPUMode> {};
template<> struct fmt::formatter<CPUModeLegacy> : config_names::NameFormatter<CPUModeLegacy> {};
template<> struct fmt::formatter<CafeConsoleRegion> : config_names::NameFormatter<CafeConsoleRegion> {};
template<> struct fmt::formatter<CafeConsoleLanguage> : config_names::NameFormatter<CafeConsoleLanguage> {};
template<> struct fmt::formatter<CrashDump> : config_names::NameFormatter<CrashDump> {};

struct CemuConfig
{
	CemuConfig() {}
	CemuConfig(const CemuConfig&) = delete;

	// stores the MLC path in the config (optionally saving it) and reloads the accounts
	void SetMLCPath(fs::path path, bool save = true);

	// general
	ConfigValue<uint64> log_flag{0};
	ConfigValue<bool> advanced_ppc_logging{false};

	ConfigValue<bool> permanent_storage{true};

	ConfigValue<std::string> mlc_path{};
	ConfigValue<std::string> proxy_server{};

	// off by default on macOS, where inhibiting the screensaver crashes
#if BOOST_OS_MACOS
	ConfigValue<bool> disable_screensaver{false};
#else
	ConfigValue<bool> disable_screensaver{true};
#endif
	ConfigValue<bool> play_boot_sound{false};

	// game list
	std::vector<std::string> game_paths;
	std::mutex game_cache_entries_mutex; // guards game_cache_entries and game_cache_favorites
	std::vector<GameEntry> game_cache_entries;
	std::set<uint64> game_cache_favorites; // title ids, for fast lookup

	struct _path_hash
	{
		std::size_t operator()(const fs::path& path) const { return fs::hash_value(path); }
	};
	// graphic pack file -> (category -> active preset); category "_disabled" marks a disabled pack
	std::unordered_map<fs::path, std::unordered_map<std::string, std::string>, _path_hash> graphic_pack_entries;

	ConfigValueBounds<CPUMode> cpu_mode{CPUMode::Auto};
	ConfigValueBounds<CafeConsoleLanguage> console_language{CafeConsoleLanguage::EN};

	// graphics
	ConfigValue<GraphicAPI> graphic_api{kDefaultGraphicsAPI};
	std::array<uint8, 16> legacy_graphic_device_uuid{}; // "device", from settings of version 2.6 and older (now "vkDevice")
	std::array<uint8, 16> vk_graphic_device_uuid;
	uint64 mtl_graphic_device_uuid{0};
	ConfigValue<int> vsync{0}; // 0 = off, other values depend on the renderer
	ConfigValue<bool> gx2drawdone_sync{true};
	ConfigValue<bool> render_upside_down{false};
	ConfigValue<bool> async_compile{true};

	// gamma
	ConfigValue<bool> overrideAppGammaPreference{false};
	ConfigValue<float> overrideGammaValue{2.2f};
	ConfigValue<float> userDisplayGamma{2.2f}; // 0 = sRGB curve, otherwise the display's gamma

	ConfigValue<bool> vk_accurate_barriers{true};

	struct
	{
		ScreenPosition position = ScreenPosition::kDisabled;
		uint32 text_color = 0xFFFFFFFF;
		sint32 text_scale = 100;
		bool fps = true;
		bool drawcalls = false;
		bool cpu_usage = false;
		bool cpu_per_core_usage = false;
		bool ram_usage = false;
		bool vram_usage = false;
		bool debug = false;
	} overlay{};

	struct
	{
		ScreenPosition position = ScreenPosition::kTopLeft;
		uint32 text_color = 0xFFFFFFFF;
		sint32 text_scale = 100;
		bool controller_profiles = true;
		bool controller_battery = false;
		bool shader_compiling = true;
		bool friends = true;
	} notification{};

	ConfigValue<sint32> upscale_filter{kBicubicFilter};
	ConfigValue<sint32> downscale_filter{kLinearFilter};
	ConfigValue<sint32> fullscreen_scaling{kKeepAspectRatio};

	// audio
	sint32 audio_api = 0;
	sint32 audio_delay = 2;
	AudioChannels tv_channels = kStereo, pad_channels = kStereo, input_channels = kMono;
	sint32 tv_volume = 50, pad_volume = 0, input_volume = 50, portal_volume = 50;
	std::wstring tv_device{L"default"}, pad_device, input_device, portal_device;

	// account
	struct
	{
		ConfigValueBounds<uint32> m_persistent_id{0x80000001, 0x80000001, 0xFFFFFFFF};
		ConfigValue<bool> legacy_online_enabled{false};
		ConfigValue<int> legacy_active_service{0};
		std::unordered_map<uint32, NetworkService> service_select; // persistent id -> network service
	} account{};

	// input
	struct
	{
		ConfigValue<std::string> host{"127.0.0.1"};
		ConfigValue<uint16> port{26760};
	} dsu_client{};

	// debug
	ConfigValueBounds<CrashDump> crash_dump{CrashDump::Disabled};
	ConfigValue<uint16> gdb_port{1337};

	XMLConfigParser Load(XMLConfigParser& parser);
	XMLConfigParser Save(XMLConfigParser& parser);

	bool IsGameListFavorite(uint64 titleId);
	void SetGameListFavorite(uint64 titleId, bool isFavorite);
	bool GetGameListCustomName(uint64 titleId, std::string& customName);
	void SetGameListCustomName(uint64 titleId, std::string customName);

	NetworkService GetAccountNetworkService(uint32 persistentId);
	void SetAccountSelectedService(uint32 persistentId, NetworkService serviceIndex);

	// emulated usb devices
	struct
	{
		ConfigValue<bool> emulate_skylander_portal{false};
		ConfigValue<bool> emulate_infinity_base{false};
		ConfigValue<bool> emulate_dimensions_toypad{false};
	} emulated_usb_devices{};

	// number of output channels for a channel setting (mono is played on both stereo channels)
	static int AudioChannelsToNChannels(AudioChannels channels)
	{
		if (channels == kMono)
			return 1;
		if (channels == kSurround)
			return 6;
		return 2;
	}

private:
	// both expect game_cache_entries_mutex to be held
	GameEntry* GetGameEntryByTitleId(uint64 titleId);
	GameEntry* CreateGameEntry(uint64 titleId);
};

typedef XMLDataConfig<CemuConfig> XMLCemuConfig_t;

inline XMLCemuConfig_t& GetConfigHandle()
{
	static XMLCemuConfig_t config;
	return config;
}

inline CemuConfig& GetConfig()
{
	return GetConfigHandle().data();
}
