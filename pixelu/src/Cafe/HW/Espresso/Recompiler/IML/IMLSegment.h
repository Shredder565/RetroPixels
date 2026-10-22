// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "IMLInstruction.h"

// Blocks and functions of the intermediate code.
//
// A block (IMLSegment) is a list of instructions. Only its last instruction
// may be a terminator. Control continues at 'taken' when a JUMP, BRANCH,
// BRANCHCMP or CYCLECHECK goes there and at 'next' otherwise; a block
// without terminator continues at 'next'. Blocks ending in an exit have no
// successors.

// a fixed size set of virtual registers
class IMLRegSet
{
public:
	void Resize(size_t count) { m_words.assign((count + 63) / 64, 0); }
	size_t WordCount() const { return m_words.size(); }

	void Add(IMLReg r) { m_words[r >> 6] |= (1ull << (r & 63)); }
	void Remove(IMLReg r) { m_words[r >> 6] &= ~(1ull << (r & 63)); }
	bool Has(IMLReg r) const { return (m_words[r >> 6] >> (r & 63)) & 1; }
	void Clear() { std::fill(m_words.begin(), m_words.end(), 0); }

	// returns true when this set changed
	bool Merge(const IMLRegSet& other)
	{
		bool changed = false;
		for (size_t i = 0; i < m_words.size(); i++)
		{
			uint64 v = m_words[i] | other.m_words[i];
			changed |= (v != m_words[i]);
			m_words[i] = v;
		}
		return changed;
	}

	void Subtract(const IMLRegSet& other)
	{
		for (size_t i = 0; i < m_words.size(); i++)
			m_words[i] &= ~other.m_words[i];
	}

	void Intersect(const IMLRegSet& other)
	{
		for (size_t i = 0; i < m_words.size(); i++)
			m_words[i] &= other.m_words[i];
	}

	bool operator==(const IMLRegSet& other) const { return m_words == other.m_words; }

	template<typename F>
	void ForEach(F f) const
	{
		for (size_t w = 0; w < m_words.size(); w++)
		{
			uint64 bits = m_words[w];
			while (bits)
			{
				uint32 b = (uint32)std::countr_zero(bits);
				bits &= bits - 1;
				f((IMLReg)(w * 64 + b));
			}
		}
	}

private:
	std::vector<uint64> m_words;
};

struct IMLSegment
{
	uint32 index{0};             // position in IMLFunction::blocks
	uint32 ppcAddress{0};        // first guest instruction, 0 for blocks made by the compiler
	uint32 ppcEndAddress{0};     // address after the last guest instruction
	bool isEntry{false};         // can be entered from the jump table
	uint32 entryAddress{0};
	std::vector<IMLInstruction> insts;
	IMLSegment* taken{nullptr};
	IMLSegment* next{nullptr};
	std::vector<IMLSegment*> preds;
	uint32 loopDepth{0};

	// analysis results
	IMLRegSet liveIn;
	IMLRegSet liveOut;

	// backend
	sint32 codeOffset{-1};

	IMLInstruction* Terminator()
	{
		if (insts.empty() || !insts.back().Info().terminator)
			return nullptr;
		return &insts.back();
	}

	bool EndsInExit()
	{
		IMLInstruction* t = Terminator();
		return t && t->Info().leaves;
	}

	IMLInstruction& Append(const IMLInstruction& inst)
	{
		insts.push_back(inst);
		return insts.back();
	}

	// inserts in front of the terminator (or at the end)
	IMLInstruction& AppendBeforeTerminator(const IMLInstruction& inst)
	{
		size_t pos = insts.size();
		if (Terminator())
			pos--;
		insts.insert(insts.begin() + pos, inst);
		return insts[pos];
	}

	template<typename F>
	void ForEachSuccessor(F f)
	{
		if (taken)
			f(taken);
		if (next && next != taken)
			f(next);
	}
};

struct IMLVRegInfo
{
	IMLRegKind kind{IMLRegKind::Int};
	// home slot: offset into PPCInterpreter_t for guest registers, spill slot index for temporaries (-1 = none)
	sint32 home{-1};
	uint8 homeSize{4};
	// register allocation result
	sint16 hostReg{-1};
	bool spilled{false};
	bool noSpill{false}; // short lived reload/store temporary made by the spiller
};

struct IMLFunction
{
	uint32 entryAddress{0};
	std::vector<IMLSegment*> blocks;
	std::vector<IMLVRegInfo> vregs;
	uint32 spillSlotCount{0};

	IMLFunction();
	~IMLFunction();
	IMLFunction(const IMLFunction&) = delete;
	IMLFunction& operator=(const IMLFunction&) = delete;

	IMLSegment* NewBlock();
	IMLSegment* InsertBlockAfter(IMLSegment* after);
	void Renumber();
	void RebuildPredecessors();
	void RemoveUnreachable();

	IMLReg NewTemp(IMLRegKind kind);
	IMLRegKind KindOf(IMLReg r) const { return vregs[r].kind; }
	size_t RegCount() const { return vregs.size(); }
	uint32 AllocSpillSlot() { return spillSlotCount++; }
};

// home slot offset of a guest register in PPCInterpreter_t
sint32 IMLGuest_HomeOffset(IMLReg r);
uint8 IMLGuest_HomeSize(IMLReg r);
