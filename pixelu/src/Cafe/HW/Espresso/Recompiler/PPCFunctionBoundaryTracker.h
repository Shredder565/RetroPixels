// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Espresso/EspressoISA.h"
#include "Cafe/HW/MMU/MMU.h"

bool GamePatch_IsNonReturnFunction(uint32 hleIndex);

// Finds the extent of the guest function that contains an entry address.
//
// Starting at the entry, guest code is followed until the flow ends (an
// unconditional branch, a return or an HLE call that does not come back).
// Targets of local branches (b/bc without link) are followed as well, so the
// result can consist of several address ranges. Calls (branches with link)
// are not followed; they leave the compiled function.
class PPCFunctionBoundaryTracker
{
public:
	struct Range
	{
		uint32 start; // first instruction
		uint32 end;   // address after the last instruction
	};

	// upper bound for the number of instructions taken into one function
	static constexpr uint32 kMaxInstructions = 0x2000;

	void Discover(uint32 entryAddress)
	{
		m_covered.clear();
		m_branchTargets.clear();
		m_ranges.clear();
		std::vector<uint32> pending;
		pending.push_back(entryAddress);
		uint32 budget = kMaxInstructions;
		while (!pending.empty() && budget > 0)
		{
			uint32 address = pending.back();
			pending.pop_back();
			ScanFlow(address, pending, budget);
		}
		BuildRanges();
	}

	const std::vector<Range>& GetRanges() const { return m_ranges; }

	bool ContainsAddress(uint32 address) const
	{
		return m_covered.find(address) != m_covered.end();
	}

	// targets of local branches (inside or outside of the function)
	const std::set<uint32>& GetBranchTargets() const { return m_branchTargets; }

	// the range that contains an address, false if there is none
	bool GetRangeForAddress(uint32 address, Range& rangeOut) const
	{
		for (const Range& r : m_ranges)
		{
			if (address >= r.start && address < r.end)
			{
				rangeOut = r;
				return true;
			}
		}
		return false;
	}

	static bool IsCodeAddress(uint32 address)
	{
		return (address & 3) == 0 && address >= 0x00001000 && address < 0x10000000 && memory_isAddressRangeAccessible(address, 4);
	}

private:
	static uint32 ReadOpcode(uint32 address)
	{
		return memory_readU32(address);
	}

	static bool IsUnconditionalJump(uint32 opcode)
	{
		return (opcode >> 26) == 18 && (opcode & 1) == 0;
	}

	// relative or absolute target of a b/bc instruction
	static uint32 BranchTarget(uint32 address, uint32 opcode)
	{
		uint32 displacement;
		if ((opcode >> 26) == 18)
			displacement = (uint32)(((sint32)(opcode << 6)) >> 6) & ~3u;
		else
			displacement = (uint32)(sint32)(sint16)(uint16)(opcode & 0xFFFC);
		return (opcode & 2) ? displacement : address + displacement;
	}

	// returns false when the flow does not continue after the instruction
	bool FlowContinues(uint32 address, uint32 opcode, std::vector<uint32>& pending)
	{
		uint32 primary = opcode >> 26;
		switch (primary)
		{
		case 0:
			return opcode != 0;
		case 1:
		{
			// HLE call. Patched game functions (ids 0x1000-0x3FFF) may continue
			// after the call unless they are known not to return.
			uint32 id = opcode & 0xFFFF;
			if (id >= 0x1000 && id < 0x4000)
				return !GamePatch_IsNonReturnFunction(id - 0x1000);
			return false;
		}
		case 16: // bc
			if ((opcode & 1) == 0)
				AddTarget(BranchTarget(address, opcode), pending);
			return true;
		case 18: // b
		{
			if (opcode & 1)
				return true; // call
			AddTarget(BranchTarget(address, opcode), pending);
			// a run of unconditional jumps is usually a jump table, keep going
			bool tableAfter = IsUnconditionalJump(ReadOpcodeSafe(address + 4)) && IsUnconditionalJump(ReadOpcodeSafe(address + 8));
			bool tableBefore = IsUnconditionalJump(ReadOpcodeSafe(address - 4)) && IsUnconditionalJump(ReadOpcodeSafe(address - 8));
			return tableAfter || tableBefore;
		}
		case 19:
		{
			uint32 xo = (opcode >> 1) & 0x3FF;
			uint32 bo = (opcode >> 21) & 0x1F;
			bool link = (opcode & 1) != 0;
			if (xo == 16) // bclr
				return !(((bo & 0x14) == 0x14) && !link);
			if (xo == 528) // bcctr
			{
				if (opcode != 0x4E800420)
					return true;
				// bctr is often a switch statement with the case code right behind it.
				// Only alignment padding or data after it ends the function.
				uint32 nextOp = ReadOpcodeSafe(address + 4);
				return !(nextOp == 0x60000000 || (nextOp >> 26) == 0);
			}
			return true;
		}
		default:
			return true;
		}
	}

	static uint32 ReadOpcodeSafe(uint32 address)
	{
		if (!IsCodeAddress(address))
			return 0;
		return ReadOpcode(address);
	}

	void AddTarget(uint32 target, std::vector<uint32>& pending)
	{
		m_branchTargets.insert(target);
		if (IsCodeAddress(target) && !ContainsAddress(target))
			pending.push_back(target);
	}

	void ScanFlow(uint32 address, std::vector<uint32>& pending, uint32& budget)
	{
		while (budget > 0 && IsCodeAddress(address) && !ContainsAddress(address))
		{
			m_covered.insert(address);
			budget--;
			uint32 opcode = ReadOpcode(address);
			if (!FlowContinues(address, opcode, pending))
				return;
			address += 4;
		}
	}

	void BuildRanges()
	{
		m_ranges.clear();
		for (uint32 address : m_covered)
		{
			if (!m_ranges.empty() && m_ranges.back().end == address)
				m_ranges.back().end += 4;
			else
				m_ranges.push_back({address, address + 4});
		}
	}

	std::set<uint32> m_covered; // ordered, used to build the ranges
	std::set<uint32> m_branchTargets;
	std::vector<Range> m_ranges;
};
