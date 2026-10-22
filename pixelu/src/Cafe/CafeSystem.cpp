// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "WindowSystem.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "util/helpers/SystemException.h"
#include "Common/cpu_features.h"
#include "input/InputManager.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"
#include "Cafe/Filesystem/FST/FST.h"
#include "Common/FileStream.h"
#include "GamePatch.h"
#include "HW/Espresso/Debugger/GDBStub.h"

// IOSU services
#include "Cafe/IOSU/legacy/iosu_ioctl.h"
#include "Cafe/IOSU/legacy/iosu_act.h"
#include "Cafe/IOSU/legacy/iosu_fpd.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/IOSU/legacy/iosu_mcp.h"
#include "Cafe/IOSU/legacy/iosu_acp.h"
#include "Cafe/IOSU/legacy/iosu_nim.h"
#include "Cafe/IOSU/PDM/iosu_pdm.h"
#include "Cafe/IOSU/ccr_nfc/iosu_ccr_nfc.h"
#include "Cafe/IOSU/nn/boss/boss_service.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/IOSU/fsa/iosu_fsa.h"
#include "Cafe/IOSU/ODM/iosu_odm.h"

// Cafe OS libraries that keep state across a title
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/gx2/GX2_Misc.h"
#include "Cafe/OS/libs/nn_save/nn_save.h"

#include "Cafe/HW/SI/si.h"

#include <time.h>

#if BOOST_OS_LINUX
#include <sys/sysinfo.h>
#elif BOOST_OS_MACOS || BOOST_OS_BSD
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

// virtual path of the running executable (coreinit reads it for argv[0])
std::string _pathToExecutable;
// entry point of the main executable (coreinit jumps there)
MPTR _entryPoint = MPTR_NULL;
// the executable is a plain ELF (homebrew), not an RPX
bool isLaunchTypeELF = false;
RPLModule* applicationRPX = nullptr;

namespace
{
	constexpr const char* kCodeFolder = "/internal/current_title/code/";
	constexpr const char* kStandaloneCodeFolder = "/internal/code/";
	constexpr uint32 kSharedAreaBase = 0xF8000000;

	std::string s_baseExecutablePath; // executable path before cos.xml may have changed it
	uint32 s_rpxHashBase = 0;
	uint32 s_rpxHashUpdated = 0;

	// checksum of an executable file; graphic packs and game fixes identify executables by it
	uint32 HashExecutable(const uint8* data, size_t size)
	{
		uint32 h = 0x3416DCBF;
		for (size_t i = 0; i < size; i++)
			h = std::rotl(h, 3) + data[i];
		return h;
	}

	// first *.rpx file in the code folder
	std::optional<std::string> FindRpxInCodeFolder()
	{
		sint32 status = 0;
		FSCVirtualFile* dir = fsc_openDirIterator(kCodeFolder, &status);
		if (!dir)
			return std::nullopt;
		std::optional<std::string> found;
		FSCDirEntry entry;
		while (!found && fsc_nextDir(dir, &entry))
		{
			const std::string_view name = entry.path;
			if (name.size() >= 4 && boost::iequals(name.substr(name.size() - 4), ".rpx"))
				found = std::string(kCodeFolder) + entry.path;
		}
		fsc_close(dir);
		return found;
	}

	[[noreturn]] void FatalLaunchError(const std::string& message)
	{
		cemuLog_log(LogType::Force, message);
		cemuLog_waitForFlush();
		cemu_assert(false);
		exit(1); // not reached
	}

	// maps the main executable into guest memory and records its entry point and checksums
	void LoadMainExecutable()
	{
		isLaunchTypeELF = false;
		if (_pathToExecutable.empty())
		{
			// disc images give no path up front
			auto rpx = FindRpxInCodeFolder();
			if (!rpx)
				FatalLaunchError("Launch failed: no .rpx file in the code folder");
			_pathToExecutable = *rpx;
		}

		uint32 size = 0;
		uint8* data = fsc_extractFile(_pathToExecutable.c_str(), &size);
		if (!data)
			FatalLaunchError(fmt::format("Launch failed: cannot read {}", _pathToExecutable));
		s_rpxHashUpdated = HashExecutable(data, size);

		static constexpr uint8 kElfBigEndian32[] = {0x7F, 'E', 'L', 'F', 0x01, 0x02, 0x01, 0x00, 0x00};
		if (size >= 10 && std::memcmp(data, kElfBigEndian32, sizeof(kElfBigEndian32)) == 0)
		{
			_entryPoint = ELF_LoadFromMemory(data, size, _pathToExecutable.c_str());
			isLaunchTypeELF = true;
		}
		else
		{
			RPLLoader_AddDependency(_pathToExecutable.c_str(), true);
			applicationRPX = RPLLoader_LoadFromMemory(data, size, (char*)_pathToExecutable.c_str());
			if (!applicationRPX)
			{
				WindowSystem::ShowErrorDialog(_tr("Failed to run this title because the executable is damaged"));
				cemuLog_createLogFile(false);
				cemuLog_waitForFlush();
				exit(0);
			}
			RPLLoader_SetMainModule(applicationRPX);
			_entryPoint = RPLLoader_GetModuleEntrypoint(applicationRPX);
		}
		free(data);

		// checksum of the executable as shipped, without the update
		const std::string& basePath = s_baseExecutablePath.empty() ? _pathToExecutable : s_baseExecutablePath;
		uint32 baseSize = 0;
		uint8* baseData = fsc_extractFile(basePath.c_str(), &baseSize, FSC_PRIORITY_BASE);
		s_rpxHashBase = baseData ? HashExecutable(baseData, baseSize) : s_rpxHashUpdated;
		free(baseData);
	}

	// ---------------------------------------------------------------- log output

	void LogLoadedTitle()
	{
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		cemuLog_log(LogType::Force, "------- Title -------");
		cemuLog_log(LogType::Force, "Title ID: {:08x}-{:08x}", (uint32)(titleId >> 32), (uint32)titleId);
		cemuLog_log(LogType::Force, "Version: v{}", CafeSystem::GetForegroundTitleVersion());
		switch (CafeSystem::GetForegroundTitleRegion())
		{
		case CafeConsoleRegion::JPN: cemuLog_log(LogType::Force, "Region: JP"); break;
		case CafeConsoleRegion::EUR: cemuLog_log(LogType::Force, "Region: EU"); break;
		case CafeConsoleRegion::USA: cemuLog_log(LogType::Force, "Region: US"); break;
		default: break;
		}

		const fs::path savePath = ActiveSettings::GetMlcPath("usr/save/{:08X}/{:08X}/user/", (uint32)(titleId >> 32), (uint32)titleId);
		std::error_code ec;
		cemuLog_log(LogType::Force, "Save folder: {}{}", _pathToUtf8(savePath), fs::exists(savePath, ec) ? "" : " (does not exist yet)");
		cemuLog_log(LogType::Force, "Shader cache: shaderCache/transferable/{:016x}.bin", titleId);
		if (g_current_game_profile->IsLoaded())
		{
			const char* folder = g_current_game_profile->IsDefaultProfile() ? "gameProfiles/default" : "gameProfiles";
			cemuLog_log(LogType::Force, "Game profile: {}/{:016x}.ini", folder, titleId);
		}
		else
			cemuLog_log(LogType::Force, "Game profile: none");
		cemuLog_log(LogType::Force, "RPX hash: {:08x} (base {:08x})", s_rpxHashUpdated, s_rpxHashBase);

		memory_logModifiedMemoryRanges();
	}

	void LogActiveSettings()
	{
		const auto& config = GetConfig();
		auto fromProfile = [](bool set) { return set ? " (game profile)" : ""; };
		cemuLog_log(LogType::Force, "------- Settings -------");
		cemuLog_log(LogType::Force, "CPU mode: {}{}", fmt::format("{}", ActiveSettings::GetCPUMode()), fromProfile(g_current_game_profile->GetCPUMode().has_value()));
		cemuLog_log(LogType::Force, "Shared libraries: {}{}", ActiveSettings::LoadSharedLibrariesEnabled(), fromProfile(g_current_game_profile->ShouldLoadSharedLibraries().has_value()));
		cemuLog_log(LogType::Force, "Precompiled shaders: {}{}", fmt::format("{}", ActiveSettings::GetPrecompiledShadersOption()), fromProfile(g_current_game_profile->GetPrecompiledShadersState().has_value()));
		cemuLog_log(LogType::Force, "Full sync at GX2DrawDone: {}", ActiveSettings::WaitForGX2DrawDoneEnabled());
		cemuLog_log(LogType::Force, "Accurate shader multiplication: {}", g_current_game_profile->GetAccurateShaderMul() == AccurateShaderMulOption::True);
		const GraphicAPI api = ActiveSettings::GetGraphicsAPI();
		if (api == GraphicAPI::kVulkan)
		{
			cemuLog_log(LogType::Force, "Async shader compile: {}", config.async_compile.GetValue());
			if (!config.vk_accurate_barriers.GetValue())
				cemuLog_log(LogType::Force, "Accurate barriers: off");
		}
		cemuLog_log(LogType::Force, "Console language: {}", stdx::to_underlying(config.console_language.GetValue()));
	}

	std::string DescribeHostOS()
	{
#if BOOST_OS_WINDOWS
		auto readValue = [](HKEY key, const char* name) -> std::string {
			char buffer[256];
			DWORD type = REG_SZ;
			DWORD size = sizeof(buffer);
			if (RegQueryValueExA(key, name, nullptr, &type, (LPBYTE)buffer, &size) != ERROR_SUCCESS || type != REG_SZ)
				return {};
			return std::string(buffer, strnlen(buffer, std::min<size_t>(size, sizeof(buffer))));
		};
		std::string product = "Windows";
		std::string feature;
		uint32 build = 0;
		HKEY key;
		if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
		{
			if (std::string p = readValue(key, "ProductName"); !p.empty())
				product = p;
			build = (uint32)atoi(readValue(key, "CurrentBuildNumber").c_str());
			feature = readValue(key, "DisplayVersion");
			if (feature.empty())
				feature = readValue(key, "ReleaseId");
			RegCloseKey(key);
		}
		// Windows 11 still reports itself as "Windows 10" in ProductName
		if (build >= 22000)
		{
			if (size_t pos = product.find("Windows 10"); pos != std::string::npos)
				product.replace(pos, 10, "Windows 11");
		}
		if (!feature.empty())
			product += " " + feature;
		return fmt::format("{} (Build {})", product, build);
#elif BOOST_OS_LINUX
		if (getenv("APPIMAGE"))
			return "Linux (AppImage)";
		if (getenv("SNAP"))
			return "Linux (Snap)";
		if (const char* container = getenv("container"))
			return strcmp(container, "flatpak") == 0 ? "Linux (Flatpak)" : fmt::format("Linux ({})", container);
		return "Linux";
#elif BOOST_OS_MACOS
		auto query = [](const char* name) -> std::string {
			char buffer[256]{};
			size_t size = sizeof(buffer);
			if (sysctlbyname(name, buffer, &size, nullptr, 0) != 0)
				return {};
			return buffer;
		};
		const std::string version = query("kern.osproductversion");
		const std::string build = query("kern.osversion");
		if (version.empty())
			return "macOS";
		return build.empty() ? fmt::format("macOS {}", version) : fmt::format("macOS {} ({})", version, build);
#elif defined(__FreeBSD__)
		return "FreeBSD";
#elif defined(__OpenBSD__)
		return "OpenBSD";
#elif defined(__NetBSD__)
		return "NetBSD";
#else
		return "unknown";
#endif
	}

	std::optional<uint64> HostMemoryMB()
	{
#if BOOST_OS_WINDOWS
		MEMORYSTATUSEX status;
		status.dwLength = sizeof(status);
		if (!GlobalMemoryStatusEx(&status))
			return std::nullopt;
		return status.ullTotalPhys / (1024 * 1024);
#elif BOOST_OS_LINUX
		struct sysinfo info{};
		if (sysinfo(&info) != 0)
			return std::nullopt;
		return (uint64)info.totalram * info.mem_unit / (1024 * 1024);
#elif BOOST_OS_MACOS || BOOST_OS_BSD
		int64_t bytes = 0;
		size_t size = sizeof(bytes);
#if BOOST_OS_MACOS
		const char* name = "hw.memsize";
#else
		const char* name = "hw.physmem";
#endif
		if (sysctlbyname(name, &bytes, &size, nullptr, 0) != 0)
			return std::nullopt;
		return (uint64)bytes / (1024 * 1024);
#else
		return std::nullopt;
#endif
	}

	void LogHostInfo()
	{
#if BOOST_OS_WINDOWS
		if (HMODULE ntdll = GetModuleHandleA("ntdll.dll"))
		{
			using WineVersionFn = const char*(__cdecl*)();
			if (auto wineVersion = (WineVersionFn)GetProcAddress(ntdll, "wine_get_version"))
				cemuLog_log(LogType::Force, "Wine: {}", wineVersion());
		}
#endif
		if (std::string cpu = g_CPUFeatures.GetCPUName(); !cpu.empty())
			cemuLog_log(LogType::Force, "CPU: {}", cpu);
		if (auto ram = HostMemoryMB())
			cemuLog_log(LogType::Force, "RAM: {}MB", *ram);
		cemuLog_log(LogType::Force, "OS: {}", DescribeHostOS());
		cemuLog_log(LogType::Force, "CPU extensions in use: {}", g_CPUFeatures.GetCommaSeparatedExtensionList());
	}

	// ---------------------------------------------------------------- shared data (system fonts)

	// entry of the table at the start of the shared data area
	struct SharedFontEntry
	{
		/* +0x00 */ uint32be id;
		/* +0x04 */ uint32be kind; // 2 = font
		/* +0x08 */ uint32be kernelName;
		/* +0x0C */ MEMPTR<void> data;
		/* +0x10 */ uint32be size;
		/* +0x14 */ uint32be reserved14;
		/* +0x18 */ uint32be reserved18;
	};
	static_assert(sizeof(SharedFontEntry) == 0x1C);

	struct SharedFontFile
	{
		uint32 id;
		const char* fileName;
	};
	// the system fonts title (0005001B-10042400) holds these files
	constexpr SharedFontFile kSharedFonts[] = {
		{0xFFCAFE01, "CafeCn.ttf"},
		{0xFFCAFE02, "CafeKr.ttf"},
		{0xFFCAFE03, "CafeStd.ttf"},
		{0xFFCAFE04, "CafeTw.ttf"},
	};

	fs::path SharedFontInMlc(const SharedFontFile& font)
	{
		return ActiveSettings::GetMlcPath(fmt::format("sys/title/0005001b/10042400/content/{}", font.fileName));
	}

	fs::path SharedFontInResources(const SharedFontFile& font)
	{
		return ActiveSettings::GetDataPath(fmt::format("resources/sharedFonts/{}", font.fileName));
	}

	// Fills the shared data area with the system fonts (from the MLC, else from the bundled copies).
	// Without all fonts a dump of the area (shareddata.bin) is used if present.
	void LoadSharedData()
	{
		bool allFontsPresent = true;
		for (const SharedFontFile& font : kSharedFonts)
		{
			if (!fs::exists(SharedFontInMlc(font)) && !fs::exists(SharedFontInResources(font)))
			{
				cemuLog_log(LogType::Force, "Shared font {} not found", font.fileName);
				allFontsPresent = false;
				break;
			}
		}
		if (allFontsPresent)
		{
			auto* table = (SharedFontEntry*)memory_getPointerFromVirtualOffset(kSharedAreaBase);
			std::memset(table, 0, sizeof(SharedFontEntry) * std::size(kSharedFonts));
			uint8* writePtr = (uint8*)(table + std::size(kSharedFonts));
			for (size_t i = 0; i < std::size(kSharedFonts); i++)
			{
				const SharedFontFile& font = kSharedFonts[i];
				std::unique_ptr<FileStream> file(FileStream::openFile2(SharedFontInMlc(font)));
				if (!file)
					file.reset(FileStream::openFile2(SharedFontInResources(font)));
				if (!file)
				{
					cemuLog_log(LogType::Force, "Shared font {} could not be opened", font.fileName);
					continue;
				}
				const uint32 size = (uint32)file->GetSize();
				file->readData(writePtr, size);
				SharedFontEntry& entry = table[i];
				entry.id = font.id;
				entry.kind = 2;
				entry.kernelName = 0;
				entry.data = writePtr;
				entry.size = size;
				entry.reserved14 = 0;
				entry.reserved18 = 0;
				writePtr += (size + 15) & ~15u;
			}
			cemuLog_log(LogType::Force, "Shared data: system fonts loaded ({}KB)", (uint32)(writePtr - (uint8*)table) / 1024);
			return;
		}
		std::unique_ptr<FileStream> dump(FileStream::openFile2(ActiveSettings::GetUserDataPath("shareddata.bin")));
		if (dump)
			dump->readData(memory_getPointerFromVirtualOffset(kSharedAreaBase), 0x02000000);
	}

	// ---------------------------------------------------------------- title start

	// guest clocks start at the host's current time (UTC and local)
	void InitGuestClocks()
	{
		const time_t sinceY2k = time(nullptr) - 946684800;
		ppcCyclesSince2000_UTC = (uint64)sinceY2k * (uint64)ESPRESSO_CORE_CLOCK;
		time_t local = sinceY2k;
		tm* parts = localtime(&local);
#if BOOST_OS_WINDOWS
		local = _mkgmtime(parts);
#else
		local = timegm(parts);
#endif
		ppcCyclesSince2000 = local * (uint64)ESPRESSO_CORE_CLOCK;
		ppcCyclesSince2000TimerClock = ppcCyclesSince2000 / 20ULL;
	}

	// runs on the title's own thread, before the scheduler takes over
	void StartGame()
	{
		WindowSystem::UpdateWindowTitles(false, true, 0.0);
		cemuLog_createLogFile(false);
		InputManager::instance().apply_game_profile();
		InitGuestClocks();
		PPCTimer_start();

		RPLLoader_LoadCoreinit();
		LoadMainExecutable();
		LogLoadedTitle();

		const uint32 linkStart = GetTickCount();
		RPLLoader_UpdateDependencies();
		RPLLoader_Link();
		RPLLoader_NotifyControlPassedToApplication();
		cemuLog_log(LogType::Force, "Linking took {}ms", GetTickCount() - linkStart);

		if (isLaunchTypeELF)
		{
			// homebrew ELFs find OSDynLoad through two fixed slots
			memory_writeU32(0x801500, rpl_mapHLEImport(nullptr, "coreinit", "OSDynLoad_Acquire", true));
			memory_writeU32(0x801504, rpl_mapHLEImport(nullptr, "coreinit", "OSDynLoad_FindExport", true));
		}
		else
			GamePatch_scan();

		LatteGPUState.isDRCPrimary = ActiveSettings::DisplayDRCEnabled();
		LogActiveSettings();
		Latte_Start();

		if (g_gdbstub)
		{
			g_gdbstub->HandleEntryStop(_entryPoint);
			g_gdbstub->Initialize();
		}
		debugger_handleEntryBreakpoint(_entryPoint);

		cemuLog_log(LogType::Force, "------- Graphic packs -------");
		GraphicPack2::ActivateForCurrentTitle();
		IAudioAPI::PrintLogging();
		IAudioInputAPI::PrintLogging();
		cemuLog_log(LogType::Force, "------- Running -------");

		while (!g_isGPUInitFinished)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		RPLLoader_CallCoreinitEntrypoint();
		snd_core::AXOut_init();
	}
}

namespace CafeSystem
{
	namespace
	{
		bool s_initialized = false;
		SystemImplementation* s_frontend = nullptr;
		bool s_running = false;
		bool s_standalone = false; // started from a loose executable without title meta data
		TitleId s_titleId = 0;
		GameInfo2 s_gameInfo;
		std::optional<std::vector<std::string>> s_overrideArgs;
		std::optional<sint32> s_returnStatus;
		// titles mounted under /vol/storage_mlc01/ (each a private copy of the title's TitleInfo)
		std::map<TitleId, std::unique_ptr<TitleInfo>> s_mlcTitles;

		// IOSU modules, started in this order and stopped in reverse
		std::vector<IOSUModule*> s_iosuModules = {
			iosu::kernel::GetModule(),
			iosu::acp::GetModule(),
			iosu::fpd::GetModule(),
			iosu::pdm::GetModule(),
			iosu::ccr_nfc::GetModule(),
			iosu::boss::GetModule(),
		};

		void MountHostFolders()
		{
			const fs::path mlc = ActiveSettings::GetMlcPath();
			FSCDeviceHostFS_Mount("/cemuBossStorage/", _pathToUtf8(mlc / "usr/boss/"), FSC_PRIORITY_BASE);
			FSCDeviceHostFS_Mount("/vol/storage_mlc01/", _pathToUtf8(mlc / ""), FSC_PRIORITY_BASE);
		}

		void UnmountHostFolders()
		{
			fsc_unmount("/vol/storage_mlc01/", FSC_PRIORITY_BASE);
			fsc_unmount("/cemuBossStorage/", FSC_PRIORITY_BASE);
		}

		// folders given on the command line
		void MountLaunchFolders()
		{
			for (const auto& [hostPath, guestPath] : LaunchSettings::CosMounts())
				FSCDeviceHostFS_Mount(boost::nowide::narrow(guestPath), _pathToUtf8(hostPath), FSC_PRIORITY_BASE);
		}

		void UnmountLaunchFolders()
		{
			for (const auto& [hostPath, guestPath] : LaunchSettings::CosMounts())
				fsc_unmount(boost::nowide::narrow(guestPath), FSC_PRIORITY_BASE);
		}

		std::string AocMountPath(TitleInfo& aoc)
		{
			return fmt::format("/vol/aoc{:016x}", aoc.GetAppTitleId());
		}

		bool MountContentAndCode(TitleInfo& title, sint32 priority)
		{
			if (title.Mount("/vol/content", "content", priority) && title.Mount(GetInternalVirtualCodeFolder(), "code", priority))
				return true;
			cemuLog_log(LogType::Force, "Mounting {} failed", title.GetPrintPath());
			return false;
		}

		// base, update and DLC of the title
		PREPARE_STATUS_CODE MountTitle(TitleId titleId)
		{
			cemuLog_log(LogType::Force, "Mounting title {:016x}", (uint64)titleId);
			s_gameInfo = CafeTitleList::GetGameInfo(titleId);
			if (!s_gameInfo.IsValid())
			{
				cemuLog_log(LogType::Force, "Mounting failed: the title's meta data (code and meta xml files) is missing, unreadable or invalid");
				return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
			}
			TitleInfo& base = s_gameInfo.GetBase();
			if (!base.IsValid() || !base.ParseXmlInfo())
				return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
			cemuLog_log(LogType::Force, "Base: {}", base.GetPrintPath());
			if (!MountContentAndCode(base, FSC_PRIORITY_BASE))
				return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;

			TitleInfo& update = s_gameInfo.GetUpdate();
			if (update.IsValid())
			{
				if (!update.ParseXmlInfo())
					return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
				cemuLog_log(LogType::Force, "Update: {}", update.GetPrintPath());
				if (!MountContentAndCode(update, FSC_PRIORITY_PATCH))
					return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
			}
			else
				cemuLog_log(LogType::Force, "Update: none");

			// only the first DLC title is used
			std::span<TitleInfo> aocList = s_gameInfo.GetAOC();
			if (!aocList.empty())
			{
				TitleInfo& aoc = aocList.front();
				if (!aoc.ParseXmlInfo())
					return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
				cemu_assert_debug(aoc.IsValid());
				cemuLog_log(LogType::Force, "DLC: {}", aoc.GetPrintPath());
				if (!aoc.Mount(AocMountPath(aoc), "content", FSC_PRIORITY_PATCH))
				{
					cemuLog_log(LogType::Force, "Mounting {} failed", aoc.GetPrintPath());
					return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
				}
			}
			else
				cemuLog_log(LogType::Force, "DLC: none");
			s_titleId = titleId;
			return PREPARE_STATUS_CODE::SUCCESS;
		}

		void UnmountTitle()
		{
			if (!s_standalone)
			{
				cemu_assert_debug(s_gameInfo.IsValid());
				if (s_gameInfo.IsValid())
				{
					TitleInfo& base = s_gameInfo.GetBase();
					base.Unmount("/vol/content");
					base.Unmount(GetInternalVirtualCodeFolder());
					if (s_gameInfo.HasUpdate())
					{
						if (TitleInfo& update = s_gameInfo.GetUpdate(); update.IsValid())
						{
							update.Unmount("/vol/content");
							update.Unmount(GetInternalVirtualCodeFolder());
						}
					}
					std::span<TitleInfo> aocList = s_gameInfo.GetAOC();
					if (!aocList.empty())
						aocList.front().Unmount(AocMountPath(aocList.front()));
				}
			}
			fsc_unmount(kStandaloneCodeFolder, FSC_PRIORITY_BASE);
		}

		// cos.xml's argstr can name a different executable in the code folder
		void PickExecutableFromArgs()
		{
			s_baseExecutablePath = _pathToExecutable;
			if (s_standalone)
				return;
			const std::string args = GetForegroundTitleArgStr();
			const size_t end = args.find(".rpx");
			if (end == std::string::npos)
				return;
			fs::path candidate = _pathToExecutable;
			candidate.replace_filename(args.substr(0, end + 4));
			const std::string path = std::string(kCodeFolder) + candidate.generic_string();
			sint32 status;
			if (FSCVirtualFile* file = fsc_open(path.c_str(), FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status))
			{
				fsc_close(file);
				_pathToExecutable = path;
			}
		}

		void PrepareMemory()
		{
			memory_mapForCurrentTitle();
			LoadSharedData();
		}

		void MountTitleInMlc(TitleInfo& title)
		{
			if (!title.IsValid())
			{
				cemu_assert_suspicious();
				return;
			}
			const TitleId titleId = title.GetAppTitleId();
			if (s_mlcTitles.contains(titleId))
				return;
			auto copy = std::make_unique<TitleInfo>(title);
			if (!copy->Mount(GetMlcStoragePath(titleId), "", FSC_PRIORITY_BASE))
			{
				cemuLog_log(LogType::Force, "Title {:016x} could not be mounted in the virtual MLC", (uint64)titleId);
				return;
			}
			s_mlcTitles.emplace(titleId, std::move(copy));
		}

		// /vol/storage_mlc01/ shows the running title's base, update and DLC; other titles are added by MlcStorageMountAllTitles()
		void MountForegroundInMlc()
		{
			if (s_gameInfo.GetBase().IsValid())
				MountTitleInMlc(s_gameInfo.GetBase());
			if (s_gameInfo.GetUpdate().IsValid())
				MountTitleInMlc(s_gameInfo.GetUpdate());
			for (TitleInfo& aoc : s_gameInfo.GetAOC())
				MountTitleInMlc(aoc);
		}

		void UnmountMlcTitles()
		{
			for (auto& [titleId, title] : s_mlcTitles)
				title->UnmountAll();
			s_mlcTitles.clear();
		}

		void TitleThread()
		{
			for (IOSUModule* module : s_iosuModules)
				module->TitleStart();
			StartGame();
			const bool multiCore = (ActiveSettings::GetCPUMode() == CPUMode::MulticoreRecompiler || LaunchSettings::ForceMultiCoreInterpreter()) && !LaunchSettings::ForceInterpreter();
			coreinit::OSSchedulerBegin(multiCore ? 3 : 1);
		}
	}

	void Initialize()
	{
		if (s_initialized)
			return;
		s_initialized = true;
		cemuLog_log(LogType::Force, "------- {} -------", BUILD_VERSION_WITH_NAME_STRING);
		fsc_init();
		memory_init();
		cemuLog_log(LogType::Force, "Guest memory at 0x{:016x}", (size_t)memory_base);
		PPCCore_init();
		RPLLoader_InitState();
		cemuLog_log(LogType::Force, "MLC: {}", _pathToUtf8(ActiveSettings::GetMlcPath()));
		LogHostInfo();
		rplSymbolStorage_init();
		// the SysAllocators need memory before the OS modules and the IOSU kernel start
		SysAllocatorContainer::GetInstance().Initialize();
		for (IOSUModule* module : s_iosuModules)
			module->SystemLaunch();
		// services not yet converted to IOSU modules, started in this order
		using StartFn = void (*)();
		static constexpr StartFn s_legacyServices[] = {
			iosuCrypto_init, iosu::fsa::Initialize, iosuIoctl_init, iosuAct_init_depr,
			iosu::act::Initialize, iosu::iosuMcp_init, iosu::mcp::Init, iosu::iosuAcp_init,
			iosu::nim::Initialize, iosu::odm::Initialize, HW_SI::Initialize,
		};
		for (StartFn start : s_legacyServices)
			start();
	}

	void SetImplementation(SystemImplementation* impl)
	{
		s_frontend = impl;
	}

	void Shutdown()
	{
		cemu_assert_debug(s_initialized);
		if (s_running)
			ShutdownTitle();
		iosu::odm::Shutdown();
		iosu::act::Stop();
		iosu::mcp::Shutdown();
		iosu::fsa::Shutdown();
		for (auto it = s_iosuModules.rbegin(); it != s_iosuModules.rend(); ++it)
			(*it)->SystemExit();
		s_initialized = false;
	}

	std::string GetInternalVirtualCodeFolder()
	{
		return kCodeFolder;
	}

	PREPARE_STATUS_CODE PrepareForegroundTitle(TitleId titleId)
	{
		CafeTitleList::WaitForMandatoryScan();
		s_standalone = false;
		s_returnStatus.reset();
		_pathToExecutable.clear();
		const auto type = TitleIdParser(titleId).GetType();
		if (type == TitleIdParser::TITLE_TYPE::AOC || type == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE)
			cemuLog_log(LogType::Force, "Note: the launched title ID is not a base title");
		MountHostFolders();
		if (PREPARE_STATUS_CODE r = MountTitle(titleId); r != PREPARE_STATUS_CODE::SUCCESS)
			return r;
		gameProfile_load();
		PrepareMemory();
		PPCRecompiler_init();
		PickExecutableFromArgs();
		MountForegroundInMlc();
		MountLaunchFolders();
		return PREPARE_STATUS_CODE::SUCCESS;
	}

	PREPARE_STATUS_CODE PrepareForegroundTitleFromStandaloneRPX(const fs::path& path)
	{
		s_standalone = true;
		cemuLog_log(LogType::Force, "Starting the executable on its own (no title folder layout or meta data found)");
		const fs::path codeFolder = path.parent_path();
		// <title>/code/<exe>: the sibling content folder becomes /vol/content
		if (boost::iequals(_pathToUtf8(codeFolder.filename()), "code"))
		{
			const fs::path contentFolder = codeFolder.parent_path() / "content";
			std::error_code ec;
			if (fs::is_directory(contentFolder, ec) && !FSCDeviceHostFS_Mount("/vol/content", _pathToUtf8(contentFolder), FSC_PRIORITY_BASE))
			{
				cemuLog_log(LogType::Force, "Mounting {} failed", _pathToUtf8(contentFolder));
				return PREPARE_STATUS_CODE::UNABLE_TO_MOUNT;
			}
		}
		FSCDeviceHostFS_Mount(kStandaloneCodeFolder, _pathToUtf8(codeFolder), FSC_PRIORITY_BASE);
		_pathToExecutable = std::string(kStandaloneCodeFolder) + _pathToUtf8(path.filename());
		// many systems (save folders, ...) need a title ID, so one is made up from the executable's checksum
		auto data = fsc_extractFile(_pathToExecutable.c_str());
		if (!data)
			return PREPARE_STATUS_CODE::INVALID_RPX;
		s_titleId = 0xFFFFFFFF00000000ULL | HashExecutable(data->data(), data->size());
		cemuLog_log(LogType::Force, "Title ID for this executable: {:016x}", s_titleId);
		PrepareMemory();
		PPCRecompiler_init();
		PickExecutableFromArgs();
		MountForegroundInMlc();
		MountLaunchFolders();
		return PREPARE_STATUS_CODE::SUCCESS;
	}

	void LaunchForegroundTitle()
	{
		PPCTimer_waitForInit();
		s_running = true;
		WindowSystem::NotifyGameLoaded();
		LatteRecorder::Start();
		std::thread(TitleThread).detach();
	}

	bool IsTitleRunning()
	{
		return s_running;
	}

	TitleId GetForegroundTitleId()
	{
		cemu_assert_debug(s_titleId != 0);
		return s_titleId;
	}

	uint16 GetForegroundTitleVersion()
	{
		return s_standalone ? 0 : s_gameInfo.GetVersion();
	}

	uint32 GetForegroundTitleSDKVersion()
	{
		return s_standalone ? 999999 : s_gameInfo.GetSDKVersion();
	}

	CafeConsoleRegion GetForegroundTitleRegion()
	{
		return s_standalone ? CafeConsoleRegion::USA : s_gameInfo.GetRegion();
	}

	std::string GetForegroundTitleName()
	{
		if (s_standalone)
			return "Unknown Game";
		ParsedMetaXml* meta = s_gameInfo.GetBase().GetMetaInfo();
		std::string name = meta->GetShortName(GetConfig().console_language);
		if (name.empty())
			name = meta->GetShortName(CafeConsoleLanguage::EN);
		return name.empty() ? "Unknown Game" : name;
	}

	uint32 GetForegroundTitleOlvAccesskey()
	{
		if (s_standalone)
			return (uint32)-1;
		return s_gameInfo.GetBase().GetMetaInfo()->GetOlvAccesskey();
	}

	std::string GetForegroundTitleArgStr()
	{
		if (auto fromCommandLine = LaunchSettings::CosArgstr())
			return *fromCommandLine;
		if (s_standalone)
			return "";
		TitleInfo& update = s_gameInfo.GetUpdate();
		return update.IsValid() ? update.GetArgStr() : s_gameInfo.GetBase().GetArgStr();
	}

	CosCapabilityBits GetForegroundTitleCosCapabilities(CosCapabilityGroup group)
	{
		if (s_standalone)
			return CosCapabilityBits::All;
		// graphic packs can grant additional permissions
		CosCapabilityBits extra = static_cast<CosCapabilityBits>(0);
		for (const auto& pack : GraphicPack2::GetActiveGraphicPacks())
		{
			for (const auto& [packGroup, mask] : pack->GetPermissionOverrides())
			{
				if (packGroup == group)
					extra |= static_cast<CosCapabilityBits>(mask);
			}
		}
		// cos.xml of the update, else of the base
		for (TitleInfo* title : {&s_gameInfo.GetUpdate(), &s_gameInfo.GetBase()})
		{
			if (!title->IsValid())
				continue;
			if (ParsedCosXml* cos = title->GetCosInfo())
				return cos->GetCapabilityBits(group) | extra;
		}
		return CosCapabilityBits::All;
	}

	bool GetOverrideArgStr(std::vector<std::string>& args)
	{
		args.clear();
		if (!s_overrideArgs)
			return false;
		args = *s_overrideArgs;
		return true;
	}

	void SetOverrideArgs(std::span<std::string> args)
	{
		s_overrideArgs.emplace(args.begin(), args.end());
	}

	void UnsetOverrideArgs()
	{
		s_overrideArgs.reset();
	}

	// the console region follows the title: Japan, then Europe, otherwise USA
	CafeConsoleRegion GetPlatformRegion()
	{
		const CafeConsoleRegion titleRegion = GetForegroundTitleRegion();
		if (HAS_FLAG(titleRegion, CafeConsoleRegion::JPN))
			return CafeConsoleRegion::JPN;
		if (HAS_FLAG(titleRegion, CafeConsoleRegion::EUR))
			return CafeConsoleRegion::EUR;
		return CafeConsoleRegion::USA;
	}

	void ShutdownTitle()
	{
		if (!s_running)
			return;
		coreinit::OSSchedulerEnd();
		Latte_Stop();
		// guest OS state
		snd_core::reset();
		LatteRecorder::Stop();
		coreinit::OSAlarm_Shutdown();
		GX2::_GX2DriverReset();
		nn::save::ResetToDefaultState();
		coreinit::__OSDeleteAllActivePPCThreads();
		RPLLoader_UnloadAll();
		for (auto it = s_iosuModules.rbegin(); it != s_iosuModules.rend(); ++it)
			(*it)->TitleStop();
		// emulator state
		PPCRecompiler_Shutdown();
		GraphicPack2::Reset();
		UnmountTitle();
		UnmountLaunchFolders();
		UnmountMlcTitles();
		UnmountHostFolders();
		memory_unmapForCurrentTitle();
		LaunchSettings::ClearCosArgstr();
		s_running = false;
	}

	// /vol/storage_mlc01/<sys|usr>/title/<high>/<low>
	std::string GetMlcStoragePath(TitleId titleId)
	{
		const char* area = TitleIdParser(titleId).IsSystemTitle() ? "sys" : "usr";
		return fmt::format("/vol/storage_mlc01/{}/title/{:08x}/{:08x}", area, (uint32)(titleId >> 32), (uint32)titleId);
	}

	void MlcStorageMountAllTitles()
	{
		for (uint64 titleId : CafeTitleList::GetAllTitleIds())
		{
			TitleInfo title;
			if (CafeTitleList::GetFirstByTitleId(titleId, title))
				MountTitleInMlc(title);
		}
	}

	uint32 GetRPXHashBase()
	{
		return s_rpxHashBase;
	}

	uint32 GetRPXHashUpdated()
	{
		return s_rpxHashUpdated;
	}

	void RequestRecreateCanvas()
	{
		s_frontend->CafeRecreateCanvas();
	}

	void NotifyPPCProcessExit(sint32 status)
	{
		s_returnStatus = status;
		s_frontend->CafePPCProcessExit();
	}

	std::optional<sint32> GetForegroundTitleReturnStatus()
	{
		return s_returnStatus;
	}

	// copies the running executable and the .rpl files next to it to <user>/dump/rpx/<title id>/
	void DumpCurrentRPX()
	{
		if (!IsTitleRunning() || _pathToExecutable.empty())
			return;
		const fs::path target = ActiveSettings::GetUserDataPath("dump/rpx/{:016x}", GetForegroundTitleId());
		std::error_code ec;
		fs::create_directories(target, ec);
		if (ec)
		{
			cemuLog_log(LogType::Force, "RPX dump: cannot create {}: {}", _pathToUtf8(target), ec.message());
			return;
		}
		auto copyOut = [&target](const std::string& guestPath) {
			auto data = fsc_extractFile(guestPath.c_str());
			if (!data)
				return;
			std::unique_ptr<FileStream> out(FileStream::createFile2(target / _utf8ToPath(guestPath).filename()));
			if (out)
				out->writeData(data->data(), (sint32)data->size());
		};
		copyOut(_pathToExecutable);
		const std::string folder = _pathToExecutable.substr(0, _pathToExecutable.find_last_of('/') + 1);
		sint32 status;
		if (FSCVirtualFile* dir = fsc_openDirIterator(folder.c_str(), &status))
		{
			FSCDirEntry entry;
			while (fsc_nextDir(dir, &entry))
			{
				if (entry.isFile && boost::iends_with(entry.path, ".rpl"))
					copyOut(folder + entry.path);
			}
			fsc_close(dir);
		}
	}
}
