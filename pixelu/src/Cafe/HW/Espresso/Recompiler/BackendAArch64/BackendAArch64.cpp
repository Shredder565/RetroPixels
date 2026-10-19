// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// AArch64 backend: code generation for every intermediate op, the helpers
// called from generated code, the entry/exit stubs and executable memory.
//
// Floating point results follow what the interpreter computes when it is
// built for AArch64: the multiply-add forms are fused like the compiler
// contracts a*c+b, single precision results are rounded with FCVT, and
// conversions that must not depend on FPCR.FZ (lfs, stfs) are done with
// integer instructions.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterHelper.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "util/MemMapper/MemMapper.h"
#include "BackendAArch64.h"

#include <xbyak_aarch64.h>

#if defined(__APPLE__)
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

namespace AArch64Backend
{
	namespace xa = Xbyak_aarch64;

	constexpr uint32 kIntColorCount = 16;
	constexpr uint32 kFloatColorCount = 16;
	// X4-X15 are not preserved by called functions, X22-X25 are
	constexpr uint32 kIntColorRegs[kIntColorCount] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 22, 23, 24, 25};
	constexpr uint32 kIntCallerSavedColors = 0x0FFF;
	// V16-V31; called functions preserve at most the low half of V8-V15, so none of them is preserved
	constexpr uint32 kFloatRegBase = 16;
	constexpr uint32 kFloatCallerSavedColors = 0xFFFF;

	constexpr uint32 kRegCPU = 19;
	constexpr uint32 kRegMemBase = 20;
	constexpr uint32 kRegJumpTable = 21;

	// stack frame below the saved registers (offsets from SP)
	constexpr uint32 kFrameIntSave = 0;                         // caller saved integer colors, 8 bytes each
	constexpr uint32 kFrameFloatSave = kFrameIntSave + 12 * 8;  // vector colors, 16 bytes each
	constexpr uint32 kFrameSpill = kFrameFloatSave + 16 * 16;
	constexpr uint32 kMaxSpillSlots = 64;                       // 16 bytes each
	constexpr uint32 kFrameSize = kFrameSpill + kMaxSpillSlots * 16;
	constexpr uint32 kSavedRegsSize = 96;                       // X19-X28, X29, X30
	static_assert(kFrameSize % 16 == 0 && kFrameSize < 4096);

	static_assert(FPSCR_VXSNAN == (1 << 24));

	// ---- helpers called from generated code ----

	uint32 Helper_Interpret(PPCInterpreter_t* hCPU, uint32 address)
	{
		hCPU->instructionPointer = address;
		PPCInterpreterSlim_executeInstruction(hCPU);
		return hCPU->instructionPointer;
	}

	void Helper_HLE(PPCInterpreter_t* hCPU, uint32 opcode)
	{
		// unimplemented imports cost extra time so guest code polling one
		// of them does not keep other threads from running
		if ((opcode & 0xFFFF) == 0xFFD0)
			hCPU->remainingCycles -= 500;
		EspressoItp::Op_HLE(hCPU, opcode);
	}

	uint32 Helper_TimeBaseLower(PPCInterpreter_t* hCPU)
	{
		return (uint32)coreinit::OSGetSystemTime();
	}

	uint32 Helper_TimeBaseUpper(PPCInterpreter_t* hCPU)
	{
		return (uint32)(coreinit::OSGetSystemTime() >> 32);
	}

	// the floating point helpers read their operand from temporaryFPR[0] and write the result back there

	void Helper_FRES(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		const double estimate = fres_espresso(t.fp0);
		t.fp0 = estimate;
		t.fp1 = estimate;
	}

	void Helper_FRSQRTE(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		t.fp0 = frsqrte_espresso(t.fp0);
	}

	void Helper_PRES(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		const double lane0 = t.fp0, lane1 = t.fp1;
		t.fp0 = (double)(float)fres_espresso(lane0);
		t.fp1 = (double)(float)fres_espresso(lane1);
	}

	void Helper_PRSQRTE(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		const double lane0 = t.fp0, lane1 = t.fp1;
		t.fp0 = (double)(float)frsqrte_espresso(lane0);
		t.fp1 = (double)(float)frsqrte_espresso(lane1);
	}

	// fctiw: rounding per FPSCR[RN]. The word goes to the low half, the
	// high half is 0xFFF80000 (+1 for negative inputs that round to 0)
	void Helper_FCTIW(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		const double input = t.fp0;
		uint32 word;
		if (input != input)
			word = 0x80000000;
		else if (input >= 2147483648.0)
			word = 0x7FFFFFFF;
		else if (input < -2147483648.0)
			word = 0x80000000;
		else
		{
			double rounded;
			switch (hCPU->fpscr & 3)
			{
			case 0: rounded = std::nearbyint(input); break;
			case 1: rounded = std::trunc(input); break;
			case 2: rounded = std::ceil(input); break;
			default: rounded = std::floor(input); break;
			}
			if (rounded >= 2147483648.0)
				word = 0x7FFFFFFF;
			else if (rounded < -2147483648.0)
				word = 0x80000000;
			else
				word = (uint32)(sint32)rounded;
		}
		uint64 bits = 0xFFF8000000000000ull | word;
		if (word == 0 && std::signbit(input))
			bits |= 1ull << 32;
		t.fp0int = bits;
	}

	// quantized accesses: EA in temporaryGPR_reg[0], GQR in temporaryGPR_reg[1], data in temporaryFPR[0]

	uint32 ReadQuantized(uint32 ea, uint32 size)
	{
		const uint8* p = memory_base + ea;
		if (size == 1)
			return p[0];
		if (size == 2)
			return ((uint32)p[0] << 8) | p[1];
		return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
	}

	void WriteQuantized(uint32 ea, uint32 size, uint32 value)
	{
		uint8* p = memory_base + ea;
		for (uint32 i = 0; i < size; i++)
			p[i] = (uint8)(value >> ((size - 1 - i) * 8));
	}

	void PairedLoad(PPCInterpreter_t* hCPU, bool singleSlot)
	{
		const uint32 ea = hCPU->temporaryGPR_reg[0];
		const uint32 gqr = hCPU->temporaryGPR_reg[1];
		const uint32 type = (gqr >> 16) & 7;
		const uint32 scale = (gqr >> 24) & 0x3F;
		const uint32 size = EspressoItp::GqrAccessSize(type);
		FPR_t& t = hCPU->temporaryFPR[0];
		const uint32 first = ReadQuantized(ea, size);
		const uint32 second = singleSlot ? 0 : ReadQuantized(ea + size, size);
		t.fp0 = (double)EspressoItp::Dequantize(first, type, scale);
		t.fp1 = singleSlot ? 1.0 : (double)EspressoItp::Dequantize(second, type, scale);
	}

	void PairedStore(PPCInterpreter_t* hCPU, bool singleSlot)
	{
		const uint32 ea = hCPU->temporaryGPR_reg[0];
		const uint32 gqr = hCPU->temporaryGPR_reg[1];
		const uint32 type = gqr & 7;
		const uint32 scale = (gqr >> 8) & 0x3F;
		const uint32 size = EspressoItp::GqrAccessSize(type);
		const FPR_t& t = hCPU->temporaryFPR[0];
		WriteQuantized(ea, size, EspressoItp::Quantize((float)t.fp0, type, scale));
		if (!singleSlot)
			WriteQuantized(ea + size, size, EspressoItp::Quantize((float)t.fp1, type, scale));
	}

	void Helper_PSQLoad(PPCInterpreter_t* hCPU) { PairedLoad(hCPU, false); }
	void Helper_PSQLoadSingle(PPCInterpreter_t* hCPU) { PairedLoad(hCPU, true); }
	void Helper_PSQStore(PPCInterpreter_t* hCPU) { PairedStore(hCPU, false); }
	void Helper_PSQStoreSingle(PPCInterpreter_t* hCPU) { PairedStore(hCPU, true); }

	// The interpreter computes fnmadds as -(float)(a*c+b) or as
	// (float)(-(a*c)-b) depending on how the host compiler contracted the
	// expression; the two differ in the sign of an exact zero. This runs the
	// interpreter's handler once on a*c == -b to find out which one it is.
	bool InterpreterNegatesSingleMaddAfterRounding()
	{
		static const bool s_result = []()
		{
			auto cpu = std::make_unique<PPCInterpreter_t>();
			memset(cpu.get(), 0, sizeof(PPCInterpreter_t));
			cpu->fpr[1].fp0 = 1.0;  // frA
			cpu->fpr[2].fp0 = 1.0;  // frC
			cpu->fpr[3].fp0 = -1.0; // frB
			const uint32 opcode = (59u << 26) | (0u << 21) | (1u << 16) | (3u << 11) | (2u << 6) | (31u << 1); // fnmadds f0, f1, f2, f3
			EspressoItp::Op_FNMADDS(cpu.get(), opcode);
			return std::signbit(cpu->fpr[0].fp0);
		}();
		return s_result;
	}

	// ---- executable memory ----

	std::mutex s_execMutex;
	uint8* s_execBlock = nullptr;
	size_t s_execBlockSize = 0;
	size_t s_execBlockUsed = 0;

	uint8* AllocateExecutable(size_t size)
	{
		size = (size + 63) & ~(size_t)63;
		if (!s_execBlock || s_execBlockUsed + size > s_execBlockSize)
		{
			const size_t blockSize = std::max<size_t>(4 * 1024 * 1024, (size + 0xFFFF) & ~(size_t)0xFFFF);
#if defined(__APPLE__)
			void* block = mmap(nullptr, blockSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
			if (block == MAP_FAILED)
				block = nullptr;
#else
			void* block = MemMapper::AllocateMemory(nullptr, blockSize, MemMapper::PAGE_PERMISSION::P_RWX);
#endif
			if (!block)
				return nullptr;
			s_execBlock = (uint8*)block;
			s_execBlockSize = blockSize;
			s_execBlockUsed = 0;
		}
		uint8* p = s_execBlock + s_execBlockUsed;
		s_execBlockUsed += size;
		return p;
	}

	// copies finished code into executable memory
	void* PublishCode(const uint8* code, size_t size)
	{
		std::lock_guard<std::mutex> lock(s_execMutex);
		uint8* p = AllocateExecutable(size);
		if (!p)
			return nullptr;
#if defined(__APPLE__)
		pthread_jit_write_protect_np(0);
		memcpy(p, code, size);
		pthread_jit_write_protect_np(1);
		sys_icache_invalidate(p, size);
#elif defined(_WIN32)
		memcpy(p, code, size);
		FlushInstructionCache(GetCurrentProcess(), p, size);
#else
		memcpy(p, code, size);
		__builtin___clear_cache((char*)p, (char*)p + size);
#endif
		return p;
	}

	// code is assembled into ordinary memory and copied to its final place afterwards
	class StagingAllocator : public xa::Allocator
	{
	public:
		uint32_t* alloc(size_t size) override { return new uint32_t[(size + 3) / 4]; }
		void free(uint32_t* p) override { delete[] p; }
		bool useProtect() const override { return false; }
	};

	// true if v is an AArch64 logical immediate for 32-bit operations
	bool IsLogicalImm32(uint32 v)
	{
		if (v == 0 || v == 0xFFFFFFFF)
			return false;
		// smallest repeating element
		uint32 width = 32;
		while (width > 2)
		{
			const uint32 half = width / 2;
			const uint32 mask = (1u << half) - 1;
			if ((v & mask) != ((v >> half) & mask))
				break;
			width = half;
		}
		const uint32 mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
		const uint32 e = v & mask;
		// a rotated run of ones has exactly two bit changes going round the element
		const uint32 rotated = ((e >> 1) | (e << (width - 1))) & mask;
		return std::popcount(e ^ rotated) == 2;
	}

	// ---- emitter with the building blocks shared by the stubs and the code generator ----

	class Emitter : public xa::CodeGenerator
	{
	public:
		Emitter(size_t maxSize, xa::Allocator* allocator) : xa::CodeGenerator(maxSize, xa::DontSetProtectRWE, allocator) {}

		const xa::XReg rCPU{kRegCPU};
		const xa::XReg rMemBase{kRegMemBase};
		const xa::XReg rJumpTable{kRegJumpTable};

		void MoveW(const xa::WReg& d, const xa::WReg& s)
		{
			if (d.getIdx() != s.getIdx())
				mov(d, s);
		}

		void MoveV(const xa::VReg& d, const xa::VReg& s)
		{
			if (d.getIdx() != s.getIdx())
				mov(d.b16, s.b16);
		}

		void LoadImm32(const xa::WReg& d, uint32 v)
		{
			const uint32 lo = v & 0xFFFF, hi = v >> 16;
			if (hi == 0)
				movz(d, lo);
			else if (lo == 0)
				movz(d, hi, 16);
			else if (hi == 0xFFFF)
				movn(d, (~v) & 0xFFFF);
			else if (lo == 0xFFFF)
				movn(d, (~v) >> 16, 16);
			else
			{
				movz(d, lo);
				movk(d, hi, 16);
			}
		}

		void LoadImm64(const xa::XReg& d, uint64 v)
		{
			bool first = true;
			for (uint32 shift = 0; shift < 64; shift += 16)
			{
				const uint32 part = (uint32)(v >> shift) & 0xFFFF;
				if (part == 0)
					continue;
				if (first)
					movz(d, part, shift);
				else
					movk(d, part, shift);
				first = false;
			}
			if (first)
				movz(d, 0);
		}

		// d = s + imm (32-bit). tmp must differ from s
		void AddImm32(const xa::WReg& d, const xa::WReg& s, uint32 imm, const xa::WReg& tmp)
		{
			const uint32 neg = 0u - imm;
			if (imm == 0)
				MoveW(d, s);
			else if (imm < 4096)
				add(d, s, imm);
			else if (neg < 4096)
				sub(d, s, neg);
			else if ((imm & 0xFFF) == 0 && imm < 0x1000000)
				add(d, s, imm >> 12, 12);
			else if ((neg & 0xFFF) == 0 && neg < 0x1000000)
				sub(d, s, neg >> 12, 12);
			else
			{
				LoadImm32(tmp, imm);
				add(d, s, tmp);
			}
		}

		// flags = a compared with imm (may use W16)
		void CompareImm32(const xa::WReg& a, uint32 imm)
		{
			const uint32 neg = 0u - imm;
			if (imm < 4096)
				cmp(a, imm);
			else if (neg < 4096)
				cmn(a, neg);
			else
			{
				LoadImm32(w16, imm);
				cmp(a, w16);
			}
		}

		// ---- PPCInterpreter_t field access (may use X17) ----

		void CpuLoadW(const xa::WReg& r, uint32 offset)
		{
			if (offset % 4 == 0 && offset < 4 * 4096)
				ldr(r, ptr(rCPU, offset));
			else if (offset < 256)
				ldur(r, ptr(rCPU, (sint32)offset));
			else
			{
				LoadImm64(x17, offset);
				ldr(r, ptr(rCPU, x17));
			}
		}

		void CpuStoreW(const xa::WReg& r, uint32 offset)
		{
			if (offset % 4 == 0 && offset < 4 * 4096)
				str(r, ptr(rCPU, offset));
			else if (offset < 256)
				stur(r, ptr(rCPU, (sint32)offset));
			else
			{
				LoadImm64(x17, offset);
				str(r, ptr(rCPU, x17));
			}
		}

		void CpuLoadB(const xa::WReg& r, uint32 offset)
		{
			if (offset < 4096)
				ldrb(r, ptr(rCPU, offset));
			else
			{
				LoadImm64(x17, offset);
				ldrb(r, ptr(rCPU, x17));
			}
		}

		void CpuStoreB(const xa::WReg& r, uint32 offset)
		{
			if (offset < 4096)
				strb(r, ptr(rCPU, offset));
			else
			{
				LoadImm64(x17, offset);
				strb(r, ptr(rCPU, x17));
			}
		}

		void CpuLoadQ(const xa::QReg& r, uint32 offset)
		{
			if (offset % 16 == 0 && offset < 16 * 4096)
				ldr(r, ptr(rCPU, offset));
			else if (offset < 256)
				ldur(r, ptr(rCPU, (sint32)offset));
			else
			{
				LoadImm64(x17, offset);
				ldr(r, ptr(rCPU, x17));
			}
		}

		void CpuStoreQ(const xa::QReg& r, uint32 offset)
		{
			if (offset % 16 == 0 && offset < 16 * 4096)
				str(r, ptr(rCPU, offset));
			else if (offset < 256)
				stur(r, ptr(rCPU, (sint32)offset));
			else
			{
				LoadImm64(x17, offset);
				str(r, ptr(rCPU, x17));
			}
		}

		void CpuLoadD(const xa::DReg& r, uint32 offset)
		{
			if (offset % 8 == 0 && offset < 8 * 4096)
				ldr(r, ptr(rCPU, offset));
			else if (offset < 256)
				ldur(r, ptr(rCPU, (sint32)offset));
			else
			{
				LoadImm64(x17, offset);
				ldr(r, ptr(rCPU, x17));
			}
		}

		// leave generated code, W0 = guest address to continue at
		void EmitLeave()
		{
			LoadImm64(x16, (uint64)(uintptr_t)PPCRecompiler_leaveRecompilerCode_visited);
			br(x16);
		}

		// continue at the guest address in W0 through the jump table (8 bytes per 4 byte instruction)
		void EmitJumpViaTable()
		{
			add(x16, rJumpTable, x0, xa::LSL, 1);
			ldr(x16, ptr(x16));
			br(x16);
		}

		// W0 = guest address; only the code area has jump table entries
		void EmitLeaveOrJumpViaTable()
		{
			static_assert(PPC_REC_CODE_AREA_START == 0 && PPC_REC_CODE_AREA_END == 0x10000000);
			xa::Label outside;
			lsr(w16, w0, 28);
			cbnz(w16, outside);
			EmitJumpViaTable();
			L(outside);
			EmitLeave();
		}
	};

	// ---- code generator for one function ----

	class Generator : public Emitter
	{
	public:
		Generator(IMLFunction& func, size_t maxSize, xa::Allocator* allocator) : Emitter(maxSize, allocator), m_func(func) {}

		bool Generate();
		const std::vector<std::pair<uint32, uint32>>& EntryPoints() const { return m_entryPoints; }

	private:
		IMLFunction& m_func;
		std::vector<std::pair<uint32, uint32>> m_entryPoints;
		std::unordered_map<const IMLSegment*, size_t> m_blockIndex;
		std::vector<xa::Label> m_blockLabels;

		xa::Label& BlockLabel(const IMLSegment* b) { return m_blockLabels[m_blockIndex.at(b)]; }

		uint32 Color(IMLReg r) const
		{
			const sint16 c = m_func.vregs[r].hostReg;
			cemu_assert_debug(c >= 0);
			return (uint32)c;
		}
		xa::WReg W(IMLReg r) const
		{
			cemu_assert_debug(m_func.KindOf(r) == IMLRegKind::Int && Color(r) < kIntColorCount);
			return xa::WReg(kIntColorRegs[Color(r)]);
		}
		xa::XReg X(IMLReg r) const { return xa::XReg(kIntColorRegs[Color(r)]); }
		xa::VReg V(IMLReg r) const
		{
			cemu_assert_debug(m_func.KindOf(r) == IMLRegKind::Float && Color(r) < kFloatColorCount);
			return xa::VReg(kFloatRegBase + Color(r));
		}
		xa::DReg D(IMLReg r) const { return xa::DReg(kFloatRegBase + Color(r)); }
		xa::QReg Q(IMLReg r) const { return xa::QReg(kFloatRegBase + Color(r)); }

		// W16 = 32-bit guest address of a memory instruction (may use W17)
		void GuestAddressToW16(const IMLInstruction& inst)
		{
			const IMLReg base = inst.r[1], index = inst.r[2];
			if (base != IML_NOREG && index != IML_NOREG)
			{
				add(w16, W(base), W(index));
				AddImm32(w16, w16, (uint32)inst.imm, w17);
			}
			else if (base != IML_NOREG || index != IML_NOREG)
				AddImm32(w16, W(base != IML_NOREG ? base : index), (uint32)inst.imm, w17);
			else
				LoadImm32(w16, (uint32)inst.imm);
		}

		// host address operand of a guest memory access (may use W16/W17)
		xa::AdrExt GuestMemory(const IMLInstruction& inst)
		{
			if (inst.r[1] != IML_NOREG && inst.r[2] == IML_NOREG && inst.imm == 0)
				return xa::ptr(rMemBase, W(inst.r[1]), xa::UXTW);
			GuestAddressToW16(inst);
			return xa::ptr(rMemBase, w16, xa::UXTW);
		}

		void CallHelper(const void* function, uint32 saveMask)
		{
			const uint32 intMask = saveMask & 0xFFFF;
			const uint32 floatMask = saveMask >> IML_SAVEMASK_FLOAT_SHIFT;
			cemu_assert_debug((intMask & ~kIntCallerSavedColors) == 0);
			for (uint32 c = 0; c < kIntColorCount; c++)
			{
				if (intMask & (1u << c))
					str(xa::XReg(kIntColorRegs[c]), ptr(sp, kFrameIntSave + c * 8));
			}
			for (uint32 c = 0; c < kFloatColorCount; c++)
			{
				if (floatMask & (1u << c))
					str(xa::QReg(kFloatRegBase + c), ptr(sp, kFrameFloatSave + c * 16));
			}
			mov(x0, rCPU);
			LoadImm64(x16, (uint64)(uintptr_t)function);
			blr(x16);
			for (uint32 c = 0; c < kIntColorCount; c++)
			{
				if (intMask & (1u << c))
					ldr(xa::XReg(kIntColorRegs[c]), ptr(sp, kFrameIntSave + c * 8));
			}
			for (uint32 c = 0; c < kFloatColorCount; c++)
			{
				if (floatMask & (1u << c))
					ldr(xa::QReg(kFloatRegBase + c), ptr(sp, kFrameFloatSave + c * 16));
			}
		}

		void CallHelperWithArg(const void* function, uint32 arg, uint32 saveMask)
		{
			LoadImm32(w1, arg);
			CallHelper(function, saveMask);
		}

		// dst = src with the significand cut to 25 bits as the Espresso does for the frC operand of single precision multiplies (both lanes, uses V2 and W0)
		void RoundMultiplier25(const xa::VReg& dst, const xa::VReg& src)
		{
			LoadImm32(w0, 0x08000000);
			dup(v2.d2, x0);
			add(dst.d2, src.d2, v2.d2);
			ushr(dst.d2, dst.d2, 28);
			shl(dst.d2, dst.d2, 28);
		}

		// both lanes rounded to single precision and widened again
		void RoundPairToSingle(const xa::VReg& r)
		{
			fcvtn(r.s2, r.d2);
			fcvtl(r.d2, r.s2);
		}

		bool EmitInt(const IMLInstruction& inst);
		bool EmitMemory(const IMLInstruction& inst);
		bool EmitFloat(const IMLInstruction& inst);
		bool EmitFloatMemory(const IMLInstruction& inst);
		bool EmitHome(const IMLInstruction& inst);
		bool EmitControl(const IMLInstruction& inst, IMLSegment* block, IMLSegment* layoutNext);
	};

	xa::Cond HostCond(IMLCond c)
	{
		switch (c)
		{
		case IMLCond::EQ: return xa::EQ;
		case IMLCond::NE: return xa::NE;
		case IMLCond::SLT: return xa::LT;
		case IMLCond::SGT: return xa::GT;
		case IMLCond::SLE: return xa::LE;
		case IMLCond::SGE: return xa::GE;
		case IMLCond::ULT: return xa::LO;
		case IMLCond::UGT: return xa::HI;
		case IMLCond::ULE: return xa::LS;
		case IMLCond::UGE: return xa::HS;
		}
		cemu_assert_debug(false);
		return xa::EQ;
	}

	xa::Cond InvertCond(xa::Cond c)
	{
		return (xa::Cond)((uint32)c ^ 1);
	}

	bool Generator::EmitInt(const IMLInstruction& inst)
	{
		switch (inst.op)
		{
		case IMLOp::MOV:
			MoveW(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::MOVI:
			LoadImm32(W(inst.r[0]), (uint32)inst.imm);
			return true;
		case IMLOp::ADD:
		case IMLOp::SUB:
		case IMLOp::AND:
		case IMLOp::OR:
		case IMLOp::XOR:
		case IMLOp::MUL:
		{
			const xa::WReg d = W(inst.r[0]);
			const xa::WReg a = W(inst.r[1]);
			if (inst.HasImm())
			{
				const uint32 imm = (uint32)inst.imm;
				switch (inst.op)
				{
				case IMLOp::ADD: AddImm32(d, a, imm, w16); return true;
				case IMLOp::SUB: AddImm32(d, a, 0u - imm, w16); return true;
				case IMLOp::MUL:
					LoadImm32(w16, imm);
					mul(d, a, w16);
					return true;
				default:
					break;
				}
				// logical operations
				if (imm == 0)
				{
					if (inst.op == IMLOp::AND)
						movz(d, 0);
					else
						MoveW(d, a);
					return true;
				}
				if (imm == 0xFFFFFFFF && inst.op != IMLOp::OR)
				{
					if (inst.op == IMLOp::AND)
						MoveW(d, a);
					else
						mvn(d, a);
					return true;
				}
				if (IsLogicalImm32(imm))
				{
					if (inst.op == IMLOp::AND)
						and_(d, a, (uint64)imm);
					else if (inst.op == IMLOp::OR)
						orr(d, a, (uint64)imm);
					else
						eor(d, a, (uint64)imm);
					return true;
				}
				LoadImm32(w16, imm);
				if (inst.op == IMLOp::AND)
					and_(d, a, w16);
				else if (inst.op == IMLOp::OR)
					orr(d, a, w16);
				else
					eor(d, a, w16);
				return true;
			}
			const xa::WReg b = W(inst.r[2]);
			switch (inst.op)
			{
			case IMLOp::ADD: add(d, a, b); break;
			case IMLOp::SUB: sub(d, a, b); break;
			case IMLOp::AND: and_(d, a, b); break;
			case IMLOp::OR: orr(d, a, b); break;
			case IMLOp::XOR: eor(d, a, b); break;
			default: mul(d, a, b); break;
			}
			return true;
		}
		case IMLOp::ANDC:
			bic(W(inst.r[0]), W(inst.r[1]), W(inst.r[2]));
			return true;
		case IMLOp::ORC:
			orn(W(inst.r[0]), W(inst.r[1]), W(inst.r[2]));
			return true;
		case IMLOp::NOT:
			mvn(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::NEG:
			neg(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::MULHS:
			smull(x0, W(inst.r[1]), W(inst.r[2]));
			lsr(X(inst.r[0]), x0, 32);
			return true;
		case IMLOp::MULHU:
			umull(x0, W(inst.r[1]), W(inst.r[2]));
			lsr(X(inst.r[0]), x0, 32);
			return true;
		case IMLOp::DIVS:
		{
			// the host gives 0 for x/0 and 0x80000000 for 0x80000000/-1,
			// the Espresso gives (x < 0 ? -1 : 0) and -1
			const xa::WReg a = W(inst.r[1]);
			const xa::WReg b = W(inst.r[2]);
			sdiv(w0, a, b);
			asr(w1, a, 31);
			cmp(b, 0);
			csel(w0, w1, w0, xa::EQ);
			LoadImm32(w2, 0x80000000);
			cmp(a, w2);
			ccmn(b, 1, 0, xa::EQ);
			movn(w1, 0);
			csel(w0, w1, w0, xa::EQ);
			mov(W(inst.r[0]), w0);
			return true;
		}
		case IMLOp::DIVU:
			udiv(W(inst.r[0]), W(inst.r[1]), W(inst.r[2]));
			return true;
		case IMLOp::SHL:
		case IMLOp::SHR:
		case IMLOp::SAR:
		{
			const xa::WReg d = W(inst.r[0]);
			const xa::WReg a = W(inst.r[1]);
			const uint32 amount = (uint32)inst.imm & 31;
			if (amount == 0)
				MoveW(d, a);
			else if (inst.op == IMLOp::SHL)
				lsl(d, a, amount);
			else if (inst.op == IMLOp::SHR)
				lsr(d, a, amount);
			else
				asr(d, a, amount);
			return true;
		}
		case IMLOp::ROTL:
		{
			const xa::WReg d = W(inst.r[0]);
			const xa::WReg a = W(inst.r[1]);
			if (inst.HasImm())
			{
				const uint32 amount = (uint32)inst.imm & 31;
				if (amount == 0)
					MoveW(d, a);
				else
					ror(d, a, 32 - amount);
				return true;
			}
			// rotating right by -n mod 32
			neg(w0, W(inst.r[2]));
			ror(d, a, w0);
			return true;
		}
		case IMLOp::SLW:
		case IMLOp::SRW:
		{
			// the host uses the amount mod 32, the guest clears the result for 32-63
			const xa::WReg b = W(inst.r[2]);
			if (inst.op == IMLOp::SLW)
				lsl(w0, W(inst.r[1]), b);
			else
				lsr(w0, W(inst.r[1]), b);
			tst(b, (uint64)32);
			csel(W(inst.r[0]), wzr, w0, xa::NE);
			return true;
		}
		case IMLOp::SRAW:
		{
			const xa::WReg d = W(inst.r[0]);
			const xa::WReg ca = W(inst.r[1]);
			const xa::WReg a = W(inst.r[2]);
			if (inst.HasImm())
			{
				const uint32 amount = (uint32)inst.imm & 31;
				if (amount == 0)
				{
					movz(w0, 0);
					MoveW(d, a);
					mov(ca, w0);
					return true;
				}
				// CA: negative value and a one bit shifted out
				tst(a, (uint64)((1u << amount) - 1));
				cset(w0, xa::NE);
				lsr(w1, a, 31);
				and_(w0, w0, w1);
				asr(d, a, amount);
				mov(ca, w0);
				return true;
			}
			// shift the sign extended value by the 6-bit amount
			and_(w16, W(inst.r[3]), (uint64)63);
			sxtw(x1, a);
			asr(x0, x1, x16);
			lsl(x2, x0, x16);
			cmp(x2, x1);
			cset(w2, xa::NE);
			lsr(x1, x1, 63);
			and_(w2, w2, w1);
			mov(d, w0);
			mov(ca, w2);
			return true;
		}
		case IMLOp::EXTS8:
			sxtb(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::EXTS16:
			sxth(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::CNTLZ:
			clz(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::BSWAP32:
			rev(W(inst.r[0]), W(inst.r[1]));
			return true;
		case IMLOp::BSWAP16:
			rev(w0, W(inst.r[1]));
			lsr(W(inst.r[0]), w0, 16);
			return true;
		case IMLOp::ADDC:
		case IMLOp::ADDE:
		{
			// r0 = result, r1 = carry (in and out for ADDE), r2 = a, r3 = b
			xa::WReg a = W(inst.r[2]);
			if (inst.flags & IMLF_INV_A)
			{
				mvn(w0, a);
				a = w0;
			}
			xa::WReg b = w1;
			if (inst.HasImm())
				LoadImm32(w1, (uint32)inst.imm);
			else
				b = W(inst.r[3]);
			if (inst.op == IMLOp::ADDE)
			{
				cmp(W(inst.r[1]), 1); // C = carry in
				adcs(w2, a, b);
			}
			else
				adds(w2, a, b);
			cset(w3, xa::CS);
			mov(W(inst.r[0]), w2);
			mov(W(inst.r[1]), w3);
			return true;
		}
		case IMLOp::CMPSET:
			if (inst.HasImm())
				CompareImm32(W(inst.r[1]), (uint32)inst.imm);
			else
				cmp(W(inst.r[1]), W(inst.r[2]));
			cset(W(inst.r[0]), HostCond(inst.Cond()));
			return true;
		case IMLOp::CMPCR:
		{
			const bool isSigned = inst.Cond() == IMLCond::SLT;
			if (inst.HasImm())
				CompareImm32(W(inst.r[3]), (uint32)inst.imm);
			else
				cmp(W(inst.r[3]), W(inst.r[4]));
			cset(W(inst.r[0]), isSigned ? xa::LT : xa::LO);
			cset(W(inst.r[1]), isSigned ? xa::GT : xa::HI);
			cset(W(inst.r[2]), xa::EQ);
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}

	bool Generator::EmitMemory(const IMLInstruction& inst)
	{
		switch (inst.op)
		{
		case IMLOp::LOAD:
		{
			const xa::WReg d = W(inst.r[0]);
			const bool reversed = (inst.flags & IMLF_BYTEREV) != 0;
			const xa::AdrExt m = GuestMemory(inst);
			if (inst.size == 4)
			{
				ldr(d, m);
				if (!reversed)
					rev(d, d);
			}
			else if (inst.size == 2)
			{
				ldrh(d, m);
				if (!reversed)
					rev16(d, d);
				if (inst.flags & IMLF_SIGNEXT)
					sxth(d, d);
			}
			else
			{
				if (inst.flags & IMLF_SIGNEXT)
					ldrsb(d, m);
				else
					ldrb(d, m);
			}
			return true;
		}
		case IMLOp::STORE:
		{
			const bool reversed = (inst.flags & IMLF_BYTEREV) != 0;
			xa::WReg v = W(inst.r[0]);
			if (!reversed && inst.size == 4)
			{
				rev(w1, v);
				v = w1;
			}
			else if (!reversed && inst.size == 2)
			{
				rev16(w1, v);
				v = w1;
			}
			const xa::AdrExt m = GuestMemory(inst);
			if (inst.size == 4)
				str(v, m);
			else if (inst.size == 2)
				strh(v, m);
			else
				strb(v, m);
			return true;
		}
		case IMLOp::ZEROLINE:
			GuestAddressToW16(inst);
			and_(w16, w16, (uint64)0xFFFFFFE0);
			add(x16, rMemBase, w16, xa::UXTW);
			stp(xzr, xzr, ptr(x16));
			stp(xzr, xzr, ptr(x16, 16));
			return true;
		case IMLOp::CMPXCHG:
		{
			// r0 = success, r1 = EA, r2 = expected value, r3 = new value, r4 = 0: fail without access
			xa::Label fail, done, retry, mismatch, unaligned;
			cbz(W(inst.r[4]), fail);
			rev(w0, W(inst.r[2]));
			rev(w1, W(inst.r[3]));
			add(x16, rMemBase, W(inst.r[1]), xa::UXTW);
			tst(W(inst.r[1]), (uint64)3);
			b(xa::NE, unaligned);
			L(retry);
			ldaxr(w2, ptr(x16));
			cmp(w2, w0);
			b(xa::NE, mismatch);
			stlxr(w3, w1, ptr(x16));
			cbnz(w3, retry);
			movz(W(inst.r[0]), 1);
			b(done);
			L(mismatch);
			clrex(15);
			b(fail);
			// exclusive accesses need natural alignment
			L(unaligned);
			ldr(w2, ptr(x16));
			cmp(w2, w0);
			b(xa::NE, fail);
			str(w1, ptr(x16));
			movz(W(inst.r[0]), 1);
			b(done);
			L(fail);
			movz(W(inst.r[0]), 0);
			L(done);
			return true;
		}
		default:
			return false;
		}
	}

	bool Generator::EmitHome(const IMLInstruction& inst)
	{
		const IMLReg owner = (IMLReg)inst.imm;
		const IMLVRegInfo& info = m_func.vregs[owner];
		const bool isFloat = m_func.KindOf(inst.r[0]) == IMLRegKind::Float;
		const bool load = inst.op == IMLOp::HOMELOAD;
		if (IMLGuest::IsGuest(owner))
		{
			const uint32 offset = (uint32)info.home;
			if (isFloat)
			{
				if (load)
					CpuLoadQ(Q(inst.r[0]), offset);
				else
					CpuStoreQ(Q(inst.r[0]), offset);
			}
			else if (info.homeSize == 1)
			{
				if (load)
					CpuLoadB(W(inst.r[0]), offset);
				else
					CpuStoreB(W(inst.r[0]), offset);
			}
			else
			{
				if (load)
					CpuLoadW(W(inst.r[0]), offset);
				else
					CpuStoreW(W(inst.r[0]), offset);
			}
			return true;
		}
		cemu_assert_debug(info.home >= 0 && info.home < (sint32)kMaxSpillSlots);
		const uint32 offset = kFrameSpill + (uint32)info.home * 16;
		if (isFloat)
		{
			if (load)
				ldr(Q(inst.r[0]), ptr(sp, offset));
			else
				str(Q(inst.r[0]), ptr(sp, offset));
		}
		else
		{
			if (load)
				ldr(W(inst.r[0]), ptr(sp, offset));
			else
				str(W(inst.r[0]), ptr(sp, offset));
		}
		return true;
	}

	bool Generator::EmitFloat(const IMLInstruction& inst)
	{
		switch (inst.op)
		{
		case IMLOp::FMOV:
			MoveV(V(inst.r[0]), V(inst.r[1]));
			return true;
		case IMLOp::FMOVLO:
			if (Color(inst.r[0]) != Color(inst.r[1]))
				ins(V(inst.r[0]).d2[0], V(inst.r[1]).d2[0]);
			return true;
		case IMLOp::FSIGN:
		case IMLOp::PSIGN:
		{
			// sign bit operations, NaNs are not touched otherwise
			const xa::VReg a = V(inst.r[1]);
			switch ((IMLSign)inst.sub)
			{
			case IMLSign::NEG:
				fneg(v0.d2, a.d2);
				break;
			case IMLSign::ABS:
				fabs(v0.d2, a.d2);
				break;
			case IMLSign::NABS:
				fabs(v0.d2, a.d2);
				fneg(v0.d2, v0.d2);
				break;
			}
			if (inst.op == IMLOp::FSIGN)
				ins(V(inst.r[0]).d2[0], v0.d2[0]);
			else
				mov(V(inst.r[0]).b16, v0.b16);
			return true;
		}
		case IMLOp::PMERGE:
			ins(v0.d2[0], V(inst.r[1]).d2[inst.sub >> 1]);
			ins(v0.d2[1], V(inst.r[2]).d2[inst.sub & 1]);
			mov(V(inst.r[0]).b16, v0.b16);
			return true;
		case IMLOp::FARITH:
		{
			const xa::DReg a = D(inst.r[1]);
			const xa::DReg b = D(inst.r[2]);
			switch ((IMLArith)inst.sub)
			{
			case IMLArith::ADD: fadd(d0, a, b); break;
			case IMLArith::SUB: fsub(d0, a, b); break;
			case IMLArith::MUL: fmul(d0, a, b); break;
			case IMLArith::DIV: fdiv(d0, a, b); break;
			}
			ins(V(inst.r[0]).d2[0], v0.d2[0]);
			return true;
		}
		case IMLOp::FARITHS:
		{
			const xa::DReg a = D(inst.r[1]);
			const xa::DReg b = D(inst.r[2]);
			switch ((IMLArith)inst.sub)
			{
			case IMLArith::ADD: fadd(d0, a, b); break;
			case IMLArith::SUB: fsub(d0, a, b); break;
			case IMLArith::MUL:
				RoundMultiplier25(v1, V(inst.r[2]));
				fmul(d0, a, d1);
				break;
			case IMLArith::DIV: fdiv(d0, a, b); break;
			}
			fcvt(s0, d0);
			fcvt(d0, s0);
			dup(V(inst.r[0]).d2, v0.d2[0]);
			return true;
		}
		case IMLOp::FMADD:
		{
			// r1 = a, r2 = c, r3 = b
			const xa::DReg a = D(inst.r[1]);
			const xa::DReg c = D(inst.r[2]);
			const xa::DReg b = D(inst.r[3]);
			switch ((IMLMadd)inst.sub)
			{
			case IMLMadd::MADD: fmadd(d0, a, c, b); break;   // a*c + b
			case IMLMadd::MSUB: fnmsub(d0, a, c, b); break;  // a*c - b
			case IMLMadd::NMADD: fnmadd(d0, a, c, b); break; // -a*c - b
			case IMLMadd::NMSUB: fmsub(d0, a, c, b); break;  // b - a*c
			}
			ins(V(inst.r[0]).d2[0], v0.d2[0]);
			return true;
		}
		case IMLOp::FMADDS:
		{
			// the negated forms are negated either after rounding to single
			// precision or inside the fused operation, like the interpreter
			const IMLMadd kind = (IMLMadd)inst.sub;
			const bool negated = kind == IMLMadd::NMADD || kind == IMLMadd::NMSUB;
			const bool negateAfterRounding = negated && InterpreterNegatesSingleMaddAfterRounding();
			const xa::DReg a = D(inst.r[1]);
			const xa::DReg b = D(inst.r[3]);
			RoundMultiplier25(v1, V(inst.r[2]));
			if (kind == IMLMadd::MADD || (kind == IMLMadd::NMADD && negateAfterRounding))
				fmadd(d0, a, d1, b);          // a*c + b
			else if (kind == IMLMadd::MSUB || (kind == IMLMadd::NMSUB && negateAfterRounding))
				fnmsub(d0, a, d1, b);         // a*c - b
			else if (kind == IMLMadd::NMADD)
				fnmadd(d0, a, d1, b);         // -a*c - b
			else
				fmsub(d0, a, d1, b);          // b - a*c
			fcvt(s0, d0);
			if (negateAfterRounding)
				fneg(s0, s0);
			fcvt(d0, s0);
			dup(V(inst.r[0]).d2, v0.d2[0]);
			return true;
		}
		case IMLOp::PARITH:
		{
			const xa::VReg a = V(inst.r[1]);
			const xa::VReg b = V(inst.r[2]);
			switch ((IMLArith)inst.sub)
			{
			case IMLArith::ADD: fadd(v0.d2, a.d2, b.d2); break;
			case IMLArith::SUB: fsub(v0.d2, a.d2, b.d2); break;
			case IMLArith::MUL:
				RoundMultiplier25(v1, b);
				fmul(v0.d2, a.d2, v1.d2);
				break;
			case IMLArith::DIV: fdiv(v0.d2, a.d2, b.d2); break;
			}
			RoundPairToSingle(v0);
			mov(V(inst.r[0]).b16, v0.b16);
			return true;
		}
		case IMLOp::PMADD:
		{
			// r1 = a, r2 = c, r3 = b
			const IMLMadd kind = (IMLMadd)inst.sub;
			const xa::VReg a = V(inst.r[1]);
			const xa::VReg b = V(inst.r[3]);
			RoundMultiplier25(v1, V(inst.r[2]));
			if (kind == IMLMadd::MADD)
			{
				// ps_madd rounds the product before adding
				fmul(v0.d2, a.d2, v1.d2);
				RoundPairToSingle(v0);
				fadd(v0.d2, v0.d2, b.d2);
				RoundPairToSingle(v0);
			}
			else
			{
				if (kind == IMLMadd::NMADD)
					mov(v0.b16, b.b16);
				else
					fneg(v0.d2, b.d2);
				fmla(v0.d2, a.d2, v1.d2);
				fcvtn(v0.s2, v0.d2);
				if (kind != IMLMadd::MSUB)
					fneg(v0.s2, v0.s2);
				fcvtl(v0.d2, v0.s2);
			}
			mov(V(inst.r[0]).b16, v0.b16);
			return true;
		}
		case IMLOp::PMULS:
		case IMLOp::PMADDS:
		{
			// both lanes of a times one lane of c (plus b)
			dup(v1.d2, V(inst.r[2]).d2[inst.sub & 1]);
			RoundMultiplier25(v1, v1);
			if (inst.op == IMLOp::PMULS)
				fmul(v0.d2, V(inst.r[1]).d2, v1.d2);
			else
			{
				mov(v0.b16, V(inst.r[3]).b16);
				fmla(v0.d2, V(inst.r[1]).d2, v1.d2);
			}
			RoundPairToSingle(v0);
			mov(V(inst.r[0]).b16, v0.b16);
			return true;
		}
		case IMLOp::PSUM:
		{
			// r1 = a, r2 = b, r3 = c. The sum is a.ps0 + b.ps1
			dup(d1, V(inst.r[2]).d2[1]);
			fadd(d0, D(inst.r[1]), d1);
			mov(v2.b16, V(inst.r[3]).b16);
			ins(v2.d2[inst.sub == 0 ? 0 : 1], v0.d2[0]);
			RoundPairToSingle(v2);
			mov(V(inst.r[0]).b16, v2.b16);
			return true;
		}
		case IMLOp::FSEL:
		case IMLOp::PSEL:
		{
			// r1 = a, r2 = c, r3 = b: c where a >= -0.0, b otherwise (also for NaN)
			if (inst.op == IMLOp::FSEL)
			{
				fcmge(d0, D(inst.r[1]), 0.0);
				dup(v0.d2, v0.d2[0]);
			}
			else
				fcmge(v0.d2, V(inst.r[1]).d2, 0.0);
			bsl(v0.b16, V(inst.r[2]).b16, V(inst.r[3]).b16);
			mov(V(inst.r[0]).b16, v0.b16);
			return true;
		}
		case IMLOp::FRSP:
			fcvt(s0, D(inst.r[1]));
			fcvt(d0, s0);
			dup(V(inst.r[0]).d2, v0.d2[0]);
			return true;
		case IMLOp::FCTIWZ:
		{
			// truncate with saturation; NaN gives 0x80000000
			const xa::DReg b = D(inst.r[1]);
			fcvtzs(w0, b);
			fcmp(b, b);
			LoadImm32(w16, 0x80000000);
			csel(w0, w16, w0, xa::VS);
			// 0xFFF80000 in the upper word, bit 32 for negative inputs giving 0
			fmov(x1, b);
			lsr(x1, x1, 63);
			cmp(w0, 0);
			csel(x1, x1, xzr, xa::EQ);
			movz(x16, 0xFFF8, 48);
			orr(x16, x16, x0);
			orr(x16, x16, x1, xa::LSL, 32);
			ins(V(inst.r[0]).d2[0], x16);
			return true;
		}
		case IMLOp::FCTIW:
		case IMLOp::FRES:
		case IMLOp::FRSQRTE:
		case IMLOp::PRES:
		case IMLOp::PRSQRTE:
		{
			const uint32 tmp = offsetof(PPCInterpreter_t, temporaryFPR);
			CpuStoreQ(Q(inst.r[1]), tmp);
			const void* helper;
			switch (inst.op)
			{
			case IMLOp::FCTIW: helper = (const void*)&Helper_FCTIW; break;
			case IMLOp::FRES: helper = (const void*)&Helper_FRES; break;
			case IMLOp::FRSQRTE: helper = (const void*)&Helper_FRSQRTE; break;
			case IMLOp::PRES: helper = (const void*)&Helper_PRES; break;
			default: helper = (const void*)&Helper_PRSQRTE; break;
			}
			CallHelper(helper, inst.saveMask);
			if (inst.op == IMLOp::FCTIW || inst.op == IMLOp::FRSQRTE)
			{
				// ps1 is kept
				CpuLoadD(d0, tmp);
				ins(V(inst.r[0]).d2[0], v0.d2[0]);
			}
			else
				CpuLoadQ(Q(inst.r[0]), tmp);
			return true;
		}
		case IMLOp::FCMP:
		{
			// r0-r3 = LT, GT, EQ, UN; r4 = a, r5 = b; sub = lane
			xa::DReg a = D(inst.r[4]);
			xa::DReg b = D(inst.r[5]);
			if (inst.sub == 1)
			{
				dup(d0, V(inst.r[4]).d2[1]);
				dup(d1, V(inst.r[5]).d2[1]);
				a = d0;
				b = d1;
			}
			const xa::WReg lt = W(inst.r[0]), gt = W(inst.r[1]), eq = W(inst.r[2]), un = W(inst.r[3]);
			fcmp(a, b);
			cset(lt, xa::MI);
			cset(gt, xa::GT);
			cset(eq, xa::EQ);
			cset(un, xa::VS);
			// FPSCR[FPCC] = LT GT EQ UN
			orr(w2, un, eq, xa::LSL, 1);
			orr(w2, w2, gt, xa::LSL, 2);
			orr(w2, w2, lt, xa::LSL, 3);
			const uint32 fpscrOffset = offsetof(PPCInterpreter_t, fpscr);
			CpuLoadW(w3, fpscrOffset);
			and_(w3, w3, (uint64)0xFFFF0FFF);
			orr(w3, w3, w2, xa::LSL, 12);
			if ((inst.flags & IMLF_ORDERED) == 0)
			{
				// signaling NaN operands set VXSNAN: all ones exponent,
				// non-zero fraction and the quiet bit clear
				movz(x16, 0xFFE0, 48);
				for (const xa::DReg& operand : {a, b})
				{
					fmov(x0, operand);
					lsl(x1, x0, 1);
					cmp(x1, x16);
					cset(w1, xa::HI);
					tst(x0, (uint64)0x0008000000000000ull);
					csel(w1, w1, wzr, xa::EQ);
					orr(w3, w3, w1, xa::LSL, 24);
				}
			}
			CpuStoreW(w3, fpscrOffset);
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}

	bool Generator::EmitFloatMemory(const IMLInstruction& inst)
	{
		switch (inst.op)
		{
		case IMLOp::LOADFS:
		{
			// lfs widens exactly with integer operations: FCVT would flush
			// denormals when FPCR.FZ is set and quiet signaling NaNs
			ldr(w0, GuestMemory(inst));
			rev(w0, w0);
			xa::Label lowExponent, assemble;
			ubfx(w1, w0, 23, 8);         // exponent
			and_(w2, w0, (uint64)0x007FFFFF); // fraction
			and_(w3, w0, (uint64)0x80000000); // sign
			cbz(w1, lowExponent);
			cmp(w1, 0xFF);
			add(w1, w1, 1023 - 127);
			movz(w16, 0x7FF);
			csel(w1, w16, w1, xa::EQ);
			b(assemble);
			L(lowExponent);
			cbz(w2, assemble); // zero
			// denormal: normalise the fraction
			clz(w16, w2);
			sub(w1, w16, 8);
			lsl(w2, w2, w1);
			and_(w2, w2, (uint64)0x007FFFFF);
			movz(w1, 905);
			sub(w1, w1, w16);
			L(assemble);
			lsl(x2, x2, 29);
			orr(x2, x2, x1, xa::LSL, 52);
			orr(x2, x2, x3, xa::LSL, 32);
			dup(V(inst.r[0]).d2, x2);
			return true;
		}
		case IMLOp::LOADFD:
			ldr(x0, GuestMemory(inst));
			rev(x0, x0);
			ins(V(inst.r[0]).d2[0], x0);
			return true;
		case IMLOp::STOREF:
		{
			const xa::VReg src = V(inst.r[0]);
			switch ((IMLFMem)inst.sub)
			{
			case IMLFMem::SINGLE:
			{
				// stfs does not round: values in the single denormal range
				// are denormalised, everything else keeps the sign, the top
				// exponent bit and the next 30 bits
				xa::Label denormal, store;
				fmov(x0, xa::DReg(src.getIdx()));
				ubfx(x1, x0, 52, 11);
				sub(w2, w1, 874);
				cmp(w2, 22);
				b(xa::LS, denormal);
				lsr(x2, x0, 32);
				and_(w2, w2, (uint64)0xC0000000);
				ubfx(x3, x0, 29, 30);
				orr(w2, w2, w3);
				b(store);
				L(denormal);
				ubfx(x3, x0, 21, 31);
				orr(w3, w3, (uint64)0x80000000);
				movz(w2, 905);
				sub(w2, w2, w1);
				lsr(w3, w3, w2);
				lsr(x2, x0, 32);
				and_(w2, w2, (uint64)0x80000000);
				orr(w2, w2, w3);
				L(store);
				rev(w2, w2);
				str(w2, GuestMemory(inst));
				return true;
			}
			case IMLFMem::DOUBLE:
				fmov(x0, xa::DReg(src.getIdx()));
				rev(x0, x0);
				str(x0, GuestMemory(inst));
				return true;
			case IMLFMem::IWX:
				fmov(w0, xa::SReg(src.getIdx()));
				rev(w0, w0);
				str(w0, GuestMemory(inst));
				return true;
			}
			return false;
		}
		case IMLOp::PSQLOAD:
		{
			const bool singleSlot = (inst.flags & IMLF_PSQ_W) != 0;
			const xa::WReg gqr = W(inst.r[3]);
			GuestAddressToW16(inst);
			const xa::AdrExt m = xa::ptr(rMemBase, w16, xa::UXTW);
			xa::Label slow, done;
			// float data (load type 0) is converted inline
			tst(gqr, (uint64)0x00070000);
			b(xa::NE, slow);
			if (singleSlot)
			{
				ldr(w0, m);
				rev(w0, w0);
				fmov(s0, w0);
				fcvt(d0, s0);
				fmov(d1, 1.0);
				ins(v0.d2[1], v1.d2[0]);
			}
			else
			{
				ldr(x0, m);
				rev32(x0, x0); // first value in the low half
				fmov(d0, x0);
				fcvtl(v0.d2, v0.s2);
			}
			mov(V(inst.r[0]).b16, v0.b16);
			b(done);
			L(slow);
			CpuStoreW(w16, offsetof(PPCInterpreter_t, temporaryGPR_reg));
			CpuStoreW(gqr, offsetof(PPCInterpreter_t, temporaryGPR_reg) + 4);
			CallHelper(singleSlot ? (const void*)&Helper_PSQLoadSingle : (const void*)&Helper_PSQLoad, inst.saveMask);
			CpuLoadQ(Q(inst.r[0]), offsetof(PPCInterpreter_t, temporaryFPR));
			L(done);
			return true;
		}
		case IMLOp::PSQSTORE:
		{
			const bool singleSlot = (inst.flags & IMLF_PSQ_W) != 0;
			const xa::WReg gqr = W(inst.r[3]);
			const xa::VReg src = V(inst.r[0]);
			GuestAddressToW16(inst);
			const xa::AdrExt m = xa::ptr(rMemBase, w16, xa::UXTW);
			xa::Label slow, done;
			// float data (store type 0) is converted inline
			tst(gqr, (uint64)0x00000007);
			b(xa::NE, slow);
			if (singleSlot)
			{
				fcvt(s0, xa::DReg(src.getIdx()));
				fmov(w0, s0);
				rev(w0, w0);
				str(w0, m);
			}
			else
			{
				fcvtn(v0.s2, src.d2);
				fmov(x0, d0);
				rev32(x0, x0);
				str(x0, m);
			}
			b(done);
			L(slow);
			CpuStoreW(w16, offsetof(PPCInterpreter_t, temporaryGPR_reg));
			CpuStoreW(gqr, offsetof(PPCInterpreter_t, temporaryGPR_reg) + 4);
			CpuStoreQ(Q(inst.r[0]), offsetof(PPCInterpreter_t, temporaryFPR));
			CallHelper(singleSlot ? (const void*)&Helper_PSQStoreSingle : (const void*)&Helper_PSQStore, inst.saveMask);
			L(done);
			return true;
		}
		default:
			return false;
		}
	}

	bool Generator::EmitControl(const IMLInstruction& inst, IMLSegment* block, IMLSegment* layoutNext)
	{
		auto conditional = [&](xa::Cond cc)
		{
			if (block->taken == layoutNext)
				b(InvertCond(cc), BlockLabel(block->next));
			else
			{
				b(cc, BlockLabel(block->taken));
				if (block->next != layoutNext)
					b(BlockLabel(block->next));
			}
		};
		const uint32 cyclesOffset = offsetof(PPCInterpreter_t, remainingCycles);
		switch (inst.op)
		{
		case IMLOp::JUMP:
			if (block->taken != layoutNext)
				b(BlockLabel(block->taken));
			return true;
		case IMLOp::BRANCH:
		{
			const xa::WReg r = W(inst.r[0]);
			const bool takenIfNonZero = inst.Cond() == IMLCond::NE;
			if (block->taken == layoutNext)
			{
				if (takenIfNonZero)
					cbz(r, BlockLabel(block->next));
				else
					cbnz(r, BlockLabel(block->next));
			}
			else
			{
				if (takenIfNonZero)
					cbnz(r, BlockLabel(block->taken));
				else
					cbz(r, BlockLabel(block->taken));
				if (block->next != layoutNext)
					b(BlockLabel(block->next));
			}
			return true;
		}
		case IMLOp::BRANCHCMP:
			if (inst.HasImm())
				CompareImm32(W(inst.r[0]), (uint32)inst.imm);
			else
				cmp(W(inst.r[0]), W(inst.r[1]));
			conditional(HostCond(inst.Cond()));
			return true;
		case IMLOp::CYCLECHECK:
			CpuLoadW(w16, cyclesOffset);
			cmp(w16, 0);
			conditional(xa::LT);
			return true;
		case IMLOp::EXITB:
			LoadImm32(w0, inst.addr);
			if (inst.addr >= PPC_REC_CODE_AREA_END)
				EmitLeave();
			else
				EmitJumpViaTable();
			return true;
		case IMLOp::EXITBR:
			mov(w0, W(inst.r[0]));
			EmitLeaveOrJumpViaTable();
			return true;
		case IMLOp::EXITLEAVE:
			LoadImm32(w0, inst.addr);
			EmitLeave();
			return true;
		case IMLOp::HLE:
		{
			LoadImm32(w16, inst.addr);
			CpuStoreW(w16, offsetof(PPCInterpreter_t, instructionPointer));
			CallHelperWithArg((const void*)&Helper_HLE, (uint32)inst.imm, 0);
			CpuLoadW(w0, offsetof(PPCInterpreter_t, instructionPointer));
			CpuLoadW(w16, cyclesOffset);
			xa::Label timeUp;
			tbnz(w16, 31, timeUp);
			EmitLeaveOrJumpViaTable();
			L(timeUp);
			EmitLeave();
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}

	bool Generator::Generate()
	{
		if (m_func.spillSlotCount > kMaxSpillSlots)
			return false;
		const size_t blockCount = m_func.blocks.size();
		m_blockLabels = std::vector<xa::Label>(blockCount);
		for (size_t i = 0; i < blockCount; i++)
			m_blockIndex[m_func.blocks[i]] = i;
		for (size_t bi = 0; bi < blockCount; bi++)
		{
			IMLSegment* block = m_func.blocks[bi];
			IMLSegment* layoutNext = bi + 1 < blockCount ? m_func.blocks[bi + 1] : nullptr;
			L(m_blockLabels[bi]);
			if (block->isEntry)
				m_entryPoints.emplace_back(block->entryAddress, (uint32)getSize());
			bool terminated = false;
			for (const IMLInstruction& inst : block->insts)
			{
				bool ok = true;
				switch (inst.op)
				{
				case IMLOp::NOP:
					break;
				case IMLOp::LOAD:
				case IMLOp::STORE:
				case IMLOp::ZEROLINE:
				case IMLOp::CMPXCHG:
					ok = EmitMemory(inst);
					break;
				case IMLOp::LOADFS:
				case IMLOp::LOADFD:
				case IMLOp::STOREF:
				case IMLOp::PSQLOAD:
				case IMLOp::PSQSTORE:
					ok = EmitFloatMemory(inst);
					break;
				case IMLOp::HOMELOAD:
				case IMLOp::HOMESTORE:
					ok = EmitHome(inst);
					break;
				case IMLOp::FMOV:
				case IMLOp::FMOVLO:
				case IMLOp::FSIGN:
				case IMLOp::PSIGN:
				case IMLOp::PMERGE:
				case IMLOp::FARITH:
				case IMLOp::FARITHS:
				case IMLOp::FMADD:
				case IMLOp::FMADDS:
				case IMLOp::PARITH:
				case IMLOp::PMADD:
				case IMLOp::PMULS:
				case IMLOp::PMADDS:
				case IMLOp::PSUM:
				case IMLOp::FSEL:
				case IMLOp::PSEL:
				case IMLOp::FRSP:
				case IMLOp::FCTIWZ:
				case IMLOp::FCTIW:
				case IMLOp::FRES:
				case IMLOp::FRSQRTE:
				case IMLOp::PRES:
				case IMLOp::PRSQRTE:
				case IMLOp::FCMP:
					ok = EmitFloat(inst);
					break;
				case IMLOp::CYCLES:
				{
					const uint32 offset = offsetof(PPCInterpreter_t, remainingCycles);
					CpuLoadW(w0, offset);
					AddImm32(w0, w0, 0u - (uint32)inst.imm, w1);
					CpuStoreW(w0, offset);
					break;
				}
				case IMLOp::INTERP:
				{
					CallHelperWithArg((const void*)&Helper_Interpret, inst.addr, inst.saveMask);
					// anything but the next instruction leaves generated code
					xa::Label next;
					CompareImm32(w0, inst.addr + 4);
					b(xa::EQ, next);
					EmitLeave();
					L(next);
					break;
				}
				case IMLOp::MFTB:
					CallHelper(inst.sub ? (const void*)&Helper_TimeBaseUpper : (const void*)&Helper_TimeBaseLower, inst.saveMask);
					mov(W(inst.r[0]), w0);
					break;
				default:
					if (inst.Info().terminator)
					{
						ok = EmitControl(inst, block, layoutNext);
						terminated = true;
					}
					else
						ok = EmitInt(inst);
					break;
				}
				if (!ok)
					return false;
			}
			if (!terminated && block->next != layoutNext)
				b(BlockLabel(block->next));
		}
		return true;
	}

	// generous upper bound of the code size of a function
	size_t EstimateCodeSize(const IMLFunction& func)
	{
		size_t count = 0;
		for (const IMLSegment* b : func.blocks)
			count += b->insts.size() + 1;
		return 4096 + count * 512;
	}
}

IMLRegisterAllocatorParameters AArch64Backend_GetAllocatorParameters()
{
	using namespace AArch64Backend;
	IMLRegisterAllocatorParameters p;
	p.intRegCount = kIntColorCount;
	p.floatRegCount = kFloatColorCount;
	p.intCallerSavedMask = kIntCallerSavedColors;
	p.floatCallerSavedMask = kFloatCallerSavedColors;
	return p;
}

bool PPCRecompiler_generateAArch64Code(PPCRecCompiledFunction* recFunc, IMLFunction& func, std::vector<std::pair<uint32, uint32>>& entryPointsOut)
{
	using namespace AArch64Backend;
	StagingAllocator allocator;
	try
	{
		Generator gen(func, EstimateCodeSize(func), &allocator);
		if (!gen.Generate())
			return false;
		void* code = PublishCode(gen.getCode(), gen.getSize());
		if (!code)
			return false;
		recFunc->hostCode = code;
		recFunc->hostCodeSize = gen.getSize();
		entryPointsOut = gen.EntryPoints();
		return true;
	}
	catch (const Xbyak_aarch64::Error& e)
	{
		// code too big or a branch out of range: the function stays interpreted
		cemuLog_log(LogType::Force, "Recompiler: AArch64 code generation failed for 0x{:08x} ({})", func.entryAddress, e.what());
		return false;
	}
}

void PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions()
{
	using namespace AArch64Backend;
	namespace xa = Xbyak_aarch64;
	StagingAllocator allocator;
	// enter: X0 = code to run, X1 = PPCInterpreter_t*
	{
		Emitter e(4096, &allocator);
		e.stp(e.x29, e.x30, xa::pre_ptr(e.sp, -(sint32)kSavedRegsSize));
		e.stp(e.x19, e.x20, xa::ptr(e.sp, 16));
		e.stp(e.x21, e.x22, xa::ptr(e.sp, 32));
		e.stp(e.x23, e.x24, xa::ptr(e.sp, 48));
		e.stp(e.x25, e.x26, xa::ptr(e.sp, 64));
		e.stp(e.x27, e.x28, xa::ptr(e.sp, 80));
		e.add(e.x29, e.sp, 0);
		e.sub(e.sp, e.sp, kFrameSize);
		e.mov(e.rCPU, e.x1);
		e.LoadImm64(e.x16, (uint64)(uintptr_t)&memory_base);
		e.ldr(e.rMemBase, xa::ptr(e.x16));
		e.LoadImm64(e.rJumpTable, (uint64)(uintptr_t)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable);
		e.br(e.x0);
		PPCRecompiler_enterRecompilerCode = (void ATTR_MS_ABI (*)(uint64, uint64))PublishCode(e.getCode(), e.getSize());
	}
	// leave: W0 = guest address to continue at. The two variants behave the
	// same; their addresses mark jump table entries as unvisited/visited
	auto makeLeave = [&]()
	{
		Emitter e(4096, &allocator);
		e.CpuStoreW(e.w0, offsetof(PPCInterpreter_t, instructionPointer));
		e.add(e.sp, e.sp, kFrameSize);
		e.ldp(e.x27, e.x28, xa::ptr(e.sp, 80));
		e.ldp(e.x25, e.x26, xa::ptr(e.sp, 64));
		e.ldp(e.x23, e.x24, xa::ptr(e.sp, 48));
		e.ldp(e.x21, e.x22, xa::ptr(e.sp, 32));
		e.ldp(e.x19, e.x20, xa::ptr(e.sp, 16));
		e.ldp(e.x29, e.x30, xa::post_ptr(e.sp, (sint32)kSavedRegsSize));
		e.ret();
		return (void ATTR_MS_ABI (*)())PublishCode(e.getCode(), e.getSize());
	};
	PPCRecompiler_leaveRecompilerCode_unvisited = makeLeave();
	PPCRecompiler_leaveRecompilerCode_visited = makeLeave();
	cemu_assert(PPCRecompiler_enterRecompilerCode && PPCRecompiler_leaveRecompilerCode_unvisited && PPCRecompiler_leaveRecompilerCode_visited);
}
