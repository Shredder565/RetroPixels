// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Transcompiler/LatteTC.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#include "util/Zir/Core/ZpIRBuilder.h"

using ZpIR::DataType;
using ZpIR::IRReg;
using IROp = ZpIR::IR::OpCode;
using ALUOp = LatteClauseInstruction_ALU::OPCODE_OP2;

namespace
{
	// special operand selectors of the ALU encoding
	constexpr uint32 kSelConst0F = 248;
	constexpr uint32 kSelConst1F = 249;
	constexpr uint32 kSelConst1I = 250;
	constexpr uint32 kSelConstM1I = 251;
	constexpr uint32 kSelConstHalfF = 252;
	constexpr uint32 kSelLiteral = 253;
	constexpr uint32 kSelPV = 254;
	constexpr uint32 kSelPS = 255;
	constexpr uint32 kSelCFileBase = 256;

	constexpr uint32 kFloatOne = 0x3F800000;

	// raw view of an ALU slot (needed for OP3 operands, which have no typed accessor)
	struct RawALUWords
	{
		uint32 w0;
		uint32 w1;
	};

	const RawALUWords& Raw(const LatteClauseInstruction_ALU* instr)
	{
		return *reinterpret_cast<const RawALUWords*>(instr);
	}

	// literal constants of a group are read from the dwords after it; returns how many
	// 64-bit slots are needed (0, 1 or 2) for an operand
	uint32 LiteralSlotsFor(uint32 sel, uint32 chan)
	{
		if (sel != kSelLiteral)
			return 0;
		return chan >= 2 ? 2 : 1;
	}
}

/* operands */

// value of gpr.channel; unknown registers are zero in the entry block and imported otherwise
IRReg LatteTCGenIR::readGPR(uint32 gpr, uint32 channel, DataType typeHint)
{
	const GPRElementIndex element = MakeElement(gpr, channel);
	IRReg value;
	if (m_values.lookup(element, value))
		return value;
	if (m_inEntryBlock)
	{
		switch (typeHint)
		{
		case DataType::F32: return m_builder->createConstF32(0.0f);
		case DataType::S32: return m_builder->createConstS32(0);
		default: return m_builder->createConstU32(0);
		}
	}
	value = m_builder->createReg(typeHint);
	m_builder->addImport(value, element);
	return value;
}

IRReg LatteTCGenIR::castTo(IRReg value, DataType type)
{
	if (m_builder->getRegType(value) == type)
		return value;
	IRReg converted = m_builder->createReg(type);
	m_builder->emit_RR(IROp::BITCAST, converted, value);
	return converted;
}

// like readGPR, but reinterprets the bits if the register currently holds another type.
// The reinterpreted value replaces the GPR binding since further reads likely want the same type.
IRReg LatteTCGenIR::readGPRAs(uint32 gpr, uint32 channel, DataType type)
{
	const IRReg original = readGPR(gpr, channel, type);
	const IRReg typed = castTo(original, type);
	if (typed != original)
		m_values.bind(MakeElement(gpr, channel), typed);
	return typed;
}

DataType LatteTCGenIR::guessOperandType(const OperandDesc& op, const uint32* literals) const
{
	if (op.sel < kNumGPRs)
	{
		IRReg r;
		if (m_values.lookup(MakeElement(op.sel, op.chan), r))
			return m_builder->getRegType(r);
		return DataType::F32; // most registers hold floats
	}
	if (op.sel == kSelLiteral && literals)
		return literals[op.chan] == kFloatOne ? DataType::F32 : DataType::S32;
	if (op.sel == kSelConst1I || op.sel == kSelConstM1I)
		return DataType::S32;
	if (op.sel == kSelPV || op.sel == kSelPS)
	{
		IRReg r;
		if (m_values.lookupPrevious(op.sel == kSelPS ? 4 : op.chan, r))
			return m_builder->getRegType(r);
	}
	return DataType::F32;
}

IRReg LatteTCGenIR::readOperand(const OperandDesc& op, const uint32* literals, DataType type)
{
	IRReg value{};
	if (op.sel < kNumGPRs)
	{
		if (op.rel)
			unsupported(); // relative addressing
		value = readGPRAs(op.sel, op.chan, type);
	}
	else if (op.sel >= kSelCFileBase)
	{
		// uniform register c[n].chan
		value = m_builder->createReg(type);
		auto location = ZpIR::ShaderSubset::ShaderImportLocation().SetUniformRegister((uint16)((op.sel - kSelCFileBase) * 4 + op.chan));
		m_builder->emit_IMPORT(location, value);
	}
	else
	{
		switch (op.sel)
		{
		case kSelConst0F: value = m_builder->createConstF32(0.0f); break;
		case kSelConst1F: value = m_builder->createConstF32(1.0f); break;
		case kSelConstHalfF: value = m_builder->createConstF32(0.5f); break;
		case kSelConst1I: value = m_builder->createConstS32(1); break;
		case kSelConstM1I: value = m_builder->createConstS32((uint32)-1); break;
		case kSelLiteral:
			if (!literals)
			{
				unsupported();
				return m_builder->createTypedConst(0, type);
			}
			value = m_builder->createTypedConst(literals[op.chan], type);
			break;
		case kSelPV:
		case kSelPS:
			if (!m_values.lookupPrevious(op.sel == kSelPS ? 4 : op.chan, value))
			{
				unsupported();
				return m_builder->createTypedConst(0, type);
			}
			break;
		default:
			unsupported();
			return m_builder->createTypedConst(0, type);
		}
		value = castTo(value, type);
	}
	if (op.neg)
	{
		if (type == DataType::F32)
			value = m_builder->emit_RRR(IROp::MUL, DataType::F32, value, m_builder->createConstF32(-1.0f));
		else
			unsupported();
	}
	return value;
}

/* vertex fetch */

void LatteTCGenIR::translateFetchShaderCall()
{
	if (!m_fetchShader || !m_semanticTable)
	{
		unsupported();
		return;
	}
	if (!m_fetchShader->bufferGroupsInvalid.empty())
		unsupported();
	for (const auto& group : m_fetchShader->bufferGroups)
	{
		for (sint32 a = 0; a < group.attribCount; a++)
		{
			const LatteParsedFetchShaderAttribute& attribute = group.attrib[a];
			// the semantic table says which input register (R1 and up) receives the attribute
			for (uint32 slot = 0; slot < 32; slot++)
			{
				if (m_semanticTable[slot] != attribute.semanticId)
					continue;
				translateFetchedAttribute(attribute, slot + 1);
				break;
			}
		}
	}
}

void LatteTCGenIR::translateFetchedAttribute(const LatteParsedFetchShaderAttribute& attribute, uint32 dstGPR)
{
	using DstSel = LatteConst::VertexFetchDstSel;
	using NumFormat = LatteClauseInstruction_VTX::NUM_FORMAT_ALL;
	for (uint32 ch = 0; ch < 4; ch++)
	{
		const DstSel sel = static_cast<DstSel>(attribute.ds[ch]);
		IRReg value;
		if (sel == DstSel::CONST_0F || sel == DstSel::CONST_1F)
		{
			value = m_builder->createReg(DataType::F32);
			m_builder->emit_RR(IROp::MOV, value, m_builder->createConstF32(sel == DstSel::CONST_1F ? 1.0f : 0.0f));
		}
		else if (sel == DstSel::MASKED)
		{
			continue; // register channel keeps its value
		}
		else if ((uint32)sel <= (uint32)DstSel::W)
		{
			const uint16 component = (uint16)sel;
			IRReg raw = m_builder->createReg(DataType::U32);
			m_builder->emit_IMPORT(ZpIR::ShaderSubset::ShaderImportLocation().SetVertexAttribute(attribute.semanticId, component), raw);
			if (attribute.endianSwap == LatteConst::VertexFetchEndianMode::SWAP_U32)
			{
				IRReg swapped = m_builder->createReg(DataType::U32);
				m_builder->emit_RR(IROp::SWAP_ENDIAN, swapped, raw);
				raw = swapped;
			}
			const NumFormat numFormat = static_cast<NumFormat>(attribute.nfa);
			switch (attribute.format)
			{
			case Latte::E_HWFMT::HWFMT_32_32_FLOAT:
			case Latte::E_HWFMT::HWFMT_32_32_32_FLOAT:
				// raw float bits
				value = m_builder->createReg(DataType::F32);
				m_builder->emit_RR(IROp::BITCAST, value, raw);
				break;
			case Latte::E_HWFMT::HWFMT_8_8_8_8:
				if (numFormat == NumFormat::NUM_FORMAT_NORM && !attribute.isSigned)
				{
					// unorm8 -> [0, 1]
					const IRReg asFloat = m_builder->emit_RR(IROp::CONVERT_INT_TO_FLOAT, DataType::F32, raw);
					const IRReg scale = m_builder->createConstF32(255.0f);
					value = m_builder->emit_RRR(IROp::DIV, DataType::F32, asFloat, scale);
				}
				else
				{
					unsupported();
					value = raw;
				}
				break;
			default:
				unsupported();
				value = raw;
				break;
			}
		}
		else
		{
			unsupported();
			continue;
		}
		m_values.bind(MakeElement(dstGPR, ch), value);
	}
}

/* ALU clauses */

void LatteTCGenIR::translateALUClause(const LatteCFInstruction_ALU& clause)
{
	// ADDR counts 64-bit units from the start of the program
	const auto* first = reinterpret_cast<const LatteClauseInstruction_ALU*>(m_program + clause.getField_ADDR() * 2);
	const LatteClauseInstruction_ALU* end = first + clause.getField_COUNT();
	const LatteClauseInstruction_ALU* programEnd = reinterpret_cast<const LatteClauseInstruction_ALU*>(m_program) + m_programSize / 8;
	if (end > programEnd)
	{
		unsupported();
		end = programEnd;
	}

	const LatteClauseInstruction_ALU_OP2* slots[kNumALUSlots] = {};
	uint32 literalSlots = 0;
	bool groupOpen = false;
	for (const LatteClauseInstruction_ALU* instr = first; instr < end; instr++)
	{
		groupOpen = true;
		const RawALUWords& raw = Raw(instr);
		literalSlots = std::max(literalSlots, LiteralSlotsFor(raw.w0 & 0x1FF, (raw.w0 >> 10) & 3));
		literalSlots = std::max(literalSlots, LiteralSlotsFor((raw.w0 >> 13) & 0x1FF, (raw.w0 >> 23) & 3));
		if (instr->isOP3())
		{
			literalSlots = std::max(literalSlots, LiteralSlotsFor(raw.w1 & 0x1FF, (raw.w1 >> 10) & 3));
			unsupported();
		}
		else
		{
			const LatteClauseInstruction_ALU_OP2* op = instr->getOP2Instruction();
			// vector ops go to the unit of their destination channel, the scalar unit
			// takes transcendental ops and a second op on an already used channel
			uint32 unit = op->isTranscedentalUnit() ? 4 : op->getDestElem();
			if (unit < 4 && slots[unit])
				unit = 4;
			if (slots[unit])
				unsupported(); // more than five ops in one group
			else
				slots[unit] = op;
		}
		if (!instr->isLastInGroup())
			continue;
		// the group's literals follow its last instruction
		const uint32* literals = nullptr;
		if (literalSlots)
		{
			literals = reinterpret_cast<const uint32*>(instr + 1);
			instr += literalSlots;
			if (instr >= end)
			{
				unsupported();
				literals = nullptr;
			}
		}
		translateALUGroup(slots, literals);
		std::fill(std::begin(slots), std::end(slots), nullptr);
		literalSlots = 0;
		groupOpen = false;
	}
	if (groupOpen)
		unsupported(); // clause ended in the middle of a group
}

void LatteTCGenIR::translateALUGroup(const LatteClauseInstruction_ALU_OP2* const slots[kNumALUSlots], const uint32* literals)
{
	auto operand = [](const LatteClauseInstruction_ALU_OP2* op, int index) -> OperandDesc {
		// the raw 9-bit selector: GPRs below 128, specials 248-255, constant file from 256
		if (index == 0)
			return { Raw(op).w0 & 0x1FF, op->getSrc0Chan(), op->isSrc0Neg(), op->isSrc0Rel() };
		return { (Raw(op).w0 >> 13) & 0x1FF, op->getSrc1Chan(), op->isSrc1Neg(), op->isSrc1Rel() };
	};

	// stores an op's result into its destination (after the group) and into PV/PS
	auto writeResult = [&](uint32 unit, const LatteClauseInstruction_ALU_OP2* op, IRReg result) {
		if (op->getDestClamp() || op->getOMod() != 0 || op->getDestRel())
			unsupported();
		if (op->getWriteMask())
			m_values.queueWrite(unit, MakeElement(op->getDestGpr(), op->getDestElem()), result);
		m_values.queuePrevious(unit, result);
	};

	for (uint32 unit = 0; unit < kNumALUSlots; unit++)
	{
		const LatteClauseInstruction_ALU_OP2* op = slots[unit];
		if (!op)
			continue;
		switch (op->getOP2Code())
		{
		case ALUOp::ADD:
		case ALUOp::MUL:
		case ALUOp::MUL_IEEE:
		{
			const IRReg a = readOperand(operand(op, 0), literals, DataType::F32);
			const IRReg b = readOperand(operand(op, 1), literals, DataType::F32);
			const IROp irOp = op->getOP2Code() == ALUOp::ADD ? IROp::ADD : IROp::MUL;
			writeResult(unit, op, m_builder->emit_RRR(irOp, DataType::F32, a, b));
			break;
		}
		case ALUOp::MOV:
		{
			// MOV copies bits; keep the type of the source
			const OperandDesc src = operand(op, 0);
			const DataType type = guessOperandType(src, literals);
			const IRReg value = readOperand(src, literals, type);
			IRReg result = m_builder->createReg(type);
			m_builder->emit_RR(IROp::MOV, result, value);
			writeResult(unit, op, result);
			break;
		}
		case ALUOp::DOT4:
		{
			// reduction over x, y, z and w; every vector unit receives the sum
			if (unit != 0 || !slots[1] || !slots[2] || !slots[3] ||
				slots[1]->getOP2Code() != ALUOp::DOT4 || slots[2]->getOP2Code() != ALUOp::DOT4 || slots[3]->getOP2Code() != ALUOp::DOT4)
			{
				unsupported();
				break;
			}
			IRReg sum{};
			for (uint32 lane = 0; lane < 4; lane++)
			{
				const IRReg a = readOperand(operand(slots[lane], 0), literals, DataType::F32);
				const IRReg b = readOperand(operand(slots[lane], 1), literals, DataType::F32);
				const IRReg product = m_builder->emit_RRR(IROp::MUL, DataType::F32, a, b);
				sum = lane == 0 ? product : m_builder->emit_RRR(IROp::ADD, DataType::F32, sum, product);
			}
			for (uint32 lane = 0; lane < 4; lane++)
				writeResult(lane, slots[lane], sum);
			unit = 3; // lanes y, z and w are done
			break;
		}
		default:
			unsupported();
			break;
		}
	}
	m_values.commitGroup();
}

/* exports */

void LatteTCGenIR::translateExport(const LatteCFInstruction_EXPORT_IMPORT& exp)
{
	using Type = LatteCFInstruction_EXPORT_IMPORT::EXPORT_TYPE;
	using CompSel = LatteCFInstruction_EXPORT_IMPORT::COMPSEL;
	const Type type = exp.getField_TYPE();
	if ((type != Type::POSITION && type != Type::PARAMETER) || exp.isEncodingBUF() || exp.getField_RW_REL())
	{
		unsupported();
		return;
	}
	const CompSel swizzle[4] = { exp.getSwizField_SEL_X(), exp.getSwizField_SEL_Y(), exp.getSwizField_SEL_Z(), exp.getSwizField_SEL_W() };
	// a burst exports consecutive GPRs to consecutive targets
	for (uint32 b = 0; b < exp.getField_BURST_COUNT(); b++)
	{
		const uint32 gpr = exp.getField_RW_GPR() + b;
		if (gpr >= kNumGPRs)
		{
			unsupported();
			return;
		}
		IRReg values[4];
		for (uint32 ch = 0; ch < 4; ch++)
		{
			switch (swizzle[ch])
			{
			case CompSel::X:
			case CompSel::Y:
			case CompSel::Z:
			case CompSel::W:
				values[ch] = readGPRAs(gpr, (uint32)swizzle[ch], DataType::F32);
				break;
			case CompSel::CONST_0F:
				values[ch] = m_builder->createConstF32(0.0f);
				break;
			case CompSel::CONST_1F:
				values[ch] = m_builder->createConstF32(1.0f);
				break;
			default:
				// masked channels would need a channel mask in the export location
				unsupported();
				values[ch] = m_builder->createConstF32(0.0f);
				break;
			}
		}
		ZpIR::ShaderSubset::ShaderExportLocation location;
		if (type == Type::POSITION)
			location.SetPosition();
		else
			location.SetOutputAttribute((uint16)(exp.getField_ARRAY_BASE() + b));
		m_builder->emit_EXPORT(location, std::span<IRReg>(values, 4));
	}
}
