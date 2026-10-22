// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Title start-up. coreinit_start runs as the first guest code on the main
// core's default thread: it builds argc/argv for the title, sets up the
// default heaps (letting the title's __preinit_user hook replace them), runs
// the remaining module entry points and then jumps to the title's entry point.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/padscore/padscore.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/vpad/vpad.h"
#include "Cafe/OS/libs/coreinit/coreinit_GHS.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/GraphicPack/GraphicPack2.h"

extern MPTR _entryPoint;
extern RPLModule* applicationRPX;
extern std::string _pathToExecutable;

MPTR OSAllocFromSystem(uint32 size, uint32 alignment)
{
	return coreinit_allocFromSysArea(size, alignment);
}

void OSFreeToSystem(MPTR mem)
{
	coreinit_freeToSysArea(mem);
}

namespace
{
	constexpr uint32 kMaxArgs = 32;
	constexpr uint32 kArgStorageSize = 0x1000;

	// argc/argv block handed to the title, lives in the system area
	struct TitleArgs
	{
		uint32be argv[kMaxArgs];
		uint32be argc;
		char storage[kArgStorageSize];
	};
	static_assert(offsetof(TitleArgs, argc) == 0x80);
	static_assert(sizeof(TitleArgs) == 0x1084);

	TitleArgs* s_titleArgs = nullptr;
	uint32 s_storageCursor = 0;

	// Appends one argument (the first len chars of text). The write cursor
	// advances past the whole remaining C string at text, not just the
	// argument, which keeps argv addresses identical to earlier builds.
	void PushArg(const char* text, size_t len)
	{
		const uint32 argc = s_titleArgs->argc;
		if (argc >= kMaxArgs || s_storageCursor + len + 1 > kArgStorageSize)
		{
			cemuLog_log(LogType::Force, "Title arguments do not fit, dropping \"{}\"", std::string_view(text, len));
			return;
		}
		char* dst = s_titleArgs->storage + s_storageCursor;
		memcpy(dst, text, len);
		dst[len] = '\0';
		s_storageCursor += (uint32)strlen(text) + 1;
		s_titleArgs->argv[argc] = memory_getVirtualOffsetFromPointer(dst);
		s_titleArgs->argc = argc + 1;
	}

	std::string LaunchArgString()
	{
		std::string args = CafeSystem::GetForegroundTitleArgStr();
		std::vector<std::string> overrides;
		if (CafeSystem::GetOverrideArgStr(overrides))
		{
			// a launch request (OSLaunchTitleByPath) supplied its own arguments:
			// keep the executable part and replace everything after it
			args = args.substr(0, args.find(' '));
			for (const std::string& a : overrides)
			{
				args.push_back(' ');
				args.append(a);
			}
		}
		return args;
	}

	// Arguments follow the first ".rpx" in the launch string and are separated by spaces.
	void PushLaunchArgs(const std::string& launchString)
	{
		if (launchString.empty())
			return;
		size_t pos = launchString.find(".rpx");
		if (pos == std::string::npos)
		{
			cemuLog_logDebug(LogType::Force, "Launch string has no .rpx, ignoring arguments");
			return;
		}
		pos += 4;
		const char* s = launchString.c_str();
		while (s[pos] != '\0')
		{
			while (s[pos] == ' ')
				pos++;
			size_t len = 0;
			while (s[pos + len] != '\0' && s[pos + len] != ' ')
			{
				cemu_assert_debug(s[pos + len] != '"' && s[pos + len] != '\''); // quoting is not supported
				len++;
			}
			if (len == 0)
				break;
			PushArg(s + pos, len);
			pos += len;
		}
	}

	void SetupTitleArgs()
	{
		// argv[0] is the executable's file name without directories
		std::string_view exePath = _pathToExecutable;
		size_t nameStart = 0;
		if (exePath.size() > 1)
		{
			nameStart = exePath.size() - 1;
			while (nameStart > 0 && exePath[nameStart - 1] != '/')
				nameStart--;
		}
		s_storageCursor = 0;
		s_titleArgs->argc = 0;
		PushArg(exePath.data() + nameStart, exePath.size() - nameStart);

		std::string launchString = LaunchArgString();
		CafeSystem::UnsetOverrideArgs(); // the override applies to one launch only
		PushLaunchArgs(launchString);
	}

	void SetupPairedSingleQuantizers()
	{
		// UGQR2..5: load/store scale 0, types u8, u16, s8, s16
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		cpu->spr.UGQR[2] = 0x00040004;
		cpu->spr.UGQR[3] = 0x00050005;
		cpu->spr.UGQR[4] = 0x00060006;
		cpu->spr.UGQR[5] = 0x00070007;
	}

	// heap handles passed to the title's __preinit_user(MEM1, FG, MEM2)
	struct PreinitHeaps
	{
		MEMPTR<coreinit::MEMHeapBase> mem1;
		MEMPTR<coreinit::MEMHeapBase> fg;
		MEMPTR<coreinit::MEMHeapBase> mem2;
	};
	SysAllocator<PreinitHeaps> s_preinitHeaps;

	void SetupDefaultHeaps()
	{
		PreinitHeaps& heaps = *s_preinitHeaps.GetPtr();
		heaps.mem1 = nullptr;
		heaps.fg = nullptr;
		heaps.mem2 = nullptr;
		coreinit::InitDefaultHeaps(heaps.mem1, heaps.fg, heaps.mem2);

		// the title may override the heaps through an exported __preinit_user
		MPTR preinit = applicationRPX ? RPLLoader_FindRPLExport(applicationRPX, "__preinit_user", false) : MPTR_NULL;
		if (preinit != MPTR_NULL)
			PPCCoreCallback(preinit, &heaps.mem1, &heaps.fg, &heaps.mem2);

		constexpr uint32 kArenaMEM1 = 0, kArenaMEM2 = 1, kArenaFG = 8;
		if (heaps.mem1 != nullptr)
			coreinit::MEMSetBaseHeapHandle(kArenaMEM1, heaps.mem1);
		if (heaps.fg != nullptr)
			coreinit::MEMSetBaseHeapHandle(kArenaFG, heaps.fg);
		if (heaps.mem2 != nullptr)
			coreinit::MEMSetBaseHeapHandle(kArenaMEM2, heaps.mem2);
	}

	void RunGraphicPackEntryCallbacks()
	{
		for (const auto& pack : GraphicPack2::GetActiveGraphicPacks())
		{
			for (const auto& [callback, type] : pack->GetCallbacks())
			{
				if (type == GPCallbackType::Entry)
					PPCCoreCallback(callback);
			}
		}
	}
}

void coreinit_start(PPCInterpreter_t* hCPU)
{
	s_titleArgs = (TitleArgs*)memory_getPointerFromVirtualOffset(coreinit_allocFromSysArea(sizeof(TitleArgs), 32));
	memset(s_titleArgs, 0, sizeof(TitleArgs));

	SetupTitleArgs();
	SetupPairedSingleQuantizers();
	coreinit::InitForegroundBucket();
	coreinit::InitSysHeap();

	SetupDefaultHeaps();
	// Exercise the OSDynLoad allocator once before the title runs. Some titles
	// (Watch Dogs) build their malloc on top of it and expect it to already own
	// a memory segment. The size has to be above 0x190 so a new segment is
	// created and must stay clear of page-size multiples.
	if (void* warmup = coreinit::OSDynLoad_AllocatorAlloc(0x500, 4))
		coreinit::OSDynLoad_AllocatorFree(warmup);
	const MPTR titleEntry = _entryPoint;

	RPLLoader_CallEntrypoints();

	// HLE modules have no entry points of their own yet, start these by hand
	padscore::start();
	vpad::start();

	RunGraphicPackEntryCallbacks();

	// resume at the title's entry point with (argc, argv)
	hCPU->gpr[3] = s_titleArgs->argc;
	hCPU->gpr[4] = memory_getVirtualOffsetFromPointer(s_titleArgs->argv);
	hCPU->instructionPointer = titleEntry;
}
