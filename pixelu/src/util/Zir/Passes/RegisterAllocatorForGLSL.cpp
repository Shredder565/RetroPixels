// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZirUtility.h"
#include "util/Zir/Core/ZpIRPasses.h"
#include "util/Zir/Core/ZpIRDebug.h"

namespace ZirPass
{
	void RegisterAllocatorForGLSL::assignPhysicalRegisters()
	{
		if (m_irFunction->m_basicBlocks.size() != 1)
			cemu_assert_unimplemented(); // values crossing block boundaries are not handled yet
		for (ZpIR::ZpIRBasicBlock* block : m_irFunction->m_basicBlocks)
			assignPhysicalRegistersForBlock(block);
	}

	void RegisterAllocatorForGLSL::assignPhysicalRegistersForBlock(ZpIR::ZpIRBasicBlock* basicBlock)
	{
		// block imports would need registers shared with the predecessor blocks
		if (!basicBlock->m_imports.empty())
			assert_dbg();

		for (ZpIR::IRRegDef& reg : basicBlock->m_regs)
		{
			if (reg.type == ZpIR::DataType::NONE || reg.hasAssignedPhysicalRegister())
				continue;
			PHYS_REG_TYPE kind;
			switch (reg.type)
			{
			case ZpIR::DataType::F32: kind = PHYS_REG_TYPE::F32; break;
			case ZpIR::DataType::S32: kind = PHYS_REG_TYPE::S32; break;
			case ZpIR::DataType::U32: kind = PHYS_REG_TYPE::U32; break;
			default:
				cemu_assert_debug(false); // GLSL output only knows 32-bit scalar types
				continue;
			}
			reg.assignPhysicalRegister(MakePhysReg(kind, m_nextIndex[(uint32)kind]++));
		}
	}

	std::string RegisterAllocatorForGLSL::DebugPrintHelper_getPhysRegisterName(ZpIR::ZpIRBasicBlock* block, ZpIR::ZpIRPhysicalReg r)
	{
		(void)block;
		const uint32 index = GetPhysRegIndex(r);
		switch (typeOf(r))
		{
		case PHYS_REG_TYPE::F32: return fmt::format("r{}f", index);
		case PHYS_REG_TYPE::U32: return fmt::format("r{}u", index);
		case PHYS_REG_TYPE::S32: return fmt::format("r{}i", index);
		default: return {};
		}
	}
}
