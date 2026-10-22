// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Special purpose register access for mfspr/mtspr. Included by
// PPCInterpreterImpl.cpp only.
//
// The HLE model (Cafe OS user mode) only knows the registers games may touch
// in problem state. The LLE model additionally handles the supervisor
// registers the Cafe OS kernel sets up (BATs, segment registers, SPRGs, the
// locked cache DMA registers, ...). Unknown registers read as zero and
// writes to them are ignored.

namespace EspressoItp
{
	// supervisor register numbers that only matter to the LLE model
	enum : uint32
	{
		kSprDSISR = 18,
		kSprDAR = 19,
		kSprSPRG0 = 272, // SPRG0-3 are 272-275
		kSprTBLWrite = 284,
		kSprTBUWrite = 285,
		kSprIBAT0U = 528, // IBAT0-3 at 528-535, DBAT0-3 at 536-543 (upper, lower pairs)
		kSprIBAT4U = 560, // IBAT4-7 at 560-567, DBAT4-7 at 568-575
		kSprHID5 = 944,
		kSprCAR = 948,
		kSprBCR = 949,
		kSprHID4 = 1011,
		kSprL2CR = 1017,
	};

	// decodes the BAT register numbers: returns false if spr is not a BAT
	inline bool DecodeBatSpr(uint32 spr, bool& isData, bool& isLower, uint32& index)
	{
		uint32 rel;
		uint32 group;
		if (spr >= kSprIBAT0U && spr < kSprIBAT0U + 16)
		{
			rel = spr - kSprIBAT0U;
			group = 0;
		}
		else if (spr >= kSprIBAT4U && spr < kSprIBAT4U + 16)
		{
			rel = spr - kSprIBAT4U;
			group = 4;
		}
		else
			return false;
		isData = rel >= 8;
		isLower = (rel & 1) != 0;
		index = group + ((rel & 7) >> 1);
		return true;
	}

	inline uint32* BatRegister(PPCInterpreter_t* cpu, bool isData, bool isLower, uint32 index)
	{
		auto& ext = cpu->sprExtended;
		if (isData)
			return isLower ? &ext.dbatL[index] : &ext.dbatU[index];
		return isLower ? &ext.ibatL[index] : &ext.ibatU[index];
	}

	// registers that are visible in problem state (both models)
	inline bool ReadUserSpr(PPCInterpreter_t* cpu, uint32 spr, uint32& value)
	{
		switch (spr)
		{
		case SPR_LR: value = cpu->spr.LR; return true;
		case SPR_CTR: value = cpu->spr.CTR; return true;
		case SPR_XER: value = PPCInterpreter_getXER(cpu); return true;
		case SPR_UPIR: value = cpu->spr.UPIR; return true;
		default: break;
		}
		if (spr >= SPR_UGQR0 && spr <= SPR_UGQR7)
		{
			value = cpu->spr.UGQR[spr - SPR_UGQR0];
			return true;
		}
		return false;
	}

	inline bool WriteUserSpr(PPCInterpreter_t* cpu, uint32 spr, uint32 value)
	{
		switch (spr)
		{
		case SPR_LR: cpu->spr.LR = value; return true;
		case SPR_CTR: cpu->spr.CTR = value; return true;
		case SPR_XER: PPCInterpreter_setXER(cpu, value); return true;
		default: break;
		}
		if (spr >= SPR_UGQR0 && spr <= SPR_UGQR7)
		{
			cpu->spr.UGQR[spr - SPR_UGQR0] = value;
			return true;
		}
		return false;
	}

	// ---- HLE model ----

	inline uint32 ReadSprHLE(PPCInterpreter_t* cpu, uint32 spr)
	{
		uint32 value = 0;
		if (ReadUserSpr(cpu, spr, value))
			return value;
		if (spr == SPR_DEC)
		{
			// Cafe OS never reads DEC from user code
			assert_dbg();
			uint64 elapsed = PPCInterpreter_getMainCoreCycleCounter() - ppcMainThreadDECCycleStart;
			return elapsed >= ppcMainThreadDECCycleValue ? 0 : (uint32)(ppcMainThreadDECCycleValue - elapsed);
		}
		if (spr == SPR_PVR)
		{
			assert_dbg(); // not readable in problem state
			return 0;
		}
		cemuLog_logDebug(LogType::Force, "Interpreter: mfspr of unhandled SPR {} at {:08x}", spr, cpu->instructionPointer);
		return 0;
	}

	inline void WriteSprHLE(PPCInterpreter_t* cpu, uint32 spr, uint32 value)
	{
		if (WriteUserSpr(cpu, spr, value))
			return;
		cemuLog_logDebug(LogType::Force, "Interpreter: mtspr of unhandled SPR {} at {:08x}", spr, cpu->instructionPointer);
	}

	// ---- LLE model ----

	inline uint32 ReadSprLLE(PPCInterpreter_t* cpu, uint32 spr)
	{
		uint32 value = 0;
		if (ReadUserSpr(cpu, spr, value))
			return value;
		auto& ext = cpu->sprExtended;
		switch (spr)
		{
		case SPR_PVR: return 0x70010101; // Espresso
		case kSprDAR: return ext.dar;
		case kSprDSISR: return ext.dsisr;
		case SPR_FPECR: return ext.fpecr;
		case kSprSPRG0 + 0:
		case kSprSPRG0 + 1:
		case kSprSPRG0 + 2:
		case kSprSPRG0 + 3:
			return ext.sprg[spr - kSprSPRG0];
		// HID0/1/2/4/5, SCR, CAR, BCR and L2CR are not modelled and read as zero
		default:
			break;
		}
		bool isData, isLower;
		uint32 index;
		if (DecodeBatSpr(spr, isData, isLower, index))
			return *BatRegister(cpu, isData, isLower, index);
		return 0;
	}

	// SCR bit 31 disables the boot ROM mapping. When the kernel sets it we
	// clear low memory where the boot ROM was visible.
	inline void WriteSCR(PPCInterpreter_t* cpu, uint32 value)
	{
		constexpr uint32 kBootRomDisable = 0x80000000;
		uint32 previous = cpu->global->sprGlobal.scr;
		value |= previous & kBootRomDisable; // the bit cannot be cleared again
		if ((previous & kBootRomDisable) == 0 && (value & kBootRomDisable) != 0)
		{
			memset(memory_base, 0, 0x4000);
#ifdef _WIN32
			// catch stray accesses to the old boot ROM area while debugging
			DWORD oldProtect;
			VirtualProtect(memory_base, 0x1000, PAGE_READONLY, &oldProtect);
#endif
		}
		cpu->global->sprGlobal.scr = value;
	}

	// DMAL with the trigger bit set copies between memory and the locked cache (0xE0000000)
	inline void WriteDMAL(PPCInterpreter_t* cpu, uint32 value)
	{
		auto& ext = cpu->sprExtended;
		ext.dmaL = value;
		if ((value & 2) == 0)
			return;
		uint32 lineCount = ((ext.dmaU & 0x1F) << 2) | ((value >> 2) & 3);
		if (lineCount == 0)
			lineCount = 128;
		uint32 byteCount = lineCount * 32;
		uint32 memAddr = ext.dmaU & ~0x1Fu;
		uint32 cacheAddr = value & ~0x1Fu;
		uint32 cacheOffset = ((cacheAddr >> 28) == 0xE) ? cacheAddr - 0xE0000000 : 0;
		uint8* cachePtr = memory_base + 0xE0000000 + cacheOffset;
		uint8* memPtr = memory_getPointerFromVirtualOffset(memAddr);
		if ((value >> 4) & 1)
			memmove(cachePtr, memPtr, byteCount); // load: memory -> locked cache
		else
			memmove(memPtr, cachePtr, byteCount); // store: locked cache -> memory
		ext.dmaL = value & ~2u;
	}

	inline void WriteSprLLE(PPCInterpreter_t* cpu, uint32 spr, uint32 value)
	{
		if (WriteUserSpr(cpu, spr, value))
			return;
		auto& ext = cpu->sprExtended;
		switch (spr)
		{
		case kSprTBLWrite:
		case kSprTBUWrite:
			// only used to reset the time base
			if (value != 0)
				assert_dbg();
			cpu->global->tb = 0;
			return;
		case SPR_FPECR: ext.fpecr = value; return;
		case SPR_SRR0: ext.srr0 = value; return;
		case SPR_SRR1: ext.srr1 = value; return;
		case kSprSPRG0 + 0:
		case kSprSPRG0 + 1:
		case kSprSPRG0 + 2:
		case kSprSPRG0 + 3:
			ext.sprg[spr - kSprSPRG0] = value;
			return;
		case SPR_SCR: WriteSCR(cpu, value); return;
		case SPR_DMAU: ext.dmaU = value; return;
		case SPR_DMAL: WriteDMAL(cpu, value); return;
		case SPR_SDR1: ext.sdr1 = value; return;
		// DEC, HID0/2/4/5, L2CR, CAR, BCR and the performance monitor are accepted and ignored
		default:
			break;
		}
		bool isData, isLower;
		uint32 index;
		if (DecodeBatSpr(spr, isData, isLower, index))
			*BatRegister(cpu, isData, isLower, index) = value;
	}
}
