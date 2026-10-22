// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "util/Zir/Core/IR.h"

namespace ZpIR
{
	struct ZpIRCmdUtil
	{
		// Calls funcRegRead(reg) for each virtual register the instruction reads
		// (constants are skipped) and then funcRegWrite(reg) for each one it writes.
		template<typename TFuncRegRead, typename TFuncRegWrite>
		static void forEachAccessedReg(ZpIRBasicBlock& block, IR::__InsBase* instruction, TFuncRegRead funcRegRead, TFuncRegWrite funcRegWrite)
		{
			(void)block;
			auto readIfReg = [&](IRReg r) {
				if (isRegVar(r))
					funcRegRead(r);
			};
			auto write = [&](IRReg r) {
				cemu_assert_debug(isRegVar(r));
				funcRegWrite(r);
			};
			if (auto rr = IR::InsRR::getIfForm(instruction))
			{
				if (!isUnaryOp(rr->opcode))
				{
					cemu_assert_unimplemented();
					return;
				}
				readIfReg(rr->rB);
				write(rr->rA);
			}
			else if (auto rrr = IR::InsRRR::getIfForm(instruction))
			{
				if (!isBinaryOp(rrr->opcode))
				{
					cemu_assert_unimplemented();
					return;
				}
				readIfReg(rrr->rB);
				readIfReg(rrr->rC);
				write(rrr->rA);
			}
			else if (auto imp = IR::InsIMPORT::getIfForm(instruction))
			{
				for (uint16 i = 0; i < imp->count; i++)
					write(imp->regArray[i]);
			}
			else if (auto exp = IR::InsEXPORT::getIfForm(instruction))
			{
				for (uint16 i = 0; i < exp->count; i++)
					readIfReg(exp->regArray[i]);
			}
			else
			{
				cemu_assert_unimplemented();
			}
		}

		// not implemented yet
		static void replaceRegisters(IR::__InsBase& ins, std::unordered_map<IRReg, IRReg>& translationTable)
		{
			(void)ins;
			(void)translationTable;
			cemu_assert_unimplemented();
		}

	private:
		static bool isUnaryOp(IR::OpCode op)
		{
			switch (op)
			{
			case IR::OpCode::MOV:
			case IR::OpCode::BITCAST:
			case IR::OpCode::SWAP_ENDIAN:
			case IR::OpCode::CONVERT_FLOAT_TO_INT:
			case IR::OpCode::CONVERT_INT_TO_FLOAT:
				return true;
			default:
				return false;
			}
		}

		static bool isBinaryOp(IR::OpCode op)
		{
			switch (op)
			{
			case IR::OpCode::ADD:
			case IR::OpCode::SUB:
			case IR::OpCode::MUL:
			case IR::OpCode::DIV:
				return true;
			default:
				return false;
			}
		}
	};
}
