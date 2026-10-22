// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZirUtility.h"
#include "util/Zir/Core/ZpIRPasses.h"
#include "util/Zir/Core/ZpIRDebug.h"

namespace ZirPass
{
	using namespace ZpIR;

	void RARegular::applyPass()
	{
		auto& blocks = m_irFunction->m_basicBlocks;
		if (blocks.size() > 1)
			cemu_assert_unimplemented(); // ranges are not connected across blocks yet
		m_raBlockArray.clear();
		m_raBlockArray.resize(blocks.size());
		for (size_t i = 0; i < blocks.size(); i++)
		{
			buildRanges(*blocks[i], m_raBlockArray[i]);
			assignRegisters(*blocks[i], m_raBlockArray[i]);
		}
		m_irFunction->state.registersAllocated = true;
	}

	void RARegular::buildRanges(ZpIRBasicBlock& block, RABlock_t& raBlock)
	{
		auto& ranges = raBlock.livenessRanges;
		ranges.clear();
		ranges.resize(block.m_regs.size());
		auto define = [&](IRReg r, sint32 at) {
			auto& slot = ranges[r];
			if (slot)
			{
				cemu_assert_suspicious(); // SSA: a register is written once
				slot->setEnd(at);
				return;
			}
			slot = std::make_unique<RALivenessRange_t>(&raBlock, r, at, at, block.m_regs[r].type);
		};
		auto use = [&](IRReg r, sint32 at) {
			auto& slot = ranges[r];
			if (!slot)
			{
				cemu_assert_suspicious(); // read before write
				slot = std::make_unique<RALivenessRange_t>(&raBlock, r, -1, at, block.m_regs[r].type);
				return;
			}
			slot->setEnd(at);
		};

		for (auto& imp : block.m_imports)
			define(imp.reg, -1);
		sint32 index = 0;
		for (IR::__InsBase* ins = block.m_instructionFirst; ins; ins = ins->next, index++)
		{
			const sint32 at = index;
			ZpIRCmdUtil::forEachAccessedReg(block, ins,
				[&](IRReg r) { use(r, at); },
				[&](IRReg r) { define(r, at); });
		}
		for (auto& exp : block.m_exports)
			use(exp.reg, index); // live until after the last instruction
	}

	std::span<const sint32> RARegular::candidatesFor(DataType dataType)
	{
		static constexpr sint32 kWide[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
		static constexpr sint32 kNarrow[16] = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
		switch (dataType)
		{
		case DataType::U64:
		case DataType::POINTER:
			return kWide;
		case DataType::U32:
			return kNarrow;
		default:
			cemu_assert_unimplemented();
			return kNarrow;
		}
	}

	void RARegular::assignRegisters(ZpIRBasicBlock& block, RABlock_t& raBlock)
	{
		// visit ranges by start index (ties: lower register number first)
		std::vector<RALivenessRange_t*> order;
		for (auto& r : raBlock.livenessRanges)
		{
			if (r)
				order.push_back(r.get());
		}
		std::stable_sort(order.begin(), order.end(), [](const RALivenessRange_t* a, const RALivenessRange_t* b) { return a->m_startIndex < b->m_startIndex; });

		std::vector<RALivenessRange_t*> assigned;
		for (RALivenessRange_t* range : order)
		{
			bool placed = false;
			for (sint32 candidate : candidatesFor(range->m_irDataType))
			{
				const bool taken = std::any_of(assigned.begin(), assigned.end(), [&](const RALivenessRange_t* other) {
					return other->isOverlapping(range) && baseRegister(other->m_physicalRegister) == baseRegister(candidate);
				});
				if (taken)
					continue;
				range->assignPhysicalRegister((ZpIRPhysicalReg)candidate);
				block.m_regs[range->m_irReg].assignPhysicalRegister((ZpIRPhysicalReg)candidate);
				assigned.push_back(range);
				placed = true;
				break;
			}
			if (!placed)
				cemu_assert_unimplemented(); // would need spilling
		}
	}
}
