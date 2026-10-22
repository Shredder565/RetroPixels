// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Integer arithmetic, logic, rotate/shift, compare and CR logic
// instructions. Included by PPCInterpreterImpl.cpp only.
//
// Overflow forms (OE=1) set XER[OV] and make XER[SO] sticky; record forms
// (Rc=1) update CR0. Carry is kept in xer_ca.

namespace EspressoItp
{
	inline void SetOverflow(PPCInterpreter_t* cpu, bool overflow)
	{
		cpu->xer_ov = overflow ? 1 : 0;
		if (overflow)
			cpu->xer_so = 1;
	}

	// signed overflow of x + y = sum (both operands same sign, result differs)
	inline bool AddOverflows(uint32 x, uint32 y, uint32 sum)
	{
		return ((~(x ^ y) & (x ^ sum)) >> 31) != 0;
	}

	// writes rD/rA of an XO/X form result and updates CR0 when Rc is set
	inline void FinishResult(PPCInterpreter_t* cpu, uint32 op, uint32 reg, uint32 value)
	{
		cpu->gpr[reg] = value;
		if (op & PPC_OPC_RC)
			ppc_update_cr0(cpu, value);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- add/subtract family ----
	//
	// All of them compute a + b + carryIn, subtracts use a = ~rA. The template
	// parameters pick the second operand, the carry input and which XER bits
	// are written.

	enum class AddB { RegB, Zero, MinusOne };
	enum class AddCarryIn { Zero, One, XerCA };

	template<bool ComplementA, AddB OperandB, AddCarryIn CarryIn, bool WritesCA, bool WritesOV>
	void Op_AddFamily(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 a = cpu->gpr[ppcOpField_A(op)];
		if constexpr (ComplementA)
			a = ~a;
		uint32 b;
		if constexpr (OperandB == AddB::RegB)
			b = cpu->gpr[ppcOpField_B(op)];
		else if constexpr (OperandB == AddB::Zero)
			b = 0;
		else
			b = 0xFFFFFFFF;
		uint32 carryIn;
		if constexpr (CarryIn == AddCarryIn::Zero)
			carryIn = 0;
		else if constexpr (CarryIn == AddCarryIn::One)
			carryIn = 1;
		else
			carryIn = cpu->xer_ca;
		uint64 wide = (uint64)a + (uint64)b + (uint64)carryIn;
		uint32 result = (uint32)wide;
		if constexpr (WritesOV)
			SetOverflow(cpu, AddOverflows(a, b, result));
		if constexpr (WritesCA)
			cpu->xer_ca = (uint8)(wide >> 32);
		FinishResult(cpu, op, ppcOpField_D(op), result);
	}

	// D-form immediates
	inline void Op_ADDI(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		cpu->gpr[ppcOpField_D(op)] = (rA ? cpu->gpr[rA] : 0) + ppcOpSImm16(op);
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_ADDIS(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		cpu->gpr[ppcOpField_D(op)] = (rA ? cpu->gpr[rA] : 0) + (op << 16);
		PPCInterpreter_nextInstruction(cpu);
	}

	// addic and addic. (the latter always records)
	template<bool Record>
	void Op_ADDIC(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 a = cpu->gpr[ppcOpField_A(op)];
		uint32 result = a + ppcOpSImm16(op);
		cpu->xer_ca = result < a ? 1 : 0;
		cpu->gpr[ppcOpField_D(op)] = result;
		if constexpr (Record)
			ppc_update_cr0(cpu, result);
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_SUBFIC(PPCInterpreter_t* cpu, uint32 op)
	{
		uint64 wide = (uint64)(uint32)~cpu->gpr[ppcOpField_A(op)] + (uint64)ppcOpSImm16(op) + 1;
		cpu->xer_ca = (uint8)(wide >> 32);
		cpu->gpr[ppcOpField_D(op)] = (uint32)wide;
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_NEG(PPCInterpreter_t* cpu, uint32 op)
	{
		FinishResult(cpu, op, ppcOpField_D(op), 0u - cpu->gpr[ppcOpField_A(op)]);
	}

	inline void Op_NEGO(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 a = cpu->gpr[ppcOpField_A(op)];
		SetOverflow(cpu, a == 0x80000000);
		FinishResult(cpu, op, ppcOpField_D(op), 0u - a);
	}

	// ---- multiply/divide ----

	inline void Op_MULLI(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->gpr[ppcOpField_D(op)] = cpu->gpr[ppcOpField_A(op)] * ppcOpSImm16(op);
		PPCInterpreter_nextInstruction(cpu);
	}

	template<bool WritesOV>
	void Op_MULLW(PPCInterpreter_t* cpu, uint32 op)
	{
		sint64 product = (sint64)(sint32)cpu->gpr[ppcOpField_A(op)] * (sint64)(sint32)cpu->gpr[ppcOpField_B(op)];
		if constexpr (WritesOV)
			SetOverflow(cpu, product != (sint64)(sint32)product);
		FinishResult(cpu, op, ppcOpField_D(op), (uint32)product);
	}

	// mulhw/mulhwu ignore OE
	inline void Op_MULHW(PPCInterpreter_t* cpu, uint32 op)
	{
		sint64 product = (sint64)(sint32)cpu->gpr[ppcOpField_A(op)] * (sint64)(sint32)cpu->gpr[ppcOpField_B(op)];
		FinishResult(cpu, op, ppcOpField_D(op), (uint32)((uint64)product >> 32));
	}

	inline void Op_MULHWU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint64 product = (uint64)cpu->gpr[ppcOpField_A(op)] * (uint64)cpu->gpr[ppcOpField_B(op)];
		FinishResult(cpu, op, ppcOpField_D(op), (uint32)(product >> 32));
	}

	// the results for the undefined cases (division by zero, 0x80000000 / -1)
	// match what the Espresso produces
	template<bool WritesOV>
	void Op_DIVW(PPCInterpreter_t* cpu, uint32 op)
	{
		sint32 dividend = (sint32)cpu->gpr[ppcOpField_A(op)];
		sint32 divisor = (sint32)cpu->gpr[ppcOpField_B(op)];
		uint32 result;
		bool overflow;
		if (divisor == 0)
		{
			result = dividend < 0 ? 0xFFFFFFFF : 0;
			overflow = true;
		}
		else if (dividend == INT32_MIN && divisor == -1)
		{
			result = 0xFFFFFFFF;
			overflow = true;
		}
		else
		{
			result = (uint32)(dividend / divisor);
			overflow = false;
		}
		if constexpr (WritesOV)
			SetOverflow(cpu, overflow);
		FinishResult(cpu, op, ppcOpField_D(op), result);
	}

	template<bool WritesOV>
	void Op_DIVWU(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 dividend = cpu->gpr[ppcOpField_A(op)];
		uint32 divisor = cpu->gpr[ppcOpField_B(op)];
		uint32 result = divisor == 0 ? 0 : dividend / divisor;
		if constexpr (WritesOV)
			SetOverflow(cpu, divisor == 0);
		FinishResult(cpu, op, ppcOpField_D(op), result);
	}

	// ---- logic (X form: rS in the D field, result to rA) ----

	template<typename TFunc>
	inline void LogicOp(PPCInterpreter_t* cpu, uint32 op, TFunc func)
	{
		uint32 s = cpu->gpr[ppcOpField_D(op)];
		uint32 b = cpu->gpr[ppcOpField_B(op)];
		FinishResult(cpu, op, ppcOpField_A(op), func(s, b));
	}

	inline void Op_AND(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return s & b; }); }
	inline void Op_ANDC(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return s & ~b; }); }
	inline void Op_OR(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return s | b; }); }
	inline void Op_ORC(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return s | ~b; }); }
	inline void Op_XOR(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return s ^ b; }); }
	inline void Op_NAND(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return ~(s & b); }); }
	inline void Op_NOR(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return ~(s | b); }); }
	inline void Op_EQV(PPCInterpreter_t* cpu, uint32 op) { LogicOp(cpu, op, [](uint32 s, uint32 b) { return ~(s ^ b); }); }

	inline void Op_EXTSB(PPCInterpreter_t* cpu, uint32 op)
	{
		FinishResult(cpu, op, ppcOpField_A(op), (uint32)(sint32)(sint8)(uint8)cpu->gpr[ppcOpField_D(op)]);
	}

	inline void Op_EXTSH(PPCInterpreter_t* cpu, uint32 op)
	{
		FinishResult(cpu, op, ppcOpField_A(op), (uint32)(sint32)(sint16)(uint16)cpu->gpr[ppcOpField_D(op)]);
	}

	inline void Op_CNTLZW(PPCInterpreter_t* cpu, uint32 op)
	{
		FinishResult(cpu, op, ppcOpField_A(op), (uint32)std::countl_zero(cpu->gpr[ppcOpField_D(op)]));
	}

	// D-form logic immediates; andi./andis. always record
	enum class ImmLogic { Or, OrShifted, Xor, XorShifted, And, AndShifted };

	template<ImmLogic Kind>
	void Op_LogicImm(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 imm = op & 0xFFFF;
		uint32 s = cpu->gpr[ppcOpField_D(op)];
		uint32 result;
		if constexpr (Kind == ImmLogic::Or) result = s | imm;
		else if constexpr (Kind == ImmLogic::OrShifted) result = s | (imm << 16);
		else if constexpr (Kind == ImmLogic::Xor) result = s ^ imm;
		else if constexpr (Kind == ImmLogic::XorShifted) result = s ^ (imm << 16);
		else if constexpr (Kind == ImmLogic::And) result = s & imm;
		else result = s & (imm << 16);
		cpu->gpr[ppcOpField_A(op)] = result;
		if constexpr (Kind == ImmLogic::And || Kind == ImmLogic::AndShifted)
			ppc_update_cr0(cpu, result);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- rotate and shift ----

	inline void Op_RLWIMI(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 mask = ppc_mask((int)ppcOpField_C(op), (int)ppcOpField_E(op));
		uint32 rotated = std::rotl(cpu->gpr[ppcOpField_D(op)], (int)ppcOpField_B(op));
		uint32 rA = ppcOpField_A(op);
		FinishResult(cpu, op, rA, (rotated & mask) | (cpu->gpr[rA] & ~mask));
	}

	inline void Op_RLWINM(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 mask = ppc_mask((int)ppcOpField_C(op), (int)ppcOpField_E(op));
		FinishResult(cpu, op, ppcOpField_A(op), std::rotl(cpu->gpr[ppcOpField_D(op)], (int)ppcOpField_B(op)) & mask);
	}

	inline void Op_RLWNM(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 mask = ppc_mask((int)ppcOpField_C(op), (int)ppcOpField_E(op));
		uint32 amount = cpu->gpr[ppcOpField_B(op)] & 0x1F;
		FinishResult(cpu, op, ppcOpField_A(op), std::rotl(cpu->gpr[ppcOpField_D(op)], (int)amount) & mask);
	}

	// shift amounts use 6 bits, anything from 32 up shifts everything out
	inline void Op_SLW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 amount = cpu->gpr[ppcOpField_B(op)] & 0x3F;
		uint32 s = cpu->gpr[ppcOpField_D(op)];
		FinishResult(cpu, op, ppcOpField_A(op), amount >= 32 ? 0 : s << amount);
	}

	inline void Op_SRW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 amount = cpu->gpr[ppcOpField_B(op)] & 0x3F;
		uint32 s = cpu->gpr[ppcOpField_D(op)];
		FinishResult(cpu, op, ppcOpField_A(op), amount >= 32 ? 0 : s >> amount);
	}

	// arithmetic right shift; CA is set when a negative value loses 1 bits
	inline void ShiftRightAlgebraic(PPCInterpreter_t* cpu, uint32 op, uint32 amount)
	{
		sint32 s = (sint32)cpu->gpr[ppcOpField_D(op)];
		uint32 result;
		bool lostOnes;
		if (amount >= 32)
		{
			result = (uint32)(s >> 31);
			lostOnes = s < 0;
		}
		else
		{
			result = (uint32)(s >> amount);
			lostOnes = s < 0 && ((uint32)s & ((1u << amount) - 1)) != 0;
		}
		cpu->xer_ca = lostOnes ? 1 : 0;
		FinishResult(cpu, op, ppcOpField_A(op), result);
	}

	inline void Op_SRAW(PPCInterpreter_t* cpu, uint32 op)
	{
		ShiftRightAlgebraic(cpu, op, cpu->gpr[ppcOpField_B(op)] & 0x3F);
	}

	inline void Op_SRAWI(PPCInterpreter_t* cpu, uint32 op)
	{
		ShiftRightAlgebraic(cpu, op, ppcOpField_B(op));
	}

	// ---- compare ----

	template<typename T>
	inline void SetCompareField(PPCInterpreter_t* cpu, uint32 op, T a, T b)
	{
		uint8* field = cpu->cr + (ppcOpField_D(op) >> 2) * 4;
		field[CR_BIT_LT] = a < b ? 1 : 0;
		field[CR_BIT_GT] = a > b ? 1 : 0;
		field[CR_BIT_EQ] = a == b ? 1 : 0;
		field[CR_BIT_SO] = cpu->xer_so;
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_CMP(PPCInterpreter_t* cpu, uint32 op)
	{
		SetCompareField<sint32>(cpu, op, (sint32)cpu->gpr[ppcOpField_A(op)], (sint32)cpu->gpr[ppcOpField_B(op)]);
	}

	inline void Op_CMPL(PPCInterpreter_t* cpu, uint32 op)
	{
		SetCompareField<uint32>(cpu, op, cpu->gpr[ppcOpField_A(op)], cpu->gpr[ppcOpField_B(op)]);
	}

	inline void Op_CMPI(PPCInterpreter_t* cpu, uint32 op)
	{
		SetCompareField<sint32>(cpu, op, (sint32)cpu->gpr[ppcOpField_A(op)], (sint32)ppcOpSImm16(op));
	}

	inline void Op_CMPLI(PPCInterpreter_t* cpu, uint32 op)
	{
		SetCompareField<uint32>(cpu, op, cpu->gpr[ppcOpField_A(op)], op & 0xFFFF);
	}

	// ---- CR logic (crbD = f(crbA, crbB)) ----

	template<typename TFunc>
	inline void CRLogic(PPCInterpreter_t* cpu, uint32 op, TFunc func)
	{
		uint8 a = cpu->cr[ppcOpField_A(op)];
		uint8 b = cpu->cr[ppcOpField_B(op)];
		cpu->cr[ppcOpField_D(op)] = (uint8)(func(a, b) & 1);
		PPCInterpreter_nextInstruction(cpu);
	}

	inline void Op_CRAND(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return a & b; }); }
	inline void Op_CRANDC(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return a & ~b; }); }
	inline void Op_CROR(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return a | b; }); }
	inline void Op_CRORC(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return a | ~b; }); }
	inline void Op_CRXOR(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return a ^ b; }); }
	inline void Op_CRNAND(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return ~(a & b); }); }
	inline void Op_CRNOR(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return ~(a | b); }); }
	inline void Op_CREQV(PPCInterpreter_t* cpu, uint32 op) { CRLogic(cpu, op, [](uint8 a, uint8 b) { return ~(a ^ b); }); }
}
