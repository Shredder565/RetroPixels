// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Espresso interpreter core: the two memory models, the instruction decode
// tables and the public single step entry points.
//
// HLE model ("slim", used when Cafe OS is emulated at a high level): guest
// code always runs in problem state with a flat 1:1 address space and paired
// singles enabled. Memory is accessed directly at memory_base + EA.
//
// LLE model ("full", used when booting the real Cafe OS kernel): data and
// instruction accesses go through the BATs and the hashed page table when
// MSR[DR]/MSR[IR] are set, supervisor SPRs are available and a failed data
// translation raises a data storage interrupt.

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

namespace EspressoItp
{
	inline uint32 LoadBE32(const uint8* p) { uint32 v; memcpy(&v, p, 4); return _swapEndianU32(v); }
	inline uint16 LoadBE16(const uint8* p) { uint16 v; memcpy(&v, p, 2); return _swapEndianU16(v); }
	inline void StoreBE32(uint8* p, uint32 v) { v = _swapEndianU32(v); memcpy(p, &v, 4); }
	inline void StoreBE16(uint8* p, uint16 v) { v = _swapEndianU16(v); memcpy(p, &v, 2); }

	inline uint8* HostPtr(uint64 physical) { return memory_base + physical; }

	// ---- HLE memory model ----

	struct FlatMemory
	{
		static constexpr bool kSupervisor = false;

		static uint32 FetchInstruction(PPCInterpreter_t* cpu, uint32 ea) { return LoadBE32(HostPtr(ea)); }

		static uint8 Read8(PPCInterpreter_t* cpu, uint32 ea) { return *HostPtr(ea); }
		static uint16 Read16(PPCInterpreter_t* cpu, uint32 ea) { return LoadBE16(HostPtr(ea)); }
		static uint32 Read32(PPCInterpreter_t* cpu, uint32 ea) { return LoadBE32(HostPtr(ea)); }
		static uint64 ReadDouble(PPCInterpreter_t* cpu, uint32 ea)
		{
			return ((uint64)LoadBE32(HostPtr(ea)) << 32) | LoadBE32(HostPtr((uint64)ea + 4));
		}

		static void Write8(PPCInterpreter_t* cpu, uint32 ea, uint8 v) { *HostPtr(ea) = v; }
		static void Write16(PPCInterpreter_t* cpu, uint32 ea, uint16 v) { StoreBE16(HostPtr(ea), v); }
		static void Write32(PPCInterpreter_t* cpu, uint32 ea, uint32 v) { StoreBE32(HostPtr(ea), v); }
		static void WriteDouble(PPCInterpreter_t* cpu, uint32 ea, uint64 v)
		{
			StoreBE32(HostPtr(ea), (uint32)(v >> 32));
			StoreBE32(HostPtr((uint64)ea + 4), (uint32)v);
		}

		static uint64 ReadSingleWidened(PPCInterpreter_t* cpu, uint32 ea) { return SingleToDoubleBits(LoadBE32(HostPtr(ea))); }
		static void WriteSingleNarrowed(PPCInterpreter_t* cpu, uint32 ea, uint64 v) { StoreBE32(HostPtr(ea), DoubleToSingleBits(v)); }

		static uint8* ReservationPointer(PPCInterpreter_t* cpu, uint32 ea) { return HostPtr(ea); }
		static bool ConsumeAccessFault(PPCInterpreter_t* cpu) { return false; }

		static bool ZeroCacheLine(PPCInterpreter_t* cpu, uint32 line)
		{
			memset(HostPtr(line), 0, 32);
			return true;
		}

		static uint64 TimeBase(PPCInterpreter_t* cpu) { return PPCInterpreter_getMainCoreCycleCounter(); }
	};

	// ---- LLE memory model ----

	struct TranslatedMemory
	{
		static constexpr bool kSupervisor = true;

		// hardware register window, not emulated in LLE mode
		static bool IsIORange(uint32 pa) { return pa >= 0x0C000000 && pa < 0x0D100000; }

		// block address translation: returns true and the physical address on a hit
		static bool LookupBAT(const uint32* upper, const uint32* lower, uint32 ea, uint32& pa)
		{
			for (uint32 i = 0; i < 8; i++)
			{
				uint32 batU = upper[i];
				if ((batU & 2) == 0)
					continue; // Vs (supervisor valid) clear
				uint32 blockMask = 0xF0000000 | ((~(batU >> 2) & 0x7FF) << 17);
				if ((ea & blockMask) == (batU & 0xFFFE0000))
				{
					pa = (ea & ~blockMask) | (lower[i] & 0xFFFE0000 & blockMask);
					return true;
				}
			}
			return false;
		}

		// hashed page table lookup for 128KiB pages (Cafe OS kernel layout)
		static bool LookupPageTable(PPCInterpreter_t* cpu, uint32 ea, uint32& pa)
		{
			const auto& ext = cpu->sprExtended;
			uint32 vsid = ext.sr[ea >> 28] & 0xFFFFFF;
			uint32 pageIndex = (ea >> 17) & 0x7FF;
			uint32 hash = (vsid ^ pageIndex) & 0x7FFFF;
			uint32 tableBase = ext.sdr1 & 0xFFFF0000;
			uint32 tableMask = ext.sdr1 & 0x1FF;
			for (uint32 pass = 0; pass < 2; pass++)
			{
				uint64 group = (uint64)(tableBase | (((hash >> 10) & tableMask) << 16)) + (uint64)(hash & 0x3FF) * 64;
				for (uint32 slot = 0; slot < 8; slot++)
				{
					uint32 w0 = LoadBE32(HostPtr(group + slot * 8));
					uint32 w1 = LoadBE32(HostPtr(group + slot * 8 + 4));
					if ((w0 & 0x80000000) == 0)
						continue;
					uint32 api = w0 & 0x3F;
					uint32 secondary = (w0 >> 6) & 1;
					uint32 entryVsid = (w0 >> 7) & 0xFFFFFF;
					if (api == (pageIndex >> 5) && entryVsid == vsid && secondary == pass)
					{
						if (pass == 1)
							assert_dbg(); // secondary hash is not expected
						pa = (ea & 0x1FFFF) | (((w1 >> 12) & 0xFFFFF) << 12);
						return true;
					}
				}
				hash = ~hash; // secondary hash
			}
			return false;
		}

		static void RaiseDataStorageInterrupt(PPCInterpreter_t* cpu, uint32 ea)
		{
			auto& ext = cpu->sprExtended;
			cemuLog_logDebug(LogType::Force, "Interpreter: DSI at {:08x} for address {:08x}", cpu->instructionPointer, ea);
			cpu->memoryException = true; // tells the current instruction to stop
			ext.srr0 = cpu->instructionPointer;
			ext.srr1 = ext.msr & 0x87C0FFFF;
			ext.dar = ea;
			ext.dsisr = 0x40000000; // no translation found
			ext.msr &= ~0x04EF36u;
			cpu->instructionPointer = 0xFFF00300;
		}

		static uint32 TranslateData(PPCInterpreter_t* cpu, uint32 ea)
		{
			if ((cpu->sprExtended.msr & MSR_DR) == 0)
				return ea;
			uint32 pa;
			if (LookupBAT(cpu->sprExtended.dbatU, cpu->sprExtended.dbatL, ea, pa))
				return pa;
			if (LookupPageTable(cpu, ea, pa))
				return pa;
			RaiseDataStorageInterrupt(cpu, ea);
			return ea;
		}

		static uint32 TranslateFetch(PPCInterpreter_t* cpu, uint32 ea)
		{
			if ((cpu->sprExtended.msr & MSR_IR) == 0)
				return ea;
			uint32 pa;
			if (LookupBAT(cpu->sprExtended.ibatU, cpu->sprExtended.ibatL, ea, pa))
				return pa;
			assert_dbg(); // instruction page table walks are not implemented
			return ea;
		}

		static uint32 FetchInstruction(PPCInterpreter_t* cpu, uint32 ea) { return LoadBE32(HostPtr(TranslateFetch(cpu, ea))); }

		// Only 32-bit accesses respect an already raised interrupt and the IO
		// window; byte reads check the IO window only.
		static uint8 Read8(PPCInterpreter_t* cpu, uint32 ea)
		{
			uint32 pa = TranslateData(cpu, ea);
			if (IsIORange(pa))
			{
				cemu_assert_unimplemented();
				return 0;
			}
			return *HostPtr(pa);
		}

		static uint16 Read16(PPCInterpreter_t* cpu, uint32 ea) { return LoadBE16(HostPtr(TranslateData(cpu, ea))); }

		static uint32 Read32(PPCInterpreter_t* cpu, uint32 ea)
		{
			uint32 pa = TranslateData(cpu, ea);
			if (cpu->memoryException)
				return 0;
			if ((pa >= 0x01FFF000 && pa < 0x02000000) || (pa >= 0xFFEB73B0 && pa < 0xFFEB77BC))
				cemuLog_logDebug(LogType::Force, "Interpreter: boot parameter read {:08x} at {:08x}", pa, cpu->instructionPointer);
			if (IsIORange(pa))
			{
				cemu_assert_unimplemented();
				return 0;
			}
			return LoadBE32(HostPtr(pa));
		}

		// each half is translated on its own (the second one at the first half's address + 4)
		static uint64 ReadDouble(PPCInterpreter_t* cpu, uint32 ea)
		{
			uint32 hi = LoadBE32(HostPtr(TranslateData(cpu, ea)));
			uint32 lo = LoadBE32(HostPtr((uint64)TranslateData(cpu, ea) + 4));
			return ((uint64)hi << 32) | lo;
		}

		static void Write8(PPCInterpreter_t* cpu, uint32 ea, uint8 v) { *HostPtr(TranslateData(cpu, ea)) = v; }
		static void Write16(PPCInterpreter_t* cpu, uint32 ea, uint16 v) { StoreBE16(HostPtr(TranslateData(cpu, ea)), v); }

		static void Write32(PPCInterpreter_t* cpu, uint32 ea, uint32 v)
		{
			uint32 pa = TranslateData(cpu, ea);
			if (cpu->memoryException)
				return;
			if (IsIORange(pa))
			{
				cemu_assert_unimplemented();
				return;
			}
			StoreBE32(HostPtr(pa), v);
		}

		static void WriteDouble(PPCInterpreter_t* cpu, uint32 ea, uint64 v)
		{
			uint64 pa = TranslateData(cpu, ea);
			StoreBE32(HostPtr(pa), (uint32)(v >> 32));
			StoreBE32(HostPtr(pa + 4), (uint32)v);
		}

		// single precision float accesses are not translated
		static uint64 ReadSingleWidened(PPCInterpreter_t* cpu, uint32 ea) { return SingleToDoubleBits(LoadBE32(HostPtr(ea))); }
		static void WriteSingleNarrowed(PPCInterpreter_t* cpu, uint32 ea, uint64 v) { StoreBE32(HostPtr(ea), DoubleToSingleBits(v)); }

		static uint8* ReservationPointer(PPCInterpreter_t* cpu, uint32 ea) { return HostPtr(TranslateData(cpu, ea)); }

		static bool ConsumeAccessFault(PPCInterpreter_t* cpu)
		{
			if (!cpu->memoryException)
				return false;
			cpu->memoryException = false;
			return true;
		}

		static bool ZeroCacheLine(PPCInterpreter_t* cpu, uint32 line)
		{
			Write32(cpu, line, 0);
			if (ConsumeAccessFault(cpu))
				return false;
			for (uint32 offset = 4; offset < 32; offset += 4)
				Write32(cpu, line + offset, 0);
			return true;
		}

		// the time base ticks once per bus clock / 4; the LLE scheduler counts core cycles
		static uint64 TimeBase(PPCInterpreter_t* cpu) { return cpu->global->tb / 20ULL; }
	};
}

#include "PPCInterpreterSPR.hpp"
#include "PPCInterpreterOPC.hpp"
#include "PPCInterpreterLoadStore.hpp"
#include "PPCInterpreterALU.hpp"

namespace EspressoItp
{
	// ---- unknown encodings ----

	// unknown primary opcodes do not complete
	void Op_UnknownPrimary(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: unknown instruction {:08x} at {:08x}", op, cpu->instructionPointer);
		cemu_assert_unimplemented();
	}

	// unknown extended opcodes are skipped
	void Op_UnknownExtended(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: unknown extended instruction {:08x} at {:08x}", op, cpu->instructionPointer);
		cemu_assert_unimplemented();
		cpu->instructionPointer += 4;
	}

	// an all-zero word usually means execution ran into data
	void Op_Zero(PPCInterpreter_t* cpu, uint32 op)
	{
		debug_printf("Interpreter: executing zero word at 0x%08X\n", (unsigned int)cpu->instructionPointer);
#ifdef CEMU_DEBUG_ASSERT
		assert_dbg();
		while (true)
			std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
		cpu->instructionPointer += 4;
	}

	void Op_Primary17(PPCInterpreter_t* cpu, uint32 op)
	{
		if ((op >> 1) & 1)
			Op_SC(cpu, op);
		else
			Op_UnknownExtended(cpu, op);
	}

	// ---- decode tables ----

	using Table1024 = std::array<OpHandler, 1024>;

	// opcode 4: index is the 10-bit extended opcode; most paired single
	// instructions only look at its low 5 bits
	template<typename Bus>
	constexpr Table1024 BuildGroup4()
	{
		Table1024 t{};
		for (uint32 xo = 0; xo < 1024; xo++)
		{
			uint32 lo = xo & 0x1F;
			uint32 hi = xo >> 5;
			OpHandler h = Op_UnknownExtended;
			switch (lo)
			{
			case 0:
				if (hi == 0) h = Op_PS_CMPU0;
				else if (hi == 1) h = Op_PS_CMPO0;
				else if (hi == 2) h = Op_PS_CMPU1;
				break;
			case 6: h = Op_PSQ_LX<Bus>; break;
			case 7: h = Op_PSQ_STX<Bus>; break;
			case 8:
				if (hi == 1) h = Op_PS_NEG;
				else if (hi == 2) h = Op_PS_MR;
				else if (hi == 4) h = Op_PS_NABS;
				else if (hi == 8) h = Op_PS_ABS;
				break;
			case 10: h = Op_PS_SUM0; break;
			case 11: h = Op_PS_SUM1; break;
			case 12: h = Op_PS_MULS0; break;
			case 13: h = Op_PS_MULS1; break;
			case 14: h = Op_PS_MADDS0; break;
			case 15: h = Op_PS_MADDS1; break;
			case 16:
				if (hi == 16) h = Op_PS_MERGE00;
				else if (hi == 17) h = Op_PS_MERGE01;
				else if (hi == 18) h = Op_PS_MERGE10;
				else if (hi == 19) h = Op_PS_MERGE11;
				break;
			case 18: h = Op_PS_DIV; break;
			case 20: h = Op_PS_SUB; break;
			case 21: h = Op_PS_ADD; break;
			case 22: h = Op_NoEffect; break; // dcbz_l (locked cache is not modelled)
			case 23: h = Op_PS_SEL; break;
			case 24: h = Op_PS_RES; break;
			case 25: h = Op_PS_MUL; break;
			case 26: h = Op_PS_RSQRTE; break;
			case 28: h = Op_PS_MSUB; break;
			case 29: h = Op_PS_MADD; break;
			case 30: h = Op_PS_NMSUB; break;
			case 31: h = Op_PS_NMADD; break;
			default: break;
			}
			t[xo] = h;
		}
		return t;
	}

	constexpr Table1024 BuildGroup19()
	{
		Table1024 t{};
		for (auto& h : t)
			h = Op_UnknownExtended;
		t[0] = Op_MCRF;
		t[16] = Op_BCLR;
		t[33] = Op_CRNOR;
		t[50] = Op_RFI;
		t[129] = Op_CRANDC;
		t[150] = Op_NoEffect; // isync
		t[193] = Op_CRXOR;
		t[225] = Op_CRNAND;
		t[257] = Op_CRAND;
		t[289] = Op_CREQV;
		t[417] = Op_CRORC;
		t[449] = Op_CROR;
		t[528] = Op_BCCTR;
		return t;
	}

	template<typename Bus>
	constexpr Table1024 BuildGroup31()
	{
		using M = Bus;
		Table1024 t{};
		for (auto& h : t)
			h = Op_UnknownExtended;
		constexpr uint32 OE = 0x200; // OE bit as seen in the 10-bit extended opcode
		// compare, trap
		t[0] = Op_CMP;
		t[32] = Op_CMPL;
		t[4] = Op_TW;
		// add/subtract: <ComplementA, operand B, carry in, writes CA, writes OV>
		t[266] = Op_AddFamily<false, AddB::RegB, AddCarryIn::Zero, false, false>;       // add
		t[266 | OE] = Op_AddFamily<false, AddB::RegB, AddCarryIn::Zero, false, true>;   // addo
		t[10] = Op_AddFamily<false, AddB::RegB, AddCarryIn::Zero, true, false>;         // addc
		t[10 | OE] = Op_AddFamily<false, AddB::RegB, AddCarryIn::Zero, true, true>;     // addco
		t[138] = Op_AddFamily<false, AddB::RegB, AddCarryIn::XerCA, true, false>;       // adde
		t[138 | OE] = Op_AddFamily<false, AddB::RegB, AddCarryIn::XerCA, true, true>;   // addeo
		t[202] = Op_AddFamily<false, AddB::Zero, AddCarryIn::XerCA, true, false>;       // addze
		t[202 | OE] = Op_AddFamily<false, AddB::Zero, AddCarryIn::XerCA, true, true>;   // addzeo
		t[234] = Op_AddFamily<false, AddB::MinusOne, AddCarryIn::XerCA, true, false>;   // addme
		t[234 | OE] = Op_AddFamily<false, AddB::MinusOne, AddCarryIn::XerCA, true, true>; // addmeo
		t[40] = Op_AddFamily<true, AddB::RegB, AddCarryIn::One, false, false>;          // subf
		t[40 | OE] = Op_AddFamily<true, AddB::RegB, AddCarryIn::One, false, true>;      // subfo
		t[8] = Op_AddFamily<true, AddB::RegB, AddCarryIn::One, true, false>;            // subfc
		t[8 | OE] = Op_AddFamily<true, AddB::RegB, AddCarryIn::One, true, true>;        // subfco
		t[136] = Op_AddFamily<true, AddB::RegB, AddCarryIn::XerCA, true, false>;        // subfe
		t[136 | OE] = Op_AddFamily<true, AddB::RegB, AddCarryIn::XerCA, true, true>;    // subfeo
		t[200] = Op_AddFamily<true, AddB::Zero, AddCarryIn::XerCA, true, false>;        // subfze
		t[200 | OE] = Op_AddFamily<true, AddB::Zero, AddCarryIn::XerCA, true, true>;    // subfzeo
		t[232] = Op_AddFamily<true, AddB::MinusOne, AddCarryIn::XerCA, true, false>;    // subfme
		t[232 | OE] = Op_AddFamily<true, AddB::MinusOne, AddCarryIn::XerCA, true, true>; // subfmeo
		t[104] = Op_NEG;
		t[104 | OE] = Op_NEGO;
		// multiply/divide
		t[235] = Op_MULLW<false>;
		t[235 | OE] = Op_MULLW<true>;
		t[75] = Op_MULHW;
		t[75 | OE] = Op_MULHW;
		t[11] = Op_MULHWU;
		t[11 | OE] = Op_MULHWU;
		t[491] = Op_DIVW<false>;
		t[491 | OE] = Op_DIVW<true>;
		t[459] = Op_DIVWU<false>;
		t[459 | OE] = Op_DIVWU<true>;
		// logic, shifts
		t[28] = Op_AND;
		t[60] = Op_ANDC;
		t[444] = Op_OR;
		t[412] = Op_ORC;
		t[316] = Op_XOR;
		t[476] = Op_NAND;
		t[124] = Op_NOR;
		t[284] = Op_EQV;
		t[954] = Op_EXTSB;
		t[922] = Op_EXTSH;
		t[26] = Op_CNTLZW;
		t[24] = Op_SLW;
		t[536] = Op_SRW;
		t[792] = Op_SRAW;
		t[824] = Op_SRAWI;
		// register moves
		t[19] = Op_MFCR;
		t[144] = Op_MTCRF;
		t[512] = Op_MCRXR;
		t[83] = Op_MFMSR;
		t[146] = Op_MTMSR;
		t[339] = Op_MFSPR<M>;
		t[467] = Op_MTSPR<M>;
		t[371] = Op_MFTB<M>;
		t[595] = Op_MFSR;
		t[210] = Op_MTSR;
		// integer loads: <size, extension, indexed, update>
		t[23] = Op_Load<M, 4, LoadExt::Zero, true, false>;          // lwzx
		t[55] = Op_Load<M, 4, LoadExt::Zero, true, true>;           // lwzux
		t[279] = Op_Load<M, 2, LoadExt::Zero, true, false>;         // lhzx
		t[311] = Op_Load<M, 2, LoadExt::Zero, true, true>;          // lhzux
		t[343] = Op_Load<M, 2, LoadExt::Sign, true, false>;         // lhax
		t[375] = Op_Load<M, 2, LoadExt::Sign, true, true>;          // lhaux
		t[87] = Op_Load<M, 1, LoadExt::Zero, true, false>;          // lbzx
		t[119] = Op_Load<M, 1, LoadExt::Zero, true, true>;          // lbzux
		t[534] = Op_Load<M, 4, LoadExt::ByteReverse, true, false>;  // lwbrx
		t[790] = Op_Load<M, 2, LoadExt::ByteReverse, true, false>;  // lhbrx
		t[20] = Op_LWARX<M>;
		t[533] = Op_LSWX<M>;
		t[597] = Op_LSWI<M>;
		// integer stores: <size, indexed, update, byte reverse>
		t[151] = Op_Store<M, 4, true, false, false>;  // stwx
		t[183] = Op_Store<M, 4, true, true, false>;   // stwux
		t[407] = Op_Store<M, 2, true, false, false>;  // sthx
		t[439] = Op_Store<M, 2, true, true, false>;   // sthux
		t[215] = Op_Store<M, 1, true, false, false>;  // stbx
		t[247] = Op_Store<M, 1, true, true, false>;   // stbux
		t[662] = Op_Store<M, 4, true, false, true>;   // stwbrx
		t[918] = Op_Store<M, 2, true, false, true>;   // sthbrx
		t[150] = Op_STWCX<M>;
		t[661] = Op_STSWX<M>;
		t[725] = Op_STSWI<M>;
		// floating point loads/stores: <indexed, update>
		t[535] = Op_LFS<M, true, false>;
		t[567] = Op_LFS<M, true, true>;
		t[599] = Op_LFD<M, true, false>;
		t[631] = Op_LFD<M, true, true>;
		t[663] = Op_STFS<M, true, false>;
		t[695] = Op_STFS<M, true, true>;
		t[727] = Op_STFD<M, true, false>;
		t[759] = Op_STFD<M, true, true>;
		t[983] = Op_STFIWX<M>;
		// cache and synchronisation
		t[54] = Op_CacheFlush;  // dcbst
		t[86] = Op_CacheFlush;  // dcbf
		t[278] = Op_NoEffect;   // dcbt
		t[470] = Op_NoEffect;   // dcbi
		t[1014] = Op_DCBZ<M>;
		t[982] = Op_ICBI;
		t[598] = Op_NoEffect;   // sync
		t[854] = Op_NoEffect;   // eieio
		t[306] = Op_NoEffect;   // tlbie
		t[566] = Op_TLBSYNC;
		return t;
	}

	constexpr std::array<OpHandler, 32> BuildGroup59()
	{
		std::array<OpHandler, 32> t{};
		for (auto& h : t)
			h = Op_UnknownExtended;
		t[18] = Op_FDIVS;
		t[20] = Op_FSUBS;
		t[21] = Op_FADDS;
		t[24] = Op_FRES;
		t[25] = Op_FMULS;
		t[28] = Op_FMSUBS;
		t[29] = Op_FMADDS;
		t[30] = Op_FNMSUBS;
		t[31] = Op_FNMADDS;
		return t;
	}

	// opcode 63: the A form opcodes (low 5 bits of the extended opcode) take
	// precedence over the X form ones, as in the hardware decoder. This also
	// means fcmpo (X form 32) executes as fcmpu.
	constexpr Table1024 BuildGroup63()
	{
		Table1024 t{};
		for (auto& h : t)
			h = Op_UnknownExtended;
		t[14] = Op_FCTIW;
		t[38] = Op_MTFSB1;
		t[40] = Op_FNEG;
		t[72] = Op_FMR;
		t[136] = Op_FNABS;
		t[264] = Op_FABS;
		t[583] = Op_MFFS;
		t[711] = Op_MTFSF;
		// an A form opcode claims every extended opcode with the same low 5 bits
		auto claimAForm = [&t](uint32 low5, OpHandler handler)
		{
			for (uint32 xo = low5; xo < 1024; xo += 32)
				t[xo] = handler;
		};
		claimAForm(0, Op_FCMPU);
		claimAForm(12, Op_FRSP);
		claimAForm(15, Op_FCTIWZ);
		claimAForm(18, Op_FDIV);
		claimAForm(20, Op_FSUB);
		claimAForm(21, Op_FADD);
		claimAForm(23, Op_FSEL);
		claimAForm(25, Op_FMUL);
		claimAForm(26, Op_FRSQRTE);
		claimAForm(28, Op_FMSUB);
		claimAForm(29, Op_FMADD);
		claimAForm(30, Op_FNMSUB);
		claimAForm(31, Op_FNMADD);
		return t;
	}

	template<typename Bus>
	struct DecodeTables
	{
		static constexpr Table1024 group4 = BuildGroup4<Bus>();
		static constexpr Table1024 group19 = BuildGroup19();
		static constexpr Table1024 group31 = BuildGroup31<Bus>();
		static constexpr std::array<OpHandler, 32> group59 = BuildGroup59();
		static constexpr Table1024 group63 = BuildGroup63();
	};

	template<typename Bus> void Group4(PPCInterpreter_t* cpu, uint32 op) { DecodeTables<Bus>::group4[(op >> 1) & 0x3FF](cpu, op); }
	template<typename Bus> void Group19(PPCInterpreter_t* cpu, uint32 op) { DecodeTables<Bus>::group19[(op >> 1) & 0x3FF](cpu, op); }
	template<typename Bus> void Group31(PPCInterpreter_t* cpu, uint32 op) { DecodeTables<Bus>::group31[(op >> 1) & 0x3FF](cpu, op); }
	template<typename Bus> void Group59(PPCInterpreter_t* cpu, uint32 op) { DecodeTables<Bus>::group59[(op >> 1) & 0x1F](cpu, op); }
	template<typename Bus> void Group63(PPCInterpreter_t* cpu, uint32 op) { DecodeTables<Bus>::group63[(op >> 1) & 0x3FF](cpu, op); }

	template<typename Bus>
	constexpr std::array<OpHandler, 64> BuildPrimary()
	{
		using M = Bus;
		std::array<OpHandler, 64> t{};
		for (auto& h : t)
			h = Op_UnknownPrimary;
		t[0] = Op_Zero;
		t[1] = Op_HLE;
		t[3] = Op_TWI;
		t[4] = Group4<M>;
		t[7] = Op_MULLI;
		t[8] = Op_SUBFIC;
		t[10] = Op_CMPLI;
		t[11] = Op_CMPI;
		t[12] = Op_ADDIC<false>;
		t[13] = Op_ADDIC<true>;
		t[14] = Op_ADDI;
		t[15] = Op_ADDIS;
		t[16] = Op_BC;
		t[17] = Op_Primary17;
		t[18] = Op_B;
		t[19] = Group19<M>;
		t[20] = Op_RLWIMI;
		t[21] = Op_RLWINM;
		t[23] = Op_RLWNM;
		t[24] = Op_LogicImm<ImmLogic::Or>;
		t[25] = Op_LogicImm<ImmLogic::OrShifted>;
		t[26] = Op_LogicImm<ImmLogic::Xor>;
		t[27] = Op_LogicImm<ImmLogic::XorShifted>;
		t[28] = Op_LogicImm<ImmLogic::And>;
		t[29] = Op_LogicImm<ImmLogic::AndShifted>;
		t[31] = Group31<M>;
		t[32] = Op_LWZ<M>;
		t[33] = Op_LWZU<M>;
		t[34] = Op_Load<M, 1, LoadExt::Zero, false, false>;    // lbz
		t[35] = Op_LBZU<M>;
		t[36] = Op_STW<M>;
		t[37] = Op_StoreUpdateAlways<M, 4>;                     // stwu
		t[38] = Op_Store<M, 1, false, false, false>;            // stb
		t[39] = Op_StoreUpdateAlways<M, 1>;                     // stbu
		t[40] = Op_Load<M, 2, LoadExt::Zero, false, false>;    // lhz
		t[41] = Op_LHZU<M>;
		t[42] = Op_Load<M, 2, LoadExt::Sign, false, false>;    // lha
		t[43] = Op_Load<M, 2, LoadExt::Sign, false, true>;     // lhau
		t[44] = Op_Store<M, 2, false, false, false>;            // sth
		t[45] = Op_Store<M, 2, false, true, false>;             // sthu
		t[46] = Op_LMW<M>;
		t[47] = Op_STMW<M>;
		t[48] = Op_LFS<M, false, false>;
		t[49] = Op_LFS<M, false, true>;
		t[50] = Op_LFD<M, false, false>;
		t[51] = Op_LFD<M, false, true>;
		t[52] = Op_STFS<M, false, false>;
		t[53] = Op_STFS<M, false, true>;
		t[54] = Op_STFD<M, false, false>;
		t[55] = Op_STFD<M, false, true>;
		t[56] = Op_PSQ_L<M, false>;
		t[57] = Op_PSQ_L<M, true>;
		t[59] = Group59<M>;
		t[60] = Op_PSQ_ST<M, false>;
		t[61] = Op_PSQ_ST<M, true>;
		t[63] = Group63<M>;
		return t;
	}

	template<typename Bus>
	struct PrimaryTable
	{
		static constexpr std::array<OpHandler, 64> table = BuildPrimary<Bus>();
	};

	template<typename Bus>
	inline void ExecuteOne(PPCInterpreter_t* cpu)
	{
		if constexpr (Bus::kSupervisor)
			cpu->global->tb++;
		uint32 op = Bus::FetchInstruction(cpu, cpu->instructionPointer);
		PrimaryTable<Bus>::table[op >> 26](cpu, op);
	}
}

// HLE mode: Cafe OS user mode code, flat memory
void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* hCPU)
{
	EspressoItp::ExecuteOne<EspressoItp::FlatMemory>(hCPU);
}

// LLE mode: supervisor code with address translation
void PPCInterpreterFull_executeInstruction(PPCInterpreter_t* hCPU)
{
	EspressoItp::ExecuteOne<EspressoItp::TranslatedMemory>(hCPU);
}
