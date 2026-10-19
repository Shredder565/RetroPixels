// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/Zir/Core/IR.h"
#include "util/Zir/Core/ZirUtility.h"
#include "util/Zir/Core/ZpIRPasses.h"
#include "util/Zir/EmitterGLSL/ZpIREmitGLSL.h"

namespace ZirEmitter
{
	using namespace ZpIR;
	using RA = ZirPass::RegisterAllocatorForGLSL;

	namespace
	{
		constexpr const char* kNewline = "\r\n";
		constexpr char kChannel[4] = { 'x', 'y', 'z', 'w' };
		// longer expressions are always stored in a variable
		constexpr size_t kMaxInlineLength = 100;

		const char* glslTypeName(DataType t)
		{
			switch (t)
			{
			case DataType::F32: return "float ";
			case DataType::S32: return "int ";
			case DataType::U32: return "uint ";
			default:
				cemu_assert_debug(false);
				return "";
			}
		}

		const char* bitcastFunction(DataType from, DataType to)
		{
			if (to == DataType::F32 && from == DataType::U32)
				return "uintBitsToFloat(";
			if (to == DataType::F32 && from == DataType::S32)
				return "intBitsToFloat(";
			if (from == DataType::F32 && to == DataType::U32)
				return "floatBitsToUint(";
			if (from == DataType::F32 && to == DataType::S32)
				return "floatBitsToInt(";
			assert_dbg();
			return "";
		}
	}

	void GLSL::Emit(ZpIRFunction* irFunction, StringBuf* output)
	{
		m_out = output;
		cemu_assert_debug(irFunction->m_entryBlocks.size() == 1);
		cemu_assert_debug(irFunction->m_basicBlocks.size() == 1); // multiple blocks are not supported yet
		m_out->add("void main()");
		m_out->add(kNewline);
		m_out->add("{");
		m_out->add(kNewline);
		emitBlock(*irFunction->m_entryBlocks[0]);
		m_out->add("}");
		m_out->add(kNewline);
	}

	void GLSL::emitBlock(ZpIRBasicBlock& block)
	{
		m_block = &block;
		m_readCount.assign(block.m_regs.size(), 0);
		m_pending.clear();
		m_pending.resize(block.m_regs.size());

		// count reads so that single-use values can be substituted
		for (IR::__InsBase* ins = block.m_instructionFirst; ins; ins = ins->next)
		{
			ZpIRCmdUtil::forEachAccessedReg(block, ins,
				[this](IRReg r) {
					uint8& n = m_readCount.at(r);
					if (n != 0xFF)
						n++;
				},
				[](IRReg) {});
		}

		for (IR::__InsBase* ins = block.m_instructionFirst; ins; ins = ins->next)
		{
			if (auto rr = IR::InsRR::getIfForm(ins))
				emit(rr);
			else if (auto rrr = IR::InsRRR::getIfForm(ins))
				emit(rrr);
			else if (auto imp = IR::InsIMPORT::getIfForm(ins))
				emit(imp);
			else if (auto exp = IR::InsEXPORT::getIfForm(ins))
				emit(exp);
			else
				assert_dbg();
		}
	}

	void GLSL::emit(IR::InsRR* ins)
	{
		const DataType srcType = m_block->getRegType(ins->rB);
		const DataType dstType = m_block->getRegType(ins->rA);
		const std::string src = operand(ins->rB);
		std::string expr;
		bool allowInline = true;
		switch (ins->opcode)
		{
		case IR::OpCode::MOV:
			expr = src;
			break;
		case IR::OpCode::BITCAST:
			expr = std::string(bitcastFunction(srcType, dstType)) + src + ")";
			break;
		case IR::OpCode::SWAP_ENDIAN:
			cemu_assert_debug(srcType == dstType);
			if (srcType != DataType::U32)
			{
				assert_dbg();
				break;
			}
			// the operand appears four times, so the result always gets its own variable
			expr = fmt::format("((({0})>>24)|((({0})>>8)&0xFF00)|((({0})<<8)&0xFF0000)|(({0})<<24))", src);
			allowInline = false;
			break;
		case IR::OpCode::CONVERT_FLOAT_TO_INT:
			cemu_assert_debug(srcType == DataType::F32);
			expr = std::string(dstType == DataType::U32 ? "uint(" : "int(") + src + ")";
			break;
		case IR::OpCode::CONVERT_INT_TO_FLOAT:
			cemu_assert_debug(srcType == DataType::S32 || srcType == DataType::U32);
			cemu_assert_debug(dstType == DataType::F32);
			expr = "float(" + src + ")";
			break;
		default:
			assert_dbg();
			break;
		}
		define(ins->rA, std::move(expr), allowInline);
	}

	void GLSL::emit(IR::InsRRR* ins)
	{
		const char* op = nullptr;
		switch (ins->opcode)
		{
		case IR::OpCode::ADD: op = " + "; break;
		case IR::OpCode::MUL: op = " * "; break;
		case IR::OpCode::DIV: op = " / "; break;
		default:
			assert_dbg();
			op = " ? ";
			break;
		}
		define(ins->rA, operand(ins->rB) + op + operand(ins->rC));
	}

	void GLSL::emit(IR::InsIMPORT* ins)
	{
		ShaderSubset::ShaderImportLocation loc(ins->importSymbol);
		if (loc.IsUniformRegister())
		{
			uint16 index;
			loc.GetUniformRegister(index);
			define(ins->regArray[0], fmt::format("uf_remappedVS[{}].{}", index / 4, kChannel[index & 3]));
		}
		else if (loc.IsVertexAttribute())
		{
			uint16 attribute, channel;
			loc.GetVertexAttribute(attribute, channel);
			cemu_assert_debug(ins->count == 1);
			cemu_assert_debug(isRegVar(ins->regArray[0]));
			cemu_assert_debug(channel < 4);
			cemu_assert_debug(m_block->getRegType(ins->regArray[0]) == DataType::U32);
			define(ins->regArray[0], fmt::format("attrDataSem{}.{}", attribute, kChannel[channel & 3]));
		}
		else
		{
			cemu_assert_debug(false);
		}
	}

	void GLSL::emit(IR::InsEXPORT* ins)
	{
		ShaderSubset::ShaderExportLocation loc(ins->exportSymbol);
		std::string line;
		const char* close = nullptr;
		if (loc.IsPosition())
		{
			line = "SET_POSITION(vec4(";
			close = "));";
		}
		else if (loc.IsOutputAttribute())
		{
			uint16 attribute;
			loc.GetOutputAttribute(attribute);
			line = fmt::format("passParameterSem{} = vec4(", attribute);
			close = ");";
		}
		else
		{
			assert_dbg();
			return;
		}
		cemu_assert_debug(ins->count == 4);
		for (uint16 i = 0; i < ins->count; i++)
		{
			if (i != 0)
				line += ", ";
			line += operand(ins->regArray[i]);
		}
		line += close;
		line += kNewline;
		m_out->add(line);
	}

	void GLSL::define(IRReg r, std::string expr, bool allowInline)
	{
		cemu_assert_debug(isRegVar(r));
		const uint16 index = getRegIndex(r);
		if (allowInline && expr.size() <= kMaxInlineLength && m_readCount[index] <= 1)
		{
			m_pending[index] = std::move(expr);
			return;
		}
		m_out->add(glslTypeName(m_block->getRegType(r)));
		m_out->add(variableName(r));
		m_out->add(" = ");
		m_out->add(expr);
		m_out->add(";");
		m_out->add(kNewline);
	}

	std::string GLSL::operand(IRReg r) const
	{
		if (isConstVar(r))
		{
			const IRRegConstDef* c = m_block->getConstant(r);
			switch (c->type)
			{
			case DataType::U32: return fmt::format("{}", c->value_u32);
			case DataType::S32: return fmt::format("{}", c->value_s32);
			case DataType::F32: return fmt::format("{}", c->value_f32);
			default:
				assert_dbg();
				return {};
			}
		}
		const auto& pending = m_pending[getRegIndex(r)];
		if (pending)
			return *pending;
		return variableName(r);
	}

	std::string GLSL::variableName(IRReg r) const
	{
		const IRRegDef& def = m_block->m_regs[getRegIndex(r)];
		cemu_assert_debug(def.hasAssignedPhysicalRegister());
		const ZpIRPhysicalReg phys = def.physicalRegister;
		char suffix = 'x';
		if (RA::IsPhysRegTypeF32(phys))
			suffix = 'f';
		else if (RA::IsPhysRegTypeS32(phys))
			suffix = 'i';
		else if (RA::IsPhysRegTypeU32(phys))
			suffix = 'u';
		else
			cemu_assert_debug(false);
		return fmt::format("r{}{}", RA::GetPhysRegIndex(phys), suffix);
	}
}
