// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ShaderInfo/ShaderInfo.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"

namespace Latte
{
	namespace
	{
		constexpr uint32 kNumColorTargets = 8;

		bool IsExport(LatteCFInstruction::OPCODE op)
		{
			return op == LatteCFInstruction::INST_EXPORT || op == LatteCFInstruction::INST_EXPORT_DONE;
		}
	}

	bool ShaderDescription::analyzeShaderCode(void* shaderProgram, size_t sizeInBytes, LatteConst::ShaderType shaderType)
	{
		renderTargetWriteMask = {};
		if (!shaderProgram)
			return false;
		const auto* program = static_cast<const LatteCFInstruction*>(shaderProgram);
		const size_t numWords = sizeInBytes / sizeof(LatteCFInstruction);
		for (size_t i = 0; i < numWords; i++)
		{
			const LatteCFInstruction& cf = program[i];
			const LatteCFInstruction::OPCODE op = cf.getField_Opcode();
			if ((op & LatteCFInstruction::MASK_ALU) != 0)
				continue; // ALU clauses carry no end-of-program flag
			if (IsExport(op) && shaderType == LatteConst::ShaderType::Pixel)
			{
				const LatteCFInstruction_EXPORT_IMPORT* exp = cf.getParser_EXPORT();
				if (exp->getField_TYPE() == LatteCFInstruction_EXPORT_IMPORT::PIXEL)
				{
					// a burst writes consecutive targets starting at ARRAY_BASE
					for (uint32 k = 0; k < exp->getField_BURST_COUNT(); k++)
					{
						const uint32 target = exp->getField_ARRAY_BASE() + k;
						if (target < kNumColorTargets)
							renderTargetWriteMask.set(target);
					}
				}
			}
			if (cf.getField_END_OF_PROGRAM())
				return true;
		}
		// no end-of-program flag inside the given size, keep what was found
		return true;
	}
};
