// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "IML/IML.h"
#include "PPCFunctionBoundaryTracker.h"

// Front end of the recompiler: turns the guest code of one function (as
// found by PPCFunctionBoundaryTracker) into intermediate code.
//
// Every guest basic block becomes one IMLSegment that starts with a CYCLES
// instruction. Blocks that can be entered from the jump table get a
// separate entry block (see PPCRecompilerIML_IsolateEntries) so that entry
// blocks never have predecessors. Loop headers are preceded by a block that
// leaves to the scheduler once the time slice is used up.

class PPCIMLTranslator
{
public:
	PPCIMLTranslator(IMLFunction& func, const PPCFunctionBoundaryTracker& tracker);

	// returns false if the function cannot be compiled
	bool Translate(uint32 entryAddress);

	// guest addresses that received an entry point
	const std::vector<uint32>& GetEntryAddresses() const { return m_entryAddresses; }

	// ---- used by the instruction translators ----

	IMLReg Temp() { return m_func.NewTemp(IMLRegKind::Int); }
	IMLReg FTemp() { return m_func.NewTemp(IMLRegKind::Float); }

	IMLInstruction& Emit(const IMLInstruction& inst) { return m_cur->Append(inst); }
	void Op2(IMLOp op, IMLReg d, IMLReg a) { Emit(IMLInstruction::Make(op, d, a)); }
	void Op3(IMLOp op, IMLReg d, IMLReg a, IMLReg b) { Emit(IMLInstruction::Make(op, d, a, b)); }
	void Op3i(IMLOp op, IMLReg d, IMLReg a, sint32 imm) { Emit(IMLInstruction::MakeImm(op, d, a, imm)); }
	void Mov(IMLReg d, IMLReg a)
	{
		if (d != a)
			Op2(IMLOp::MOV, d, a);
	}
	void Movi(IMLReg d, uint32 v)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::MOVI, d);
		i.imm = (sint32)v;
		Emit(i);
	}
	// CR0 from a result (record forms)
	void UpdateCR0(IMLReg result);
	// CR field from a compare
	void CompareToCR(uint32 field, IMLReg a, IMLReg b, bool hasImm, sint32 imm, bool isSigned);
	// effective address base + imm into a new temporary (rA = 0 means no base)
	IMLReg AddressD(uint32 rA, sint32 imm);
	IMLReg AddressX(uint32 rA, uint32 rB);
	// hands the current instruction to the interpreter
	void Interp();
	// leaves to the interpreter right before the current instruction
	void LeaveHere();

	// floating point and paired single instructions (PPCRecompilerImlGenFPU.cpp)
	bool TranslateFPLoadStore(uint32 op);
	bool TranslateGroup4(uint32 op);
	bool TranslateGroup59(uint32 op);
	bool TranslateGroup63(uint32 op);

	IMLFunction& m_func;
	IMLSegment* m_cur{nullptr};
	uint32 m_addr{0}; // address of the instruction being translated

private:
	struct GuestBlock
	{
		uint32 start;
		uint32 end;
		IMLSegment* head{nullptr};  // first block (the cycle check block of loop headers)
		IMLSegment* body{nullptr};
		bool needsCycleCheck{false};
		bool isEntry{false};
	};

	void CollectBlocks(uint32 entryAddress);
	void TranslateBlock(GuestBlock& block);
	bool TranslateInstruction(uint32 op);
	bool TranslateGroup19(uint32 op);
	bool TranslateGroup31(uint32 op);
	bool TranslateIntLoadStore(uint32 op);

	// branches
	void TranslateB(uint32 op);
	void TranslateBC(uint32 op);
	void TranslateBCLR(uint32 op);
	void TranslateBCCTR(uint32 op);
	// emits the BO/BI test of a conditional branch. Jumps to 'taken' when
	// the branch is taken, continues in a new current block otherwise
	void EmitBranchCondition(uint32 bo, uint32 bi, bool decrementsCtr, IMLSegment* taken);

	// block that continues at a guest address: the local block or an exit
	IMLSegment* BlockForTarget(uint32 address);
	IMLSegment* NewExitBlock(IMLOp exitOp, uint32 address);
	// starts a new block in the middle of a guest block (after a branch)
	IMLSegment* ContinueInNewBlock();
	void EndBlock(IMLInstruction terminator, IMLSegment* taken, IMLSegment* next);

	const PPCFunctionBoundaryTracker& m_tracker;
	std::map<uint32, GuestBlock> m_blocks; // by start address
	std::set<uint32> m_entrySet;
	std::vector<uint32> m_entryAddresses;
	bool m_blockEnded{false}; // current guest instruction ended the guest block
};

// PPCRecompilerIntermediate.cpp

// gives every entry block a separate entry segment without predecessors
void PPCRecompilerIML_IsolateEntries(IMLFunction& func);
