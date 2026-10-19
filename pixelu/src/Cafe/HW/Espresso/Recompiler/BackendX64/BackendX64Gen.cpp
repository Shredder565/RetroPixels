// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// x86-64 backend: integer instructions, integer memory accesses, home slot
// accesses and control flow.

#include "Cafe/HW/Espresso/PPCState.h"
#include "BackendX64.h"
#include "X64Emit.hpp"

using namespace x64;

namespace X64Backend
{
	namespace
	{
		bool IsCommutative(IMLOp op)
		{
			return op == IMLOp::ADD || op == IMLOp::AND || op == IMLOp::OR || op == IMLOp::XOR || op == IMLOp::MUL;
		}

		AluOp ToAlu(IMLOp op)
		{
			switch (op)
			{
			case IMLOp::ADD: return ALU_ADD;
			case IMLOp::SUB: return ALU_SUB;
			case IMLOp::AND: return ALU_AND;
			case IMLOp::OR: return ALU_OR;
			case IMLOp::XOR: return ALU_XOR;
			default: break;
			}
			cemu_assert_debug(false);
			return ALU_ADD;
		}
	}

	bool CodeGenerator::EmitInt(const IMLInstruction& inst)
	{
		auto& e = m_e;
		switch (inst.op)
		{
		case IMLOp::MOV:
			MoveGpr(Gpr(inst.r[0]), Gpr(inst.r[1]));
			return true;
		case IMLOp::MOVI:
			e.MOV_ri32(Gpr(inst.r[0]), (uint32)inst.imm);
			return true;
		case IMLOp::ADD:
		case IMLOp::SUB:
		case IMLOp::AND:
		case IMLOp::OR:
		case IMLOp::XOR:
		case IMLOp::MUL:
		{
			uint8 d = Gpr(inst.r[0]);
			uint8 a = Gpr(inst.r[1]);
			bool isMul = inst.op == IMLOp::MUL;
			if (inst.HasImm())
			{
				if (isMul)
				{
					e.IMUL_rri(d, a, (uint32)inst.imm);
					return true;
				}
				MoveGpr(d, a);
				e.ALU_ri(ToAlu(inst.op), d, (uint32)inst.imm);
				return true;
			}
			uint8 b = Gpr(inst.r[2]);
			auto op2 = [&](uint8 dst, uint8 src)
			{
				if (isMul)
					e.IMUL_rr(dst, src);
				else
					e.ALU_rr(ToAlu(inst.op), dst, src);
			};
			if (d == a)
				op2(d, b);
			else if (d == b)
			{
				if (IsCommutative(inst.op))
					op2(d, a);
				else
				{
					e.MOV_rr(RAX, a);
					op2(RAX, b);
					e.MOV_rr(d, RAX);
				}
			}
			else
			{
				e.MOV_rr(d, a);
				op2(d, b);
			}
			return true;
		}
		case IMLOp::ANDC:
		case IMLOp::ORC:
			e.MOV_rr(RAX, Gpr(inst.r[2]));
			e.NOT(RAX);
			e.ALU_rr(inst.op == IMLOp::ANDC ? ALU_AND : ALU_OR, RAX, Gpr(inst.r[1]));
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		case IMLOp::NOT:
		case IMLOp::NEG:
		{
			uint8 d = Gpr(inst.r[0]);
			MoveGpr(d, Gpr(inst.r[1]));
			if (inst.op == IMLOp::NOT)
				e.NOT(d);
			else
				e.NEG(d);
			return true;
		}
		case IMLOp::MULHS:
		case IMLOp::MULHU:
			// 64-bit product of the extended operands, high half
			if (inst.op == IMLOp::MULHS)
			{
				e.MOVSXD_rr(RAX, Gpr(inst.r[1]));
				e.MOVSXD_rr(RCX, Gpr(inst.r[2]));
			}
			else
			{
				e.MOV_rr(RAX, Gpr(inst.r[1]));
				e.MOV_rr(RCX, Gpr(inst.r[2]));
			}
			e.IMUL_rr(RAX, RCX, true);
			e.SHIFT_ri(SH_SHR, RAX, 32, true);
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		case IMLOp::DIVS:
		{
			// x / 0 = (x < 0 ? -1 : 0), 0x80000000 / -1 = -1
			e.MOV_rr(RAX, Gpr(inst.r[1]));
			e.MOV_rr(RCX, Gpr(inst.r[2]));
			e.TEST_rr(RCX, RCX);
			size_t toZero = e.Jcc_rel32(CC_E);
			e.ALU_ri(ALU_CMP, RCX, 0xFFFFFFFF);
			size_t toDiv1 = e.Jcc_rel32(CC_NE);
			e.ALU_ri(ALU_CMP, RAX, 0x80000000);
			size_t toDiv2 = e.Jcc_rel32(CC_NE);
			e.MOV_ri32(RAX, 0xFFFFFFFF);
			size_t toDone1 = e.JMP_rel32();
			e.BindRel32(toDiv1, e.Size());
			e.BindRel32(toDiv2, e.Size());
			e.CDQ();
			e.IDIV(RCX);
			size_t toDone2 = e.JMP_rel32();
			e.BindRel32(toZero, e.Size());
			e.SHIFT_ri(SH_SAR, RAX, 31);
			e.BindRel32(toDone1, e.Size());
			e.BindRel32(toDone2, e.Size());
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		}
		case IMLOp::DIVU:
		{
			e.MOV_rr(RAX, Gpr(inst.r[1]));
			e.MOV_rr(RCX, Gpr(inst.r[2]));
			e.TEST_rr(RCX, RCX);
			size_t toZero = e.Jcc_rel32(CC_E);
			e.ALU_rr(ALU_XOR, RDX, RDX);
			e.DIV(RCX);
			size_t toDone = e.JMP_rel32();
			e.BindRel32(toZero, e.Size());
			e.ALU_rr(ALU_XOR, RAX, RAX);
			e.BindRel32(toDone, e.Size());
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		}
		case IMLOp::SHL:
		case IMLOp::SHR:
		case IMLOp::SAR:
		{
			uint8 d = Gpr(inst.r[0]);
			MoveGpr(d, Gpr(inst.r[1]));
			uint8 amount = (uint8)(inst.imm & 31);
			if (amount)
				e.SHIFT_ri(inst.op == IMLOp::SHL ? SH_SHL : inst.op == IMLOp::SHR ? SH_SHR : SH_SAR, d, amount);
			return true;
		}
		case IMLOp::ROTL:
		{
			uint8 d = Gpr(inst.r[0]);
			if (inst.HasImm())
			{
				MoveGpr(d, Gpr(inst.r[1]));
				if (inst.imm & 31)
					e.SHIFT_ri(SH_ROL, d, (uint8)(inst.imm & 31));
				return true;
			}
			e.MOV_rr(RCX, Gpr(inst.r[2]));
			e.MOV_rr(RAX, Gpr(inst.r[1]));
			e.SHIFT_rcl(SH_ROL, RAX);
			e.MOV_rr(d, RAX);
			return true;
		}
		case IMLOp::SLW:
		case IMLOp::SRW:
			// 6-bit amount: 32 or more clears the result
			e.MOV_rr(RCX, Gpr(inst.r[2]));
			e.MOV_rr(RAX, Gpr(inst.r[1]));
			e.SHIFT_rcl(inst.op == IMLOp::SLW ? SH_SHL : SH_SHR, RAX);
			e.ALU_rr(ALU_XOR, RDX, RDX);
			e.TEST_ri(RCX, 32);
			e.CMOV_rr(CC_NE, RAX, RDX);
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		case IMLOp::SRAW:
		{
			uint8 d = Gpr(inst.r[0]);
			uint8 ca = Gpr(inst.r[1]);
			if (inst.HasImm())
			{
				uint32 amount = (uint32)inst.imm & 31;
				e.MOV_rr(RAX, Gpr(inst.r[2]));
				if (amount == 0)
				{
					e.MOV_rr(d, RAX);
					e.MOV_ri32(ca, 0);
					return true;
				}
				// CA = negative and one bits shifted out
				e.MOV_rr(RDX, RAX);
				e.SHIFT_ri(SH_SAR, RDX, 31);
				e.ALU_rr(ALU_AND, RDX, RAX);
				e.TEST_ri(RDX, (1u << amount) - 1);
				e.SETcc(CC_NE, RCX);
				e.MOVZX_r8(RCX, RCX);
				e.SHIFT_ri(SH_SAR, RAX, (uint8)amount);
				e.MOV_rr(d, RAX);
				e.MOV_rr(ca, RCX);
				return true;
			}
			// shift the sign extended 64-bit value by up to 63
			e.MOV_rr(RCX, Gpr(inst.r[3]));
			e.ALU_ri(ALU_AND, RCX, 63);
			e.MOVSXD_rr(RDX, Gpr(inst.r[2]));
			e.MOV_rr(RAX, RDX, true);
			e.SHIFT_rcl(SH_SAR, RAX, true);
			e.MOV_rr(d, RAX);
			// bits were lost if shifting back does not give the original value
			e.SHIFT_rcl(SH_SHL, RAX, true);
			e.ALU_rr(ALU_CMP, RAX, RDX, true);
			e.SETcc(CC_NE, RAX);
			e.MOVZX_r8(RAX, RAX);
			e.SHIFT_ri(SH_SHR, RDX, 63, true);
			e.ALU_rr(ALU_AND, RAX, RDX);
			e.MOV_rr(ca, RAX);
			return true;
		}
		case IMLOp::EXTS8:
			e.MOVSX_r8(Gpr(inst.r[0]), Gpr(inst.r[1]));
			return true;
		case IMLOp::EXTS16:
			e.MOVSX_r16(Gpr(inst.r[0]), Gpr(inst.r[1]));
			return true;
		case IMLOp::CNTLZ:
			EmitCountLeadingZeros(Gpr(inst.r[0]), Gpr(inst.r[1]));
			return true;
		case IMLOp::BSWAP32:
		{
			uint8 d = Gpr(inst.r[0]);
			MoveGpr(d, Gpr(inst.r[1]));
			e.BSWAP(d);
			return true;
		}
		case IMLOp::BSWAP16:
			e.MOVZX_r16(RAX, Gpr(inst.r[1]));
			e.ROL16_ri(RAX, 8);
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		case IMLOp::ADDC:
		case IMLOp::ADDE:
		{
			e.MOV_rr(RAX, Gpr(inst.r[2]));
			if (inst.flags & IMLF_INV_A)
				e.NOT(RAX);
			if (inst.op == IMLOp::ADDE)
			{
				// CF = carry in
				e.MOV_rr(RCX, Gpr(inst.r[1]));
				e.NEG(RCX);
			}
			AluOp op = inst.op == IMLOp::ADDE ? ALU_ADC : ALU_ADD;
			if (inst.HasImm())
				e.ALU_ri(op, RAX, (uint32)inst.imm);
			else
				e.ALU_rr(op, RAX, Gpr(inst.r[3]));
			e.SETcc(CC_B, RCX);
			e.MOVZX_r8(RCX, RCX);
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			e.MOV_rr(Gpr(inst.r[1]), RCX);
			return true;
		}
		case IMLOp::CMPSET:
			if (inst.HasImm())
				e.ALU_ri(ALU_CMP, Gpr(inst.r[1]), (uint32)inst.imm);
			else
				e.ALU_rr(ALU_CMP, Gpr(inst.r[1]), Gpr(inst.r[2]));
			e.SETcc(ToHostCond(inst.Cond()), RAX);
			e.MOVZX_r8(RAX, RAX);
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		case IMLOp::CMPCR:
		{
			bool isSigned = inst.Cond() == IMLCond::SLT;
			if (inst.HasImm())
				e.ALU_ri(ALU_CMP, Gpr(inst.r[3]), (uint32)inst.imm);
			else
				e.ALU_rr(ALU_CMP, Gpr(inst.r[3]), Gpr(inst.r[4]));
			e.SETcc(isSigned ? CC_L : CC_B, RAX);
			e.SETcc(isSigned ? CC_G : CC_A, RCX);
			e.SETcc(CC_E, RDX);
			e.MOVZX_r8(Gpr(inst.r[0]), RAX);
			e.MOVZX_r8(Gpr(inst.r[1]), RCX);
			e.MOVZX_r8(Gpr(inst.r[2]), RDX);
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}

	bool CodeGenerator::EmitMemory(const IMLInstruction& inst)
	{
		auto& e = m_e;
		switch (inst.op)
		{
		case IMLOp::LOAD:
		{
			uint8 d = Gpr(inst.r[0]);
			Mem m = GuestAddress(inst);
			bool reverse = (inst.flags & IMLF_BYTEREV) != 0;
			if (inst.size == 4)
			{
				if (reverse)
					e.MOV_rm(d, m);
				else
					EmitLoadSwapped32(d, m);
			}
			else if (inst.size == 2)
			{
				e.MOVZX_m16(d, m);
				if (!reverse)
					e.ROL16_ri(d, 8);
				if (inst.flags & IMLF_SIGNEXT)
					e.MOVSX_r16(d, d);
			}
			else
			{
				if (inst.flags & IMLF_SIGNEXT)
					e.MOVSX_m8(d, m);
				else
					e.MOVZX_m8(d, m);
			}
			return true;
		}
		case IMLOp::STORE:
		{
			e.MOV_rr(RCX, Gpr(inst.r[0]));
			Mem m = GuestAddress(inst);
			bool reverse = (inst.flags & IMLF_BYTEREV) != 0;
			if (inst.size == 4)
			{
				if (!reverse)
					e.BSWAP(RCX);
				e.MOV_mr(m, RCX);
			}
			else if (inst.size == 2)
			{
				if (!reverse)
					e.ROL16_ri(RCX, 8);
				e.MOV_mr16(m, RCX);
			}
			else
				e.MOV_mr8(m, RCX);
			return true;
		}
		case IMLOp::ZEROLINE:
			GuestAddressToEAX(inst);
			e.ALU_ri(ALU_AND, RAX, ~31u);
			e.XORPD(kXmmScratch0, kXmmScratch0);
			e.MOVUPS_mr(mem(kRegMemBase, RAX, 1, 0), kXmmScratch0);
			e.MOVUPS_mr(mem(kRegMemBase, RAX, 1, 16), kXmmScratch0);
			return true;
		case IMLOp::CMPXCHG:
		{
			// r0 = ok, r1 = ea, r2 = expected, r3 = new value, r4 = attempt (0: fail without access)
			e.MOV_rr(RDX, Gpr(inst.r[1]));
			e.MOV_rr(RAX, Gpr(inst.r[2]));
			e.BSWAP(RAX);
			e.MOV_rr(RCX, Gpr(inst.r[3]));
			e.BSWAP(RCX);
			e.TEST_rr(Gpr(inst.r[4]), Gpr(inst.r[4]));
			size_t skip = e.Jcc_rel32(CC_E);
			e.LOCK_CMPXCHG_mr(mem(kRegMemBase, RDX, 1, 0), RCX);
			e.SETcc(CC_E, RAX);
			e.MOVZX_r8(RAX, RAX);
			size_t done = e.JMP_rel32();
			e.BindRel32(skip, e.Size());
			e.ALU_rr(ALU_XOR, RAX, RAX);
			e.BindRel32(done, e.Size());
			e.MOV_rr(Gpr(inst.r[0]), RAX);
			return true;
		}
		default:
			return false;
		}
	}

	bool CodeGenerator::EmitHome(const IMLInstruction& inst)
	{
		auto& e = m_e;
		IMLReg owner = (IMLReg)inst.imm;
		const IMLVRegInfo& info = m_func.vregs[owner];
		Mem m;
		if (IMLGuest::IsGuest(owner))
			m = CPU(info.home);
		else
		{
			cemu_assert_debug(info.home >= 0 && info.home < (sint32)kMaxSpillSlots);
			m = Stack(kFrameSpillArea + info.home * 16);
		}
		bool isFloat = m_func.KindOf(inst.r[0]) == IMLRegKind::Float;
		bool isByte = IMLGuest::IsGuest(owner) && info.homeSize == 1;
		if (inst.op == IMLOp::HOMELOAD)
		{
			if (isFloat)
				e.MOVUPD_rm(Fpr(inst.r[0]), m);
			else if (isByte)
				e.MOVZX_m8(Gpr(inst.r[0]), m);
			else
				e.MOV_rm(Gpr(inst.r[0]), m);
		}
		else
		{
			if (isFloat)
				e.MOVUPD_mr(m, Fpr(inst.r[0]));
			else if (isByte)
				e.MOV_mr8(m, Gpr(inst.r[0]));
			else
				e.MOV_mr(m, Gpr(inst.r[0]));
		}
		return true;
	}

	bool CodeGenerator::EmitControl(const IMLInstruction& inst, IMLSegment* block, IMLSegment* layoutNext)
	{
		auto& e = m_e;
		auto conditional = [&](Cond cc)
		{
			if (block->taken == layoutNext)
				JccToBlock(InvertHostCond(cc), block->next);
			else
			{
				JccToBlock(cc, block->taken);
				if (block->next != layoutNext)
					JumpToBlock(block->next);
			}
		};
		auto leaveOrTable = [&]()
		{
			// RAX = guest address. Only the code area has jump table entries
			e.ALU_ri(ALU_CMP, RAX, PPC_REC_CODE_AREA_END);
			size_t outside = e.Jcc_rel32(CC_AE);
			EmitJumpViaTableEAX();
			e.BindRel32(outside, e.Size());
			EmitLeaveEAX();
		};
		switch (inst.op)
		{
		case IMLOp::JUMP:
			if (block->taken != layoutNext)
				JumpToBlock(block->taken);
			return true;
		case IMLOp::BRANCH:
		{
			uint8 r = Gpr(inst.r[0]);
			e.TEST_rr(r, r);
			conditional(inst.Cond() == IMLCond::NE ? CC_NE : CC_E);
			return true;
		}
		case IMLOp::BRANCHCMP:
			if (inst.HasImm())
				e.ALU_ri(ALU_CMP, Gpr(inst.r[0]), (uint32)inst.imm);
			else
				e.ALU_rr(ALU_CMP, Gpr(inst.r[0]), Gpr(inst.r[1]));
			conditional(ToHostCond(inst.Cond()));
			return true;
		case IMLOp::CYCLECHECK:
			e.ALU_mi(ALU_CMP, CPU(offsetof(PPCInterpreter_t, remainingCycles)), 0);
			conditional(CC_L);
			return true;
		case IMLOp::EXITB:
			e.MOV_ri32(RAX, inst.addr);
			if (inst.addr >= PPC_REC_CODE_AREA_END)
				EmitLeaveEAX();
			else
				EmitJumpViaTableEAX();
			return true;
		case IMLOp::EXITBR:
			e.MOV_rr(RAX, Gpr(inst.r[0]));
			leaveOrTable();
			return true;
		case IMLOp::EXITLEAVE:
			e.MOV_ri32(RAX, inst.addr);
			EmitLeaveEAX();
			return true;
		case IMLOp::HLE:
		{
			e.MOV_mi32(CPU(offsetof(PPCInterpreter_t, instructionPointer)), inst.addr);
			CallHelper2((const void*)&Helper_HLE, (uint32)inst.imm, 0);
			e.MOV_rm(RAX, CPU(offsetof(PPCInterpreter_t, instructionPointer)));
			e.ALU_mi(ALU_CMP, CPU(offsetof(PPCInterpreter_t, remainingCycles)), 0);
			size_t timeUp = e.Jcc_rel32(CC_L);
			leaveOrTable();
			e.BindRel32(timeUp, e.Size());
			EmitLeaveEAX();
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}
}
