// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
// Interpreter-private helpers: floating point bit conversions, paired single
// quantization and the handlers that are shared by both memory models.

#include "PPCInterpreterInternal.h"

namespace EspressoItp
{
	using OpHandler = void (*)(PPCInterpreter_t* cpu, uint32 opcode);

	// ---- floating point bit access ----

	inline uint64 DoubleBits(double v) { return std::bit_cast<uint64>(v); }
	inline double DoubleFromBits(uint64 v) { return std::bit_cast<double>(v); }
	inline uint32 FloatBits(float v) { return std::bit_cast<uint32>(v); }
	inline float FloatFromBits(uint32 v) { return std::bit_cast<float>(v); }

	constexpr uint64 kDoubleSignBit = 0x8000000000000000ull;
	constexpr uint64 kDoubleExpMask = 0x7FF0000000000000ull;
	constexpr uint64 kDoubleFracMask = 0x000FFFFFFFFFFFFFull;

	inline bool IsNaNBits(uint64 v) { return (v & kDoubleExpMask) == kDoubleExpMask && (v & kDoubleFracMask) != 0; }
	inline bool IsSignalingNaNBits(uint64 v) { return IsNaNBits(v) && (v & 0x0008000000000000ull) == 0; }
	inline bool IsQuietNaNBits(uint64 v) { return IsNaNBits(v) && (v & 0x0008000000000000ull) != 0; }

	// Single to double as done by lfs/psq_l on the 750CL: exact widening,
	// denormals are normalised and NaNs keep their payload (no quieting).
	inline uint64 SingleToDoubleBits(uint32 s)
	{
		const uint64 sign = (uint64)(s & 0x80000000u) << 32;
		const uint32 exp = (s >> 23) & 0xFF;
		const uint32 frac = s & 0x007FFFFF;
		if (exp == 0xFF)
			return sign | kDoubleExpMask | ((uint64)frac << 29);
		if (exp != 0)
			return sign | ((uint64)(exp + (1023 - 127)) << 52) | ((uint64)frac << 29);
		if (frac == 0)
			return sign;
		// denormal single: value is frac * 2^-149, top set bit at position p
		const uint32 p = 31 - (uint32)std::countl_zero(frac);
		const uint64 normFrac = ((uint64)frac << (23 - p)) & 0x007FFFFF;
		return sign | ((uint64)(p + 874) << 52) | (normFrac << 29);
	}

	// Double to single as done by stfs (750CL manual, "floating-point store"):
	// no rounding, values in the single denormal range are denormalised and
	// everything else keeps sign, the top exponent bit and the next 30 bits.
	inline uint32 DoubleToSingleBits(uint64 d)
	{
		const uint32 exp = (uint32)(d >> 52) & 0x7FF;
		if (exp >= 874 && exp <= 896)
		{
			uint32 mant = 0x80000000u | (uint32)((d & kDoubleFracMask) >> 21);
			return (mant >> (905 - exp)) | ((uint32)(d >> 32) & 0x80000000u);
		}
		return ((uint32)(d >> 32) & 0xC0000000u) | ((uint32)(d >> 29) & 0x3FFFFFFFu);
	}

	// single precision multiplies on the Espresso only see the upper 25 bits
	// of the frC significand (rounded at the dropped bit)
	inline double RoundMultiplier25(double v)
	{
		return DoubleFromBits((DoubleBits(v) + 0x08000000ull) & ~0x0FFFFFFFull);
	}

	// ---- paired single quantization (GQR) ----

	// GQR data type field values
	constexpr uint32 kGqrFloat = 0;
	constexpr uint32 kGqrU8 = 4;
	constexpr uint32 kGqrU16 = 5;
	constexpr uint32 kGqrS8 = 6;
	constexpr uint32 kGqrS16 = 7;

	inline uint32 GqrAccessSize(uint32 type)
	{
		if (type == kGqrU8 || type == kGqrS8)
			return 1;
		if (type == kGqrU16 || type == kGqrS16)
			return 2;
		return 4; // float32, also used for the reserved types 1-3
	}

	// 2^exponent for exponents in [-32, 32] built directly from the float bits
	inline float PowerOfTwoF(sint32 exponent)
	{
		return FloatFromBits((uint32)(127 + exponent) << 23);
	}

	// scale fields are 6-bit two's complement numbers
	inline sint32 GqrScaleExponent(uint32 scaleField)
	{
		return ((sint32)(scaleField << 26)) >> 26;
	}

	inline float Dequantize(uint32 raw, uint32 type, uint32 scaleField)
	{
		float v;
		switch (type)
		{
		case kGqrU8: v = (float)(uint8)raw; break;
		case kGqrU16: v = (float)(uint16)raw; break;
		case kGqrS8: v = (float)(sint8)(uint8)raw; break;
		case kGqrS16: v = (float)(sint16)(uint16)raw; break;
		default:
			return FloatFromBits(raw); // floats are never scaled
		}
		return v * PowerOfTwoF(-GqrScaleExponent(scaleField));
	}

	inline uint32 Quantize(float value, uint32 type, uint32 scaleField)
	{
		float lo, hi;
		switch (type)
		{
		case kGqrU8: lo = 0.0f; hi = 255.0f; break;
		case kGqrU16: lo = 0.0f; hi = 65535.0f; break;
		case kGqrS8: lo = -128.0f; hi = 127.0f; break;
		case kGqrS16: lo = -32768.0f; hi = 32767.0f; break;
		default:
			return FloatBits(value);
		}
		float v = value * PowerOfTwoF(GqrScaleExponent(scaleField));
		if (v != v)
			return 0; // NaN
		v = std::min(std::max(v, lo), hi);
		return (uint32)(sint32)v; // truncates toward zero
	}

	// ---- handlers implemented in the .cpp files (no memory access) ----

	// PPCInterpreterOPC.cpp
	void Op_B(PPCInterpreter_t* cpu, uint32 op);
	void Op_BC(PPCInterpreter_t* cpu, uint32 op);
	void Op_BCLR(PPCInterpreter_t* cpu, uint32 op);
	void Op_BCCTR(PPCInterpreter_t* cpu, uint32 op);
	void Op_SC(PPCInterpreter_t* cpu, uint32 op);
	void Op_RFI(PPCInterpreter_t* cpu, uint32 op);
	void Op_MFMSR(PPCInterpreter_t* cpu, uint32 op);
	void Op_MTMSR(PPCInterpreter_t* cpu, uint32 op);
	void Op_MCRF(PPCInterpreter_t* cpu, uint32 op);
	void Op_MFCR(PPCInterpreter_t* cpu, uint32 op);
	void Op_MTCRF(PPCInterpreter_t* cpu, uint32 op);
	void Op_MCRXR(PPCInterpreter_t* cpu, uint32 op);
	void Op_CacheFlush(PPCInterpreter_t* cpu, uint32 op);  // dcbst, dcbf
	void Op_ICBI(PPCInterpreter_t* cpu, uint32 op);
	void Op_NoEffect(PPCInterpreter_t* cpu, uint32 op);    // dcbt, dcbi, dcbz_l, sync, isync, eieio, tlbie
	void Op_TLBSYNC(PPCInterpreter_t* cpu, uint32 op);
	void Op_TW(PPCInterpreter_t* cpu, uint32 op);
	void Op_TWI(PPCInterpreter_t* cpu, uint32 op);
	void Op_HLE(PPCInterpreter_t* cpu, uint32 op);         // PPCInterpreterHLE.cpp

	// PPCInterpreterFPU.cpp
	void Op_FCMPU(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMR(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNEG(PPCInterpreter_t* cpu, uint32 op);
	void Op_FABS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNABS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FSEL(PPCInterpreter_t* cpu, uint32 op);
	void Op_FCTIW(PPCInterpreter_t* cpu, uint32 op);
	void Op_FCTIWZ(PPCInterpreter_t* cpu, uint32 op);
	void Op_FRSP(PPCInterpreter_t* cpu, uint32 op);
	void Op_FRES(PPCInterpreter_t* cpu, uint32 op);
	void Op_FRSQRTE(PPCInterpreter_t* cpu, uint32 op);
	void Op_FADD(PPCInterpreter_t* cpu, uint32 op);
	void Op_FSUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMUL(PPCInterpreter_t* cpu, uint32 op);
	void Op_FDIV(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMADD(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMSUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNMADD(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNMSUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_FADDS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FSUBS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMULS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FDIVS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMADDS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FMSUBS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNMADDS(PPCInterpreter_t* cpu, uint32 op);
	void Op_FNMSUBS(PPCInterpreter_t* cpu, uint32 op);
	void Op_MFFS(PPCInterpreter_t* cpu, uint32 op);
	void Op_MTFSF(PPCInterpreter_t* cpu, uint32 op);
	void Op_MTFSB1(PPCInterpreter_t* cpu, uint32 op);

	// PPCInterpreterPS.cpp
	void Op_PS_CMPU0(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_CMPO0(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_CMPU1(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_NEG(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MR(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_NABS(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_ABS(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_SUM0(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_SUM1(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MULS0(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MULS1(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MADDS0(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MADDS1(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MERGE00(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MERGE01(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MERGE10(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MERGE11(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_DIV(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_SUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_ADD(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_SEL(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_RES(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MUL(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_RSQRTE(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MSUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_MADD(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_NMSUB(PPCInterpreter_t* cpu, uint32 op);
	void Op_PS_NMADD(PPCInterpreter_t* cpu, uint32 op);

	// FPCC/CR update shared by fcmpu and ps_cmpu0/1; crBitBase is the cr[] index of the field's LT bit
	void CompareUnordered(PPCInterpreter_t* cpu, uint32 crBitBase, double a, double b);
}
