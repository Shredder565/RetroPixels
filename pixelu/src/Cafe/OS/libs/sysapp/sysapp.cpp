// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// sysapp is how games talk to the console's built-in applications: it knows
// their title ids, starts them (Mii Maker, the manual, ...) and decodes the
// arguments a system application was started with.
//
// Arguments travel in the process "copy data" area as a stack of entries,
// each one the raw argument bytes followed by a tag "<name size=N>" (or just
// "<name>" without data). Readers start at the end of the area and walk
// backwards. A "sys:pack" entry marks how much of the area belongs to the
// system; the rest is left for the application.
//
// PixelU runs one game and no system menu, so launching another system
// application is not supported; queries return empty argument sets.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "sysapp.h"

namespace
{
	// ---- system application title ids ---------------------------------------

	// Japanese title ids; the US version adds 0x100, the European/Australian one 0x200.
	constexpr uint64 kSystemAppBaseIds[] = {
		0x0005001010040000, // 0  Wii U Menu
		0x0005001010047000, // 1  System Settings
		0x0005001010048000, // 2  Parental Controls
		0x0005001010049000, // 3  User Settings
		0x000500101004A000, // 4  Mii Maker
		0x000500101004B000, // 5  Account Settings
		0x000500101004C000, // 6  Daily Log
		0x000500101004D000, // 7  Notifications
		0x000500101004E000, // 8  Health and Safety Information
		0x0005001B10059000, // 9  Wii U Electronic Manual
		0x000500101005A000, // 10 Wii U Chat
		0x0005001010062000, // 11 Software/Data Transfer
	};
	constexpr uint32 kMiiMakerApp = 4;

	uint64 SystemAppTitleIdForRegion(uint32 app, uint32 region)
	{
		if (app >= std::size(kSystemAppBaseIds))
		{
			cemuLog_log(LogType::Force, "sysapp: unknown system application {}", app);
			return 0;
		}
		uint64 regionOffset = 0x100; // USA and anything else
		if (region == 1)
			regionOffset = 0; // JPN
		else if (region == 4 || region == 8)
			regionOffset = 0x200; // EUR, AUS
		return kSystemAppBaseIds[app] + regionOffset;
	}

	// ---- argument area -----------------------------------------------------

	struct SysArg
	{
		std::string_view name;
		const uint8* data;
		uint32 size;
	};

	// Walks the argument stack from the top, calling visit for every entry.
	// Stops quietly at the first entry that doesn't parse.
	template<typename F>
	void ForEachSysArg(const uint8* area, uint32 areaSize, F&& visit)
	{
		sint64 end = (sint64)areaSize; // one past the current entry's tag
		while (end > 0)
		{
			if (area[end - 1] != '>')
				return;
			sint64 open = end - 2;
			while (open >= 0 && area[open] != '<')
				open--;
			if (open < 0)
				return;
			const std::string_view tag((const char*)area + open + 1, (size_t)(end - 1 - (open + 1)));
			const size_t space = tag.find(' ');
			const std::string_view name = tag.substr(0, space);
			if (name.empty() || name.size() >= 0x40)
				return;
			uint32 dataSize = 0;
			if (space != std::string_view::npos)
			{
				std::string_view rest = tag.substr(space);
				while (!rest.empty() && rest.front() == ' ')
					rest.remove_prefix(1);
				if (!rest.starts_with("size"))
					return;
				rest.remove_prefix(4);
				while (!rest.empty() && (rest.front() == ' ' || rest.front() == '='))
					rest.remove_prefix(1);
				if (rest.empty() || rest.size() > 10)
					return;
				uint64 value = 0;
				for (char c : rest)
				{
					if (c < '0' || c > '9')
						return;
					value = value * 10 + (uint64)(c - '0');
				}
				if (value > (uint64)open || value >= 8 * 1024 * 1024)
					return;
				dataSize = (uint32)value;
			}
			const sint64 dataStart = open - (sint64)dataSize;
			visit(SysArg{ name, area + dataStart, dataSize });
			end = dataStart;
		}
	}

	template<typename F>
	void ForEachCopyDataArg(F&& visit)
	{
		ForEachSysArg(coreinit::__OSGetCopyDataPtr(), coreinit::__OSGetCopyDataSize(), visit);
	}

	// Drops everything above the "sys:pack" mark so only the system's arguments remain.
	void UnpackSysArgs()
	{
		uint32 packSize = 0;
		ForEachCopyDataArg([&](const SysArg& arg) {
			if (arg.name == "sys:pack")
				packSize = arg.size;
		});
		if (packSize != 0)
			coreinit::__OSResizeCopyData(packSize);
	}

	uint32 ReadU32Arg(const SysArg& arg)
	{
		if (arg.size < 4)
			return 0;
		return _swapEndianU32(*(const uint32*)arg.data);
	}

	// ---- argument structs --------------------------------------------------

	struct StandardArgs
	{
		MEMPTR<char> anchor;
		uint32be anchorSize;
	};
	static_assert(sizeof(StandardArgs) == 8);

	struct StandardArgsOut
	{
		MEMPTR<uint8> anchor;
		uint32be anchorSize;
		MEMPTR<uint8> result;
		uint32be resultSize;
	};
	static_assert(sizeof(StandardArgsOut) == 0x10);

	// what a game passes to _SYSLaunchMiiStudio
	struct MiiStudioLaunchArgs
	{
		StandardArgs standard;
		uint32be mode;
		uint32be slotId;
		MEMPTR<uint8> mii;
	};
	static_assert(sizeof(MiiStudioLaunchArgs) == 0x14);

	// what Mii Maker reads with _SYSGetMiiStudioArgs
	struct MiiStudioArgs
	{
		StandardArgs standard;
		uint32 unk08;
		uint32 unk0C;
		uint32be mode;
		uint32be slotId;
		MEMPTR<uint8> mii;
		uint32be miiSize;
	};
	static_assert(sizeof(MiiStudioArgs) == 0x20);

	// what System Settings reads with _SYSGetSettingsArgs
	struct SettingsArgs
	{
		StandardArgsOut standard;
		uint32be jumpTo; // 0 normal start, 1 internet, 2 data management, ...
		uint32be needsReturn;
		uint32be firstBootKind;
	};
	static_assert(sizeof(SettingsArgs) == 0x1C);

	struct EShopArgs
	{
		StandardArgsOut standard;
		uint32be unk10;
		uint32be unk14;
	};
	static_assert(sizeof(EShopArgs) == 0x18);

	// _SYSGetLauncherArgs, two layouts depending on the SDK the game was built with
	struct LauncherArgsOld
	{
		uint64be callerId;
		uint64be launchTitle;
		uint32be mode;
		uint32be slotId;
	};
	static_assert(sizeof(LauncherArgsOld) == 0x18);

	struct LauncherArgsNew
	{
		uint32 standard[4];
		uint64be callerId;
		uint64be launchTitle;
		uint32be mode;
		uint32be slotId;
	};
	static_assert(sizeof(LauncherArgsNew) == 0x28);

	struct AccountArgs
	{
		uint32be standard[4];
		uint32be slotId;
		uint32be mode;
	};
	static_assert(sizeof(AccountArgs) == 0x18);

	struct EManualArgs
	{
		StandardArgs standard;
		uint64be titleId;
	};
	static_assert(sizeof(EManualArgs) == 0x10);

	// ---- exports -------------------------------------------------------------

	uint64 _SYSGetSystemApplicationTitleIdExport(uint32 app)
	{
		return _SYSGetSystemApplicationTitleId((sint32)app);
	}

	// Maps a title id to the "unique process id" the system would run it under.
	sint32 SYSGetUPIDFromTitleID(uint64 titleId)
	{
		const uint32 high = (uint32)(titleId >> 32);
		const uint32 low = (uint32)titleId;
		if ((high & 0xFFFF0000) != 0x00050000 || (low & 0xF0000000) != 0x10000000)
			return -1;
		const uint32 kind = high & 0xFF;
		const uint32 unique = (low >> 12) & 0xFFFF;
		switch (kind)
		{
		case 0x30: // applets: two blocks of ten (0x10-0x19 and 0x21-0x29)
		{
			static constexpr uint8 kAppletUpid[10] = { 5, 6, 8, 3, 0xA, 0xB, 9, 4, 0xC, 7 };
			if (unique >= 0x10 && unique <= 0x19)
				return kAppletUpid[unique - 0x10];
			if (unique >= 0x21 && unique <= 0x29)
				return kAppletUpid[unique - 0x20];
			return -1;
		}
		case 0x10: // system applications
			return unique == 0x40 ? 2 : 0xF; // the Wii U Menu has its own
		case 0x00: // games
		case 0x02: // demos
			return 0xF;
		default:
			return -1;
		}
	}

	sint32 _SYSGetMiiStudioArgs(MiiStudioArgs* args)
	{
		UnpackSysArgs();
		memset(args, 0, sizeof(MiiStudioArgs));
		ForEachCopyDataArg([&](const SysArg& arg) {
			if (arg.name == "mode")
				args->mode = ReadU32Arg(arg);
		});
		return 0;
	}

	sint32 _SYSGetSettingsArgs(SettingsArgs* args)
	{
		UnpackSysArgs();
		memset(args, 0, sizeof(SettingsArgs));
		ForEachCopyDataArg([&](const SysArg& arg) {
			if (boost::iequals(arg.name, "needs_return"))
				args->needsReturn = 1;
			else if (boost::iequals(arg.name, "jump_to"))
				args->jumpTo = ReadU32Arg(arg);
			else if (boost::iequals(arg.name, "first_boot_kind"))
				args->firstBootKind = ReadU32Arg(arg);
		});
		return 0;
	}

	sint32 _SYSGetEShopArgs(EShopArgs* args)
	{
		memset(args, 0, sizeof(EShopArgs));
		return 0;
	}

	uint32 SYSGetVodArgs()
	{
		return 1; // no video-on-demand arguments
	}

	uint32 SYSGetStandardResult(uint32be* resultOut, void* data, uint32 size)
	{
		*resultOut = 0;
		return 0;
	}

	uint32 _SYSGetLauncherArgs(void* out)
	{
		if (coreinit::__OSGetProcessSDKVersion() < 21103)
			memset(out, 0, sizeof(LauncherArgsOld));
		else
			memset(out, 0, sizeof(LauncherArgsNew));
		return 0;
	}

	uint32 _SYSGetAccountArgs(AccountArgs* out)
	{
		memset(out, 0, sizeof(AccountArgs));
		return 0;
	}

	void _SYSLaunchMiiStudio(MiiStudioLaunchArgs* args)
	{
		const uint64 miiMaker = SystemAppTitleIdForRegion(kMiiMakerApp, (uint32)CafeSystem::GetPlatformRegion());
		cemuLog_log(LogType::Force, "sysapp: game asked to open Mii Maker ({:016x}), which PixelU can't start", miiMaker);
	}

	// The game hands control back to the app that started it. With nothing to
	// return to, the calling thread just waits until the emulator is closed.
	void _SYSReturnToCallerWithStandardResult(uint32be* result)
	{
		cemuLog_log(LogType::Force, "sysapp: _SYSReturnToCallerWithStandardResult({:08x}), nothing to return to", result ? (uint32)*result : 0);
		while (true)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	void SYSClearSysArgs()
	{
		coreinit::__OSClearCopyData();
	}

	uint32 _SYSLaunchTitleByPathFromLauncher(const char* path, uint32 pathLength)
	{
		coreinit::__OSClearCopyData();
		return coreinit::OSLaunchTitleByPathl(path, pathLength, 0);
	}

	uint32 SYSRelaunchTitle(uint32 argc, MEMPTR<char>* argv)
	{
		coreinit::__OSClearCopyData();
		return coreinit::OSRestartGame(argc, argv);
	}

	// The manual isn't available; the game still goes through the
	// background/foreground switch it expects.
	void _SYSSwitchToEManual(EManualArgs* args)
	{
		cemuLog_log(LogType::Force, "sysapp: the electronic manual is not supported");
		coreinit::StartBackgroundForegroundTransition();
	}

	void SYSSwitchToEManual()
	{
		EManualArgs args{};
		args.titleId = coreinit::OSGetTitleID();
		_SYSSwitchToEManual(&args);
	}
}

uint64 _SYSGetSystemApplicationTitleId(sint32 index)
{
	return SystemAppTitleIdForRegion((uint32)index, (uint32)CafeSystem::GetPlatformRegion());
}

namespace sysapp
{
	namespace
	{
		class SysappLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "sysapp"; }

			void RPLMapped() override
			{
				constexpr auto lt = LogType::Placeholder;
				cafeExportRegisterFunc(_SYSGetSystemApplicationTitleIdExport, "sysapp", "_SYSGetSystemApplicationTitleId", lt);
				cafeExportRegister("sysapp", SYSGetUPIDFromTitleID, lt);
				cafeExportRegister("sysapp", _SYSGetMiiStudioArgs, lt);
				cafeExportRegister("sysapp", _SYSGetSettingsArgs, lt);
				cafeExportRegister("sysapp", _SYSGetEShopArgs, lt);
				cafeExportRegister("sysapp", SYSGetVodArgs, lt);
				cafeExportRegister("sysapp", SYSGetStandardResult, lt);
				cafeExportRegister("sysapp", _SYSGetLauncherArgs, lt);
				cafeExportRegister("sysapp", _SYSGetAccountArgs, lt);
				cafeExportRegister("sysapp", _SYSLaunchMiiStudio, lt);
				cafeExportRegister("sysapp", _SYSReturnToCallerWithStandardResult, lt);
				cafeExportRegister("sysapp", SYSClearSysArgs, lt);
				cafeExportRegister("sysapp", _SYSLaunchTitleByPathFromLauncher, lt);
				cafeExportRegister("sysapp", SYSRelaunchTitle, lt);
				cafeExportRegister("sysapp", _SYSSwitchToEManual, lt);
				cafeExportRegister("sysapp", SYSSwitchToEManual, lt);
			}
		};

		SysappLibrary g_sysappLibrary;
	}

	COSModule* GetModule()
	{
		return &g_sysappLibrary;
	}
}
