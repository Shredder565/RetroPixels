// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Transcompiler/LatteTC.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"

#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZpIRBuilder.h"

/* ValueMap */

bool LatteTCGenIR::ValueMap::lookup(GPRElementIndex element, ZpIR::IRReg& value) const
{
	if (!m_gprKnown.test(element))
		return false;
	value = m_gprValue[element];
	return true;
}

void LatteTCGenIR::ValueMap::bind(GPRElementIndex element, ZpIR::IRReg value)
{
	m_gprKnown.set(element);
	m_gprValue[element] = value;
}

bool LatteTCGenIR::ValueMap::lookupPrevious(uint32 slot, ZpIR::IRReg& value) const
{
	if (slot >= kNumALUSlots || !m_prevKnown.test(slot))
		return false;
	value = m_prevValue[slot];
	return true;
}

void LatteTCGenIR::ValueMap::queueWrite(uint32 slot, GPRElementIndex element, ZpIR::IRReg value)
{
	cemu_assert_debug(slot < kNumALUSlots && !m_queue[slot].toGpr);
	m_queue[slot].element = element;
	m_queue[slot].value = value;
	m_queue[slot].toGpr = true;
}

void LatteTCGenIR::ValueMap::queuePrevious(uint32 slot, ZpIR::IRReg value)
{
	cemu_assert_debug(slot < kNumALUSlots && !m_queue[slot].toPrevious);
	m_queue[slot].previousValue = value;
	m_queue[slot].toPrevious = true;
}

void LatteTCGenIR::ValueMap::commitGroup()
{
	// PV/PS only hold the results of the most recent group
	m_prevKnown.reset();
	for (uint32 slot = 0; slot < kNumALUSlots; slot++)
	{
		QueuedWrite& q = m_queue[slot];
		if (q.toGpr)
			bind(q.element, q.value);
		if (q.toPrevious)
		{
			m_prevKnown.set(slot);
			m_prevValue[slot] = q.previousValue;
		}
		q = {};
	}
}

void LatteTCGenIR::ValueMap::clear()
{
	m_gprKnown.reset();
	m_prevKnown.reset();
	m_queue = {};
}

/* driver */

void LatteTCGenIR::setVertexShaderContext(const LatteFetchShader* parsedFetchShader, const uint32* vtxSemanticTable)
{
	m_fetchShader = parsedFetchShader;
	m_semanticTable = vtxSemanticTable;
}

void LatteTCGenIR::unsupported()
{
	m_incomplete = true;
	cemu_assert_debug(false);
}

// Translates the CF instructions [firstCF, endCF) into one basic block
void LatteTCGenIR::translateBlock(ZpIR::ZpIRBasicBlock* block, uint32 firstCF, uint32 endCF, bool isEntry)
{
	ZpIR::BasicBlockBuilder builder(block);
	m_builder = &builder;
	m_inEntryBlock = isEntry;
	m_values.clear();

	if (isEntry && m_shaderType == VERTEX)
	{
		// R0 starts out as zero (the vertex/instance id setup is not modelled yet)
		for (uint32 ch = 0; ch < 4; ch++)
		{
			ZpIR::IRReg r0 = builder.createReg(ZpIR::DataType::U32);
			builder.emit_RR(ZpIR::IR::OpCode::MOV, r0, builder.createConstU32(0));
			m_values.bind(MakeElement(0, ch), r0);
		}
	}

	const auto* cfList = reinterpret_cast<const LatteCFInstruction*>(m_program);
	for (uint32 i = firstCF; i < endCF; i++)
	{
		const LatteCFInstruction& cf = cfList[i];
		switch (cf.getField_Opcode())
		{
		case LatteCFInstruction::INST_NOP:
			break;
		case LatteCFInstruction::INST_CALL_FS:
			if (m_shaderType == VERTEX)
				translateFetchShaderCall();
			else
				unsupported();
			break;
		case LatteCFInstruction::INST_ALU:
			translateALUClause(*cf.getParser_ALU());
			break;
		case LatteCFInstruction::INST_EXPORT:
		case LatteCFInstruction::INST_EXPORT_DONE:
			translateExport(*cf.getParser_EXPORT());
			break;
		default:
			unsupported();
			break;
		}
	}
	m_builder = nullptr;
}

ZpIR::ZpIRFunction* LatteTCGenIR::transcompileLatteToIR(const void* programData, uint32 programSize, SHADER_TYPE shaderType)
{
	m_program = static_cast<const uint32*>(programData);
	m_programSize = programSize;
	m_shaderType = shaderType;
	m_incomplete = false;

	auto* function = new ZpIR::ZpIRFunction();

	// The CF program runs until the first instruction with END_OF_PROGRAM set (ALU
	// clauses have no such flag). Branches are not supported yet, so main() is a
	// single basic block.
	const auto* cfList = reinterpret_cast<const LatteCFInstruction*>(m_program);
	const uint32 maxCF = programSize / (uint32)sizeof(LatteCFInstruction);
	uint32 endCF = 0;
	while (endCF < maxCF)
	{
		const LatteCFInstruction& cf = cfList[endCF++];
		if ((cf.getField_Opcode() & LatteCFInstruction::MASK_ALU) == 0 && cf.getField_END_OF_PROGRAM())
			break;
	}

	auto* mainBlock = new ZpIR::ZpIRBasicBlock();
	function->m_basicBlocks.emplace_back(mainBlock);
	function->m_entryBlocks.emplace_back(mainBlock);
	translateBlock(mainBlock, 0, endCF, true);
	return function;
}
