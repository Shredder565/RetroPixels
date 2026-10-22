// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"

// The settings currently in effect, combining settings.xml, the command line
// and the running game's profile. Also owns the user/config/cache/data
// folder locations.
class ActiveSettings
{
private:
	// base / formatted sub path (UTF-8 format string, fmt syntax)
	template<typename... TArgs>
	static fs::path GetPath(const fs::path& path, std::string_view format, TArgs&&... args)
	{
		cemu_assert_debug(format.empty() || (format[0] != '/' && format[0] != '\\')); // must be relative
		return path / _utf8ToPath(fmt::format(fmt::runtime(format), std::forward<TArgs>(args)...));
	}

	// base / formatted sub path (wide format string)
	template<typename... TArgs>
	static fs::path GetPath(const fs::path& path, std::wstring_view format, TArgs&&... args)
	{
		cemu_assert_debug(format.empty() || (format[0] != L'/' && format[0] != L'\\')); // must be relative
		return path / fmt::format(fmt::runtime(format), std::forward<TArgs>(args)...);
	}

	// base / UTF-8 sub path, used as is
	static fs::path GetPath(const fs::path& path, std::string_view subPath)
	{
		return path / fs::path(std::u8string_view((const char8_t*)subPath.data(), subPath.size()));
	}

	static fs::path GetPath(const fs::path& path)
	{
		return path;
	}

public:
	// Sets the folders (once, before Init) and creates the writable ones.
	// failedWriteAccess receives the folders that cannot be written to.
	static void
	SetPaths(bool isPortableMode,
			 const fs::path& executablePath,
			 const fs::path& userDataPath,
			 const fs::path& configPath,
			 const fs::path& cachePath,
			 const fs::path& dataPath,
			 std::set<fs::path>& failedWriteAccess);

	static void Init();

	[[nodiscard]] static fs::path GetExecutablePath() { return s_executable_path; }
	[[nodiscard]] static fs::path GetExecutableFilename() { return s_executable_filename; }
	template<typename... TArgs>
	[[nodiscard]] static fs::path GetUserDataPath(TArgs&&... args) { return GetPath(s_user_data_path, std::forward<TArgs>(args)...); }
	template<typename... TArgs>
	[[nodiscard]] static fs::path GetConfigPath(TArgs&&... args) { return GetPath(s_config_path, std::forward<TArgs>(args)...); }
	template<typename... TArgs>
	[[nodiscard]] static fs::path GetCachePath(TArgs&&... args) { return GetPath(s_cache_path, std::forward<TArgs>(args)...); }
	template<typename... TArgs>
	[[nodiscard]] static fs::path GetDataPath(TArgs&&... args) { return GetPath(s_data_path, std::forward<TArgs>(args)...); }

	// command line, then settings.xml, then <user data>/mlc01
	[[nodiscard]] static fs::path GetMlcPath();

	template<typename... TArgs>
	[[nodiscard]] static fs::path GetMlcPath(TArgs&&... args) { return GetPath(GetMlcPath(), std::forward<TArgs>(args)...); }
	static bool IsCustomMlcPath();
	static bool IsCommandLineMlcPath();

	// <user data>/mlc01
	[[nodiscard]] static fs::path GetDefaultMLCPath();

private:
	inline static bool s_isPortableMode{false};
	inline static fs::path s_executable_path;
	inline static fs::path s_user_data_path;
	inline static fs::path s_config_path;
	inline static fs::path s_cache_path;
	inline static fs::path s_data_path;
	inline static fs::path s_executable_filename; // file name part of s_executable_path
	inline static fs::path s_mlc_path;

public:
	// valid before Init
	[[nodiscard]] static bool IsPortableMode();

	// general
	[[nodiscard]] static bool LoadSharedLibrariesEnabled();
	[[nodiscard]] static bool DisplayDRCEnabled();

	// cpu
	[[nodiscard]] static CPUMode GetCPUMode();
	[[nodiscard]] static uint8 GetTimerShiftFactor();

	static void SetTimerShiftFactor(uint8 shiftFactor);

	// gpu
	[[nodiscard]] static PrecompiledShaderOption GetPrecompiledShadersOption();
	[[nodiscard]] static bool RenderUpsideDownEnabled();
	[[nodiscard]] static bool WaitForGX2DrawDoneEnabled();
	[[nodiscard]] static GraphicAPI GetGraphicsAPI();

	// gamma
	[[nodiscard]] static float GetTVGamma();
	[[nodiscard]] static float GetDRCGamma();

	// audio
	[[nodiscard]] static bool AudioOutputOnlyAux();
	static void EnableAudioOnlyAux(bool state);

	// account
	[[nodiscard]] static uint32 GetPersistentId();
	[[nodiscard]] static bool IsOnlineEnabled();
	[[nodiscard]] static bool HasRequiredOnlineFiles();
	[[nodiscard]] static NetworkService GetNetworkService();

	// dump options
	[[nodiscard]] static bool DumpShadersEnabled();
	[[nodiscard]] static bool DumpTexturesEnabled();
	[[nodiscard]] static bool DumpRecompilerFunctionsEnabled();
	[[nodiscard]] static bool DumpLibcurlRequestsEnabled();
	static void EnableDumpShaders(bool state);
	static void EnableDumpTextures(bool state);
	static void EnableDumpRecompilerFunctions(bool state);
	static void EnableDumpLibcurlRequests(bool state);

	// per-game workarounds
	[[nodiscard]] static bool VPADDelayEnabled();
	[[nodiscard]] static bool ShaderPreventInfiniteLoopsEnabled();
	[[nodiscard]] static bool FlushGPUCacheOnSwap();
	[[nodiscard]] static bool ForceSamplerRoundToPrecision();

private:
	inline static bool s_setPathsCalled = false;

	inline static bool s_dump_shaders = false;
	inline static bool s_dump_textures = false;
	inline static bool s_dump_recompiler_functions = false;
	inline static bool s_dump_libcurl_requests = false;

	// guest timer speed as a right shift: 0 = 8x, 3 = 1x (normal), 4 = 0.5x
	inline static uint8 s_timer_shift = 3;

	inline static bool s_audio_aux_only = false;

	inline static bool s_has_required_online_files = false;
};
