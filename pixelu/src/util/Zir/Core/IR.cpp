// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZpIRDebug.h"

namespace ZpIR
{
	namespace
	{
		const char* opcodeText(IR::OpCode op)
		{
			switch (op)
			{
			case IR::OpCode::MOV: return "MOV";
			case IR::OpCode::ADD: return "ADD";
			case IR::OpCode::SUB: return "SUB";
			case IR::OpCode::MUL: return "MUL";
			case IR::OpCode::DIV: return "DIV";
			case IR::OpCode::BITCAST: return "BITCAST";
			case IR::OpCode::SWAP_ENDIAN: return "SWAP_ENDIAN";
			case IR::OpCode::CONVERT_INT_TO_FLOAT: return "CONV_I2F";
			case IR::OpCode::CONVERT_FLOAT_TO_INT: return "CONV_F2I";
			case IR::OpCode::IMPORT_SINGLE: return "IMPORT_S";
			case IR::OpCode::IMPORT: return "IMPORT";
			case IR::OpCode::EXPORT: return "EXPORT";
			default: return "UKN";
			}
		}

		// register name prefix by type
		const char* regPrefix(DataType t)
		{
			switch (t)
			{
			case DataType::U64: return "uq";
			case DataType::U32: return "ud";
			case DataType::U16: return "uw";
			case DataType::U8: return "uc";
			case DataType::S64: return "sq";
			case DataType::S32: return "sd";
			case DataType::S16: return "sw";
			case DataType::S8: return "sc";
			case DataType::F32: return "fv";
			case DataType::POINTER: return "ptr";
			default: return "r";
			}
		}

		std::string constantText(const IRRegConstDef& c)
		{
			switch (c.type)
			{
			case DataType::POINTER: return fmt::format("ptr:{}", c.value_ptr);
			case DataType::U64: return c.value_u64 >= 0x1000 ? fmt::format("u64:0x{:x}", c.value_u64) : fmt::format("u64:{}", c.value_u64);
			case DataType::U32: return fmt::format("u32:{}", c.value_u32);
			case DataType::S32: return fmt::format("s32:{}", c.value_s32);
			case DataType::F32: return fmt::format("f32:{}", c.value_f32);
			default: return "ukn_const_type";
			}
		}
	}

	std::string DebugPrinter::getRegisterName(ZpIRBasicBlock* block, IRReg r)
	{
		if (isConstVar(r))
		{
			const IRRegConstDef* c = block->getConstant(r);
			return c ? constantText(*c) : std::string("ukn_const");
		}
		const IRRegDef& def = block->m_regs[r];
		if (m_showPhysicalRegisters)
		{
			if (!def.hasAssignedPhysicalRegister())
				return "UNASSIGNED";
			if (m_physicalRegName)
				return m_physicalRegName(block, def.physicalRegister);
			return fmt::format("phys{}", def.physicalRegister);
		}
		if (m_virtualRegName)
			return m_virtualRegName(block, r);
		std::string name = fmt::format("{}{}", regPrefix(def.type), (uint32)r);
		if (def.elementCount != 1)
			name += fmt::format("x{}", def.elementCount);
		return name;
	}

	std::string DebugPrinter::getInstructionHRF(ZpIRBasicBlock* block, IR::__InsBase* cmd)
	{
		std::string operands;
		auto addOperand = [&](IRReg r) {
			if (!operands.empty())
				operands += ", ";
			operands += getRegisterName(block, r);
		};
		if (auto rr = IR::InsRR::getIfForm(cmd))
		{
			addOperand(rr->rA);
			addOperand(rr->rB);
		}
		else if (auto rrr = IR::InsRRR::getIfForm(cmd))
		{
			addOperand(rrr->rA);
			addOperand(rrr->rB);
			addOperand(rrr->rC);
		}
		else if (auto exp = IR::InsEXPORT::getIfForm(cmd))
		{
			for (uint16 i = 0; i < exp->count; i++)
				addOperand(exp->regArray[i]);
			operands += fmt::format(", loc: {}", ShaderSubset::ShaderExportLocation(exp->exportSymbol).GetDebugName());
		}
		else if (auto imp = IR::InsIMPORT::getIfForm(cmd))
		{
			for (uint16 i = 0; i < imp->count; i++)
				addOperand(imp->regArray[i]);
			operands += fmt::format(", loc: {}", ShaderSubset::ShaderImportLocation(imp->importSymbol).GetDebugName());
		}
		else
		{
			return "<unknown instruction form>";
		}
		return fmt::format("{:<10} {}", opcodeText(cmd->opcode), operands);
	}

	void DebugPrinter::debugPrintBlock(ZpIRBasicBlock* block)
	{
		std::string out = fmt::format("IRBasicBlock {}\n", (const void*)block);
		out += "Imports:\n";
		for (auto& imp : block->m_imports)
			out += fmt::format("   reg: {} sym:0x{:x}\n", getRegisterName(block, imp.reg), imp.name);
		out += "Exports:\n";
		for (auto& exp : block->m_exports)
			out += fmt::format("   reg: {} sym:0x{:x}\n", getRegisterName(block, exp.reg), exp.name);
		out += "Assembly:\n";
		uint32 index = 0;
		for (IR::__InsBase* ins = block->m_instructionFirst; ins; ins = ins->next)
			out += fmt::format("{:04x} {}\n", index++, getInstructionHRF(block, ins));
		fputs(out.c_str(), stdout);
	}

	void DebugPrinter::debugPrint(ZpIRFunction* irFunction)
	{
		fputs("--- Print IR function assembly ---\n", stdout);
		for (ZpIRBasicBlock* block : irFunction->m_basicBlocks)
		{
			debugPrintBlock(block);
			fputs("\n", stdout);
		}
	}
}
