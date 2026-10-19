// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "../PPCRecompiler.h"
#include "../IML/IML.h"
#include "../IML/IMLRegisterAllocator.h"
#include "x86Emitter.h"

// x86-64 backend.
//
// Register use inside generated code:
//   R15        PPCInterpreter_t* of the running core
//   R14        memory_base
//   RAX/RCX/RDX, XMM14/XMM15   scratch for the instruction being emitted
//   RBX RBP RSI RDI R8-R13     allocatable integer registers (colors 0-9)
//   XMM0-XMM13                 allocatable vector registers (colors 0-13)
//
// Generated code is entered through PPCRecompiler_enterRecompilerCode,
// which saves the host's callee saved registers and sets up a fixed size
// stack frame; RSP stays unchanged while guest code runs, also across jumps
// between functions. All helpers called from generated code use the
// Microsoft x64 calling convention on every host OS.

namespace X64Backend
{
	constexpr uint8 kRegCPU = x64::R15;
	constexpr uint8 kRegMemBase = x64::R14;

	constexpr uint32 kIntColorCount = 10;
	constexpr uint32 kFloatColorCount = 14;
	constexpr uint8 kIntColorRegs[kIntColorCount] = {x64::RBX, x64::RBP, x64::RSI, x64::RDI, x64::R8, x64::R9, x64::R10, x64::R11, x64::R12, x64::R13};
	// R8-R11 and XMM0-XMM5 are not preserved by called functions
	constexpr uint32 kIntCallerSavedColors = 0xF0;
	constexpr uint32 kFloatCallerSavedColors = 0x3F;

	constexpr x64::Xmm kXmmScratch0 = 15;
	constexpr x64::Xmm kXmmScratch1 = 14;

	// stack frame layout (offsets from RSP inside generated code)
	constexpr sint32 kFrameShadowSpace = 0;       // 32 bytes for called functions
	constexpr sint32 kFrameCallSaveArea = 32;     // 4 GPRs and 6 XMMs, 16 bytes each
	constexpr sint32 kFrameSpillArea = kFrameCallSaveArea + 10 * 16;
	constexpr uint32 kMaxSpillSlots = 64;         // 16 bytes each
	constexpr sint32 kFrameXmmSaveArea = kFrameSpillArea + kMaxSpillSlots * 16;
	constexpr sint32 kFrameSize = kFrameXmmSaveArea + 10 * 16 + 8; // keeps RSP 16-byte aligned after 8 pushes

	// constants for vector code, 16-byte aligned
	struct alignas(16) VectorConstants
	{
		uint64 signBits[2];     // both lanes 0x8000000000000000
		uint64 absMask[2];      // both lanes 0x7FFFFFFFFFFFFFFF
		uint64 mul25Round[2];   // added before the frC significand is cut to 25 bits
		uint64 mul25Mask[2];
		double one[2];          // 1.0, 1.0
		uint64 snanBias;        // (bits << 1) + snanBias < snanLimit for signaling NaNs
		uint64 snanLimit;
	};
	extern VectorConstants g_vectorConstants;

	IMLRegisterAllocatorParameters GetAllocatorParameters();

	// helpers called from generated code
	ATTR_MS_ABI uint32 Helper_Interpret(PPCInterpreter_t* hCPU, uint32 address);
	ATTR_MS_ABI void Helper_HLE(PPCInterpreter_t* hCPU, uint32 opcode);
	ATTR_MS_ABI uint32 Helper_TimeBaseLower(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI uint32 Helper_TimeBaseUpper(PPCInterpreter_t* hCPU);
	// the floating point helpers take their operand from temporaryFPR[0] and
	// return the result there
	ATTR_MS_ABI void Helper_FRES(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_FRSQRTE(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_PRES(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_PRSQRTE(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_FCTIW(PPCInterpreter_t* hCPU);
	// quantized load/store: EA in temporaryGPR[0], GQR value in temporaryGPR[1],
	// the paired value in temporaryFPR[0]
	ATTR_MS_ABI void Helper_PSQLoad(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_PSQLoadSingle(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_PSQStore(PPCInterpreter_t* hCPU);
	ATTR_MS_ABI void Helper_PSQStoreSingle(PPCInterpreter_t* hCPU);

	class CodeGenerator
	{
	public:
		explicit CodeGenerator(IMLFunction& func) : m_func(func) {}

		bool Generate();
		const std::vector<uint8>& Code() { return m_e.Buffer(); }
		// relative to the start of the code: entry points and their guest address
		const std::vector<std::pair<uint32, uint32>>& EntryPoints() const { return m_entryPoints; }

		// ---- shared by the instruction emitters ----
		x64::Emitter m_e;
		IMLFunction& m_func;

		uint8 Gpr(IMLReg r) const
		{
			sint16 c = m_func.vregs[r].hostReg;
			cemu_assert_debug(c >= 0 && c < (sint16)kIntColorCount && m_func.KindOf(r) == IMLRegKind::Int);
			return kIntColorRegs[c];
		}
		x64::Xmm Fpr(IMLReg r) const
		{
			sint16 c = m_func.vregs[r].hostReg;
			cemu_assert_debug(c >= 0 && c < (sint16)kFloatColorCount && m_func.KindOf(r) == IMLRegKind::Float);
			return (x64::Xmm)c;
		}
		static x64::Mem CPU(sint32 offset) { return x64::mem(kRegCPU, offset); }
		static x64::Mem Stack(sint32 offset) { return x64::mem(x64::RSP, offset); }

		// memory operand for a guest access (may use RAX)
		x64::Mem GuestAddress(const IMLInstruction& inst);
		// RAX = guest address of a memory instruction (32-bit, wrapped)
		void GuestAddressToEAX(const IMLInstruction& inst);
		void MoveGpr(uint8 dst, uint8 src)
		{
			if (dst != src)
				m_e.MOV_rr(dst, src);
		}
		void MoveXmm(x64::Xmm dst, x64::Xmm src)
		{
			if (dst != src)
				m_e.MOVAPD(dst, src);
		}
		void LoadConstantTable() { m_e.MOV_ri64(x64::RAX, (uint64)(uintptr_t)&g_vectorConstants); }
		// rounds the significand of the frC operand(s) in xmm to 25 bits (clobbers RAX)
		void RoundMultiplier25(x64::Xmm x);
		// helper call with hCPU as the only argument, preserving the registers in saveMask
		void CallHelper(const void* function, uint32 saveMask);
		void CallHelper2(const void* function, uint32 arg2, uint32 saveMask);
		// leave generated code: RAX holds the guest address to continue at
		void EmitLeaveEAX();
		// continue at the guest address in RAX through the jump table
		void EmitJumpViaTableEAX();
		// jumps to another block
		void JumpToBlock(IMLSegment* target);
		void JccToBlock(x64::Cond cc, IMLSegment* target);

		// instruction emitters (BackendX64Gen.cpp, BackendX64GenFPU.cpp, BackendX64FPU.cpp)
		bool EmitInt(const IMLInstruction& inst);
		bool EmitMemory(const IMLInstruction& inst);
		bool EmitControl(const IMLInstruction& inst, IMLSegment* block, IMLSegment* layoutNext);
		bool EmitFloat(const IMLInstruction& inst);
		bool EmitFloatMemory(const IMLInstruction& inst);
		bool EmitHome(const IMLInstruction& inst);
		// feature dependent variants (BackendX64BMI.cpp, BackendX64AVX.cpp)
		void EmitCountLeadingZeros(uint8 dst, uint8 src);
		void EmitLoadSwapped32(uint8 dst, const x64::Mem& m);

	private:
		std::vector<std::pair<size_t, IMLSegment*>> m_blockFixups;
		std::vector<std::pair<uint32, uint32>> m_entryPoints;
	};
}

// creates the entry and exit stubs (PPCRecompiler_enterRecompilerCode and the leave functions)
void PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
// returns nullptr if the function could not be compiled
uint8* PPCRecompilerX64_allocateExecutableMemory(size_t size);
bool PPCRecompiler_generateX64Code(PPCRecCompiledFunction* recFunc, IMLFunction& func, std::vector<std::pair<uint32, uint32>>& entryPointsOut);
