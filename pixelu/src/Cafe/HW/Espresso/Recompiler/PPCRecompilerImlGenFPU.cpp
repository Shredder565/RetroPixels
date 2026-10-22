// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Translation of the floating point and paired single instructions,
// including the floating point and quantized loads and stores.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"

using namespace IMLGuest;

namespace
{
	inline uint32 FieldD(uint32 op) { return (op >> 21) & 0x1F; }
	inline uint32 FieldA(uint32 op) { return (op >> 16) & 0x1F; }
	inline uint32 FieldB(uint32 op) { return (op >> 11) & 0x1F; }
	inline uint32 FieldC(uint32 op) { return (op >> 6) & 0x1F; }
	inline sint32 SImm16(uint32 op) { return (sint32)(sint16)(uint16)op; }
}

bool PPCIMLTranslator::TranslateFPLoadStore(uint32 op)
{
	uint32 primary = op >> 26;
	uint32 rD = FieldD(op), rA = FieldA(op), rB = FieldB(op);
	IMLReg F = FPR(rD);
	IMLReg A = GPR(rA);

	auto memOp = [&](IMLOp memOpcode, uint8 sub, IMLReg base, IMLReg index, sint32 disp)
	{
		IMLInstruction i = IMLInstruction::Make(memOpcode, F, base, index);
		i.sub = sub;
		i.imm = disp;
		Emit(i);
	};

	if (primary == 31)
	{
		uint32 xo = (op >> 1) & 0x3FF;
		IMLOp memOpcode;
		uint8 sub = 0;
		bool update = false;
		switch (xo)
		{
		case 567: update = true; [[fallthrough]];
		case 535: memOpcode = IMLOp::LOADFS; break;
		case 631: update = true; [[fallthrough]];
		case 599: memOpcode = IMLOp::LOADFD; break;
		case 695: update = true; [[fallthrough]];
		case 663: memOpcode = IMLOp::STOREF; sub = (uint8)IMLFMem::SINGLE; break;
		case 759: update = true; [[fallthrough]];
		case 727: memOpcode = IMLOp::STOREF; sub = (uint8)IMLFMem::DOUBLE; break;
		case 983: memOpcode = IMLOp::STOREF; sub = (uint8)IMLFMem::IWX; break;
		default:
			return false;
		}
		// update forms with rA = 0 behave like the plain forms (EA = rB)
		if (update && rA != 0)
		{
			IMLReg ea = AddressX(rA, rB);
			memOp(memOpcode, sub, ea, IML_NOREG, 0);
			Mov(A, ea);
		}
		else
			memOp(memOpcode, sub, rA ? A : IML_NOREG, GPR(rB), 0);
		return true;
	}

	if (primary >= 56)
	{
		// psq_l, psq_lu, psq_st, psq_stu: 12-bit displacement, W at 0x8000, I at bits 12-14
		bool isStore = primary >= 60;
		bool update = (primary & 1) != 0;
		sint32 disp = ((sint32)(op << 20)) >> 20;
		IMLInstruction i = IMLInstruction::Make(isStore ? IMLOp::PSQSTORE : IMLOp::PSQLOAD, F);
		i.r[3] = GQR((op >> 12) & 7);
		if (op & 0x8000)
			i.flags |= IMLF_PSQ_W;
		if (update && rA != 0)
		{
			// rA is written before the access
			IMLReg ea = Temp();
			Op3i(IMLOp::ADD, ea, A, disp);
			Mov(A, ea);
			i.r[1] = ea;
		}
		else
		{
			i.r[1] = rA ? A : IML_NOREG;
			i.imm = disp;
		}
		Emit(i);
		return true;
	}

	sint32 imm = SImm16(op);
	IMLOp memOpcode;
	uint8 sub = 0;
	bool update = (primary & 1) != 0;
	switch (primary & ~1u)
	{
	case 48: memOpcode = IMLOp::LOADFS; break;
	case 50: memOpcode = IMLOp::LOADFD; break;
	case 52: memOpcode = IMLOp::STOREF; sub = (uint8)IMLFMem::SINGLE; break;
	case 54:
		memOpcode = IMLOp::STOREF;
		sub = (uint8)IMLFMem::DOUBLE;
		if (update && rA == 0)
			return false; // stfdu with rA = 0 stores to r0 without the displacement
		break;
	default:
		return false;
	}
	if (update && rA != 0)
	{
		if (memOpcode == IMLOp::STOREF)
		{
			memOp(memOpcode, sub, A, IML_NOREG, imm);
			Op3i(IMLOp::ADD, A, A, imm);
		}
		else
		{
			IMLReg ea = Temp();
			Op3i(IMLOp::ADD, ea, A, imm);
			memOp(memOpcode, sub, ea, IML_NOREG, 0);
			Mov(A, ea);
		}
	}
	else
		memOp(memOpcode, sub, rA ? A : IML_NOREG, IML_NOREG, imm);
	return true;
}

bool PPCIMLTranslator::TranslateGroup4(uint32 op)
{
	uint32 xo = (op >> 1) & 0x3FF;
	uint32 lo = xo & 0x1F;
	uint32 hi = xo >> 5;
	IMLReg D = FPR(FieldD(op)), A = FPR(FieldA(op)), B = FPR(FieldB(op)), C = FPR(FieldC(op));

	auto withSub = [&](IMLInstruction i, uint8 sub) { i.sub = sub; Emit(i); };
	auto compare = [&](uint8 lane, bool ordered)
	{
		uint32 base = ((op >> 23) & 7) * 4;
		IMLInstruction i = IMLInstruction::Make(IMLOp::FCMP, CR(base), CR(base + 1), CR(base + 2), CR(base + 3));
		i.r[4] = A;
		i.r[5] = B;
		i.sub = lane;
		if (ordered)
			i.flags |= IMLF_ORDERED;
		Emit(i);
	};

	switch (lo)
	{
	case 0:
		if (hi == 0) { compare(0, false); return true; }
		if (hi == 1) { compare(0, true); return true; }
		if (hi == 2) { compare(1, false); return true; }
		return false;
	case 6:
	case 7:
	{
		// psq_lx/psq_stx (also the update encodings, which execute as these)
		IMLInstruction i = IMLInstruction::Make(lo == 6 ? IMLOp::PSQLOAD : IMLOp::PSQSTORE, D);
		uint32 rA = FieldA(op);
		i.r[1] = rA ? GPR(rA) : IML_NOREG;
		i.r[2] = GPR(FieldB(op));
		i.r[3] = GQR((op >> 7) & 7);
		if (op & 0x400)
			i.flags |= IMLF_PSQ_W;
		Emit(i);
		return true;
	}
	case 8:
		if (hi == 1) { withSub(IMLInstruction::Make(IMLOp::PSIGN, D, B), (uint8)IMLSign::NEG); return true; }
		if (hi == 2) { Op2(IMLOp::FMOV, D, B); return true; }
		if (hi == 4) { withSub(IMLInstruction::Make(IMLOp::PSIGN, D, B), (uint8)IMLSign::NABS); return true; }
		if (hi == 8) { withSub(IMLInstruction::Make(IMLOp::PSIGN, D, B), (uint8)IMLSign::ABS); return true; }
		return false;
	case 10: withSub(IMLInstruction::Make(IMLOp::PSUM, D, A, B, C), 0); return true;
	case 11: withSub(IMLInstruction::Make(IMLOp::PSUM, D, A, B, C), 1); return true;
	case 12: withSub(IMLInstruction::Make(IMLOp::PMULS, D, A, C), 0); return true;
	case 13: withSub(IMLInstruction::Make(IMLOp::PMULS, D, A, C), 1); return true;
	case 14: withSub(IMLInstruction::Make(IMLOp::PMADDS, D, A, C, B), 0); return true;
	case 15: withSub(IMLInstruction::Make(IMLOp::PMADDS, D, A, C, B), 1); return true;
	case 16:
		if (hi >= 16 && hi <= 19)
		{
			withSub(IMLInstruction::Make(IMLOp::PMERGE, D, A, B), (uint8)(hi - 16));
			return true;
		}
		return false;
	case 18: withSub(IMLInstruction::Make(IMLOp::PARITH, D, A, B), (uint8)IMLArith::DIV); return true;
	case 20: withSub(IMLInstruction::Make(IMLOp::PARITH, D, A, B), (uint8)IMLArith::SUB); return true;
	case 21: withSub(IMLInstruction::Make(IMLOp::PARITH, D, A, B), (uint8)IMLArith::ADD); return true;
	case 22: return true; // dcbz_l, the locked cache is not modelled
	case 23: Emit(IMLInstruction::Make(IMLOp::PSEL, D, A, C, B)); return true;
	case 24: Op2(IMLOp::PRES, D, B); return true;
	case 25: withSub(IMLInstruction::Make(IMLOp::PARITH, D, A, C), (uint8)IMLArith::MUL); return true;
	case 26: Op2(IMLOp::PRSQRTE, D, B); return true;
	case 28: withSub(IMLInstruction::Make(IMLOp::PMADD, D, A, C, B), (uint8)IMLMadd::MSUB); return true;
	case 29: withSub(IMLInstruction::Make(IMLOp::PMADD, D, A, C, B), (uint8)IMLMadd::MADD); return true;
	case 30: withSub(IMLInstruction::Make(IMLOp::PMADD, D, A, C, B), (uint8)IMLMadd::NMSUB); return true;
	case 31: withSub(IMLInstruction::Make(IMLOp::PMADD, D, A, C, B), (uint8)IMLMadd::NMADD); return true;
	default:
		return false;
	}
}

bool PPCIMLTranslator::TranslateGroup59(uint32 op)
{
	IMLReg D = FPR(FieldD(op)), A = FPR(FieldA(op)), B = FPR(FieldB(op)), C = FPR(FieldC(op));
	auto arith = [&](IMLArith a, IMLReg second)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::FARITHS, D, A, second);
		i.sub = (uint8)a;
		Emit(i);
	};
	auto madd = [&](IMLMadd m)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::FMADDS, D, A, C, B);
		i.sub = (uint8)m;
		Emit(i);
	};
	switch ((op >> 1) & 0x1F)
	{
	case 18: arith(IMLArith::DIV, B); return true;
	case 20: arith(IMLArith::SUB, B); return true;
	case 21: arith(IMLArith::ADD, B); return true;
	case 24: Op2(IMLOp::FRES, D, B); return true;
	case 25: arith(IMLArith::MUL, C); return true;
	case 28: madd(IMLMadd::MSUB); return true;
	case 29: madd(IMLMadd::MADD); return true;
	case 30: madd(IMLMadd::NMSUB); return true;
	case 31: madd(IMLMadd::NMADD); return true;
	default:
		return false;
	}
}

bool PPCIMLTranslator::TranslateGroup63(uint32 op)
{
	IMLReg D = FPR(FieldD(op)), A = FPR(FieldA(op)), B = FPR(FieldB(op)), C = FPR(FieldC(op));
	uint32 xo = (op >> 1) & 0x3FF;
	auto arith = [&](IMLArith a, IMLReg second)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::FARITH, D, A, second);
		i.sub = (uint8)a;
		Emit(i);
	};
	auto madd = [&](IMLMadd m)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::FMADD, D, A, C, B);
		i.sub = (uint8)m;
		Emit(i);
	};
	auto sign = [&](IMLSign s)
	{
		IMLInstruction i = IMLInstruction::Make(IMLOp::FSIGN, D, B);
		i.sub = (uint8)s;
		Emit(i);
	};
	// A form opcodes are decoded from the low 5 bits and take precedence
	switch (xo & 0x1F)
	{
	case 0:
	{
		// fcmpu (fcmpo and mcrfs decode to this as well)
		uint32 base = (FieldD(op) >> 2) * 4;
		IMLInstruction i = IMLInstruction::Make(IMLOp::FCMP, CR(base), CR(base + 1), CR(base + 2), CR(base + 3));
		i.r[4] = A;
		i.r[5] = B;
		Emit(i);
		return true;
	}
	case 12: Op2(IMLOp::FRSP, D, B); return true;
	case 15: Op2(IMLOp::FCTIWZ, D, B); return true;
	case 18: arith(IMLArith::DIV, B); return true;
	case 20: arith(IMLArith::SUB, B); return true;
	case 21: arith(IMLArith::ADD, B); return true;
	case 23: Emit(IMLInstruction::Make(IMLOp::FSEL, D, A, C, B)); return true;
	case 25: arith(IMLArith::MUL, C); return true;
	case 26: Op2(IMLOp::FRSQRTE, D, B); return true;
	case 28: madd(IMLMadd::MSUB); return true;
	case 29: madd(IMLMadd::MADD); return true;
	case 30: madd(IMLMadd::NMSUB); return true;
	case 31: madd(IMLMadd::NMADD); return true;
	default:
		break;
	}
	switch (xo)
	{
	case 14: Op2(IMLOp::FCTIW, D, B); return true;
	case 40: sign(IMLSign::NEG); return true;
	case 72: Op2(IMLOp::FMOVLO, D, B); return true;
	case 136: sign(IMLSign::NABS); return true;
	case 264: sign(IMLSign::ABS); return true;
	default:
		return false; // mffs, mtfsf, mtfsb1 and unknown encodings
	}
}
