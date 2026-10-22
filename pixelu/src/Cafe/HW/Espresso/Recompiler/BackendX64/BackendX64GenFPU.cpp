// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// x86-64 backend: floating point and paired single arithmetic.
//
// Every operation is computed in the same way as the interpreter computes it
// with host doubles: double precision results in ps0, single precision
// results rounded to float and widened back, the frC operand of single
// precision multiplies cut to 25 significant bits.

#include "Cafe/HW/Espresso/PPCState.h"
#include "BackendX64.h"
#include "X64Emit.hpp"

using namespace x64;

namespace X64Backend
{
	namespace
	{
		constexpr uint8 kCmpLE = 2; // CMPSD/CMPPD predicate "less or equal" (false for NaN)
	}

	bool CodeGenerator::EmitFloat(const IMLInstruction& inst)
	{
		auto& e = m_e;
		const Xmm s0 = kXmmScratch0;
		const Xmm s1 = kXmmScratch1;

		// rounds both lanes of x to single precision and back
		auto roundPairToSingle = [&](Xmm x)
		{
			e.CVTPD2PS(x, x);
			e.CVTPS2PD(x, x);
		};
		// rounds the low lane to single precision and copies it to both lanes
		auto roundLowToSingleBoth = [&](Xmm x)
		{
			e.CVTSD2SS(x, x);
			e.CVTSS2SD(x, x);
			e.UNPCKLPD(x, x);
		};
		auto negate = [&](Xmm x)
		{
			LoadConstantTable();
			e.XORPD_m(x, mem(RAX, offsetof(VectorConstants, signBits)));
		};
		auto scalarArith = [&](IMLArith op, Xmm d, Xmm src)
		{
			switch (op)
			{
			case IMLArith::ADD: e.ADDSD(d, src); break;
			case IMLArith::SUB: e.SUBSD(d, src); break;
			case IMLArith::MUL: e.MULSD(d, src); break;
			case IMLArith::DIV: e.DIVSD(d, src); break;
			}
		};
		auto pairArith = [&](IMLArith op, Xmm d, Xmm src)
		{
			switch (op)
			{
			case IMLArith::ADD: e.ADDPD(d, src); break;
			case IMLArith::SUB: e.SUBPD(d, src); break;
			case IMLArith::MUL: e.MULPD(d, src); break;
			case IMLArith::DIV: e.DIVPD(d, src); break;
			}
		};

		switch (inst.op)
		{
		case IMLOp::FMOV:
			MoveXmm(Fpr(inst.r[0]), Fpr(inst.r[1]));
			return true;
		case IMLOp::FMOVLO:
			if (Fpr(inst.r[0]) != Fpr(inst.r[1]))
				e.MOVSD_rr(Fpr(inst.r[0]), Fpr(inst.r[1]));
			return true;
		case IMLOp::FSIGN:
		case IMLOp::PSIGN:
		{
			e.MOVAPD(s0, Fpr(inst.r[1]));
			LoadConstantTable();
			switch ((IMLSign)inst.sub)
			{
			case IMLSign::NEG: e.XORPD_m(s0, mem(RAX, offsetof(VectorConstants, signBits))); break;
			case IMLSign::ABS: e.ANDPD_m(s0, mem(RAX, offsetof(VectorConstants, absMask))); break;
			case IMLSign::NABS: e.ORPD_m(s0, mem(RAX, offsetof(VectorConstants, signBits))); break;
			}
			if (inst.op == IMLOp::FSIGN)
				e.MOVSD_rr(Fpr(inst.r[0]), s0);
			else
				e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::PMERGE:
			// ps0 from a (lane sub>>1), ps1 from b (lane sub&1)
			e.MOVAPD(s0, Fpr(inst.r[1]));
			e.SHUFPD(s0, Fpr(inst.r[2]), (uint8)((inst.sub >> 1) | ((inst.sub & 1) << 1)));
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		case IMLOp::FARITH:
			e.MOVAPD(s0, Fpr(inst.r[1]));
			scalarArith((IMLArith)inst.sub, s0, Fpr(inst.r[2]));
			e.MOVSD_rr(Fpr(inst.r[0]), s0);
			return true;
		case IMLOp::FARITHS:
			e.MOVAPD(s0, Fpr(inst.r[1]));
			if ((IMLArith)inst.sub == IMLArith::MUL)
			{
				e.MOVAPD(s1, Fpr(inst.r[2]));
				RoundMultiplier25(s1);
				e.MULSD(s0, s1);
			}
			else
				scalarArith((IMLArith)inst.sub, s0, Fpr(inst.r[2]));
			roundLowToSingleBoth(s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		case IMLOp::FMADD:
		case IMLOp::FMADDS:
		{
			// r1 = a, r2 = c, r3 = b
			IMLMadd kind = (IMLMadd)inst.sub;
			bool single = inst.op == IMLOp::FMADDS;
			if (single)
			{
				e.MOVAPD(s1, Fpr(inst.r[2]));
				RoundMultiplier25(s1);
				e.MOVAPD(s0, Fpr(inst.r[1]));
				e.MULSD(s0, s1);
			}
			else
			{
				e.MOVAPD(s0, Fpr(inst.r[1]));
				e.MULSD(s0, Fpr(inst.r[2]));
			}
			if (kind == IMLMadd::MADD || kind == IMLMadd::NMADD)
				e.ADDSD(s0, Fpr(inst.r[3]));
			else
				e.SUBSD(s0, Fpr(inst.r[3]));
			if (kind == IMLMadd::NMADD || kind == IMLMadd::NMSUB)
				negate(s0);
			if (single)
			{
				roundLowToSingleBoth(s0);
				e.MOVAPD(Fpr(inst.r[0]), s0);
			}
			else
				e.MOVSD_rr(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::PARITH:
			e.MOVAPD(s0, Fpr(inst.r[1]));
			if ((IMLArith)inst.sub == IMLArith::MUL)
			{
				e.MOVAPD(s1, Fpr(inst.r[2]));
				RoundMultiplier25(s1);
				e.MULPD(s0, s1);
			}
			else
				pairArith((IMLArith)inst.sub, s0, Fpr(inst.r[2]));
			roundPairToSingle(s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		case IMLOp::PMADD:
		{
			// r1 = a, r2 = c, r3 = b
			IMLMadd kind = (IMLMadd)inst.sub;
			e.MOVAPD(s1, Fpr(inst.r[2]));
			RoundMultiplier25(s1);
			e.MOVAPD(s0, Fpr(inst.r[1]));
			e.MULPD(s0, s1);
			if (kind == IMLMadd::MADD)
				roundPairToSingle(s0); // ps_madd rounds the product first
			if (kind == IMLMadd::MADD || kind == IMLMadd::NMADD)
				e.ADDPD(s0, Fpr(inst.r[3]));
			else
				e.SUBPD(s0, Fpr(inst.r[3]));
			if (kind == IMLMadd::NMADD || kind == IMLMadd::NMSUB)
				negate(s0);
			roundPairToSingle(s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::PMULS:
		case IMLOp::PMADDS:
		{
			// one lane of c times both lanes of a (plus b)
			e.MOVAPD(s1, Fpr(inst.r[2]));
			if (inst.sub == 0)
				e.UNPCKLPD(s1, s1);
			else
				e.UNPCKHPD(s1, s1);
			RoundMultiplier25(s1);
			e.MOVAPD(s0, Fpr(inst.r[1]));
			e.MULPD(s0, s1);
			if (inst.op == IMLOp::PMADDS)
				e.ADDPD(s0, Fpr(inst.r[3]));
			roundPairToSingle(s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::PSUM:
		{
			// r1 = a, r2 = b, r3 = c. sum = a.ps0 + b.ps1
			e.MOVAPD(s1, Fpr(inst.r[2]));
			e.UNPCKHPD(s1, s1);
			e.MOVAPD(s0, Fpr(inst.r[1]));
			e.ADDSD(s0, s1);
			e.MOVAPD(s1, Fpr(inst.r[3]));
			if (inst.sub == 0)
			{
				// ps0 = sum, ps1 = c.ps1
				e.SHUFPD(s0, s1, 0x2);
				roundPairToSingle(s0);
				e.MOVAPD(Fpr(inst.r[0]), s0);
			}
			else
			{
				// ps0 = c.ps0, ps1 = sum
				e.SHUFPD(s1, s0, 0x0);
				roundPairToSingle(s1);
				e.MOVAPD(Fpr(inst.r[0]), s1);
			}
			return true;
		}
		case IMLOp::FSEL:
		case IMLOp::PSEL:
		{
			// r1 = a, r2 = c, r3 = b: c where a >= -0.0, b otherwise (also for NaN)
			e.XORPD(s1, s1);
			if (inst.op == IMLOp::FSEL)
			{
				e.CMPSD(s1, Fpr(inst.r[1]), kCmpLE);
				e.UNPCKLPD(s1, s1);
			}
			else
				e.CMPPD(s1, Fpr(inst.r[1]), kCmpLE);
			e.MOVAPD(s0, Fpr(inst.r[2]));
			e.ANDPD(s0, s1);
			e.ANDNPD(s1, Fpr(inst.r[3]));
			e.ORPD(s0, s1);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::FRSP:
			e.MOVAPD(s0, Fpr(inst.r[1]));
			roundLowToSingleBoth(s0);
			e.MOVAPD(Fpr(inst.r[0]), s0);
			return true;
		case IMLOp::FCTIWZ:
		{
			Xmm b = Fpr(inst.r[1]);
			// truncate; out of range and NaN give 0x80000000, positive overflow becomes 0x7FFFFFFF
			e.CVTTSD2SI(RAX, b);
			e.ALU_ri(ALU_CMP, RAX, 0x80000000);
			size_t inRange = e.Jcc_rel32(CC_NE);
			e.XORPD(s0, s0);
			e.UCOMISD(b, s0);
			size_t notPositive = e.Jcc_rel32(CC_BE);
			e.MOV_ri32(RAX, 0x7FFFFFFF);
			e.BindRel32(inRange, e.Size());
			e.BindRel32(notPositive, e.Size());
			// 0xFFF80000 in the upper word, bit 32 for negative inputs that give 0
			e.MOV_rr(RCX, RAX);
			e.MOV_ri64(RDX, 0xFFF8000000000000ull);
			e.ALU_rr(ALU_OR, RCX, RDX, true);
			e.TEST_rr(RAX, RAX);
			size_t nonZero = e.Jcc_rel32(CC_NE);
			e.MOVQ_rx(RDX, b);
			e.SHIFT_ri(SH_SHR, RDX, 63, true);
			e.SHIFT_ri(SH_SHL, RDX, 32, true);
			e.ALU_rr(ALU_OR, RCX, RDX, true);
			e.BindRel32(nonZero, e.Size());
			e.MOVQ_xr(s0, RCX);
			e.MOVSD_rr(Fpr(inst.r[0]), s0);
			return true;
		}
		case IMLOp::FCTIW:
		case IMLOp::FRES:
		case IMLOp::FRSQRTE:
		case IMLOp::PRES:
		case IMLOp::PRSQRTE:
		{
			const Mem tmp = CPU(offsetof(PPCInterpreter_t, temporaryFPR));
			e.MOVUPD_mr(tmp, Fpr(inst.r[1]));
			const void* helper = nullptr;
			switch (inst.op)
			{
			case IMLOp::FCTIW: helper = (const void*)&Helper_FCTIW; break;
			case IMLOp::FRES: helper = (const void*)&Helper_FRES; break;
			case IMLOp::FRSQRTE: helper = (const void*)&Helper_FRSQRTE; break;
			case IMLOp::PRES: helper = (const void*)&Helper_PRES; break;
			default: helper = (const void*)&Helper_PRSQRTE; break;
			}
			CallHelper(helper, inst.saveMask);
			if (inst.op == IMLOp::FCTIW || inst.op == IMLOp::FRSQRTE)
				e.MOVLPD_rm(Fpr(inst.r[0]), tmp); // ps1 is kept
			else
				e.MOVUPD_rm(Fpr(inst.r[0]), tmp);
			return true;
		}
		case IMLOp::FCMP:
		{
			// r0-r3 = LT, GT, EQ, UN; r4 = a, r5 = b; sub = lane
			Xmm a = Fpr(inst.r[4]);
			Xmm b = Fpr(inst.r[5]);
			if (inst.sub == 1)
			{
				e.MOVAPD(s0, a);
				e.UNPCKHPD(s0, s0);
				e.MOVAPD(s1, b);
				e.UNPCKHPD(s1, s1);
				a = s0;
				b = s1;
			}
			uint8 lt = Gpr(inst.r[0]), gt = Gpr(inst.r[1]), eq = Gpr(inst.r[2]), un = Gpr(inst.r[3]);
			e.UCOMISD(a, b);
			e.SETcc(CC_P, RAX);   // unordered
			e.SETcc(CC_A, RCX);   // greater (never for unordered)
			e.SETcc(CC_B, RDX);   // less or unordered
			e.SETcc(CC_E, eq);    // equal or unordered
			e.MOVZX_r8(RAX, RAX);
			e.MOVZX_r8(RCX, RCX);
			e.MOVZX_r8(RDX, RDX);
			e.MOVZX_r8(eq, eq);
			e.MOV_rr(gt, RCX);
			e.MOV_rr(RCX, RAX);
			e.ALU_ri(ALU_XOR, RCX, 1);
			e.ALU_rr(ALU_AND, RDX, RCX);
			e.MOV_rr(lt, RDX);
			e.ALU_rr(ALU_AND, eq, RCX);
			e.MOV_rr(un, RAX);
			// FPSCR[FPCC] = LT GT EQ UN
			e.MOV_rr(RCX, lt);
			e.SHIFT_ri(SH_SHL, RCX, 3);
			e.MOV_rr(RDX, gt);
			e.SHIFT_ri(SH_SHL, RDX, 2);
			e.ALU_rr(ALU_OR, RCX, RDX);
			e.MOV_rr(RDX, eq);
			e.ALU_rr(ALU_ADD, RDX, RDX);
			e.ALU_rr(ALU_OR, RCX, RDX);
			e.ALU_rr(ALU_OR, RCX, un);
			e.SHIFT_ri(SH_SHL, RCX, 12);
			const Mem fpscr = CPU(offsetof(PPCInterpreter_t, fpscr));
			e.MOV_rm(RDX, fpscr);
			e.ALU_ri(ALU_AND, RDX, ~0x0000F000u);
			e.ALU_rr(ALU_OR, RCX, RDX);
			if ((inst.flags & IMLF_ORDERED) == 0)
			{
				// unordered compares set VXSNAN for signaling NaN operands
				LoadConstantTable();
				for (Xmm x : {a, b})
				{
					e.MOVQ_rx(RDX, x);
					e.ALU_rr(ALU_ADD, RDX, RDX, true);
					e.ALU_rm(ALU_ADD, RDX, mem(RAX, offsetof(VectorConstants, snanBias)), true);
					e.ALU_rm(ALU_CMP, RDX, mem(RAX, offsetof(VectorConstants, snanLimit)), true);
					size_t notSignaling = e.Jcc_rel32(CC_AE);
					e.ALU_ri(ALU_OR, RCX, 0x01000000);
					e.BindRel32(notSignaling, e.Size());
				}
			}
			e.MOV_mr(fpscr, RCX);
			return true;
		}
		default:
			cemu_assert_debug(false);
			return false;
		}
	}
}
