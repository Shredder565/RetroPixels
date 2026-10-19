// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#endif
#include "Cafe/CafeSystem.h"
#include "Cemu/Logging/CemuLogging.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/Account/Account.h"
#include "util/helpers/helpers.h"

namespace
{
	bool isAnyOf(uint64 titleId, std::initializer_list<uint64> titleIds)
	{
		return std::find(titleIds.begin(), titleIds.end(), titleId) != titleIds.end();
	}

	bool isOnlineService(NetworkService service)
	{
		return service == NetworkService::Nintendo || service == NetworkService::Pretendo || service == NetworkService::Custom;
	}
}

void ActiveSettings::SetPaths(bool isPortableMode,
							  const fs::path& executablePath,
							  const fs::path& userDataPath,
							  const fs::path& configPath,
							  const fs::path& cachePath,
							  const fs::path& dataPath,
							  std::set<fs::path>& failedWriteAccess)
{
	cemu_assert_debug(!s_setPathsCalled); // the folders cannot change once in use
	s_isPortableMode = isPortableMode;
	s_executable_path = executablePath;
	s_executable_filename = executablePath.filename();
	s_user_data_path = userDataPath;
	s_config_path = configPath;
	s_cache_path = cachePath;
	s_data_path = dataPath;

	// the data folder may be read-only, the others must be writable
	failedWriteAccess.clear();
	for (const fs::path& dir : {userDataPath, configPath, cachePath})
	{
		std::error_code ec;
		if (!fs::exists(dir, ec))
			fs::create_directories(dir, ec);
		if (TestWriteAccess(dir))
			continue;
		cemuLog_log(LogType::Force, "Failed to write to {}", _pathToUtf8(dir));
		failedWriteAccess.insert(dir);
	}
	s_setPathsCalled = true;
}

[[nodiscard]] bool ActiveSettings::IsPortableMode()
{
	return s_isPortableMode;
}

void ActiveSettings::Init()
{
	cemu_assert_debug(s_setPathsCalled);
	std::string errorDetails;
	s_has_required_online_files = iosuCrypt_checkRequirementsForOnlineMode(errorDetails) == IOS_CRYPTO_ONLINE_REQ_OK;
}

bool ActiveSettings::LoadSharedLibrariesEnabled()
{
	return g_current_game_profile->ShouldLoadSharedLibraries().value_or(true);
}

bool ActiveSettings::DisplayDRCEnabled()
{
	return g_current_game_profile->StartWithGamepadView();
}

CPUMode ActiveSettings::GetCPUMode()
{
	const CPUMode mode = g_current_game_profile->GetCPUMode().value_or(CPUMode::Auto);
	switch (mode)
	{
	case CPUMode::Auto:
		// the multi-core recompiler needs at least four physical cores to pay off
		return GetPhysicalCoreCount() >= 4 ? CPUMode::MulticoreRecompiler : CPUMode::SinglecoreRecompiler;
	case CPUMode::DualcoreRecompiler: // retired, mapped to multi-core
		return CPUMode::MulticoreRecompiler;
	default:
		return mode;
	}
}

uint8 ActiveSettings::GetTimerShiftFactor()
{
	return s_timer_shift;
}

void ActiveSettings::SetTimerShiftFactor(uint8 shiftFactor)
{
	s_timer_shift = shiftFactor;
}

PrecompiledShaderOption ActiveSettings::GetPrecompiledShadersOption()
{
	return PrecompiledShaderOption::Auto; // not configurable at the moment
}

bool ActiveSettings::RenderUpsideDownEnabled()
{
	return LaunchSettings::RenderUpsideDownEnabled().value_or(GetConfig().render_upside_down);
}

bool ActiveSettings::WaitForGX2DrawDoneEnabled()
{
	return GetConfig().gx2drawdone_sync;
}

// The game profile's API, else the configured one; --rp-opengl overrides both.
// If that API is not available, the first available of Vulkan, Metal, OpenGL
// is used.
GraphicAPI ActiveSettings::GetGraphicsAPI()
{
	GraphicAPI wanted = g_current_game_profile->GetGraphicsAPI().value_or(GetConfig().graphic_api);
#ifdef ENABLE_OPENGL
	if (LaunchSettings::RPForceOpenGL()) // PixelU: RetroPixels asked for OpenGL (Vulkan does not work on this PC)
		wanted = kOpenGL;
#endif
	std::optional<GraphicAPI> firstAvailable;
	auto offer = [&](GraphicAPI api) -> bool {
		if (wanted == api)
			return true;
		if (!firstAvailable)
			firstAvailable = api;
		return false;
	};
#ifdef ENABLE_VULKAN
	if (g_vulkan_available && offer(kVulkan))
		return kVulkan;
#endif
#ifdef ENABLE_METAL
	if (offer(kMetal))
		return kMetal;
#endif
#ifdef ENABLE_OPENGL
	if (offer(kOpenGL))
		return kOpenGL;
#endif
	cemu_assert(firstAvailable.has_value());
	return *firstAvailable;
}

// the game's gamma unless overridden, plus the configured value
float ActiveSettings::GetTVGamma()
{
	const CemuConfig& cfg = GetConfig();
	const float gameGamma = cfg.overrideAppGammaPreference.GetValue() ? 0.0f : LatteGPUState.tvGamma;
	return cfg.overrideGammaValue.GetValue() + gameGamma;
}

float ActiveSettings::GetDRCGamma()
{
	const CemuConfig& cfg = GetConfig();
	const float gameGamma = cfg.overrideAppGammaPreference.GetValue() ? 0.0f : LatteGPUState.drcGamma;
	return cfg.overrideGammaValue.GetValue() + gameGamma;
}

bool ActiveSettings::AudioOutputOnlyAux()
{
	return s_audio_aux_only;
}

void ActiveSettings::EnableAudioOnlyAux(bool state)
{
	s_audio_aux_only = state;
}

uint32 ActiveSettings::GetPersistentId()
{
	return LaunchSettings::GetPersistentId().value_or(GetConfig().account.m_persistent_id);
}

bool ActiveSettings::IsOnlineEnabled()
{
	if (!Account::GetAccount(GetPersistentId()).IsValidOnlineAccount())
		return false;
	if (!HasRequiredOnlineFiles())
		return false;
	return isOnlineService(GetConfig().GetAccountNetworkService(GetPersistentId()));
}

bool ActiveSettings::HasRequiredOnlineFiles()
{
	return s_has_required_online_files;
}

NetworkService ActiveSettings::GetNetworkService()
{
	return GetConfig().GetAccountNetworkService(GetPersistentId());
}

bool ActiveSettings::DumpShadersEnabled() { return s_dump_shaders; }
bool ActiveSettings::DumpTexturesEnabled() { return s_dump_textures; }
bool ActiveSettings::DumpRecompilerFunctionsEnabled() { return s_dump_recompiler_functions; }
bool ActiveSettings::DumpLibcurlRequestsEnabled() { return s_dump_libcurl_requests; }
void ActiveSettings::EnableDumpShaders(bool state) { s_dump_shaders = state; }
void ActiveSettings::EnableDumpTextures(bool state) { s_dump_textures = state; }
void ActiveSettings::EnableDumpRecompilerFunctions(bool state) { s_dump_recompiler_functions = state; }
void ActiveSettings::EnableDumpLibcurlRequests(bool state) { s_dump_libcurl_requests = state; }

// Art Academy calls VPADRead in a tight loop
bool ActiveSettings::VPADDelayEnabled()
{
	return isAnyOf(CafeSystem::GetForegroundTitleId(), {
		0x000500001017BF00, // Art Academy: Home Studio (US)
		0x000500001017BE00, // Art Academy: Home Studio (JP)
		0x000500001017B500, // Art Academy: Atelier (EU)
	});
}

// games with shaders that can loop forever (a guard is added to loops)
bool ActiveSettings::ShaderPreventInfiniteLoopsEnabled()
{
	return isAnyOf(CafeSystem::GetForegroundTitleId(), {
		0x0005000010101C00, // New Super Mario Bros. U (JP)
		0x0005000010101D00, // New Super Mario Bros. U (US)
		0x0005000010101E00, // New Super Mario Bros. U (EU)
		0x000500001014B700, // New Super Mario Bros. U + Luigi (US)
		0x000500001014B800, // New Super Mario Bros. U + Luigi (EU)
		0x0005000010142300, // New Super Luigi U (US)
		0x0005000010142400, // New Super Luigi U (EU)
		0x00050000101D0300, // Project Zero: Maiden of Black Water (EU)
		0x00050000101D0600, // Fatal Frame: Maiden of Black Water (US)
		0x000500001014D200, // Project Zero: Maiden of Black Water (JP)
		0x00050000101D3F00, // Project Zero: Maiden of Black Water (trial, EU)
	});
}

// games that need the GPU caches flushed between frames
bool ActiveSettings::FlushGPUCacheOnSwap()
{
	return isAnyOf(CafeSystem::GetForegroundTitleId(), {
		0x0005000010147900, // PAC-MAN and the Ghostly Adventures (EU)
		0x0005000010146300, // PAC-MAN and the Ghostly Adventures (US)
		0x000500001017E500, // PAC-MAN and the Ghostly Adventures 2 (EU)
		0x000500001017E600, // PAC-MAN and the Ghostly Adventures 2 (US)
	});
}

// Games that sample exactly on texel edges. The console's sampler rounds
// there, while host GPUs truncate when converting coordinates, which picks
// the neighbouring texel.
bool ActiveSettings::ForceSamplerRoundToPrecision()
{
	return isAnyOf(CafeSystem::GetForegroundTitleId(), {
		0x000500001014E100, // Adventure Time: Explore the Dungeon... (EU)
		0x0005000010144000, // Adventure Time: Explore the Dungeon... (US)
		0x0005000010129200, // DuckTales: Remastered (EU)
		0x0005000010129000, // DuckTales: Remastered (US)
	});
}

fs::path ActiveSettings::GetMlcPath()
{
	cemu_assert_debug(s_setPathsCalled);
	if (const auto fromCommandLine = LaunchSettings::GetMLCPath())
		return *fromCommandLine;
	if (const std::string fromConfig = GetConfig().mlc_path.GetValue(); !fromConfig.empty())
		return _utf8ToPath(fromConfig);
	return GetDefaultMLCPath();
}

bool ActiveSettings::IsCustomMlcPath()
{
	cemu_assert_debug(s_setPathsCalled);
	return !GetConfig().mlc_path.GetValue().empty();
}

bool ActiveSettings::IsCommandLineMlcPath()
{
	return LaunchSettings::GetMLCPath().has_value();
}

fs::path ActiveSettings::GetDefaultMLCPath()
{
	return GetUserDataPath("mlc01");
}
