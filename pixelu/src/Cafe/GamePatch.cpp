// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Fixes for individual games. After the executable is linked the code area is
// searched for known instruction sequences, and each match is patched in memory
// or redirected to a host function.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "CafeSystem.h"
#include "config/ActiveSettings.h"

namespace
{
	// ---------------------------------------------------------------- PowerPC encodings

	constexpr uint32 kOpBlr = 0x4E800020;
	constexpr uint32 kOpNop = 0x60000000;
	constexpr uint32 kOpLiR3_0 = 0x38600000;
	constexpr uint32 kOpLiR3_1 = 0x38600001;

	// "bla target": absolute branch with link
	uint32 AbsoluteCall(uint32 target)
	{
		return 0x48000000 | PPC_OPC_LK | PPC_OPC_AA | target;
	}

	// instruction that enters the HLE function with the given registered index
	uint32 HleInstruction(sint32 functionIndex)
	{
		return (1u << 26) | (uint32)functionIndex;
	}

	// ---------------------------------------------------------------- signatures

	// A signature is a run of big-endian instruction words; a mask word of 0 marks
	// words that differ between builds (usually branch offsets) and are ignored.
	struct Signature
	{
		std::span<const uint32> words;
		std::span<const uint32> masks; // empty: every bit must match
	};

	bool MatchesAt(const uint8* p, const Signature& sig)
	{
		for (size_t i = 0; i < sig.words.size(); i++)
		{
			const uint32 have = _swapEndianU32(*(const uint32*)(p + i * 4));
			const uint32 mask = sig.masks.empty() ? 0xFFFFFFFF : sig.masks[i];
			if ((have & mask) != (sig.words[i] & mask))
				return false;
		}
		return true;
	}

	// first 4-byte aligned match inside the loaded code, or 0
	MPTR FindSignature(const Signature& sig)
	{
		const uint32 length = (uint32)(sig.words.size() * 4);
		const uint8* p = memory_getPointerFromVirtualOffset(MEMORY_CODEAREA_ADDR);
		const uint8* end = memory_getPointerFromVirtualOffset(RPLLoader_GetMaxCodeOffset() - length);
		const uint32 firstWord = _swapEndianU32(sig.words[0]);
		const uint32 firstMask = _swapEndianU32(sig.masks.empty() ? 0xFFFFFFFF : sig.masks[0]);
		for (; p < end; p += 4)
		{
			if (((*(const uint32*)p ^ firstWord) & firstMask) != 0)
				continue;
			if (MatchesAt(p, sig))
				return memory_getVirtualOffsetFromPointer((void*)p);
		}
		return MPTR_NULL;
	}

	template<size_t N>
	Signature Exact(const uint32 (&words)[N])
	{
		return {words, {}};
	}

	template<size_t N>
	Signature Masked(const uint32 (&words)[N], const uint32 (&masks)[N])
	{
		return {words, masks};
	}

	constexpr uint32 F = 0xFFFFFFFF; // compare the whole word
	constexpr uint32 OP = 0xFF000000; // compare only the primary opcode byte (branch offsets differ)

	// Xenoblade Chronicles X: counter that lowers the framebuffer quality when the GPU seems hung
	const uint32 sigXcxHangCounter[] = {0x3B390001, 0x28194E20, 0x40810044};
	// Xenoblade Chronicles X: call to the "is this a reduction buffer" check
	const uint32 sigXcxReduction[] = {0x80C9001C, 0x38A00001, 0x807E0080, 0x809E02EC, 0x80E90020, 0x48061ED1, 0x7E731B78};
	const uint32 maskXcxReduction[] = {F, F, F, F, F, 0, F};
	// Breath of the Wild: two spin loops on an atomic flag
	const uint32 sigBotwSpin1[] = {0x80E60000, 0x2C070001, 0x4182FFF8, 0x7D003028, 0x2C080000, 0x4082FFF8, 0x7C00306C, 0x39000001, 0x7D00312D, 0x4082FFE8};
	const uint32 sigBotwSpin2[] = {0x800C0000, 0x2C000001, 0x4182FFF8, 0x7CA06028, 0x2C050000, 0x4082FFF8, 0x7C00606C, 0x38800001, 0x7C80612D, 0x4082FFE8};
	// FFL (Mii library): float array byte swap, two compiler schedules of the same function
	const uint32 sigFflSwapA[] = {0x7C0802A6, 0x9421FFE8, 0x93C10010, 0x7C7E1B78, 0x93E10014, 0x93810008, 0x7C9F2378, 0x93A1000C, 0x9001001C, 0x3BA00000, 0x7C1DF840, 0x40800020, 0x57BC103A, 0x7C3EE42E};
	const uint32 sigFflSwapB[] = {0x7C0802A6, 0x9421FFE8, 0x93E10014, 0x93A1000C, 0x7C9F2378, 0x93810008, 0x93C10010, 0x9001001C, 0x3BA00000, 0x7C1DF840, 0x7C7E1B78, 0x40800020, 0x57BC103A, 0x7C3EE42E};
	// Monster Hunter 3 Ultimate: start-up race
	const uint32 sigMh3uRace[] = {0x38210040, 0x38600000, 0x4E800020, 0x807BDB9C, 0x48116B81, 0x2C030000};
	const uint32 maskMh3uRace[] = {F, F, F, F, OP, F};
	// Super Smash Bros. for Wii U: soft lock after matches
	const uint32 sigSmashSoftlock[] = {0x2C030000, 0x41820020, 0x3860000A, 0x4833B8AD, 0x7FA3EB78, 0x7FC4F378, 0x4BFFFF09, 0x2C030000};
	const uint32 maskSmashSoftlock[] = {F, F, F, OP, F, F, OP, F};
	// Paper Mario Color Splash: crash in the cutscene after the yellow paint star
	const uint32 sigPmcsPaintStar[] = {0x9421FFB8, 0xBE610014, 0x7C0802A6, 0x7CB32B78, 0x9001004C, 0x7C9D2378, 0x833D000C, 0x813904A8, 0x5467103A, 0x7FC7482E, 0x831E00DC, 0x82F80008, 0x2C170000};
	// Bayonetta 2: audio queue compare that loops forever after a stutter
	const uint32 sigBayo2AudioQueue[] = {0x8003003C, 0x8143005C, 0x81830040, 0x5548B2BE, 0x3D40101D, 0x7D6C4214, 0x394A46F0, 0x7D8B0050};
	const uint32 maskBayo2AudioQueue[] = {F, F, F, F, 0xFFFF0000, F, 0xFFFF0000, F};
	// Super Mario 3D World: dynamic framebuffer scaling
	const uint32 sigSm3dwDynamicRes[] = {0x9421FFB8, 0xBF21002C, 0x7C0802A6, 0x9001004C, 0x7C7E1B78, 0x817E07D8, 0x38800002, 0x386B0003};
	// The Wind Waker HD: wait function holding a mutex it does not need
	const uint32 sigTwwWait[] = {0x7C0802A6, 0x9421FFF0, 0x93E1000C, 0x7C7F1B78, 0x90010014, 0x807F02E0, 0x8183000C, 0x800C001C, 0x7C0903A6, 0x38A00000, 0x389F0368};
	// Miiverse / eShop (wave.rpx): domain whitelist check
	const uint32 sigWaveWhitelist[] = {0x89450000, 0x2C0A002E, 0x40820008, 0x4BFFFF5C};
	// libopenssl.rpl: ssl_verify_cert_chain
	const uint32 sigOpensslVerifyChain[] = {0x9421FF58, 0x93C100A0, 0x93E100A4, 0x7C9F2379, 0x7C7E1B78, 0x900100AC, 0x41820014, 0x7FE3FB78};

	// Wind Waker HD: branch in the DSP channel shutdown, per region (RPX hash -> address)
	struct TwwDspFix
	{
		uint32 rpxHash;
		MPTR address;
	};
	const TwwDspFix kTwwDspFixes[] = {
		{0xB1C033DD, 0x02813878}, // USA
		{0xCDC68ACD, 0x02814138}, // EUR
	};
	constexpr uint32 kTwwDspBranchGe = 0x40800018; // bge +0x18
	constexpr uint32 kTwwDspBranch = (18u << 26) | 0x18; // b +0x18

	// indices of the registered HLE functions, -1 until GamePatch_scan() ran
	sint32 s_hleBotwSpin1 = -1;
	sint32 s_hleBotwSpin2 = -1;
	sint32 s_hleFflSwap = -1;

	bool s_mh3uFirstCall = true;

	// ---------------------------------------------------------------- host replacements

	// Breath of the Wild spin loops: yield while the lock is still held so other threads can run
	void BotwSpinStep(PPCInterpreter_t* hCPU, uint32 targetGpr)
	{
		const uint32 object = hCPU->gpr[24];
		const uint32 pending = memory_readU32(object + 0x7C);
		const uint8 busy = memory_readU8(object + 0x80);
		if (busy != 0 && hCPU->gpr[22] == 0 && pending == 0)
			PPCInterpreter_relinquishTimeslice();
		hCPU->gpr[targetGpr] = hCPU->gpr[29]; // the instruction the hook replaced
		hCPU->instructionPointer += 4;
	}

	void HleBotwSpin1(PPCInterpreter_t* hCPU)
	{
		BotwSpinStep(hCPU, 6);
	}

	void HleBotwSpin2(PPCInterpreter_t* hCPU)
	{
		BotwSpinStep(hCPU, 12);
	}

	// FFL byte swaps floats through FPRs, which does not keep NaN payloads intact under the recompiler
	void HleFflSwapFloats(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(values, uint32, 0);
		ppcDefineParamS32(count, 1);
		for (sint32 i = 0; i < count; i++)
			values[i] = _swapEndianU32(values[i]);
		osLib_returnFromFunction(hCPU, 0);
	}

	// recursive lock used by Xenoblade Chronicles X (registered, currently not hooked)
	struct XcxLock
	{
		std::atomic<uint32be> depth;
		uint32be owner;
		uint32 unused08;
	};

	void HleXcxEnterLock(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(lock, XcxLock, 0);
		const uint32 self = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();
		if (lock->owner == (uint32be)self)
		{
			lock->depth.store(lock->depth.load() + 1);
			osLib_returnFromFunction(hCPU, 0);
			return;
		}
		auto tryTake = [&]() {
			uint32be expected = 0;
			if (!lock->depth.compare_exchange_strong(expected, (uint32be)1))
				return false;
			lock->owner = self;
			return true;
		};
		while (!tryTake())
		{
			bool taken = false;
			for (int spin = 0; spin < 50 && !taken; spin++)
			{
				_mm_pause();
				taken = tryTake();
			}
			if (taken)
				break;
			PPCCore_switchToScheduler();
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	// Monster Hunter 3 Ultimate: the flag read here must come back 0 the first time
	void HleMh3uRace(PPCInterpreter_t* hCPU)
	{
		uint8 result = memory_readU8(hCPU->gpr[3] + 0x3E5) ^ 1;
		if (s_mh3uFirstCall)
		{
			s_mh3uFirstCall = false;
			result = 0;
		}
		osLib_returnFromFunction(hCPU, result);
	}

	// Paper Mario Color Splash: an out of range index makes the caller return 0 instead of crashing
	void HlePmcsPaintStar(PPCInterpreter_t* hCPU)
	{
		const uint32 index = hCPU->gpr[3];
		hCPU->gpr[7] = index * 4;
		if (index >= 0x00800000)
		{
			hCPU->instructionPointer = memory_readU32(hCPU->gpr[1] + 0x4C); // caller's saved LR
			hCPU->gpr[1] += 0x48; // drop the caller's stack frame
			hCPU->gpr[3] = 0;
			return;
		}
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	// ---------------------------------------------------------------- patch steps

	void FixXcxHangCounter()
	{
		// the counter increment is replaced by the compare that follows it
		if (MPTR at = FindSignature(Exact(sigXcxHangCounter)))
		{
#ifdef CEMU_DEBUG_ASSERT
			cemuLog_log(LogType::Force, "GamePatch: XCX hang counter at 0x{:08x}", at);
#endif
			memory_writeU32(at, memory_readU32(at + 4));
		}
	}

	void FixXcxReductionBuffer()
	{
		MPTR at = FindSignature(Masked(sigXcxReduction, maskXcxReduction));
		if (!at)
			return;
#ifdef CEMU_DEBUG_ASSERT
		cemuLog_log(LogType::Force, "GamePatch: XCX reduction buffer check at 0x{:08x}", at);
#endif
		// make the called check always answer "no"
		const MPTR call = at + 0x14;
		const MPTR check = call + (memory_readU32(call) & 0x3FFFFFC);
		memory_writeU32(check, kOpLiR3_0);
		memory_writeU32(check + 4, kOpBlr);
	}

	void HookBotwSpinLoops()
	{
		s_hleBotwSpin1 = osLib_getFunctionIndex("hle", "h000000001");
		if (MPTR at = FindSignature(Exact(sigBotwSpin1)))
		{
#ifdef CEMU_DEBUG_ASSERT
			cemuLog_log(LogType::Force, "GamePatch: BotW spin loop 1 at 0x{:08x}", at);
#endif
			memory_writeU32(at - 4, HleInstruction(s_hleBotwSpin1));
		}
		s_hleBotwSpin2 = osLib_getFunctionIndex("hle", "h000000002");
		if (MPTR at = FindSignature(Exact(sigBotwSpin2)))
		{
#ifdef CEMU_DEBUG_ASSERT
			cemuLog_log(LogType::Force, "GamePatch: BotW spin loop 2 at 0x{:08x}", at);
#endif
			memory_writeU32(at - 4, HleInstruction(s_hleBotwSpin2));
		}
	}

	void HookFflSwap()
	{
		s_hleFflSwap = osLib_getFunctionIndex("hle", "h000000003");
		const Signature variants[] = {Exact(sigFflSwapA), Exact(sigFflSwapB)};
		for (size_t v = 0; v < std::size(variants); v++)
		{
			if (MPTR at = FindSignature(variants[v]))
			{
				cemuLog_logDebug(LogType::Force, "GamePatch: FFL float swap (variant {}) at 0x{:08x}", v + 1, at);
				memory_writeU32(at, HleInstruction(s_hleFflSwap));
				return;
			}
		}
	}

	void FixMh3uRace()
	{
		if (MPTR at = FindSignature(Masked(sigMh3uRace, maskMh3uRace)))
		{
			const MPTR call = at + 0x10;
			cemuLog_log(LogType::Force, "GamePatch: MH3U race fix at 0x{:08x}", call);
			memory_writeU32(call, AbsoluteCall(PPCInterpreter_makeCallableExportDepr(HleMh3uRace)));
		}
	}

	void FixSmashSoftlock()
	{
		if (MPTR at = FindSignature(Masked(sigSmashSoftlock, maskSmashSoftlock)))
		{
			cemuLog_logDebug(LogType::Force, "GamePatch: Smash soft lock fix at 0x{:08x}", at);
			memory_writeU32(at + 0x20, memory_readU32(at + 0x1C));
		}
	}

	void FixPmcsPaintStar()
	{
		if (MPTR at = FindSignature(Exact(sigPmcsPaintStar)))
		{
			cemuLog_logDebug(LogType::Force, "GamePatch: Color Splash crash fix at 0x{:08x}", at);
			memory_writeU32(at + 0x20, AbsoluteCall(PPCInterpreter_makeCallableExportDepr(HlePmcsPaintStar)));
		}
	}

	void FixBayo2AudioQueue()
	{
		if (MPTR at = FindSignature(Masked(sigBayo2AudioQueue, maskBayo2AudioQueue)))
		{
			// unsigned compare (cmpl) becomes signed compare (cmp)
			const MPTR compare = at + 0x34;
			cemuLog_log(LogType::Force, "GamePatch: Bayonetta 2 audio queue fix at 0x{:08x}", compare);
			memory_writeU32(compare, memory_readU32(compare) & ~(0x3FFu << 1));
		}
	}

	void FixTwwDspChannel()
	{
		const uint32 hash = CafeSystem::GetRPXHashUpdated();
		for (const TwwDspFix& fix : kTwwDspFixes)
		{
			if (fix.rpxHash != hash)
				continue;
			if (memory_readU32(fix.address) == kTwwDspBranchGe)
			{
				debug_printf("GamePatch: TWW DSP channel fix at 0x%08x\n", fix.address);
				memory_writeU32(fix.address, kTwwDspBranch);
			}
			return;
		}
	}

	void FixSm3dwDynamicRes()
	{
		if (MPTR at = FindSignature(Exact(sigSm3dwDynamicRes)))
		{
			cemuLog_log(LogType::Force, "GamePatch: SM3DW dynamic resolution disabled at 0x{:08x}", at);
			memory_writeU32(at, kOpBlr);
		}
	}

	void FixTwwWaitLock()
	{
		if (MPTR at = FindSignature(Exact(sigTwwWait)))
		{
			cemuLog_log(LogType::Force, "GamePatch: TWW wait lock removed at 0x{:08x}", at);
			for (uint32 offset : {0x34u, 0x48u, 0x50u, 0x64u}) // mutex lock and unlock calls
				memory_writeU32(at + offset, kOpNop);
		}
	}

	void PatchBrowserForCustomServices()
	{
		const NetworkService service = ActiveSettings::GetNetworkService();
		// let the Miiverse and eShop browsers open any domain
		if (service != NetworkService::Nintendo && CafeSystem::GetForegroundTitleArgStr().ends_with("wave.rpx"))
		{
			if (MPTR at = FindSignature(Exact(sigWaveWhitelist)))
			{
				cemuLog_log(LogType::Force, "GamePatch: browser domain whitelist disabled at 0x{:08x}", at);
				memory_writeU32(at, kOpLiR3_1);
				memory_writeU32(at + 4, kOpBlr);
			}
		}
		// accept the service's certificates in the browser's OpenSSL
		if (IsNetworkServiceSSLDisabled(service) && RPLLoader_GetHandleByModuleName("libopenssl.rpl") != RPL_INVALID_HANDLE)
		{
			if (MPTR at = FindSignature(Exact(sigOpensslVerifyChain)))
			{
				cemuLog_log(LogType::Force, "GamePatch: OpenSSL certificate chain check disabled at 0x{:08x}", at);
				memory_writeU32(at + 0x28, kOpNop);
				memory_writeU32(at + 0x40, kOpLiR3_1);
			}
		}
	}
}

bool GamePatch_IsNonReturnFunction(uint32 hleIndex)
{
	// the spin loop hooks continue after the hooked instruction instead of returning to LR
	return hleIndex == (uint32)s_hleBotwSpin1 || hleIndex == (uint32)s_hleBotwSpin2;
}

void GamePatch_scan()
{
	const uint32 startTick = GetTickCount();
	FixXcxHangCounter();
	FixXcxReductionBuffer();
	HookBotwSpinLoops();
	HookFflSwap();
	FixMh3uRace();
	FixSmashSoftlock();
	FixPmcsPaintStar();
	FixBayo2AudioQueue();
	FixTwwDspChannel();
	FixSm3dwDynamicRes();
	FixTwwWaitLock();
	PatchBrowserForCustomServices();
	cemuLog_log(LogType::Force, "GamePatch: scan took {}ms", GetTickCount() - startTick);
}

RunAtCemuBoot g_gamePatchHleRegistration([]()
	{
		osLib_addFunction("hle", "h000000001", HleBotwSpin1);
		osLib_addFunction("hle", "h000000002", HleBotwSpin2);
		osLib_addFunction("hle", "h000000003", HleFflSwapFloats);
		osLib_addFunction("hle", "h000000004", HleXcxEnterLock);
	});
