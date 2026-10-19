// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Paired single arithmetic (primary opcode 4, except the quantized loads and
// stores which live with the other memory instructions).
//
// Both slots are computed with host doubles and rounded to float. Multiplies
// round the frC operand to 25 significant bits like the Espresso does.

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

namespace EspressoItp
{
	struct PairedOperands
	{
		double a0, a1, b0, b1, c0, c1;
	};

	static inline PairedOperands ReadPaired(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& a = cpu->fpr[ppcOpField_A(op)];
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		const FPR_t& c = cpu->fpr[ppcOpField_C(op)];
		return { a.fp0, a.fp1, b.fp0, b.fp1, c.fp0, c.fp1 };
	}

	static inline void WritePaired(PPCInterpreter_t* cpu, uint32 op, float ps0, float ps1)
	{
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0 = (double)ps0;
		d.fp1 = (double)ps1;
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- two operand arithmetic ----

	void Op_PS_ADD(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 + v.b0), (float)(v.a1 + v.b1));
	}

	void Op_PS_SUB(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 - v.b0), (float)(v.a1 - v.b1));
	}

	void Op_PS_DIV(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 / v.b0), (float)(v.a1 / v.b1));
	}

	void Op_PS_MUL(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 * RoundMultiplier25(v.c0)), (float)(v.a1 * RoundMultiplier25(v.c1)));
	}

	// ps_muls0/1: both slots of frA times one slot of frC
	void Op_PS_MULS0(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		double c = RoundMultiplier25(v.c0);
		WritePaired(cpu, op, (float)(v.a0 * c), (float)(v.a1 * c));
	}

	void Op_PS_MULS1(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		double c = RoundMultiplier25(v.c1);
		WritePaired(cpu, op, (float)(v.a0 * c), (float)(v.a1 * c));
	}

	// ---- multiply-add family ----

	// ps_madd rounds the product to single precision before adding
	void Op_PS_MADD(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		float p0 = (float)(v.a0 * RoundMultiplier25(v.c0));
		float p1 = (float)(v.a1 * RoundMultiplier25(v.c1));
		WritePaired(cpu, op, (float)((double)p0 + v.b0), (float)((double)p1 + v.b1));
	}

	void Op_PS_NMADD(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)-(v.a0 * RoundMultiplier25(v.c0) + v.b0), (float)-(v.a1 * RoundMultiplier25(v.c1) + v.b1));
	}

	void Op_PS_MSUB(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 * RoundMultiplier25(v.c0) - v.b0), (float)(v.a1 * RoundMultiplier25(v.c1) - v.b1));
	}

	void Op_PS_NMSUB(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)-(v.a0 * RoundMultiplier25(v.c0) - v.b0), (float)-(v.a1 * RoundMultiplier25(v.c1) - v.b1));
	}

	void Op_PS_MADDS0(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		double c = RoundMultiplier25(v.c0);
		WritePaired(cpu, op, (float)(v.a0 * c + v.b0), (float)(v.a1 * c + v.b1));
	}

	void Op_PS_MADDS1(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		double c = RoundMultiplier25(v.c1);
		WritePaired(cpu, op, (float)(v.a0 * c + v.b0), (float)(v.a1 * c + v.b1));
	}

	// ---- sums, select ----

	void Op_PS_SUM0(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)(v.a0 + v.b1), (float)v.c1);
	}

	void Op_PS_SUM1(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		WritePaired(cpu, op, (float)v.c0, (float)(v.a0 + v.b1));
	}

	void Op_PS_SEL(PPCInterpreter_t* cpu, uint32 op)
	{
		PairedOperands v = ReadPaired(cpu, op);
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0 = (v.a0 >= -0.0) ? v.c0 : v.b0;
		d.fp1 = (v.a1 >= -0.0) ? v.c1 : v.b1;
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- estimates ----

	void Op_PS_RES(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		double b0 = b.fp0, b1 = b.fp1;
		WritePaired(cpu, op, (float)fres_espresso(b0), (float)fres_espresso(b1));
	}

	void Op_PS_RSQRTE(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		double b0 = b.fp0, b1 = b.fp1;
		WritePaired(cpu, op, (float)frsqrte_espresso(b0), (float)frsqrte_espresso(b1));
	}

	// ---- moves (bit exact) ----

	static inline void WritePairedBits(PPCInterpreter_t* cpu, uint32 op, uint64 ps0, uint64 ps1)
	{
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0int = ps0;
		d.fp1int = ps1;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_PS_MR(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		WritePairedBits(cpu, op, b.fp0int, b.fp1int);
	}

	void Op_PS_NEG(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		WritePairedBits(cpu, op, b.fp0int ^ kDoubleSignBit, b.fp1int ^ kDoubleSignBit);
	}

	void Op_PS_ABS(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		WritePairedBits(cpu, op, b.fp0int & ~kDoubleSignBit, b.fp1int & ~kDoubleSignBit);
	}

	void Op_PS_NABS(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& b = cpu->fpr[ppcOpField_B(op)];
		WritePairedBits(cpu, op, b.fp0int | kDoubleSignBit, b.fp1int | kDoubleSignBit);
	}

	void Op_PS_MERGE00(PPCInterpreter_t* cpu, uint32 op)
	{
		WritePairedBits(cpu, op, cpu->fpr[ppcOpField_A(op)].fp0int, cpu->fpr[ppcOpField_B(op)].fp0int);
	}

	void Op_PS_MERGE01(PPCInterpreter_t* cpu, uint32 op)
	{
		WritePairedBits(cpu, op, cpu->fpr[ppcOpField_A(op)].fp0int, cpu->fpr[ppcOpField_B(op)].fp1int);
	}

	void Op_PS_MERGE10(PPCInterpreter_t* cpu, uint32 op)
	{
		WritePairedBits(cpu, op, cpu->fpr[ppcOpField_A(op)].fp1int, cpu->fpr[ppcOpField_B(op)].fp0int);
	}

	void Op_PS_MERGE11(PPCInterpreter_t* cpu, uint32 op)
	{
		WritePairedBits(cpu, op, cpu->fpr[ppcOpField_A(op)].fp1int, cpu->fpr[ppcOpField_B(op)].fp1int);
	}

	// ---- compares ----

	void Op_PS_CMPU0(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 crField = (op >> 23) & 7;
		CompareUnordered(cpu, crField * 4, cpu->fpr[ppcOpField_A(op)].fp0, cpu->fpr[ppcOpField_B(op)].fp0);
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_PS_CMPU1(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 crField = (op >> 23) & 7;
		CompareUnordered(cpu, crField * 4, cpu->fpr[ppcOpField_A(op)].fp1, cpu->fpr[ppcOpField_B(op)].fp1);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ordered compare of ps0: sets CR and FPCC but, unlike the unordered
	// form, leaves the FPSCR exception bits alone
	void Op_PS_CMPO0(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 crBase = ((op >> 23) & 7) * 4;
		double a = cpu->fpr[ppcOpField_A(op)].fp0;
		double b = cpu->fpr[ppcOpField_B(op)].fp0;
		uint32 fpcc;
		if (IsNaNBits(DoubleBits(a)) || IsNaNBits(DoubleBits(b)))
			fpcc = 0x1;
		else if (a < b)
			fpcc = 0x8;
		else if (a > b)
			fpcc = 0x4;
		else
			fpcc = 0x2;
		for (uint32 i = 0; i < 4; i++)
			cpu->cr[crBase + i] = (uint8)((fpcc >> (3 - i)) & 1);
		cpu->fpscr = (cpu->fpscr & ~0x0000F000u) | (fpcc << 12);
		PPCInterpreter_nextInstruction(cpu);
	}
}
