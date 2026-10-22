// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// x86-64 backend: code generation driver, helper functions called from
// generated code, the entry/exit stubs and executable memory.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterHelper.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "util/MemMapper/MemMapper.h"
#include "BackendX64.h"
#include "X64Emit.hpp"

using namespace x64;

namespace X64Backend
{
	VectorConstants g_vectorConstants =
	{
		{0x8000000000000000ull, 0x8000000000000000ull},
		{0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull},
		{0x0000000008000000ull, 0x0000000008000000ull},
		{0xFFFFFFFFF0000000ull, 0xFFFFFFFFF0000000ull},
		{1.0, 1.0},
		0x001FFFFFFFFFFFFFull,
		0x000FFFFFFFFFFFFFull,
	};

	IMLRegisterAllocatorParameters GetAllocatorParameters()
	{
		IMLRegisterAllocatorParameters p;
		p.intRegCount = kIntColorCount;
		p.floatRegCount = kFloatColorCount;
		p.intCallerSavedMask = kIntCallerSavedColors;
		p.floatCallerSavedMask = kFloatCallerSavedColors;
		return p;
	}

	// ---- helpers called from generated code ----

	ATTR_MS_ABI uint32 Helper_Interpret(PPCInterpreter_t* hCPU, uint32 address)
	{
		hCPU->instructionPointer = address;
		PPCInterpreterSlim_executeInstruction(hCPU);
		return hCPU->instructionPointer;
	}

	ATTR_MS_ABI void Helper_HLE(PPCInterpreter_t* hCPU, uint32 opcode)
	{
		// calls to unimplemented imports cost extra time, this keeps guest
		// code that polls such a function from starving other threads
		if ((opcode & 0xFFFF) == 0xFFD0)
			hCPU->remainingCycles -= 500;
		EspressoItp::Op_HLE(hCPU, opcode);
	}

	// the time base as seen by recompiled code ticks with the bus clock / 4
	ATTR_MS_ABI uint32 Helper_TimeBaseLower(PPCInterpreter_t* hCPU)
	{
		return (uint32)coreinit::OSGetSystemTime();
	}

	ATTR_MS_ABI uint32 Helper_TimeBaseUpper(PPCInterpreter_t* hCPU)
	{
		return (uint32)(coreinit::OSGetSystemTime() >> 32);
	}

	ATTR_MS_ABI void Helper_FRES(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		double r = fres_espresso(t.fp0);
		t.fp0 = r;
		t.fp1 = r;
	}

	ATTR_MS_ABI void Helper_FRSQRTE(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		t.fp0 = frsqrte_espresso(t.fp0);
	}

	ATTR_MS_ABI void Helper_PRES(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		double in0 = t.fp0, in1 = t.fp1;
		t.fp0 = (double)(float)fres_espresso(in0);
		t.fp1 = (double)(float)fres_espresso(in1);
	}

	ATTR_MS_ABI void Helper_PRSQRTE(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		double in0 = t.fp0, in1 = t.fp1;
		t.fp0 = (double)(float)frsqrte_espresso(in0);
		t.fp1 = (double)(float)frsqrte_espresso(in1);
	}

	// fctiw with the rounding mode in FPSCR[RN]; same result layout as fctiwz
	ATTR_MS_ABI void Helper_FCTIW(PPCInterpreter_t* hCPU)
	{
		FPR_t& t = hCPU->temporaryFPR[0];
		double v = t.fp0;
		uint32 word;
		if (v > 2147483647.0)
			word = 0x7FFFFFFF;
		else if (v < -2147483648.0)
			word = 0x80000000;
		else if (v != v)
			word = 0x80000000;
		else
		{
			double r;
			switch (hCPU->fpscr & 3)
			{
			case 0: r = std::nearbyint(v); break;
			case 1: r = std::trunc(v); break;
			case 2: r = std::ceil(v); break;
			default: r = std::floor(v); break;
			}
			if (r > 2147483647.0)
				word = 0x7FFFFFFF;
			else if (r < -2147483648.0)
				word = 0x80000000;
			else
				word = (uint32)(sint32)r;
		}
		uint64 result = 0xFFF8000000000000ull | word;
		if (word == 0 && (t.fp0int & 0x8000000000000000ull))
			result |= 0x100000000ull;
		t.fp0int = result;
	}

	static uint32 ReadQuantRaw(uint32 ea, uint32 size)
	{
		uint8* p = memory_base + ea;
		if (size == 1)
			return *p;
		if (size == 2)
			return _swapEndianU16(*(uint16*)p);
		return _swapEndianU32(*(uint32*)p);
	}

	static void WriteQuantRaw(uint32 ea, uint32 size, uint32 value)
	{
		uint8* p = memory_base + ea;
		if (size == 1)
			*p = (uint8)value;
		else if (size == 2)
			*(uint16*)p = _swapEndianU16((uint16)value);
		else
			*(uint32*)p = _swapEndianU32(value);
	}

	static void QuantLoad(PPCInterpreter_t* hCPU, bool single)
	{
		uint32 ea = hCPU->temporaryGPR[0];
		uint32 gqr = hCPU->temporaryGPR[1];
		uint32 type = (gqr >> 16) & 7;
		uint32 scale = (gqr >> 24) & 0x3F;
		uint32 size = EspressoItp::GqrAccessSize(type);
		uint32 raw0 = ReadQuantRaw(ea, size);
		FPR_t& t = hCPU->temporaryFPR[0];
		if (single)
		{
			t.fp0 = (double)EspressoItp::Dequantize(raw0, type, scale);
			t.fp1 = 1.0;
		}
		else
		{
			uint32 raw1 = ReadQuantRaw(ea + size, size);
			t.fp0 = (double)EspressoItp::Dequantize(raw0, type, scale);
			t.fp1 = (double)EspressoItp::Dequantize(raw1, type, scale);
		}
	}

	static void QuantStore(PPCInterpreter_t* hCPU, bool single)
	{
		uint32 ea = hCPU->temporaryGPR[0];
		uint32 gqr = hCPU->temporaryGPR[1];
		uint32 type = gqr & 7;
		uint32 scale = (gqr >> 8) & 0x3F;
		uint32 size = EspressoItp::GqrAccessSize(type);
		const FPR_t& t = hCPU->temporaryFPR[0];
		WriteQuantRaw(ea, size, EspressoItp::Quantize((float)t.fp0, type, scale));
		if (!single)
			WriteQuantRaw(ea + size, size, EspressoItp::Quantize((float)t.fp1, type, scale));
	}

	ATTR_MS_ABI void Helper_PSQLoad(PPCInterpreter_t* hCPU) { QuantLoad(hCPU, false); }
	ATTR_MS_ABI void Helper_PSQLoadSingle(PPCInterpreter_t* hCPU) { QuantLoad(hCPU, true); }
	ATTR_MS_ABI void Helper_PSQStore(PPCInterpreter_t* hCPU) { QuantStore(hCPU, false); }
	ATTR_MS_ABI void Helper_PSQStoreSingle(PPCInterpreter_t* hCPU) { QuantStore(hCPU, true); }

	// ---- code generation ----

	x64::Mem CodeGenerator::GuestAddress(const IMLInstruction& inst)
	{
		if (inst.r[1] != IML_NOREG && inst.r[2] == IML_NOREG && inst.imm == 0)
			return mem(kRegMemBase, Gpr(inst.r[1]), 1, 0);
		GuestAddressToEAX(inst);
		return mem(kRegMemBase, RAX, 1, 0);
	}

	void CodeGenerator::GuestAddressToEAX(const IMLInstruction& inst)
	{
		IMLReg base = inst.r[1], index = inst.r[2];
		if (base != IML_NOREG && index != IML_NOREG)
			m_e.LEA32(RAX, mem(Gpr(base), Gpr(index), 1, inst.imm));
		else if (base != IML_NOREG || index != IML_NOREG)
		{
			uint8 r = Gpr(base != IML_NOREG ? base : index);
			if (inst.imm == 0)
				m_e.MOV_rr(RAX, r);
			else
				m_e.LEA32(RAX, mem(r, inst.imm));
		}
		else
			m_e.MOV_ri32(RAX, (uint32)inst.imm);
	}

	void CodeGenerator::RoundMultiplier25(x64::Xmm x)
	{
		LoadConstantTable();
		m_e.PADDQ_m(x, mem(RAX, offsetof(VectorConstants, mul25Round)));
		m_e.PAND_m(x, mem(RAX, offsetof(VectorConstants, mul25Mask)));
	}

	namespace
	{
		// position of a caller saved register in the save area
		sint32 SaveSlotOffset(bool isFloat, uint32 color)
		{
			if (isFloat)
				return kFrameCallSaveArea + 4 * 16 + (sint32)std::popcount(kFloatCallerSavedColors & ((1u << color) - 1)) * 16;
			return kFrameCallSaveArea + (sint32)std::popcount(kIntCallerSavedColors & ((1u << color) - 1)) * 16;
		}
	}

	void CodeGenerator::CallHelper(const void* function, uint32 saveMask)
	{
		uint32 intMask = saveMask & 0xFFFF;
		uint32 floatMask = saveMask >> IML_SAVEMASK_FLOAT_SHIFT;
		for (uint32 c = 0; c < kIntColorCount; c++)
		{
			if (intMask & (1u << c))
				m_e.MOV_mr(Stack(SaveSlotOffset(false, c)), kIntColorRegs[c], true);
		}
		for (uint32 c = 0; c < kFloatColorCount; c++)
		{
			if (floatMask & (1u << c))
				m_e.MOVUPD_mr(Stack(SaveSlotOffset(true, c)), (x64::Xmm)c);
		}
		m_e.MOV_rr(RCX, kRegCPU, true);
		m_e.MOV_ri64(RAX, (uint64)(uintptr_t)function);
		m_e.CALL_r(RAX);
		for (uint32 c = 0; c < kIntColorCount; c++)
		{
			if (intMask & (1u << c))
				m_e.MOV_rm(kIntColorRegs[c], Stack(SaveSlotOffset(false, c)), true);
		}
		for (uint32 c = 0; c < kFloatColorCount; c++)
		{
			if (floatMask & (1u << c))
				m_e.MOVUPD_rm((x64::Xmm)c, Stack(SaveSlotOffset(true, c)));
		}
	}

	void CodeGenerator::CallHelper2(const void* function, uint32 arg2, uint32 saveMask)
	{
		m_e.MOV_ri32(RDX, arg2);
		CallHelper(function, saveMask);
	}

	void CodeGenerator::EmitLeaveEAX()
	{
		m_e.MOV_ri64(RCX, (uint64)(uintptr_t)PPCRecompiler_leaveRecompilerCode_visited);
		m_e.JMP_r(RCX);
	}

	void CodeGenerator::EmitJumpViaTableEAX()
	{
		// table entries are 8 bytes per 4 byte guest instruction
		m_e.MOV_ri64(RCX, (uint64)(uintptr_t)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable);
		m_e.JMP_m(mem(RCX, RAX, 2, 0));
	}

	void CodeGenerator::JumpToBlock(IMLSegment* target)
	{
		m_blockFixups.emplace_back(m_e.JMP_rel32(), target);
	}

	void CodeGenerator::JccToBlock(x64::Cond cc, IMLSegment* target)
	{
		m_blockFixups.emplace_back(m_e.Jcc_rel32(cc), target);
	}

	bool CodeGenerator::Generate()
	{
		if (m_func.spillSlotCount > kMaxSpillSlots)
			return false;
		const size_t blockCount = m_func.blocks.size();
		for (size_t bi = 0; bi < blockCount; bi++)
		{
			IMLSegment* block = m_func.blocks[bi];
			IMLSegment* layoutNext = bi + 1 < blockCount ? m_func.blocks[bi + 1] : nullptr;
			block->codeOffset = (sint32)m_e.Size();
			if (block->isEntry)
				m_entryPoints.emplace_back(block->entryAddress, (uint32)m_e.Size());
			bool terminated = false;
			for (const IMLInstruction& inst : block->insts)
			{
				bool ok;
				switch (inst.op)
				{
				case IMLOp::NOP:
					ok = true;
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
				case IMLOp::CYCLES:
					m_e.ALU_mi(ALU_SUB, CPU(offsetof(PPCInterpreter_t, remainingCycles)), (uint32)inst.imm);
					ok = true;
					break;
				case IMLOp::INTERP:
				{
					CallHelper2((const void*)&Helper_Interpret, inst.addr, inst.saveMask);
					m_e.ALU_ri(ALU_CMP, RAX, inst.addr + 4);
					size_t skip = m_e.Jcc_rel32(CC_E);
					EmitLeaveEAX();
					m_e.BindRel32(skip, m_e.Size());
					ok = true;
					break;
				}
				case IMLOp::MFTB:
					CallHelper(inst.sub ? (const void*)&Helper_TimeBaseUpper : (const void*)&Helper_TimeBaseLower, inst.saveMask);
					m_e.MOV_rr(Gpr(inst.r[0]), RAX);
					ok = true;
					break;
				default:
					if (inst.Info().terminator)
					{
						ok = EmitControl(inst, block, layoutNext);
						terminated = true;
					}
					else if ((uint32)inst.op >= (uint32)IMLOp::FMOV && (uint32)inst.op <= (uint32)IMLOp::FCMP)
						ok = EmitFloat(inst);
					else
						ok = EmitInt(inst);
					break;
				}
				if (!ok)
					return false;
			}
			if (!terminated && block->next != layoutNext)
				JumpToBlock(block->next);
		}
		for (auto& [at, target] : m_blockFixups)
			m_e.BindRel32(at, (size_t)target->codeOffset);
		return true;
	}
}

// ---- executable memory ----

static std::mutex s_codeMemoryMutex;
static uint8* s_codeMemoryBlock = nullptr;
static size_t s_codeMemoryBlockSize = 0;
static size_t s_codeMemoryBlockUsed = 0;

uint8* PPCRecompilerX64_allocateExecutableMemory(size_t size)
{
	std::lock_guard<std::mutex> lock(s_codeMemoryMutex);
	size = (size + 15) & ~(size_t)15;
	if (!s_codeMemoryBlock || s_codeMemoryBlockUsed + size > s_codeMemoryBlockSize)
	{
		size_t blockSize = std::max<size_t>(4 * 1024 * 1024, size);
		uint8* block = (uint8*)MemMapper::AllocateMemory(nullptr, blockSize, MemMapper::PAGE_PERMISSION::P_RWX);
		if (!block)
			return nullptr;
		s_codeMemoryBlock = block;
		s_codeMemoryBlockSize = blockSize;
		s_codeMemoryBlockUsed = 0;
	}
	uint8* p = s_codeMemoryBlock + s_codeMemoryBlockUsed;
	s_codeMemoryBlockUsed += size;
	return p;
}

static void* PublishCode(const std::vector<uint8>& code)
{
	uint8* p = PPCRecompilerX64_allocateExecutableMemory(code.size());
	if (!p)
		return nullptr;
	memcpy(p, code.data(), code.size());
	return p;
}

bool PPCRecompiler_generateX64Code(PPCRecCompiledFunction* recFunc, IMLFunction& func, std::vector<std::pair<uint32, uint32>>& entryPointsOut)
{
	X64Backend::CodeGenerator gen(func);
	if (!gen.Generate())
		return false;
	void* code = PublishCode(gen.Code());
	if (!code)
		return false;
	recFunc->hostCode = code;
	recFunc->hostCodeSize = gen.Code().size();
	entryPointsOut = gen.EntryPoints();
	return true;
}

// ---- entry and exit stubs ----

namespace
{
	constexpr uint8 kSavedGprs[] = {RBX, RBP, RDI, RSI, R12, R13, R14, R15};
}

void PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions()
{
	using namespace X64Backend;
	// enter: RCX = code to run, RDX = PPCInterpreter_t* (Microsoft x64 convention)
	{
		Emitter e;
		for (uint8 r : kSavedGprs)
			e.PUSH(r);
		e.ALU_ri(ALU_SUB, RSP, (uint32)kFrameSize, true);
		for (uint32 i = 0; i < 10; i++)
			e.MOVAPS_mr(mem(RSP, kFrameXmmSaveArea + (sint32)i * 16), (Xmm)(6 + i));
		e.MOV_rr(kRegCPU, RDX, true);
		e.MOV_ri64(RAX, (uint64)(uintptr_t)&memory_base);
		e.MOV_rm(kRegMemBase, mem(RAX, 0), true);
		e.JMP_r(RCX);
		PPCRecompiler_enterRecompilerCode = (void ATTR_MS_ABI (*)(uint64, uint64))PublishCode(e.Buffer());
	}
	// leave: EAX = guest address to continue at. The two variants behave the
	// same; their addresses mark jump table entries as unvisited/visited
	auto makeLeave = []()
	{
		Emitter e;
		e.MOV_mr(mem(kRegCPU, offsetof(PPCInterpreter_t, instructionPointer)), RAX);
		for (uint32 i = 0; i < 10; i++)
			e.MOVAPS_rm((Xmm)(6 + i), mem(RSP, kFrameXmmSaveArea + (sint32)i * 16));
		e.ALU_ri(ALU_ADD, RSP, (uint32)kFrameSize, true);
		for (size_t i = sizeof(kSavedGprs); i-- > 0;)
			e.POP(kSavedGprs[i]);
		e.RET();
		return (void ATTR_MS_ABI (*)())PublishCode(e.Buffer());
	};
	PPCRecompiler_leaveRecompilerCode_unvisited = makeLeave();
	PPCRecompiler_leaveRecompilerCode_visited = makeLeave();
	cemu_assert(PPCRecompiler_enterRecompilerCode && PPCRecompiler_leaveRecompilerCode_unvisited && PPCRecompiler_leaveRecompilerCode_visited);
}
