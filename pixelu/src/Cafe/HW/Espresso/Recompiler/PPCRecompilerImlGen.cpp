// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Translation of guest code into intermediate code: block structure,
// branches, integer, condition register and integer memory instructions.
// The floating point side lives in PPCRecompilerImlGenFPU.cpp.
//
// Every instruction is translated to have the same effect on guest state as
// the interpreter (see Interpreter/). Instructions that are rare or have
// effects outside of the registers modelled here are handed to the
// interpreter with an INTERP instruction.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"

using namespace IMLGuest;

namespace
{
	inline uint32 FieldD(uint32 op) { return (op >> 21) & 0x1F; }
	inline uint32 FieldA(uint32 op) { return (op >> 16) & 0x1F; }
	inline uint32 FieldB(uint32 op) { return (op >> 11) & 0x1F; }
	inline uint32 FieldC(uint32 op) { return (op >> 6) & 0x1F; }
	inline uint32 FieldE(uint32 op) { return (op >> 1) & 0x1F; }
	inline sint32 SImm16(uint32 op) { return (sint32)(sint16)(uint16)op; }
	inline uint32 UImm16(uint32 op) { return op & 0xFFFF; }
	inline bool Rc(uint32 op) { return (op & 1) != 0; }

	// mask with ones from IBM bit mb to bit me (wrapping)
	inline uint32 RotateMask(uint32 mb, uint32 me)
	{
		uint32 fromMB = 0xFFFFFFFFu >> mb;
		uint32 toME = 0xFFFFFFFFu << (31 - me);
		return (mb <= me) ? (fromMB & toME) : (fromMB | toME);
	}

	inline uint32 BranchDisplacement24(uint32 op)
	{
		return (uint32)(((sint32)(op << 6)) >> 6) & ~3u;
	}

	inline bool IsPlainCtrLoop(uint32 op)
	{
		// bc with "decrement CTR, branch while CTR != 0, ignore the CR bit"
		uint32 bo = FieldD(op);
		return (op >> 26) == 16 && (bo & 0x16) == 0x10;
	}
}

PPCIMLTranslator::PPCIMLTranslator(IMLFunction& func, const PPCFunctionBoundaryTracker& tracker)
	: m_func(func), m_tracker(tracker)
{
}

// ---------------------------------------------------------------------------
// helpers used by the instruction translators

void PPCIMLTranslator::UpdateCR0(IMLReg result)
{
	CompareToCR(0, result, IML_NOREG, true, 0, true);
}

void PPCIMLTranslator::CompareToCR(uint32 field, IMLReg a, IMLReg b, bool hasImm, sint32 imm, bool isSigned)
{
	IMLInstruction i = IMLInstruction::Make(IMLOp::CMPCR, CR(field * 4 + 0), CR(field * 4 + 1), CR(field * 4 + 2), a);
	i.sub = (uint8)(isSigned ? IMLCond::SLT : IMLCond::ULT);
	if (hasImm)
	{
		i.flags |= IMLF_IMM;
		i.imm = imm;
	}
	else
		i.r[4] = b;
	Emit(i);
	Mov(CR(field * 4 + 3), XER_SO);
}

IMLReg PPCIMLTranslator::AddressD(uint32 rA, sint32 imm)
{
	IMLReg t = Temp();
	if (rA == 0)
		Movi(t, (uint32)imm);
	else
		Op3i(IMLOp::ADD, t, GPR(rA), imm);
	return t;
}

IMLReg PPCIMLTranslator::AddressX(uint32 rA, uint32 rB)
{
	IMLReg t = Temp();
	if (rA == 0)
		Mov(t, GPR(rB));
	else
		Op3(IMLOp::ADD, t, GPR(rA), GPR(rB));
	return t;
}

void PPCIMLTranslator::Interp()
{
	IMLInstruction i = IMLInstruction::Make(IMLOp::INTERP);
	i.addr = m_addr;
	Emit(i);
}

void PPCIMLTranslator::LeaveHere()
{
	IMLInstruction i = IMLInstruction::Make(IMLOp::EXITLEAVE);
	i.addr = m_addr;
	EndBlock(i, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// block structure

IMLSegment* PPCIMLTranslator::NewExitBlock(IMLOp exitOp, uint32 address)
{
	IMLSegment* b = m_func.NewBlock();
	IMLInstruction i = IMLInstruction::Make(exitOp);
	i.addr = address;
	b->Append(i);
	return b;
}

IMLSegment* PPCIMLTranslator::BlockForTarget(uint32 address)
{
	auto it = m_blocks.find(address);
	if (it != m_blocks.end())
		return it->second.head;
	return NewExitBlock(IMLOp::EXITB, address);
}

IMLSegment* PPCIMLTranslator::ContinueInNewBlock()
{
	IMLSegment* b = m_func.InsertBlockAfter(m_cur);
	m_cur = b;
	return b;
}

void PPCIMLTranslator::EndBlock(IMLInstruction terminator, IMLSegment* taken, IMLSegment* next)
{
	m_cur->Append(terminator);
	m_cur->taken = taken;
	m_cur->next = next;
	m_blockEnded = true;
}

void PPCIMLTranslator::CollectBlocks(uint32 entryAddress)
{
	std::set<uint32> leaders;
	std::set<uint32> entries;
	std::vector<std::pair<uint32, uint32>> backEdges; // (branch address, target)

	auto covered = [this](uint32 a) { return m_tracker.ContainsAddress(a); };
	auto addEntry = [&](uint32 a)
	{
		if (covered(a))
		{
			entries.insert(a);
			leaders.insert(a);
		}
	};

	addEntry(entryAddress);
	for (const auto& range : m_tracker.GetRanges())
	{
		leaders.insert(range.start);
		for (uint32 a = range.start; a < range.end; a += 4)
		{
			uint32 op = memory_readU32(a);
			uint32 primary = op >> 26;
			bool endsBlock = false;
			bool link = (op & 1) != 0;
			if (primary == 16 || primary == 18)
			{
				endsBlock = true;
				if ((op & 2) == 0 || primary == 18)
				{
					uint32 target = (primary == 18) ? BranchDisplacement24(op) : (uint32)SImm16(op & 0xFFFC);
					if ((op & 2) == 0)
						target += a;
					if (!link && covered(target))
					{
						leaders.insert(target);
						if (target <= a)
							backEdges.emplace_back(a, target);
					}
				}
				if (link)
					addEntry(a + 4);
			}
			else if (primary == 19)
			{
				uint32 xo = (op >> 1) & 0x3FF;
				if (xo == 16 || xo == 528 || xo == 50)
				{
					endsBlock = true;
					if (link && xo != 50)
						addEntry(a + 4);
				}
			}
			else if (primary == 1)
			{
				endsBlock = true;
				addEntry(a + 4);
			}
			else if (primary == 31 && ((op >> 1) & 0x3FF) == 4)
				endsBlock = true; // tw
			if (endsBlock && covered(a + 4))
				leaders.insert(a + 4);
		}
	}

	// split the ranges at the leaders
	for (const auto& range : m_tracker.GetRanges())
	{
		uint32 start = range.start;
		for (uint32 a = range.start + 4; a <= range.end; a += 4)
		{
			if (a == range.end || leaders.count(a))
			{
				GuestBlock b;
				b.start = start;
				b.end = a;
				m_blocks.emplace(start, b);
				start = a;
			}
		}
	}

	// loop headers check the remaining time slice, they also become entry
	// points so that execution can resume there after a thread switch
	for (auto& [branchAddr, target] : backEdges)
	{
		auto it = m_blocks.find(target);
		cemu_assert_debug(it != m_blocks.end());
		if (it == m_blocks.end())
			continue;
		GuestBlock& header = it->second;
		bool selfLoop = branchAddr + 4 == header.end;
		if (selfLoop && IsPlainCtrLoop(memory_readU32(branchAddr)))
			continue;
		header.needsCycleCheck = true;
		entries.insert(target);
	}

	for (auto& [start, b] : m_blocks)
	{
		if (b.needsCycleCheck)
			b.head = m_func.NewBlock();
		b.body = m_func.NewBlock();
		if (!b.head)
			b.head = b.body;
		b.body->ppcAddress = b.start;
		b.body->ppcEndAddress = b.end;
		b.isEntry = entries.count(start) != 0;
		if (b.isEntry)
		{
			b.head->isEntry = true;
			b.head->entryAddress = start;
			m_entryAddresses.push_back(start);
		}
	}
	for (auto& [start, b] : m_blocks)
	{
		if (b.head != b.body)
		{
			IMLInstruction check = IMLInstruction::Make(IMLOp::CYCLECHECK);
			b.head->Append(check);
			b.head->taken = NewExitBlock(IMLOp::EXITLEAVE, start);
			b.head->next = b.body;
			b.head->ppcAddress = b.start;
			b.head->ppcEndAddress = b.start;
		}
	}
}

bool PPCIMLTranslator::Translate(uint32 entryAddress)
{
	if (!m_tracker.ContainsAddress(entryAddress))
		return false;
	CollectBlocks(entryAddress);
	if (m_entryAddresses.empty())
		return false;
	for (auto& [start, b] : m_blocks)
		TranslateBlock(b);
	m_func.entryAddress = entryAddress;
	m_func.Renumber();
	m_func.RebuildPredecessors();
	return true;
}

void PPCIMLTranslator::TranslateBlock(GuestBlock& block)
{
	m_cur = block.body;
	m_blockEnded = false;
	IMLInstruction cycles = IMLInstruction::Make(IMLOp::CYCLES);
	cycles.imm = (sint32)((block.end - block.start) / 4);
	Emit(cycles);
	for (uint32 a = block.start; a < block.end && !m_blockEnded; a += 4)
	{
		m_addr = a;
		uint32 op = memory_readU32(a);
		if (!TranslateInstruction(op))
			Interp();
	}
	// falls through into the next guest block (or leaves the function there)
	if (!m_blockEnded)
		m_cur->next = BlockForTarget(block.end);
}

// ---------------------------------------------------------------------------
// branches

void PPCIMLTranslator::TranslateB(uint32 op)
{
	uint32 target = BranchDisplacement24(op);
	if ((op & 2) == 0)
		target += m_addr;
	if (op & 1)
	{
		Movi(LR, m_addr + 4);
		IMLInstruction exit = IMLInstruction::Make(IMLOp::EXITB);
		exit.addr = target;
		EndBlock(exit, nullptr, nullptr);
		return;
	}
	if (m_blocks.count(target))
	{
		EndBlock(IMLInstruction::Make(IMLOp::JUMP), BlockForTarget(target), nullptr);
		return;
	}
	IMLInstruction exit = IMLInstruction::Make(IMLOp::EXITB);
	exit.addr = target;
	EndBlock(exit, nullptr, nullptr);
}

void PPCIMLTranslator::EmitBranchCondition(uint32 bo, uint32 bi, bool decrementsCtr, IMLSegment* taken)
{
	bool useCtr = decrementsCtr && (bo & 4) == 0;
	bool useCond = (bo & 16) == 0;
	IMLSegment* notTaken = BlockForTarget(m_addr + 4);
	if (useCtr)
		Op3i(IMLOp::ADD, CTR, CTR, -1);
	if (!useCtr && !useCond)
	{
		EndBlock(IMLInstruction::Make(IMLOp::JUMP), taken, nullptr);
		return;
	}
	if (useCtr)
	{
		// branch condition on CTR: != 0, or == 0 with BO[2]
		IMLInstruction br = IMLInstruction::Make(IMLOp::BRANCH, CTR);
		if (useCond)
		{
			// leave to the not taken path when the CTR test fails
			br.sub = (uint8)((bo & 2) ? IMLCond::NE : IMLCond::EQ);
			m_cur->Append(br);
			m_cur->taken = notTaken;
			IMLSegment* prev = m_cur;
			ContinueInNewBlock();
			prev->next = m_cur;
		}
		else
		{
			br.sub = (uint8)((bo & 2) ? IMLCond::EQ : IMLCond::NE);
			EndBlock(br, taken, notTaken);
			return;
		}
	}
	IMLInstruction br = IMLInstruction::Make(IMLOp::BRANCH, CR(bi));
	br.sub = (uint8)((bo & 8) ? IMLCond::NE : IMLCond::EQ);
	EndBlock(br, taken, notTaken);
}

void PPCIMLTranslator::TranslateBC(uint32 op)
{
	if (op & 2)
	{
		// absolute conditional branches do not occur in Wii U code
		LeaveHere();
		return;
	}
	uint32 bo = FieldD(op);
	uint32 bi = FieldA(op);
	uint32 target = m_addr + (uint32)SImm16(op & 0xFFFC);
	IMLSegment* taken;
	if (op & 1)
	{
		taken = NewExitBlock(IMLOp::EXITB, target);
		IMLInstruction setLR = IMLInstruction::Make(IMLOp::MOVI, LR);
		setLR.imm = (sint32)(m_addr + 4);
		taken->insts.insert(taken->insts.begin(), setLR);
	}
	else
		taken = BlockForTarget(target);
	EmitBranchCondition(bo, bi, true, taken);
}

// bclr and bcctr: the taken path leaves to the address in LR or CTR
void PPCIMLTranslator::TranslateBCLR(uint32 op)
{
	uint32 bo = FieldD(op);
	bool unconditional = (bo & 0x14) == 0x14;
	IMLSegment* saved = m_cur;
	IMLSegment* taken = nullptr;
	if (!unconditional)
	{
		taken = m_func.NewBlock();
		m_cur = taken;
	}
	IMLReg t = Temp();
	Op3i(IMLOp::AND, t, LR, (sint32)~3u);
	if (op & 1)
		Movi(LR, m_addr + 4);
	IMLInstruction exit = IMLInstruction::Make(IMLOp::EXITBR, t);
	exit.addr = m_addr;
	m_cur->Append(exit);
	if (unconditional)
	{
		m_blockEnded = true;
		return;
	}
	m_cur = saved;
	EmitBranchCondition(bo, FieldA(op), true, taken);
}

void PPCIMLTranslator::TranslateBCCTR(uint32 op)
{
	uint32 bo = FieldD(op);
	bool unconditional = (bo & 0x10) != 0;
	IMLSegment* saved = m_cur;
	IMLSegment* taken = nullptr;
	if (!unconditional)
	{
		taken = m_func.NewBlock();
		m_cur = taken;
	}
	IMLReg t = Temp();
	Op3i(IMLOp::AND, t, CTR, (sint32)~3u);
	if (op & 1)
		Movi(LR, m_addr + 4);
	IMLInstruction exit = IMLInstruction::Make(IMLOp::EXITBR, t);
	exit.addr = m_addr;
	m_cur->Append(exit);
	if (unconditional)
	{
		m_blockEnded = true;
		return;
	}
	m_cur = saved;
	EmitBranchCondition(bo, FieldA(op), false, taken);
}

// ---------------------------------------------------------------------------
// instruction dispatch

bool PPCIMLTranslator::TranslateInstruction(uint32 op)
{
	uint32 primary = op >> 26;
	uint32 rD = FieldD(op), rA = FieldA(op), rB = FieldB(op);
	switch (primary)
	{
	case 0:
		return false; // zero word, the interpreter skips it
	case 1:
	{
		IMLInstruction hle = IMLInstruction::Make(IMLOp::HLE);
		hle.imm = (sint32)op;
		hle.addr = m_addr;
		EndBlock(hle, nullptr, nullptr);
		return true;
	}
	case 4:
		return TranslateGroup4(op);
	case 7: // mulli
		Op3i(IMLOp::MUL, GPR(rD), GPR(rA), SImm16(op));
		return true;
	case 8: // subfic: ~rA + imm + 1
		Movi(XER_CA, 1);
	{
		IMLInstruction i = IMLInstruction::MakeImm(IMLOp::ADDE, GPR(rD), XER_CA, SImm16(op));
		i.r[1] = XER_CA;
		i.r[2] = GPR(rA);
		i.flags |= IMLF_INV_A;
		Emit(i);
		return true;
	}
	case 10: // cmpli
		CompareToCR(rD >> 2, GPR(rA), IML_NOREG, true, (sint32)UImm16(op), false);
		return true;
	case 11: // cmpi
		CompareToCR(rD >> 2, GPR(rA), IML_NOREG, true, SImm16(op), true);
		return true;
	case 12: // addic
	case 13: // addic.
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::ADDC, GPR(rD), XER_CA, GPR(rA));
		i.flags |= IMLF_IMM;
		i.imm = SImm16(op);
		Emit(i);
		if (primary == 13)
			UpdateCR0(GPR(rD));
		return true;
	}
	case 14: // addi
		if (rA == 0)
			Movi(GPR(rD), (uint32)SImm16(op));
		else
			Op3i(IMLOp::ADD, GPR(rD), GPR(rA), SImm16(op));
		return true;
	case 15: // addis
		if (rA == 0)
			Movi(GPR(rD), op << 16);
		else
			Op3i(IMLOp::ADD, GPR(rD), GPR(rA), (sint32)(op << 16));
		return true;
	case 16:
		TranslateBC(op);
		return true;
	case 17:
		if ((op & 2) == 0)
			return false;
		return true; // sc is ignored in user mode
	case 18:
		TranslateB(op);
		return true;
	case 19:
		return TranslateGroup19(op);
	case 20: // rlwimi
	{
		uint32 mask = RotateMask(FieldC(op), FieldE(op));
		IMLReg t = Temp();
		IMLReg u = Temp();
		Op3i(IMLOp::ROTL, t, GPR(rD), (sint32)rB);
		Op3i(IMLOp::AND, t, t, (sint32)mask);
		Op3i(IMLOp::AND, u, GPR(rA), (sint32)~mask);
		Op3(IMLOp::OR, GPR(rA), t, u);
		if (Rc(op))
			UpdateCR0(GPR(rA));
		return true;
	}
	case 21: // rlwinm
	{
		uint32 sh = rB;
		uint32 mb = FieldC(op), me = FieldE(op);
		uint32 mask = RotateMask(mb, me);
		IMLReg s = GPR(rD), d = GPR(rA);
		if (mask == 0xFFFFFFFF)
		{
			if (sh == 0)
				Mov(d, s);
			else
				Op3i(IMLOp::ROTL, d, s, (sint32)sh);
		}
		else if (sh == 0)
			Op3i(IMLOp::AND, d, s, (sint32)mask);
		else if (mb == 0 && me == 31 - sh)
			Op3i(IMLOp::SHL, d, s, (sint32)sh); // slwi
		else if (me == 31 && mb == 32 - sh)
			Op3i(IMLOp::SHR, d, s, (sint32)(32 - sh)); // srwi
		else
		{
			IMLReg t = Temp();
			Op3i(IMLOp::ROTL, t, s, (sint32)sh);
			Op3i(IMLOp::AND, d, t, (sint32)mask);
		}
		if (Rc(op))
			UpdateCR0(d);
		return true;
	}
	case 23: // rlwnm
	{
		uint32 mask = RotateMask(FieldC(op), FieldE(op));
		if (mask == 0xFFFFFFFF)
			Op3(IMLOp::ROTL, GPR(rA), GPR(rD), GPR(rB));
		else
		{
			IMLReg t = Temp();
			Op3(IMLOp::ROTL, t, GPR(rD), GPR(rB));
			Op3i(IMLOp::AND, GPR(rA), t, (sint32)mask);
		}
		if (Rc(op))
			UpdateCR0(GPR(rA));
		return true;
	}
	case 24: // ori
		if (UImm16(op) == 0)
			Mov(GPR(rA), GPR(rD));
		else
			Op3i(IMLOp::OR, GPR(rA), GPR(rD), (sint32)UImm16(op));
		return true;
	case 25: // oris
		Op3i(IMLOp::OR, GPR(rA), GPR(rD), (sint32)(op << 16));
		return true;
	case 26: // xori
		Op3i(IMLOp::XOR, GPR(rA), GPR(rD), (sint32)UImm16(op));
		return true;
	case 27: // xoris
		Op3i(IMLOp::XOR, GPR(rA), GPR(rD), (sint32)(op << 16));
		return true;
	case 28: // andi.
		Op3i(IMLOp::AND, GPR(rA), GPR(rD), (sint32)UImm16(op));
		UpdateCR0(GPR(rA));
		return true;
	case 29: // andis.
		Op3i(IMLOp::AND, GPR(rA), GPR(rD), (sint32)(op << 16));
		UpdateCR0(GPR(rA));
		return true;
	case 31:
		return TranslateGroup31(op);
	case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
	case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
		return TranslateIntLoadStore(op);
	case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
	case 56: case 57: case 60: case 61:
		return TranslateFPLoadStore(op);
	case 59:
		return TranslateGroup59(op);
	case 63:
		return TranslateGroup63(op);
	default:
		return false; // twi, unknown opcodes
	}
}

bool PPCIMLTranslator::TranslateGroup19(uint32 op)
{
	uint32 xo = (op >> 1) & 0x3FF;
	uint32 d = FieldD(op), a = FieldA(op), b = FieldB(op);
	auto crLogic = [&](IMLOp logic, bool invert, bool maskOne)
	{
		Op3(logic, CR(d), CR(a), CR(b));
		if (maskOne)
			Op3i(IMLOp::AND, CR(d), CR(d), 1);
		if (invert)
			Op3i(IMLOp::XOR, CR(d), CR(d), 1);
	};
	switch (xo)
	{
	case 0: // mcrf
	{
		uint32 dst = (d >> 2) * 4, src = (a >> 2) * 4;
		if (dst != src)
		{
			for (uint32 i = 0; i < 4; i++)
				Mov(CR(dst + i), CR(src + i));
		}
		return true;
	}
	case 16:
		TranslateBCLR(op);
		return true;
	case 528:
		TranslateBCCTR(op);
		return true;
	case 33: crLogic(IMLOp::OR, true, false); return true;    // crnor
	case 129: crLogic(IMLOp::ANDC, false, false); return true; // crandc
	case 150: return true;                                     // isync
	case 193: crLogic(IMLOp::XOR, false, false); return true; // crxor
	case 225: crLogic(IMLOp::AND, true, false); return true;  // crnand
	case 257: crLogic(IMLOp::AND, false, false); return true; // crand
	case 289: crLogic(IMLOp::XOR, true, false); return true;  // creqv
	case 417: crLogic(IMLOp::ORC, false, true); return true;  // crorc
	case 449: crLogic(IMLOp::OR, false, false); return true;  // cror
	case 50: // rfi
	{
		Interp();
		// the interpreter always leaves the instruction stream here
		IMLInstruction exit = IMLInstruction::Make(IMLOp::EXITB);
		exit.addr = m_addr + 4;
		EndBlock(exit, nullptr, nullptr);
		return true;
	}
	default:
		return false;
	}
}

bool PPCIMLTranslator::TranslateGroup31(uint32 op)
{
	uint32 xo = (op >> 1) & 0x3FF;
	uint32 rD = FieldD(op), rA = FieldA(op), rB = FieldB(op);
	IMLReg D = GPR(rD), A = GPR(rA), B = GPR(rB);

	// add family helpers (results to rD, CR0 on Rc)
	auto finishD = [&]() { if (Rc(op)) UpdateCR0(D); };
	auto finishA = [&]() { if (Rc(op)) UpdateCR0(A); };
	auto addExtended = [&](bool invertA, bool regB, sint32 immB)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::ADDE, D, XER_CA, A);
		if (regB)
			i.r[3] = B;
		else
		{
			i.flags |= IMLF_IMM;
			i.imm = immB;
		}
		if (invertA)
			i.flags |= IMLF_INV_A;
		Emit(i);
		finishD();
	};

	switch (xo)
	{
	// ---- compare, trap ----
	case 0: CompareToCR(rD >> 2, A, B, false, 0, true); return true;
	case 32: CompareToCR(rD >> 2, A, B, false, 0, false); return true;
	case 4: LeaveHere(); return true; // tw: debugger breakpoints, handled by the interpreter

	// ---- add/subtract (the OE forms go to the interpreter) ----
	case 266: Op3(IMLOp::ADD, D, A, B); finishD(); return true;
	case 10:
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::ADDC, D, XER_CA, A, B);
		Emit(i);
		finishD();
		return true;
	}
	case 138: addExtended(false, true, 0); return true;   // adde
	case 202: addExtended(false, false, 0); return true;  // addze
	case 234: addExtended(false, false, -1); return true; // addme
	case 40: Op3(IMLOp::SUB, D, B, A); finishD(); return true; // subf
	case 8: // subfc: ~rA + rB + 1
		Movi(XER_CA, 1);
		addExtended(true, true, 0);
		return true;
	case 136: addExtended(true, true, 0); return true;    // subfe
	case 200: addExtended(true, false, 0); return true;   // subfze
	case 232: addExtended(true, false, -1); return true;  // subfme
	case 104: Op2(IMLOp::NEG, D, A); finishD(); return true;

	// ---- multiply/divide ----
	case 235: Op3(IMLOp::MUL, D, A, B); finishD(); return true;
	case 75:
	case 75 | 0x200:
		Op3(IMLOp::MULHS, D, A, B); finishD(); return true;
	case 11:
	case 11 | 0x200:
		Op3(IMLOp::MULHU, D, A, B); finishD(); return true;
	case 491: Op3(IMLOp::DIVS, D, A, B); finishD(); return true;
	case 459: Op3(IMLOp::DIVU, D, A, B); finishD(); return true;

	// ---- logic (rS in the D field, result to rA) ----
	case 28: Op3(IMLOp::AND, A, D, B); finishA(); return true;
	case 60: Op3(IMLOp::ANDC, A, D, B); finishA(); return true;
	case 444:
		if (rD == rB)
			Mov(A, D); // mr
		else
			Op3(IMLOp::OR, A, D, B);
		finishA();
		return true;
	case 412: Op3(IMLOp::ORC, A, D, B); finishA(); return true;
	case 316: Op3(IMLOp::XOR, A, D, B); finishA(); return true;
	case 476: Op3(IMLOp::AND, A, D, B); Op2(IMLOp::NOT, A, A); finishA(); return true; // nand
	case 124: Op3(IMLOp::OR, A, D, B); Op2(IMLOp::NOT, A, A); finishA(); return true;  // nor
	case 284: Op3(IMLOp::XOR, A, D, B); Op2(IMLOp::NOT, A, A); finishA(); return true; // eqv
	case 954: Op2(IMLOp::EXTS8, A, D); finishA(); return true;
	case 922: Op2(IMLOp::EXTS16, A, D); finishA(); return true;
	case 26: Op2(IMLOp::CNTLZ, A, D); finishA(); return true;
	case 24: Op3(IMLOp::SLW, A, D, B); finishA(); return true;
	case 536: Op3(IMLOp::SRW, A, D, B); finishA(); return true;
	case 792: // sraw
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::SRAW, A, XER_CA, D, B);
		Emit(i);
		finishA();
		return true;
	}
	case 824: // srawi
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::SRAW, A, XER_CA, D);
		i.flags |= IMLF_IMM;
		i.imm = (sint32)rB;
		Emit(i);
		finishA();
		return true;
	}

	// ---- condition register and SPR moves ----
	case 19: // mfcr
	{
		IMLReg acc = Temp();
		IMLReg t = Temp();
		Op3i(IMLOp::SHL, acc, CR(0), 31);
		for (uint32 bit = 1; bit < 32; bit++)
		{
			if (bit == 31)
			{
				Op3(IMLOp::OR, acc, acc, CR(31));
				break;
			}
			Op3i(IMLOp::SHL, t, CR(bit), (sint32)(31 - bit));
			Op3(IMLOp::OR, acc, acc, t);
		}
		Mov(D, acc);
		return true;
	}
	case 144: // mtcrf
	{
		uint32 fieldMask = (op >> 12) & 0xFF;
		IMLReg t = Temp();
		for (uint32 field = 0; field < 8; field++)
		{
			if (((fieldMask >> (7 - field)) & 1) == 0)
				continue;
			for (uint32 i = 0; i < 4; i++)
			{
				uint32 bit = field * 4 + i;
				if (bit == 31)
					Op3i(IMLOp::AND, CR(bit), D, 1);
				else
				{
					Op3i(IMLOp::SHR, t, D, (sint32)(31 - bit));
					Op3i(IMLOp::AND, CR(bit), t, 1);
				}
			}
		}
		return true;
	}
	case 339: // mfspr
	{
		uint32 spr = rA | (rB << 5);
		if (spr == SPR_LR)
			Mov(D, LR);
		else if (spr == SPR_CTR)
			Mov(D, CTR);
		else if (spr >= SPR_UGQR0 && spr <= SPR_UGQR7)
			Mov(D, GQR(spr - SPR_UGQR0));
		else
			return false;
		return true;
	}
	case 467: // mtspr
	{
		uint32 spr = rA | (rB << 5);
		if (spr == SPR_LR)
			Mov(LR, D);
		else if (spr == SPR_CTR)
			Mov(CTR, D);
		else if (spr >= SPR_UGQR0 && spr <= SPR_UGQR7)
			Mov(GQR(spr - SPR_UGQR0), D);
		else
			return false;
		return true;
	}
	case 371: // mftb
	{
		uint32 spr = rA | (rB << 5);
		if (spr != SPR_TBL && spr != SPR_TBU)
			return false;
		IMLInstruction i = IMLInstruction::Make(IMLOp::MFTB, D);
		i.sub = spr == SPR_TBU ? 1 : 0;
		Emit(i);
		return true;
	}

	// ---- integer loads ----
	case 23:  // lwzx
	case 55:  // lwzux
	case 279: // lhzx
	case 311: // lhzux
	case 343: // lhax
	case 375: // lhaux
	case 87:  // lbzx
	case 119: // lbzux
	case 534: // lwbrx
	case 790: // lhbrx
	{
		uint8 size = 4;
		uint8 flags = 0;
		bool update = false;
		switch (xo)
		{
		case 55: update = true; [[fallthrough]];
		case 23: size = 4; break;
		case 311: update = true; [[fallthrough]];
		case 279: size = 2; break;
		case 375: update = true; [[fallthrough]];
		case 343: size = 2; flags = IMLF_SIGNEXT; break;
		case 119: update = true; [[fallthrough]];
		case 87: size = 1; break;
		case 534: size = 4; flags = IMLF_BYTEREV; break;
		case 790: size = 2; flags = IMLF_BYTEREV; break;
		}
		IMLInstruction i = IMLInstruction::Make(IMLOp::LOAD, D);
		i.size = size;
		i.flags = flags;
		if (update)
		{
			IMLReg ea = AddressX(rA, rB);
			i.r[1] = ea;
			Emit(i);
			if (rA != 0 && rA != rD)
				Mov(A, ea);
		}
		else
		{
			i.r[1] = rA ? A : IML_NOREG;
			i.r[2] = B;
			Emit(i);
		}
		return true;
	}
	case 20: // lwarx
	{
		IMLReg ea = AddressX(rA, rB);
		IMLInstruction i = IMLInstruction::Make(IMLOp::LOAD, D, ea);
		i.size = 4;
		Emit(i);
		Mov(RESV_ADDR, ea);
		Mov(RESV_VALUE, D);
		return true;
	}

	// ---- integer stores ----
	case 151: // stwx
	case 183: // stwux
	case 407: // sthx
	case 439: // sthux
	case 215: // stbx
	case 247: // stbux
	case 662: // stwbrx
	case 918: // sthbrx
	{
		uint8 size = 4;
		uint8 flags = 0;
		bool update = false;
		switch (xo)
		{
		case 183: update = true; [[fallthrough]];
		case 151: size = 4; break;
		case 439: update = true; [[fallthrough]];
		case 407: size = 2; break;
		case 247: update = true; [[fallthrough]];
		case 215: size = 1; break;
		case 662: size = 4; flags = IMLF_BYTEREV; break;
		case 918: size = 2; flags = IMLF_BYTEREV; break;
		}
		IMLInstruction i = IMLInstruction::Make(IMLOp::STORE, D);
		i.size = size;
		i.flags = flags;
		if (update)
		{
			IMLReg ea = AddressX(rA, rB);
			i.r[1] = ea;
			Emit(i);
			if (rA != 0)
				Mov(A, ea);
		}
		else
		{
			i.r[1] = rA ? A : IML_NOREG;
			i.r[2] = B;
			Emit(i);
		}
		return true;
	}
	case 150: // stwcx.
	{
		IMLReg ea = AddressX(rA, rB);
		IMLReg match = Temp();
		IMLReg ok = Temp();
		IMLReg t = Temp();
		IMLReg u = Temp();
		Op3(IMLOp::CMPSET, match, RESV_ADDR, ea);
		m_cur->insts.back().sub = (uint8)IMLCond::EQ;
		IMLInstruction x = IMLInstruction::Make(IMLOp::CMPXCHG, ok, ea, RESV_VALUE, D);
		x.r[4] = match;
		Emit(x);
		// on a matching reservation: cr0.SO = XER.SO and the reservation is cleared
		Op3(IMLOp::AND, t, match, XER_SO);
		Op3i(IMLOp::XOR, u, match, 1);
		Op3(IMLOp::AND, u, u, CR(3));
		Op3(IMLOp::OR, CR(3), t, u);
		Op3i(IMLOp::ADD, t, match, -1); // 0 on a match, all ones otherwise
		Op3(IMLOp::AND, RESV_ADDR, RESV_ADDR, t);
		Op3(IMLOp::AND, RESV_VALUE, RESV_VALUE, t);
		Movi(CR(0), 0);
		Movi(CR(1), 0);
		Mov(CR(2), ok);
		return true;
	}

	// ---- floating point loads and stores ----
	case 535: case 567: case 599: case 631:
	case 663: case 695: case 727: case 759: case 983:
		return TranslateFPLoadStore(op);

	// ---- cache and synchronisation ----
	case 1014: // dcbz
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::ZEROLINE, IML_NOREG, rA ? A : IML_NOREG, B);
		Emit(i);
		return true;
	}
	case 278: // dcbt
	case 470: // dcbi
	case 598: // sync
	case 854: // eieio
	case 306: // tlbie
		return true;
	default:
		// OE forms, mcrxr, mfmsr/mtmsr, mfsr/mtsr, string instructions,
		// dcbst/dcbf (notify the GPU buffer cache), icbi, tlbsync, unknown
		return false;
	}
}

bool PPCIMLTranslator::TranslateIntLoadStore(uint32 op)
{
	uint32 primary = op >> 26;
	uint32 rD = FieldD(op), rA = FieldA(op);
	IMLReg D = GPR(rD), A = GPR(rA);
	sint32 imm = SImm16(op);

	auto load = [&](IMLReg base, sint32 disp, uint8 size, uint8 flags)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::LOAD, D, base);
		i.size = size;
		i.flags = flags;
		i.imm = disp;
		Emit(i);
	};
	auto store = [&](IMLReg base, sint32 disp, uint8 size)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::STORE, D, base);
		i.size = size;
		i.imm = disp;
		Emit(i);
	};
	IMLReg baseOrNone = rA ? A : IML_NOREG;

	switch (primary)
	{
	case 32: load(baseOrNone, imm, 4, 0); return true; // lwz
	case 34: load(baseOrNone, imm, 1, 0); return true; // lbz
	case 40: load(baseOrNone, imm, 2, 0); return true; // lhz
	case 42: load(baseOrNone, imm, 2, IMLF_SIGNEXT); return true; // lha
	case 33: // lwzu: rA (also r0) is updated first, then rD loaded from it
	case 35: // lbzu: same order
		Op3i(IMLOp::ADD, A, A, imm);
		load(A, 0, primary == 33 ? 4 : 1, 0);
		return true;
	case 41: // lhzu: rD first, then rA (also r0) = ea
	{
		IMLReg ea = Temp();
		Op3i(IMLOp::ADD, ea, A, imm);
		load(ea, 0, 2, 0);
		Mov(A, ea);
		return true;
	}
	case 43: // lhau: update unless rA = 0 or rA = rD
	{
		IMLReg ea = AddressD(rA, imm);
		load(ea, 0, 2, IMLF_SIGNEXT);
		if (rA != 0 && rA != rD)
			Mov(A, ea);
		return true;
	}
	case 36: // stw: nothing is stored when rA = 0
		if (rA != 0)
			store(A, imm, 4);
		return true;
	case 37: // stwu: always uses and updates rA
	case 39: // stbu
		store(A, imm, primary == 37 ? 4 : 1);
		Op3i(IMLOp::ADD, A, A, imm);
		return true;
	case 38: store(baseOrNone, imm, 1); return true; // stb
	case 44: store(baseOrNone, imm, 2); return true; // sth
	case 45: // sthu: update unless rA = 0
		store(baseOrNone, imm, 2);
		if (rA != 0)
			Op3i(IMLOp::ADD, A, A, imm);
		return true;
	case 46: // lmw
	{
		IMLReg ea = AddressD(rA, imm);
		for (uint32 r = rD; r < 32; r++)
		{
			IMLInstruction i = IMLInstruction::Make(IMLOp::LOAD, GPR(r), ea);
			i.size = 4;
			i.imm = (sint32)((r - rD) * 4);
			Emit(i);
		}
		return true;
	}
	case 47: // stmw
	{
		IMLReg ea = AddressD(rA, imm);
		for (uint32 r = rD; r < 32; r++)
		{
			IMLInstruction i = IMLInstruction::Make(IMLOp::STORE, GPR(r), ea);
			i.size = 4;
			i.imm = (sint32)((r - rD) * 4);
			Emit(i);
		}
		return true;
	}
	default:
		return false;
	}
}
