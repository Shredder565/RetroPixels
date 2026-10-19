// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Scalar floating point instructions (primary opcodes 59 and 63) and the
// Espresso reciprocal estimate instructions.
//
// Arithmetic is done with host doubles, the FPSCR is not maintained apart
// from what the compare instructions and mtfsf/mtfsb1/mffs touch. Single
// precision forms round the double result to float and copy it into ps1.

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

namespace EspressoItp
{
	// ---- reciprocal estimates ----
	//
	// The Espresso computes fres/frsqrte with a 32 entry piecewise linear
	// table indexed by the top significand bits (frsqrte also uses the lowest
	// exponent bit). The base/slope values below are the hardware's, as
	// measured on real consoles.

	struct EstimateSegment
	{
		uint32 base;
		uint32 slope;
	};

	static constexpr EstimateSegment kRecipSqrtTable[32] =
	{
		// odd exponent (index bit 4 clear)
		{0x1a7e800, 0x568}, {0x17cb800, 0x4f3}, {0x1552800, 0x48d}, {0x130c000, 0x435},
		{0x10f2000, 0x3e7}, {0x0eff000, 0x3a2}, {0x0d2e000, 0x365}, {0x0b7c000, 0x32e},
		{0x09e5000, 0x2fc}, {0x0867000, 0x2d0}, {0x06ff000, 0x2a8}, {0x05ab800, 0x283},
		{0x046a000, 0x261}, {0x0339800, 0x243}, {0x0218800, 0x226}, {0x0105800, 0x20b},
		// even exponent (index bit 4 set)
		{0x3ffa000, 0x7a4}, {0x3c29000, 0x700}, {0x38aa000, 0x670}, {0x3572000, 0x5f2},
		{0x3279000, 0x584}, {0x2fb7000, 0x524}, {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e},
		{0x2881000, 0x43a}, {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e},
		{0x20c1000, 0x35e}, {0x1f12000, 0x332}, {0x1d79000, 0x30a}, {0x1bf4000, 0x2e6},
	};

	static constexpr EstimateSegment kRecipTable[32] =
	{
		{0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
		{0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
		{0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
		{0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
		{0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
		{0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
		{0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
		{0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106},
	};

	static double EstimateRecipSqrt(double input)
	{
		uint64 bits = DoubleBits(input);
		if ((bits & ~kDoubleSignBit) == 0)
			return DoubleFromBits((bits & kDoubleSignBit) | kDoubleExpMask); // +-0 -> +-inf
		uint32 exponent = (uint32)(bits >> 52) & 0x7FF;
		if (exponent == 0x7FF)
		{
			if ((bits & kDoubleFracMask) != 0)
				return input; // NaN passes through unchanged
			return (bits & kDoubleSignBit) ? DoubleFromBits(0x7FF8000000000000ull) : 0.0; // -inf -> NaN, +inf -> +0
		}
		if (bits & kDoubleSignBit)
			return DoubleFromBits(0x7FF8000000000000ull); // negative -> default NaN
		// denormal inputs are not special cased (the exponent arithmetic below wraps like the original model)
		const EstimateSegment& seg = kRecipSqrtTable[(bits >> 48) & 0x1F];
		uint32 position = (uint32)(bits >> 37) & 0x7FF;
		sint32 estimate = (sint32)(seg.base - seg.slope * position);
		uint32 resultExp = 1023 - ((exponent - 1021) >> 1);
		uint64 result = (bits & kDoubleSignBit) | ((uint64)resultExp << 52);
		result += (uint64)(sint64)estimate << 26;
		return DoubleFromBits(result);
	}

	static double EstimateRecip(double input)
	{
		uint64 bits = DoubleBits(input);
		uint32 exponent = (uint32)(bits >> 52) & 0x7FF;
		if (exponent == 0)
			return DoubleFromBits(bits | kDoubleExpMask); // zero/denormal: exponent forced to all ones
		if (exponent == 0x7FF)
		{
			if ((bits & kDoubleFracMask) != 0)
				return input; // NaN passes through unchanged
			return (bits & kDoubleSignBit) ? DoubleFromBits(kDoubleSignBit) : 0.0; // 1/+-inf = +-0
		}
		const EstimateSegment& seg = kRecipTable[(bits >> 47) & 0x1F];
		uint32 position = (uint32)(bits >> 37) & 0x3FF;
		uint32 estimate = seg.base - (seg.slope * position + 1) / 2;
		uint32 resultExp = 2045 - exponent;
		uint64 result = (bits & kDoubleSignBit) | ((uint64)resultExp << 52);
		result += (uint64)estimate << 29;
		return DoubleFromBits(result);
	}

	// ---- compare ----

	// fills the CR field at crBitBase and FPSCR[FPCC] from an unordered compare
	void CompareUnordered(PPCInterpreter_t* cpu, uint32 crBitBase, double a, double b)
	{
		uint64 aBits = DoubleBits(a), bBits = DoubleBits(b);
		uint32 fpcc; // LT GT EQ UN as a 4-bit value
		if (IsNaNBits(aBits) || IsNaNBits(bBits))
			fpcc = 0x1;
		else if (a < b)
			fpcc = 0x8;
		else if (a > b)
			fpcc = 0x4;
		else
			fpcc = 0x2;
		for (uint32 i = 0; i < 4; i++)
			cpu->cr[crBitBase + i] = (uint8)((fpcc >> (3 - i)) & 1);
		if (IsSignalingNaNBits(aBits) || IsSignalingNaNBits(bBits))
			cpu->fpscr |= FPSCR_VXSNAN;
		cpu->fpscr = (cpu->fpscr & ~0x0000F000u) | (fpcc << 12);
	}

	void Op_FCMPU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 crField = ppcOpField_D(op) >> 2; // low two bits of the field are reserved
		CompareUnordered(cpu, crField * 4, cpu->fpr[ppcOpField_A(op)].fp0, cpu->fpr[ppcOpField_B(op)].fp0);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- moves and sign manipulation (bit exact, fp1 untouched unless noted) ----

	void Op_FMR(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0int = cpu->fpr[ppcOpField_B(op)].fp0int;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FNEG(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0int = cpu->fpr[ppcOpField_B(op)].fp0int ^ kDoubleSignBit;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FABS(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0int = cpu->fpr[ppcOpField_B(op)].fp0int & ~kDoubleSignBit;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FNABS(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0int = cpu->fpr[ppcOpField_B(op)].fp0int | kDoubleSignBit;
		PPCInterpreter_nextInstruction(cpu);
	}

	// fsel copies the whole register (both paired single slots)
	void Op_FSEL(PPCInterpreter_t* cpu, uint32 op)
	{
		const FPR_t& selected = (cpu->fpr[ppcOpField_A(op)].fp0 >= -0.0) ? cpu->fpr[ppcOpField_C(op)] : cpu->fpr[ppcOpField_B(op)];
		FPR_t copy = selected;
		cpu->fpr[ppcOpField_D(op)] = copy;
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- conversion to integer ----

	// result layout of fctiw/fctiwz: 0xFFF80000 in the upper word, the
	// integer in the lower word, plus bit 32 for a negative input that
	// converts to zero
	static void StoreConvertedWord(PPCInterpreter_t* cpu, uint32 frD, double input, uint32 word)
	{
		uint64 result = 0xFFF8000000000000ull | word;
		if (word == 0 && (DoubleBits(input) & kDoubleSignBit))
			result |= 0x100000000ull;
		cpu->fpr[frD].fp0int = result;
	}

	static uint32 ConvertToWord(double v, uint32 roundingMode)
	{
		if (v > 2147483647.0)
			return 0x7FFFFFFF;
		if (v < -2147483648.0)
			return 0x80000000;
		if (v != v)
			return 0x80000000; // NaN
		double r;
		switch (roundingMode & 3)
		{
		case 0: r = std::nearbyint(v); break; // host default rounding is round to nearest even
		case 1: r = std::trunc(v); break;
		case 2: r = std::ceil(v); break;
		default: r = std::floor(v); break;
		}
		if (r > 2147483647.0)
			return 0x7FFFFFFF;
		if (r < -2147483648.0)
			return 0x80000000;
		return (uint32)(sint32)r;
	}

	void Op_FCTIWZ(PPCInterpreter_t* cpu, uint32 op)
	{
		double b = cpu->fpr[ppcOpField_B(op)].fp0;
		StoreConvertedWord(cpu, ppcOpField_D(op), b, ConvertToWord(b, 1));
		PPCInterpreter_nextInstruction(cpu);
	}

	// uses the rounding mode selected by FPSCR[RN]
	void Op_FCTIW(PPCInterpreter_t* cpu, uint32 op)
	{
		double b = cpu->fpr[ppcOpField_B(op)].fp0;
		StoreConvertedWord(cpu, ppcOpField_D(op), b, ConvertToWord(b, cpu->fpscr & 3));
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- rounding and estimates ----

	void Op_FRSP(PPCInterpreter_t* cpu, uint32 op)
	{
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		float single = (float)cpu->fpr[ppcOpField_B(op)].fp0;
		d.fp0 = (double)single;
		d.fp1 = d.fp0;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FRES(PPCInterpreter_t* cpu, uint32 op)
	{
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0 = EstimateRecip(cpu->fpr[ppcOpField_B(op)].fp0);
		d.fp1 = d.fp0;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FRSQRTE(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0 = EstimateRecipSqrt(cpu->fpr[ppcOpField_B(op)].fp0);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- double precision arithmetic (writes ps0 only) ----

	template<typename TFunc>
	static inline void DoubleOp(PPCInterpreter_t* cpu, uint32 op, TFunc func)
	{
		const double a = cpu->fpr[ppcOpField_A(op)].fp0;
		const double b = cpu->fpr[ppcOpField_B(op)].fp0;
		const double c = cpu->fpr[ppcOpField_C(op)].fp0;
		cpu->fpr[ppcOpField_D(op)].fp0 = func(a, b, c);
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FADD(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a + b; }); }
	void Op_FSUB(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a - b; }); }
	void Op_FMUL(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a * c; }); }
	void Op_FDIV(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a / b; }); }
	void Op_FMADD(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a * c + b; }); }
	void Op_FMSUB(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return a * c - b; }); }
	void Op_FNMADD(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return -(a * c + b); }); }
	void Op_FNMSUB(PPCInterpreter_t* cpu, uint32 op) { DoubleOp(cpu, op, [](double a, double b, double c) { return -(a * c - b); }); }

	// ---- single precision arithmetic (result rounded to float, copied to ps1) ----

	template<typename TFunc>
	static inline void SingleOp(PPCInterpreter_t* cpu, uint32 op, TFunc func)
	{
		const double a = cpu->fpr[ppcOpField_A(op)].fp0;
		const double b = cpu->fpr[ppcOpField_B(op)].fp0;
		const double c = cpu->fpr[ppcOpField_C(op)].fp0;
		FPR_t& d = cpu->fpr[ppcOpField_D(op)];
		d.fp0 = (double)(float)func(a, b, c);
		d.fp1 = d.fp0;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_FADDS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a + b; }); }
	void Op_FSUBS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a - b; }); }
	void Op_FDIVS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a / b; }); }
	void Op_FMULS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a * RoundMultiplier25(c); }); }
	void Op_FMADDS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a * RoundMultiplier25(c) + b; }); }
	void Op_FMSUBS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return a * RoundMultiplier25(c) - b; }); }
	void Op_FNMADDS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return -(a * RoundMultiplier25(c) + b); }); }
	void Op_FNMSUBS(PPCInterpreter_t* cpu, uint32 op) { SingleOp(cpu, op, [](double a, double b, double c) { return -(a * RoundMultiplier25(c) - b); }); }

	// ---- FPSCR access ----

	void Op_MFFS(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->fpr[ppcOpField_D(op)].fp0int = (uint64)cpu->fpscr;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_MTFSF(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 fieldMask = (op >> 17) & 0xFF;
		uint32 bitMask = 0;
		for (uint32 field = 0; field < 8; field++)
		{
			if ((fieldMask >> (7 - field)) & 1)
				bitMask |= 0xF0000000u >> (field * 4);
		}
		uint32 source = (uint32)cpu->fpr[ppcOpField_B(op)].fp0int;
		cpu->fpscr = (source & bitMask) | (cpu->fpscr & ~bitMask);
		static std::atomic_bool s_loggedWrite{false};
		if (!s_loggedWrite.exchange(true))
			cemuLog_log(LogType::Force, "Interpreter: guest wrote FPSCR with mtfsf (FPSCR is not emulated)");
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_MTFSB1(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 bit = ppcOpField_D(op);
		// FEX and VX are summary bits and cannot be set directly
		if (bit != 1 && bit != 2)
			cpu->fpscr |= 1u << (31 - bit);
		PPCInterpreter_nextInstruction(cpu);
	}
}

ATTR_MS_ABI double fres_espresso(double input)
{
	return EspressoItp::EstimateRecip(input);
}

ATTR_MS_ABI double frsqrte_espresso(double input)
{
	return EspressoItp::EstimateRecipSqrt(input);
}
