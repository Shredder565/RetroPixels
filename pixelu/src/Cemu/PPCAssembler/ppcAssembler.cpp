// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cemu/ExpressionParser/ExpressionParser.h"
#include "util/helpers/helpers.h"

#include <boost/container/small_vector.hpp>
#include <charconv>

/*
 * Overview
 *
 * Every supported mnemonic has one row in kInstrTable. A row names the primary
 * and extended opcode, fixed bits, an optional predicate on the instruction
 * word and either a list of operand slots (Form::Generic) or one of the older
 * hand written layouts (all other forms).
 *
 * Assembling looks the mnemonic up and fills in the operands. Disassembling
 * scans all rows that match the word and keeps the one with the highest
 * priority; on equal priority the row further down the table wins, so the row
 * order is significant.
 *
 * Operand expressions go through ExpressionParser. When an expression is not
 * constant (it refers to symbols resolved later) a relocation is recorded
 * instead of an error.
 */

namespace
{
	/* ---------------------------------------------------------------- */
	/* name table                                                       */
	/* ---------------------------------------------------------------- */

	struct OpName
	{
		PPCASM_OP op;
		const char* name;
	};

	constexpr OpName kOpNames[] =
	{
		{ PPCASM_OP_UKN, "UKN" },
		// integer arithmetic
		{ PPCASM_OP_ADDI, "ADDI" }, { PPCASM_OP_SUBI, "SUBI" }, { PPCASM_OP_ADDIS, "ADDIS" }, { PPCASM_OP_ADDIC, "ADDIC" }, { PPCASM_OP_ADDIC_, "ADDIC." },
		{ PPCASM_OP_ADD, "ADD" }, { PPCASM_OP_ADD_, "ADD." }, { PPCASM_OP_SUBF, "SUBF" }, { PPCASM_OP_SUBF_, "SUBF." }, { PPCASM_OP_SUBFC, "SUBFC" }, { PPCASM_OP_SUBFC_, "SUBFC." },
		{ PPCASM_OP_SUBFE, "SUBFE" }, { PPCASM_OP_SUBFE_, "SUBFE." }, { PPCASM_OP_SUBFIC, "SUBFIC" }, { PPCASM_OP_SUB, "SUB" }, { PPCASM_OP_SUB_, "SUB." },
		{ PPCASM_OP_MULLI, "MULLI" }, { PPCASM_OP_MULLW, "MULLW" }, { PPCASM_OP_MULLW_, "MULLW." }, { PPCASM_OP_MULHW, "MULHW" }, { PPCASM_OP_MULHW_, "MULHW." },
		{ PPCASM_OP_MULHWU, "MULHWU" }, { PPCASM_OP_MULHWU_, "MULHWU." }, { PPCASM_OP_DIVW, "DIVW" }, { PPCASM_OP_DIVW_, "DIVW." }, { PPCASM_OP_DIVWU, "DIVWU" }, { PPCASM_OP_DIVWU_, "DIVWU." },
		// logic
		{ PPCASM_OP_AND, "AND" }, { PPCASM_OP_AND_, "AND." }, { PPCASM_OP_ANDC, "ANDC" }, { PPCASM_OP_ANDC_, "ANDC." }, { PPCASM_OP_ANDI_, "ANDI." }, { PPCASM_OP_ANDIS_, "ANDIS." },
		{ PPCASM_OP_OR, "OR" }, { PPCASM_OP_OR_, "OR." }, { PPCASM_OP_ORI, "ORI" }, { PPCASM_OP_ORIS, "ORIS" }, { PPCASM_OP_ORC, "ORC" }, { PPCASM_OP_XOR, "XOR" },
		{ PPCASM_OP_NOR, "NOR" }, { PPCASM_OP_NOR_, "NOR." }, { PPCASM_OP_NOT, "NOT" }, { PPCASM_OP_NOT_, "NOT." }, { PPCASM_OP_NEG, "NEG" }, { PPCASM_OP_NEG_, "NEG." },
		{ PPCASM_OP_XORI, "XORI" }, { PPCASM_OP_XORIS, "XORIS" },
		// shifts and rotates
		{ PPCASM_OP_SRAW, "SRAW" }, { PPCASM_OP_SRAW_, "SRAW." }, { PPCASM_OP_SRAWI, "SRAWI" }, { PPCASM_OP_SRAWI_, "SRAWI." }, { PPCASM_OP_SLW, "SLW" }, { PPCASM_OP_SLW_, "SLW." },
		{ PPCASM_OP_SRW, "SRW" }, { PPCASM_OP_SRW_, "SRW." }, { PPCASM_OP_RLWINM, "RLWINM" }, { PPCASM_OP_RLWINM_, "RLWINM." }, { PPCASM_OP_RLWIMI, "RLWIMI" }, { PPCASM_OP_RLWIMI_, "RLWIMI." },
		{ PPCASM_OP_EXTLWI, "EXTLWI" }, { PPCASM_OP_EXTLWI_, "EXTLWI." }, { PPCASM_OP_EXTRWI, "EXTRWI" }, { PPCASM_OP_EXTRWI_, "EXTRWI." },
		{ PPCASM_OP_ROTLWI, "ROTLWI" }, { PPCASM_OP_ROTLWI_, "ROTLWI." }, { PPCASM_OP_ROTRWI, "ROTRWI" }, { PPCASM_OP_ROTRWI_, "ROTRWI." },
		{ PPCASM_OP_SLWI, "SLWI" }, { PPCASM_OP_SLWI_, "SLWI." }, { PPCASM_OP_SRWI, "SRWI" }, { PPCASM_OP_SRWI_, "SRWI." },
		{ PPCASM_OP_CLRLWI, "CLRLWI" }, { PPCASM_OP_CLRLWI_, "CLRLWI." }, { PPCASM_OP_CLRRWI, "CLRRWI" }, { PPCASM_OP_CLRRWI_, "CLRRWI." },
		{ PPCASM_OP_RLWNM, "RLWNM" }, { PPCASM_OP_RLWNM_, "RLWNM." }, { PPCASM_OP_ROTLW, "ROTLW" }, { PPCASM_OP_ROTLW_, "ROTLW." },
		// compare, moves
		{ PPCASM_OP_CMPWI, "CMPWI" }, { PPCASM_OP_CMPLWI, "CMPLWI" }, { PPCASM_OP_CMPW, "CMPW" }, { PPCASM_OP_CMPLW, "CMPLW" },
		{ PPCASM_OP_MR, "MR" }, { PPCASM_OP_MR_, "MR." },
		{ PPCASM_OP_EXTSB, "EXTSB" }, { PPCASM_OP_EXTSH, "EXTSH" }, { PPCASM_OP_CNTLZW, "CNTLZW" }, { PPCASM_OP_EXTSB_, "EXTSB." }, { PPCASM_OP_EXTSH_, "EXTSH." }, { PPCASM_OP_CNTLZW_, "CNTLZW." },
		{ PPCASM_OP_MFSPR, "MFSPR" }, { PPCASM_OP_MTSPR, "MTSPR" },
		// integer load/store
		{ PPCASM_OP_LMW, "LMW" }, { PPCASM_OP_LWZ, "LWZ" }, { PPCASM_OP_LWZU, "LWZU" }, { PPCASM_OP_LWZX, "LWZX" }, { PPCASM_OP_LWZUX, "LWZUX" },
		{ PPCASM_OP_LHZ, "LHZ" }, { PPCASM_OP_LHZU, "LHZU" }, { PPCASM_OP_LHZX, "LHZX" }, { PPCASM_OP_LHZUX, "LHZUX" },
		{ PPCASM_OP_LHA, "LHA" }, { PPCASM_OP_LHAU, "LHAU" }, { PPCASM_OP_LHAX, "LHAX" }, { PPCASM_OP_LHAUX, "LHAUX" },
		{ PPCASM_OP_LBZ, "LBZ" }, { PPCASM_OP_LBZU, "LBZU" }, { PPCASM_OP_LBZX, "LBZX" }, { PPCASM_OP_LBZUX, "LBZUX" },
		{ PPCASM_OP_STMW, "STMW" }, { PPCASM_OP_STW, "STW" }, { PPCASM_OP_STWU, "STWU" }, { PPCASM_OP_STWX, "STWX" }, { PPCASM_OP_STWUX, "STWUX" },
		{ PPCASM_OP_STH, "STH" }, { PPCASM_OP_STHU, "STHU" }, { PPCASM_OP_STHX, "STHX" }, { PPCASM_OP_STHUX, "STHUX" },
		{ PPCASM_OP_STB, "STB" }, { PPCASM_OP_STBU, "STBU" }, { PPCASM_OP_STBX, "STBX" }, { PPCASM_OP_STBUX, "STBUX" },
		{ PPCASM_OP_STSWI, "STSWI" }, { PPCASM_OP_LWBRX, "LWBRX" }, { PPCASM_OP_LHBRX, "LHBRX" }, { PPCASM_OP_STWBRX, "STWBRX" }, { PPCASM_OP_STHBRX, "STHBRX" },
		{ PPCASM_OP_LWARX, "LWARX" }, { PPCASM_OP_STWCX_, "STWCX." },
		// branches
		{ PPCASM_OP_B, "B" }, { PPCASM_OP_BL, "BL" }, { PPCASM_OP_BA, "BA" }, { PPCASM_OP_BLA, "BLA" },
		{ PPCASM_OP_BC, "BC" }, { PPCASM_OP_BNE, "BNE" }, { PPCASM_OP_BEQ, "BEQ" }, { PPCASM_OP_BGE, "BGE" }, { PPCASM_OP_BGT, "BGT" }, { PPCASM_OP_BLT, "BLT" }, { PPCASM_OP_BLE, "BLE" },
		{ PPCASM_OP_BDZ, "BDZ" }, { PPCASM_OP_BDNZ, "BDNZ" },
		{ PPCASM_OP_BLR, "BLR" }, { PPCASM_OP_BLRL, "BLRL" }, { PPCASM_OP_BLTLR, "BLTLR" }, { PPCASM_OP_BLELR, "BLELR" }, { PPCASM_OP_BEQLR, "BEQLR" }, { PPCASM_OP_BGELR, "BGELR" },
		{ PPCASM_OP_BGTLR, "BGTLR" }, { PPCASM_OP_BNELR, "BNELR" }, { PPCASM_OP_BDZLR, "BDZLR" }, { PPCASM_OP_BDNZLR, "BDNZLR" }, { PPCASM_OP_BDZLRL, "BDZLRL" }, { PPCASM_OP_BDNZLRL, "BDNZLRL" },
		{ PPCASM_OP_BCTR, "BCTR" }, { PPCASM_OP_BCTRL, "BCTRL" },
		// floating point load/store
		{ PPCASM_OP_LFS, "LFS" }, { PPCASM_OP_LFSU, "LFSU" }, { PPCASM_OP_LFSX, "LFSX" }, { PPCASM_OP_LFSUX, "LFSUX" },
		{ PPCASM_OP_LFD, "LFD" }, { PPCASM_OP_LFDU, "LFDU" }, { PPCASM_OP_LFDX, "LFDX" }, { PPCASM_OP_LFDUX, "LFDUX" },
		{ PPCASM_OP_STFS, "STFS" }, { PPCASM_OP_STFSU, "STFSU" }, { PPCASM_OP_STFSX, "STFSX" }, { PPCASM_OP_STFSUX, "STFSUX" },
		{ PPCASM_OP_STFD, "STFD" }, { PPCASM_OP_STFDU, "STFDU" }, { PPCASM_OP_STFDX, "STFDX" }, { PPCASM_OP_STFDUX, "STFDUX" },
		{ PPCASM_OP_STFIWX, "STFIWX" },
		// paired singles
		{ PPCASM_OP_PS_MERGE00, "PS_MERGE00" }, { PPCASM_OP_PS_MERGE01, "PS_MERGE01" }, { PPCASM_OP_PS_MERGE10, "PS_MERGE10" }, { PPCASM_OP_PS_MERGE11, "PS_MERGE11" },
		{ PPCASM_OP_PS_ADD, "PS_ADD" }, { PPCASM_OP_PS_SUB, "PS_SUB" }, { PPCASM_OP_PS_DIV, "PS_DIV" }, { PPCASM_OP_PS_MUL, "PS_MUL" },
		{ PPCASM_OP_PS_MADD, "PS_MADD" }, { PPCASM_OP_PS_MSUB, "PS_MSUB" }, { PPCASM_OP_PS_NMADD, "PS_NMADD" }, { PPCASM_OP_PS_NMSUB, "PS_NMSUB" },
		// floating point arithmetic
		{ PPCASM_OP_FMR, "FMR" }, { PPCASM_OP_FABS, "FABS" }, { PPCASM_OP_FNEG, "FNEG" }, { PPCASM_OP_FRSP, "FRSP" }, { PPCASM_OP_FRSQRTE, "FRSQRTE" },
		{ PPCASM_OP_FADD, "FADD" }, { PPCASM_OP_FADDS, "FADDS" }, { PPCASM_OP_FSUB, "FSUB" }, { PPCASM_OP_FSUBS, "FSUBS" },
		{ PPCASM_OP_FMUL, "FMUL" }, { PPCASM_OP_FMULS, "FMULS" }, { PPCASM_OP_FDIV, "FDIV" }, { PPCASM_OP_FDIVS, "FDIVS" },
		{ PPCASM_OP_FMADD, "FMADD" }, { PPCASM_OP_FMADDS, "FMADDS" }, { PPCASM_OP_FNMADD, "FNMADD" }, { PPCASM_OP_FNMADDS, "FNMADDS" },
		{ PPCASM_OP_FMSUB, "FMSUB" }, { PPCASM_OP_FMSUBS, "FMSUBS" }, { PPCASM_OP_FNMSUB, "FNMSUB" }, { PPCASM_OP_FNMSUBS, "FNMSUBS" },
		{ PPCASM_OP_FCTIWZ, "FCTIWZ" }, { PPCASM_OP_FCMPU, "FCMPU" }, { PPCASM_OP_FCMPO, "FCMPO" },
		// misc
		{ PPCASM_OP_ISYNC, "ISYNC" }, { PPCASM_OP_NOP, "NOP" }, { PPCASM_OP_LI, "LI" }, { PPCASM_OP_LIS, "LIS" },
		{ PPCASM_OP_MFLR, "MFLR" }, { PPCASM_OP_MTLR, "MTLR" }, { PPCASM_OP_MFCTR, "MFCTR" }, { PPCASM_OP_MTCTR, "MTCTR" },
		{ PPCASM_OP_MFCR, "MFCR" }, { PPCASM_OP_MTCRF, "MTCRF" }, { PPCASM_OP_MCRF, "MCRF" },
		// condition register logic
		{ PPCASM_OP_CROR, "CROR" }, { PPCASM_OP_CRNOR, "CRNOR" }, { PPCASM_OP_CRORC, "CRORC" }, { PPCASM_OP_CRXOR, "CRXOR" }, { PPCASM_OP_CREQV, "CREQV" },
		{ PPCASM_OP_CRAND, "CRAND" }, { PPCASM_OP_CRNAND, "CRNAND" }, { PPCASM_OP_CRANDC, "CRANDC" },
		{ PPCASM_OP_CRSET, "CRSET" }, { PPCASM_OP_CRCLR, "CRCLR" }, { PPCASM_OP_CRMOVE, "CRMOVE" }, { PPCASM_OP_CRNOT, "CRNOT" },
	};

	/* ---------------------------------------------------------------- */
	/* instruction word helpers                                         */
	/* ---------------------------------------------------------------- */

	inline uint32 Field5(uint32 word, uint32 lsb) { return (word >> lsb) & 0x1F; }
	inline void PutField5(uint32& word, uint32 lsb, uint32 value) { word = (word & ~(0x1Fu << lsb)) | ((value & 0x1F) << lsb); }

	inline uint32 SignExtend16(uint32 v) { return (uint32)(sint32)(sint16)(uint16)v; }

	// sign extend the low `bits` bits of v
	inline uint32 SignExtendBits(uint32 v, uint32 bits)
	{
		const uint32 shift = 32 - bits;
		return (uint32)((sint32)(v << shift) >> shift);
	}

	// would `value` survive being stored into `bits` bits (signed or unsigned)?
	bool FitsInBits(uint32 value, uint32 bits, bool asSigned)
	{
		const uint32 shift = 32 - bits;
		if (asSigned)
			return (uint32)((sint32)(value << shift) >> shift) == value;
		return ((value << shift) >> shift) == value;
	}

	// double -> integer truncation the way x86 cvttsd2si behaves (out of range and
	// NaN produce the "integer indefinite" value instead of being undefined)
	sint32 TruncToS32(double d)
	{
		if (!(d > -2147483649.0 && d < 2147483648.0))
			return std::numeric_limits<sint32>::min();
		return (sint32)d;
	}

	sint64 TruncToS64(double d)
	{
		if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
			return std::numeric_limits<sint64>::min();
		return (sint64)d;
	}

	// rotate field decoding (rlwinm family)
	struct RotateFields
	{
		sint32 rS, rA, sh, mb, me;
		explicit RotateFields(uint32 w) : rS((sint32)Field5(w, 21)), rA((sint32)Field5(w, 16)), sh((sint32)Field5(w, 11)), mb((sint32)Field5(w, 6)), me((sint32)Field5(w, 1)) {}
	};

	/* ---------------------------------------------------------------- */
	/* table definitions                                                */
	/* ---------------------------------------------------------------- */

	enum class Form : uint8
	{
		Generic,        // operand slots described in the row
		CompareReg,     // [crN,] rA, rB
		ShiftImm,       // rA, rS, SH (srawi)
		CondBranch,     // bc BO, BI, target | bxx [crN,] target
		Branch,         // b/bl/ba/bla target
		CondReturn,     // bxxlr [crN]
		CompareImm,     // [crN,] rA, simm
		Bare,           // no operands
		CompareFp,      // [crN,] fA, fB
		Rotate,         // rA, rS, SH, MB, ME
		RotateAlias,    // extlwi, extrwi, slwi, srwi, clrlwi, clrrwi
		RotateReg,      // rA, rS, rB, MB, ME
		RotateRegAlias, // rotlw
		CrLogic,        // crbD, crbA, crbB
	};

	// extra test on the instruction word that a row requires to match
	enum class Rule : uint8 { Any, ExtLwi, ExtRwi, SlWi, SrWi, ClrLWi, ClrRWi, FullMask };

	bool RuleHolds(Rule rule, uint32 word)
	{
		const RotateFields f(word);
		switch (rule)
		{
		case Rule::Any: return true;
		case Rule::ExtLwi: return f.mb == 0;
		case Rule::ExtRwi:
		{
			if (f.me != 31)
				return false;
			const sint8 n = (sint8)(32 - f.mb);
			return (sint8)(f.sh - n) >= 0;
		}
		case Rule::SlWi: return f.mb == 0 && f.sh == 31 - f.me;
		case Rule::SrWi: return f.me == 31 && 32 - f.sh == f.mb;
		case Rule::ClrLWi: return f.sh == 0 && f.me == 31;
		case Rule::ClrRWi: return f.sh == 0 && f.mb == 0;
		case Rule::FullMask: return f.mb == 0 && f.me == 31;
		}
		return false;
	}

	// operand slot of a Generic row
	enum class Slot : uint8 { Unused, Gpr, GprOrZero, Fpr, CrField, Spr, Imm, FromBase, Mem };

	struct SlotDef
	{
		Slot kind{ Slot::Unused };
		uint8 lsb{};        // bit position of the field
		uint8 width{};      // Imm: field width. FromBase: the base value
		bool isSigned{};
		bool negate{};      // Imm: operand is negated before encoding (subi)
		bool allowUnsigned{}; // Imm: signed field that also takes unsigned values (li, lis, oris...)
	};

	constexpr SlotDef Gpr(uint8 lsb) { return { Slot::Gpr, lsb }; }
	constexpr SlotDef GprOr0(uint8 lsb) { return { Slot::GprOrZero, lsb }; }
	constexpr SlotDef Fpr(uint8 lsb) { return { Slot::Fpr, lsb }; }
	constexpr SlotDef CrField(uint8 lsb) { return { Slot::CrField, lsb }; }
	constexpr SlotDef SprNum() { return { Slot::Spr }; }
	constexpr SlotDef Mem() { return { Slot::Mem }; }
	constexpr SlotDef UImm(uint8 lsb, uint8 width) { return { Slot::Imm, lsb, width, false }; }
	constexpr SlotDef SImm(uint8 lsb, uint8 width) { return { Slot::Imm, lsb, width, true }; }
	constexpr SlotDef SImmNeg(uint8 lsb, uint8 width) { return { Slot::Imm, lsb, width, true, true }; }
	constexpr SlotDef SImmOrU(uint8 lsb, uint8 width) { return { Slot::Imm, lsb, width, true, false, true }; }
	constexpr SlotDef FromBase(uint8 lsb, uint8 base) { return { Slot::FromBase, lsb, base }; }

	// fixed relationship between instruction fields of a Generic row
	enum class Tie : uint8 { None, SameField, FieldIs, SprIs, SignIs };

	struct TieDef
	{
		Tie kind{ Tie::None };
		uint8 a{};   // SameField: source lsb. FieldIs/SignIs: lsb
		uint16 b{};  // SameField: destination lsb. FieldIs/SignIs: value. SprIs: spr number
	};

	constexpr TieDef SameField(uint8 srcLsb, uint8 dstLsb) { return { Tie::SameField, srcLsb, dstLsb }; }
	constexpr TieDef FieldIs(uint8 lsb, uint8 value) { return { Tie::FieldIs, lsb, value }; }
	constexpr TieDef SprIs(uint16 spr) { return { Tie::SprIs, 0, spr }; }
	constexpr TieDef SignIs(uint8 lsb, uint8 value) { return { Tie::SignIs, lsb, value }; }

	// marks a 10 bit extended opcode in group 63 (default there is 5 bits)
	constexpr sint32 X10(sint32 xo) { return xo | 0x10000; }

	// fixed bit masks / values
	constexpr uint32 kRc = 1u;            // record bit
	constexpr uint32 kLk = 1u;            // link bit
	constexpr uint32 kAa = 1u << 1;       // absolute address bit
	constexpr uint32 kOe = 1u << 10;      // overflow enable
	constexpr uint32 kBo = 0x1Fu << 21;   // whole BO field
	constexpr uint32 kBoNoHint = 0x1Eu << 21; // BO without the hint bit
	constexpr uint32 kBiCond = 0x3u << 16; // condition within the CR field
	constexpr uint32 kIfClear = 0x04u << 21;
	constexpr uint32 kIfSet = 0x0Cu << 21;
	constexpr uint32 kAlways = 0x14u << 21;
	constexpr uint32 kDecNz = 0x10u << 21;
	constexpr uint32 kDecZ = 0x12u << 21;
	constexpr uint32 kLt = 0u << 16;
	constexpr uint32 kGt = 1u << 16;
	constexpr uint32 kEq = 2u << 16;

	struct InstrDef
	{
		PPCASM_OP op;
		uint8 priority;
		sint32 primary;  // bits 0..5
		sint32 xo;       // extended opcode, -1 = none
		sint32 xo2;      // second extended opcode (ps_merge), -1 = none
		Form form;
		uint32 fixedMask;
		uint32 fixedBits;
		Rule rule;
		std::array<SlotDef, 4> slots;
		std::array<TieDef, 3> ties;
	};


	const InstrDef kInstrTable[] =
	{
	{ PPCASM_OP_PS_MERGE00, 0, 4, 16, 16, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_MERGE01, 0, 4, 16, 17, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_MERGE10, 0, 4, 16, 18, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_MERGE11, 0, 4, 16, 19, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_DIV, 0, 4, 18, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_SUB, 0, 4, 20, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_ADD, 0, 4, 21, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_MUL, 0, 4, 25, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6) }, {  } },
	{ PPCASM_OP_PS_MSUB, 0, 4, 28, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_MADD, 0, 4, 29, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_NMSUB, 0, 4, 30, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_PS_NMADD, 0, 4, 31, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_MULLI, 0, 7, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImm(0, 16) }, {  } },
	{ PPCASM_OP_SUBFIC, 0, 8, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImm(0, 16) }, {  } },
	{ PPCASM_OP_CMPLWI, 0, 10, -1, -1, Form::CompareImm, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CMPWI, 0, 11, -1, -1, Form::CompareImm, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_ADDIC, 0, 12, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImm(0, 16) }, {  } },
	{ PPCASM_OP_ADDIC_, 0, 13, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImm(0, 16) }, {  } },
	{ PPCASM_OP_ADDI, 0, 14, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImm(0, 16) }, {  } },
	{ PPCASM_OP_SUBI, 1, 14, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImmNeg(0, 16) }, { SignIs(15, 1) } },
	{ PPCASM_OP_LI, 1, 14, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), SImmOrU(0, 16) }, { FieldIs(16, 0) } },
	{ PPCASM_OP_ADDIS, 0, 15, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), SImmOrU(0, 16) }, {  } },
	{ PPCASM_OP_LIS, 1, 15, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), SImmOrU(0, 16) }, { FieldIs(16, 0) } },
	{ PPCASM_OP_BC, 0, 16, -1, -1, Form::CondBranch, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BNE, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfClear | kEq, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BEQ, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfSet | kEq, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BGE, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfClear | kLt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BGT, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfSet | kGt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLT, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfSet | kLt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLE, 1, 16, -1, -1, Form::CondBranch, kBoNoHint | kBiCond, kIfClear | kGt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDZ, 1, 16, -1, -1, Form::CondBranch, kBo, kDecZ, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDNZ, 1, 16, -1, -1, Form::CondBranch, kBo, kDecNz, Rule::Any, {  }, {  } },
	{ PPCASM_OP_B, 0, 18, -1, -1, Form::Branch, kLk | kAa, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BL, 0, 18, -1, -1, Form::Branch, kLk | kAa, kLk, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BA, 0, 18, -1, -1, Form::Branch, kLk | kAa, kAa, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLA, 0, 18, -1, -1, Form::Branch, kLk | kAa, kLk | kAa, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLR, 0, 19, 16, -1, Form::Bare, kBo | kLk, kAlways, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLRL, 0, 19, 16, -1, Form::Bare, kBo | kLk, kAlways | kLk, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLTLR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfSet | kLt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BGTLR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfSet | kGt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BEQLR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfSet | kEq, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BLELR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfClear | kGt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BGELR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfClear | kLt, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BNELR, 0, 19, 16, -1, Form::CondReturn, kBo | kBiCond | kLk, kIfClear | kEq, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDZLR, 0, 19, 16, -1, Form::Bare, kBo | kLk, kDecZ, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDNZLR, 0, 19, 16, -1, Form::Bare, kBo | kLk, kDecNz, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDZLRL, 0, 19, 16, -1, Form::Bare, kBo | kLk, kDecZ | kLk, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BDNZLRL, 0, 19, 16, -1, Form::Bare, kBo | kLk, kDecNz | kLk, Rule::Any, {  }, {  } },
	{ PPCASM_OP_ISYNC, 0, 19, 150, -1, Form::Bare, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRNOR, 0, 19, 33, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRANDC, 0, 19, 129, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRXOR, 0, 19, 193, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRNAND, 0, 19, 255, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRAND, 0, 19, 257, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CREQV, 0, 19, 289, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CRORC, 0, 19, 417, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CROR, 0, 19, 449, -1, Form::CrLogic, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BCTR, 0, 19, 528, -1, Form::Bare, kBo | kLk, kAlways, Rule::Any, {  }, {  } },
	{ PPCASM_OP_BCTRL, 0, 19, 528, -1, Form::Bare, kBo | kLk, kAlways | kLk, Rule::Any, {  }, {  } },
	{ PPCASM_OP_RLWIMI, 0, 20, -1, -1, Form::Rotate, kRc, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_RLWIMI_, 0, 20, -1, -1, Form::Rotate, kRc, kRc, Rule::Any, {  }, {  } },
	{ PPCASM_OP_RLWINM, 0, 21, -1, -1, Form::Rotate, kRc, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_RLWINM_, 0, 21, -1, -1, Form::Rotate, kRc, kRc, Rule::Any, {  }, {  } },
	{ PPCASM_OP_ROTLWI, 2, 21, -1, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), UImm(11, 5) }, { FieldIs(6, 0), FieldIs(1, 31) } },
	{ PPCASM_OP_ROTLWI_, 2, 21, -1, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), UImm(11, 5) }, { FieldIs(6, 0), FieldIs(1, 31) } },
	{ PPCASM_OP_ROTRWI, 0, 21, -1, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), FromBase(11, 32) }, { FieldIs(6, 0), FieldIs(1, 31) } },
	{ PPCASM_OP_ROTRWI_, 0, 21, -1, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), FromBase(11, 32) }, { FieldIs(6, 0), FieldIs(1, 31) } },
	{ PPCASM_OP_EXTLWI, 1, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::ExtLwi, {  }, {  } },
	{ PPCASM_OP_EXTLWI_, 1, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::ExtLwi, {  }, {  } },
	{ PPCASM_OP_EXTRWI, 1, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::ExtRwi, {  }, {  } },
	{ PPCASM_OP_EXTRWI_, 1, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::ExtRwi, {  }, {  } },
	{ PPCASM_OP_SLWI, 2, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::SlWi, {  }, {  } },
	{ PPCASM_OP_SLWI_, 2, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::SlWi, {  }, {  } },
	{ PPCASM_OP_SRWI, 2, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::SrWi, {  }, {  } },
	{ PPCASM_OP_SRWI_, 2, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::SrWi, {  }, {  } },
	{ PPCASM_OP_CLRLWI, 2, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::ClrLWi, {  }, {  } },
	{ PPCASM_OP_CLRLWI_, 2, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::ClrLWi, {  }, {  } },
	{ PPCASM_OP_CLRRWI, 2, 21, -1, -1, Form::RotateAlias, kRc, 0, Rule::ClrRWi, {  }, {  } },
	{ PPCASM_OP_CLRRWI_, 2, 21, -1, -1, Form::RotateAlias, kRc, kRc, Rule::ClrRWi, {  }, {  } },
	{ PPCASM_OP_RLWNM, 0, 23, -1, -1, Form::RotateReg, kRc, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_RLWNM_, 0, 23, -1, -1, Form::RotateReg, kRc, kRc, Rule::Any, {  }, {  } },
	{ PPCASM_OP_ROTLW, 0, 23, -1, -1, Form::RotateRegAlias, kRc, 0, Rule::FullMask, {  }, {  } },
	{ PPCASM_OP_ROTLW_, 0, 23, -1, -1, Form::RotateRegAlias, kRc, kRc, Rule::FullMask, {  }, {  } },
	{ PPCASM_OP_ORI, 0, 24, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), UImm(0, 16) }, {  } },
	{ PPCASM_OP_NOP, 1, 24, -1, -1, Form::Bare, 0x3FFFFFF, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_ORIS, 0, 25, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), SImmOrU(0, 16) }, {  } },
	{ PPCASM_OP_XORI, 0, 26, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), UImm(0, 16) }, {  } },
	{ PPCASM_OP_XORIS, 0, 27, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), SImmOrU(0, 16) }, {  } },
	{ PPCASM_OP_ANDI_, 0, 28, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), UImm(0, 16) }, {  } },
	{ PPCASM_OP_ANDIS_, 0, 29, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), SImmOrU(0, 16) }, {  } },
	{ PPCASM_OP_CMPW, 0, 31, 0, -1, Form::CompareReg, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_SUBFC, 1, 31, 8, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SUBFC_, 1, 31, 8, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MULHWU, 0, 31, 11, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MULHWU_, 0, 31, 11, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_CMPLW, 0, 31, 32, -1, Form::CompareReg, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_SUBF, 1, 31, 40, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SUBF_, 1, 31, 40, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SUB, 0, 31, 40, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(11), Gpr(16) }, {  } },
	{ PPCASM_OP_SUB_, 0, 31, 40, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(11), Gpr(16) }, {  } },
	{ PPCASM_OP_MULHW, 0, 31, 75, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MULHW_, 0, 31, 75, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_NEG, 0, 31, 104, -1, Form::Generic, kRc | kOe, 0, Rule::Any, { Gpr(21), Gpr(16) }, {  } },
	{ PPCASM_OP_NEG_, 0, 31, 104, -1, Form::Generic, kRc | kOe, kRc, Rule::Any, { Gpr(21), Gpr(16) }, {  } },
	{ PPCASM_OP_NOR, 0, 31, 124, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_NOR_, 0, 31, 124, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_NOT, 1, 31, 124, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(11) }, { SameField(11, 21) } },
	{ PPCASM_OP_NOT_, 1, 31, 124, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(11) }, { SameField(11, 21) } },
	{ PPCASM_OP_SUBFE, 1, 31, 136, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SUBFE_, 1, 31, 136, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MULLW, 0, 31, 235, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MULLW_, 0, 31, 235, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_MFSPR, 0, 31, 339, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), SprNum() }, {  } },
	{ PPCASM_OP_MTSPR, 0, 31, 467, -1, Form::Generic, 0, 0, Rule::Any, { SprNum(), Gpr(21) }, {  } },
	{ PPCASM_OP_MFLR, 0, 31, 339, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21) }, { SprIs(8) } },
	{ PPCASM_OP_MTLR, 0, 31, 467, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21) }, { SprIs(8) } },
	{ PPCASM_OP_MFCTR, 0, 31, 339, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21) }, { SprIs(9) } },
	{ PPCASM_OP_MTCTR, 0, 31, 467, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21) }, { SprIs(9) } },
	{ PPCASM_OP_MFCR, 0, 31, 19, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21) }, {  } },
	{ PPCASM_OP_MTCRF, 0, 31, 144, -1, Form::Generic, 0, 0, Rule::Any, { UImm(12, 8), Gpr(21) }, {  } },
	{ PPCASM_OP_MCRF, 0, 19, 0, -1, Form::Generic, 0, 0, Rule::Any, { CrField(21), CrField(16) }, {  } },
	{ PPCASM_OP_ADD, 0, 31, 266, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_ADD_, 0, 31, 266, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SLW, 0, 31, 24, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_SLW_, 0, 31, 24, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_SRW, 0, 31, 536, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_SRW_, 0, 31, 536, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_SRAW, 0, 31, 792, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_SRAW_, 0, 31, 792, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_AND, 0, 31, 28, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_AND_, 0, 31, 28, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_ANDC, 0, 31, 60, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_ANDC_, 0, 31, 60, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_XOR, 0, 31, 316, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_ORC, 0, 31, 412, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_OR, 0, 31, 444, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_OR_, 0, 31, 444, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21), Gpr(11) }, {  } },
	{ PPCASM_OP_MR, 1, 31, 444, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(11) }, { SameField(11, 21) } },
	{ PPCASM_OP_MR_, 1, 31, 444, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(11) }, { SameField(11, 21) } },
	{ PPCASM_OP_DIVWU, 0, 31, 459, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_DIVWU_, 0, 31, 459, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_DIVW, 0, 31, 491, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_DIVW_, 0, 31, 491, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), Gpr(16), Gpr(11) }, {  } },
	{ PPCASM_OP_SRAWI, 0, 31, 824, -1, Form::ShiftImm, kRc, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_SRAWI_, 0, 31, 824, -1, Form::ShiftImm, kRc, kRc, Rule::Any, {  }, {  } },
	{ PPCASM_OP_CNTLZW, 0, 31, 26, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_EXTSB, 0, 31, 954, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_EXTSH, 0, 31, 922, -1, Form::Generic, kRc, 0, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_CNTLZW_, 0, 31, 26, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_EXTSB_, 0, 31, 954, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_EXTSH_, 0, 31, 922, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(16), Gpr(21) }, {  } },
	{ PPCASM_OP_LWZX, 0, 31, 23, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LWZUX, 0, 31, 55, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LBZX, 0, 31, 87, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LBZUX, 0, 31, 119, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LHZX, 0, 31, 279, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LHZUX, 0, 31, 311, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LHAX, 0, 31, 343, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LHAUX, 0, 31, 375, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STWX, 0, 31, 151, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STWUX, 0, 31, 183, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STHX, 0, 31, 407, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STHUX, 0, 31, 439, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STBX, 0, 31, 215, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STBUX, 0, 31, 247, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LWBRX, 0, 31, 534, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LHBRX, 0, 31, 790, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STWBRX, 0, 31, 662, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STHBRX, 0, 31, 918, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LFSX, 0, 31, 535, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LFSUX, 0, 31, 567, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STFSX, 0, 31, 663, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STFSUX, 0, 31, 695, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STSWI, 0, 31, 725, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Gpr(16), UImm(11, 5) }, {  } },
	{ PPCASM_OP_LWARX, 0, 31, 20, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STWCX_, 0, 31, 150, -1, Form::Generic, kRc, kRc, Rule::Any, { Gpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LFDX, 0, 31, 599, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LFDUX, 0, 31, 631, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STFDX, 0, 31, 727, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STFDUX, 0, 31, 759, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_STFIWX, 0, 31, 983, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), GprOr0(16), Gpr(11) }, {  } },
	{ PPCASM_OP_LWZ, 0, 32, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LWZU, 0, 33, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LBZ, 0, 34, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LBZU, 0, 35, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STW, 0, 36, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STWU, 0, 37, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STB, 0, 38, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STBU, 0, 39, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LHZ, 0, 40, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LHZU, 0, 41, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LHA, 0, 42, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LHAU, 0, 43, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STH, 0, 44, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STHU, 0, 45, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LMW, 0, 46, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_STMW, 0, 47, -1, -1, Form::Generic, 0, 0, Rule::Any, { Gpr(21), Mem() }, {  } },
	{ PPCASM_OP_LFS, 0, 48, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_LFSU, 0, 49, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_LFD, 0, 50, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_LFDU, 0, 51, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_STFS, 0, 52, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_STFSU, 0, 53, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_STFD, 0, 54, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_STFDU, 0, 55, -1, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Mem() }, {  } },
	{ PPCASM_OP_FDIVS, 0, 59, 18, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FSUBS, 0, 59, 20, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FADDS, 0, 59, 21, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FMULS, 0, 59, 25, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6) }, {  } },
	{ PPCASM_OP_FMSUBS, 0, 59, 28, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FMADDS, 0, 59, 29, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FNMSUBS, 0, 59, 30, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FNMADDS, 0, 59, 31, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FCMPU, 0, 63, 0, -1, Form::CompareFp, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_FCTIWZ, 0, 63, 15, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	{ PPCASM_OP_FDIV, 0, 63, 18, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FSUB, 0, 63, 20, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FADD, 0, 63, 21, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(11) }, {  } },
	{ PPCASM_OP_FMUL, 0, 63, 25, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6) }, {  } },
	{ PPCASM_OP_FRSQRTE, 0, 63, 26, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	{ PPCASM_OP_FMSUB, 0, 63, 28, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FMADD, 0, 63, 29, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FNMSUB, 0, 63, 30, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FNMADD, 0, 63, 31, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(16), Fpr(6), Fpr(11) }, {  } },
	{ PPCASM_OP_FRSP, 0, 63, 12, -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	{ PPCASM_OP_FCMPO, 0, 63, X10(32), -1, Form::CompareFp, 0, 0, Rule::Any, {  }, {  } },
	{ PPCASM_OP_FNEG, 0, 63, X10(40), -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	{ PPCASM_OP_FMR, 0, 63, X10(72), -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	{ PPCASM_OP_FABS, 0, 63, X10(264), -1, Form::Generic, 0, 0, Rule::Any, { Fpr(21), Fpr(11) }, {  } },
	};

	constexpr size_t kInstrCount = sizeof(kInstrTable) / sizeof(kInstrTable[0]);

	// opcode bits (primary + extended opcodes) a row requires
	void PrimaryMatchBits(const InstrDef& d, uint32& mask, uint32& bits)
	{
		mask = 0x3Fu << 26;
		bits = (uint32)d.primary << 26;
		auto addXo = [&](uint32 width, uint32 lsb, uint32 value)
		{
			mask |= ((1u << width) - 1) << lsb;
			bits |= value << lsb;
		};
		switch (d.primary)
		{
		case 19:
		case 31:
			addXo(10, 1, (uint32)d.xo);
			break;
		case 4:
			addXo(5, 1, (uint32)d.xo);
			if (d.xo == 16)
				addXo(5, 6, (uint32)d.xo2);
			break;
		case 59:
			addXo(5, 1, (uint32)d.xo);
			break;
		case 63:
			if (d.xo & 0x10000)
				addXo(10, 1, (uint32)(d.xo & 0xFFFF));
			else
				addXo(5, 1, (uint32)d.xo);
			break;
		default:
			break;
		}
	}

	struct MatchBits
	{
		uint32 mask;
		uint32 bits;
	};

	const std::array<MatchBits, kInstrCount>& GetMatchBits()
	{
		static const std::array<MatchBits, kInstrCount> s_bits = []()
		{
			std::array<MatchBits, kInstrCount> r{};
			for (size_t i = 0; i < kInstrCount; i++)
				PrimaryMatchBits(kInstrTable[i], r[i].mask, r[i].bits);
			return r;
		}();
		return s_bits;
	}

	/* ---------------------------------------------------------------- */
	/* disassembler                                                     */
	/* ---------------------------------------------------------------- */

	void ShowOperand(PPCDisassembledInstruction* out, size_t index, uint8 type)
	{
		out->operandMask |= (uint8)(1u << index);
		out->operand[index].type = type;
	}

	void ShowRegister(PPCDisassembledInstruction* out, size_t index, uint8 type, uint32 reg)
	{
		ShowOperand(out, index, type);
		out->operand[index].registerIndex = (uint16)reg;
	}

	void ShowImmediate(PPCDisassembledInstruction* out, size_t index, sint32 value, uint8 width, bool isSigned)
	{
		ShowOperand(out, index, PPCASM_OPERAND_TYPE_IMM);
		out->operand[index].immS32 = value;
		out->operand[index].immWidth = width;
		out->operand[index].isSignedImm = isSigned;
	}

	// sets operand fields without touching operandMask (callers set the mask as a whole)
	void SetRegister(PPCDisassembledInstruction* out, size_t index, uint8 type, uint32 reg)
	{
		out->operand[index].type = type;
		out->operand[index].registerIndex = (uint16)reg;
	}

	void SetImmediate(PPCDisassembledInstruction* out, size_t index, sint32 value, uint8 width)
	{
		out->operand[index].type = PPCASM_OPERAND_TYPE_IMM;
		out->operand[index].immS32 = value;
		out->operand[index].immWidth = width;
	}

	void SetTarget(PPCDisassembledInstruction* out, size_t index, uint32 address)
	{
		out->operand[index].type = PPCASM_OPERAND_TYPE_CIMM;
		out->operand[index].immU32 = address;
	}

	bool TieHolds(const TieDef& t, uint32 word)
	{
		switch (t.kind)
		{
		case Tie::None: return true;
		case Tie::SameField: return Field5(word, t.a) == Field5(word, (uint32)t.b);
		case Tie::FieldIs: return Field5(word, t.a) == (uint32)t.b;
		case Tie::SprIs: return (uint16)(Field5(word, 16) | (Field5(word, 11) << 5)) == t.b;
		case Tie::SignIs: return ((word >> t.a) & 1) == (uint32)t.b;
		}
		return false;
	}

	void DecodeSlot(PPCDisassembledInstruction* out, const SlotDef& s, uint32 word, size_t index)
	{
		switch (s.kind)
		{
		case Slot::Unused:
			break;
		case Slot::Gpr:
		case Slot::GprOrZero:
			ShowRegister(out, index, PPCASM_OPERAND_TYPE_GPR, Field5(word, s.lsb));
			break;
		case Slot::Fpr:
			ShowRegister(out, index, PPCASM_OPERAND_TYPE_FPR, Field5(word, s.lsb));
			break;
		case Slot::CrField:
			ShowRegister(out, index, PPCASM_OPERAND_TYPE_CR, Field5(word, s.lsb) >> 2);
			break;
		case Slot::Spr:
			ShowRegister(out, index, PPCASM_OPERAND_TYPE_SPR, Field5(word, 16) | (Field5(word, 11) << 5));
			break;
		case Slot::Imm:
		{
			uint32 v = (word >> s.lsb) & ((1u << s.width) - 1);
			if (s.isSigned)
				v = SignExtendBits(v, s.width);
			ShowImmediate(out, index, (sint32)v, s.width, s.isSigned);
			break;
		}
		case Slot::FromBase:
			ShowImmediate(out, index, (sint32)((uint32)s.width - Field5(word, s.lsb)), 5, false);
			break;
		case Slot::Mem:
			ShowRegister(out, index, PPCASM_OPERAND_TYPE_MEM, Field5(word, 16));
			out->operand[index].immS32 = (sint32)SignExtend16(word);
			out->operand[index].immWidth = 16;
			out->operand[index].isSignedImm = true;
			break;
		}
	}

	void DecodeFixedLayout(const InstrDef& d, uint32 address, uint32 word, PPCDisassembledInstruction* out)
	{
		const RotateFields rf(word);
		switch (d.form)
		{
		case Form::Generic:
			break;
		case Form::CompareReg:
		{
			const uint32 crBits = Field5(word, 21);
			out->operandMask = crBits != 0 ? 1 : 0;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_CR, crBits >> 2);
			if ((crBits & 3) != 0)
				cemuLog_log(LogType::Force, "[PPC-Disassembler] Unexpected CR encoding for instruction 0x{0:08x}", word);
			ShowRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, Field5(word, 16));
			ShowRegister(out, 2, PPCASM_OPERAND_TYPE_GPR, Field5(word, 11));
			break;
		}
		case Form::ShiftImm:
			out->operandMask = 0x7;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_GPR, Field5(word, 16));
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, Field5(word, 21));
			SetImmediate(out, 2, (sint32)Field5(word, 11), 5);
			out->operand[2].isSignedImm = false;
			break;
		case Form::CondBranch:
		{
			const uint32 bo = Field5(word, 21);
			const uint32 bi = Field5(word, 16);
			uint32 target = SignExtend16(word & 0xFFFC);
			if ((word & kAa) == 0)
				target += address;
			if (d.op == PPCASM_OP_BC)
			{
				out->operandMask = 0x7;
				out->operand[0].type = PPCASM_OPERAND_TYPE_CIMM;
				out->operand[0].registerIndex = (uint16)bo;
				SetRegister(out, 1, PPCASM_OPERAND_TYPE_CR_BIT, bi);
				SetTarget(out, 2, target);
			}
			else if ((bi >> 2) != 0)
			{
				out->operandMask = 0x3;
				SetRegister(out, 0, PPCASM_OPERAND_TYPE_CR, bi >> 2);
				SetTarget(out, 1, target);
			}
			else
			{
				out->operandMask = 0x1;
				SetTarget(out, 0, target);
			}
			out->branchHintBitSet = (bo & 1) != 0;
			break;
		}
		case Form::Branch:
		{
			uint32 target = SignExtendBits(word, 26) & ~3u;
			if ((word & kAa) == 0)
				target += address;
			out->operandMask = 0x1;
			SetTarget(out, 0, target);
			break;
		}
		case Form::CondReturn:
		{
			const uint32 crIndex = Field5(word, 16) / 4;
			if (crIndex != 0)
				ShowRegister(out, 0, PPCASM_OPERAND_TYPE_CR, crIndex);
			break;
		}
		case Form::CompareImm:
		{
			const uint32 cr = Field5(word, 21) / 4;
			out->operandMask = cr != 0 ? 0x7 : 0x6;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_CR, cr);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, Field5(word, 16));
			SetImmediate(out, 2, (sint32)SignExtend16(word), 16);
			out->operand[2].isSignedImm = true;
			break;
		}
		case Form::Bare:
			out->operandMask = 0;
			break;
		case Form::CompareFp:
		{
			const uint32 crBits = Field5(word, 21);
			cemu_assert_debug((crBits % 4) == 0);
			out->operandMask = 0x7;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_CR, crBits / 4);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_FPR, Field5(word, 16));
			SetRegister(out, 2, PPCASM_OPERAND_TYPE_FPR, Field5(word, 11));
			break;
		}
		case Form::Rotate:
			out->operandMask = 0x1F;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rA);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rS);
			SetImmediate(out, 2, rf.sh, 8);
			SetImmediate(out, 3, rf.mb, 8);
			SetImmediate(out, 4, rf.me, 8);
			break;
		case Form::RotateAlias:
		{
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rA);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rS);
			switch (d.op)
			{
			case PPCASM_OP_EXTLWI:
			case PPCASM_OP_EXTLWI_:
				out->operandMask = 0xF;
				SetImmediate(out, 2, rf.me + 1, 8);
				SetImmediate(out, 3, rf.sh, 8);
				break;
			case PPCASM_OP_EXTRWI:
			case PPCASM_OP_EXTRWI_:
			{
				const sint8 n = (sint8)(32 - rf.mb);
				const sint8 b = (sint8)(rf.sh - n);
				out->operandMask = 0xF;
				SetImmediate(out, 2, n, 8);
				SetImmediate(out, 3, b, 8);
				break;
			}
			case PPCASM_OP_SLWI:
			case PPCASM_OP_SLWI_:
				out->operandMask = 0x7;
				SetImmediate(out, 2, (sint8)rf.sh, 8);
				break;
			case PPCASM_OP_SRWI:
			case PPCASM_OP_SRWI_:
				out->operandMask = 0x7;
				SetImmediate(out, 2, (sint8)(32 - rf.sh), 8);
				break;
			case PPCASM_OP_CLRLWI:
			case PPCASM_OP_CLRLWI_:
				out->operandMask = 0x7;
				SetImmediate(out, 2, (sint8)rf.mb, 8);
				break;
			case PPCASM_OP_CLRRWI:
			case PPCASM_OP_CLRRWI_:
				out->operandMask = 0x7;
				SetImmediate(out, 2, (sint8)(31 - rf.me), 8);
				break;
			default:
				cemu_assert_debug(false);
				break;
			}
			break;
		}
		case Form::RotateReg:
			out->operandMask = 0x1F;
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rA);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rS);
			SetRegister(out, 2, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.sh);
			SetImmediate(out, 3, rf.mb, 8);
			SetImmediate(out, 4, rf.me, 8);
			break;
		case Form::RotateRegAlias:
			SetRegister(out, 0, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rA);
			SetRegister(out, 1, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.rS);
			SetRegister(out, 2, PPCASM_OPERAND_TYPE_GPR, (uint32)rf.sh);
			if (d.op == PPCASM_OP_ROTLW || d.op == PPCASM_OP_ROTLW_)
				out->operandMask = 0x7;
			break;
		case Form::CrLogic:
			ShowRegister(out, 0, PPCASM_OPERAND_TYPE_CR_BIT, Field5(word, 21));
			ShowRegister(out, 1, PPCASM_OPERAND_TYPE_CR_BIT, Field5(word, 16));
			ShowRegister(out, 2, PPCASM_OPERAND_TYPE_CR_BIT, Field5(word, 11));
			break;
		}
	}

	/* ---------------------------------------------------------------- */
	/* assembler: shared state and error helpers                         */
	/* ---------------------------------------------------------------- */

	struct AsmState
	{
		PPCAssemblerInOut* io;
		boost::container::small_vector<std::string_view, 8> args;
		const InstrDef* def{};
		uint32 word{};
	};

	bool Fail(PPCAssemblerInOut* io, std::string_view message)
	{
		io->errorMsg = message;
		return false;
	}

	// nth operand text, or fail with "Missing operand"
	bool RequireArg(AsmState& st, size_t index, std::string_view& text)
	{
		if (index >= st.args.size())
			return Fail(st.io, "Missing operand");
		text = st.args[index];
		return true;
	}

	void AddReloc(AsmState& st, PPCASM_RELOC type, std::string expression, uint32 byteOffset, uint8 bitOffset, uint8 bitCount)
	{
		st.io->list_relocs.emplace_back(type, std::move(expression), byteOffset, bitOffset, bitCount);
	}

	/* register names, strict form used by Generic rows: <prefix><decimal> with
	   the prefix matched case-insensitively and nothing else around it */
	int ParseRegisterStrict(std::string_view text, const char* prefix)
	{
		for (; *prefix; prefix++)
		{
			if (text.empty())
				return -1;
			char c = text.front();
			if (c >= 'A' && c <= 'Z')
				c = (char)(c - 'A' + 'a');
			if (c != *prefix)
				return -1;
			text.remove_prefix(1);
		}
		int value = 0;
		auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (ec != std::errc() || end != text.data() + text.size())
			return -1;
		return value;
	}

	/* register names, lenient form used by the fixed layouts: surrounding spaces
	   are ignored, at least one digit is required and values wrap like a 32 bit
	   accumulator */
	bool ParseRegisterLenient(std::string_view text, const char* prefix, sint32& regOut)
	{
		size_t b = 0, e = text.size();
		while (b < e && text[b] == ' ')
			b++;
		while (e > b && text[e - 1] == ' ')
			e--;
		if (e - b < 2)
			return false;
		for (; *prefix; prefix++, b++)
		{
			if (b >= e)
				return false;
			if (tolower((unsigned char)*prefix) != tolower((unsigned char)text[b]))
				return false;
		}
		if (b >= e)
			return false;
		uint32 acc = 0;
		for (; b < e; b++)
		{
			const char c = text[b];
			acc *= 10;
			if (c < '0' || c > '9')
				return false;
			acc += (uint32)(c - '0');
		}
		if ((sint32)acc >= 32)
			return false;
		regOut = (sint32)acc;
		return true;
	}

	/* ---------------------------------------------------------------- */
	/* assembler: Generic row slots                                      */
	/* ---------------------------------------------------------------- */

	bool EncodeRegisterSlot(AsmState& st, std::string_view text, const char* prefix, int limit, uint32 lsb, uint32 shiftUp, const char* what)
	{
		const int reg = ParseRegisterStrict(text, prefix);
		if (reg < 0 || reg >= limit)
			return Fail(st.io, fmt::format("Operand \"{}\" is not a valid {}", text, what));
		PutField5(st.word, lsb, (uint32)reg << shiftUp);
		return true;
	}

	// evaluates an immediate; false + filled relocation when not constant
	enum class ImmResult { Value, Relocated };

	ImmResult EvaluateOrRelocate(AsmState& st, const std::string& expression, uint8 lsb, uint8 width, double& value)
	{
		ExpressionParser ep;
		if (ep.IsConstantExpression(expression))
		{
			value = ep.Evaluate(expression);
			return ImmResult::Value;
		}
		AddReloc(st, PPCASM_RELOC::U32_MASKED_IMM, expression, 0, lsb, width);
		return ImmResult::Relocated;
	}

	bool EncodeImmediateSlot(AsmState& st, const SlotDef& s, std::string_view text)
	{
		std::string expression(text);
		if (s.negate)
			expression = "0-(" + expression + ")";
		double d = 0.0;
		if (EvaluateOrRelocate(st, expression, s.lsb, s.width, d) == ImmResult::Relocated)
			return true;
		const uint32 v = (uint32)TruncToS32(d);
		const bool fits = FitsInBits(v, s.width, s.isSigned) || (s.allowUnsigned && FitsInBits(v, s.width, false));
		if (!fits)
			return Fail(st.io, fmt::format("Value of operand \"{}\" is out of range", text));
		const uint32 mask = (1u << s.width) - 1;
		st.word = (st.word & ~(mask << s.lsb)) | ((v & mask) << s.lsb);
		return true;
	}

	bool EncodeFromBaseSlot(AsmState& st, const SlotDef& s, std::string_view text)
	{
		const std::string expression = fmt::format("{}-(", s.width) + std::string(text) + ")";
		double d = 0.0;
		if (EvaluateOrRelocate(st, expression, s.lsb, 5, d) == ImmResult::Relocated)
			return true;
		const uint32 v = (uint32)TruncToS32(d);
		if (!FitsInBits(v, 5, false))
			return Fail(st.io, fmt::format("Value of operand \"{}\" is out of range", text));
		PutField5(st.word, s.lsb, v);
		return true;
	}

	// displacement(rA)
	bool EncodeMemorySlot(AsmState& st, std::string_view text)
	{
		auto syntaxError = [&]()
		{
			return Fail(st.io, fmt::format("\'{}\' does not end with valid memory register syntax. Memory operand must have the form offset(gpr). Example: 0x20(r3)", text));
		};
		size_t end = text.size();
		while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t'))
			end--;
		if (end == 0 || text[end - 1] != ')')
			return syntaxError();
		const size_t close = end - 1;
		const size_t open = text.find_last_of('(', close == 0 ? 0 : close - 1);
		if (close == 0 || open == std::string_view::npos)
			return syntaxError();
		std::string_view displacement = text.substr(0, open);
		std::string_view baseReg = text.substr(open + 1, close - open - 1);
		const int reg = ParseRegisterStrict(baseReg, "r");
		if (reg < 0 || reg >= 32)
			return Fail(st.io, fmt::format("Memory operand register \"{}\" is not a valid GPR (expected r0 - r31)", baseReg));
		PutField5(st.word, 16, (uint32)reg);

		ExpressionParser ep;
		if (!ep.IsConstantExpression(displacement))
		{
			AddReloc(st, PPCASM_RELOC::U32_MASKED_IMM, std::string(displacement), 0, 0, 16);
			return true;
		}
		const sint32 offset = TruncToS32(ep.Evaluate(displacement));
		if (offset < -32768 || offset > 32767)
			return Fail(st.io, fmt::format("\"{}\" evaluates to offset out of range (Valid range is -32768 to 32767)", displacement));
		st.word = (st.word & ~0xFFFFu) | ((uint32)offset & 0xFFFF);
		return true;
	}

	bool EncodeSlot(AsmState& st, const SlotDef& s, size_t index)
	{
		if (s.kind == Slot::Unused)
		{
			// a slot past the instruction's last operand must not be filled
			if (index < st.args.size() && !st.args[index].empty())
				return Fail(st.io, "Too many operands");
			return true;
		}
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		switch (s.kind)
		{
		case Slot::GprOrZero:
			if (text == "0")
			{
				PutField5(st.word, s.lsb, 0);
				return true;
			}
			[[fallthrough]];
		case Slot::Gpr:
			return EncodeRegisterSlot(st, text, "r", 32, s.lsb, 0, "GPR (expected r0 - r31)");
		case Slot::Fpr:
			return EncodeRegisterSlot(st, text, "f", 32, s.lsb, 0, "FPR (expected f0 - f31)");
		case Slot::CrField:
			return EncodeRegisterSlot(st, text, "cr", 8, s.lsb, 2, "CR field (expected cr0 - cr7)");
		case Slot::Spr:
		{
			const int spr = ParseRegisterStrict(text, "spr");
			if (spr < 0 || spr >= 1024)
				return Fail(st.io, fmt::format("Operand \"{}\" is not a valid GPR (expected spr0 - spr1023)", text));
			PutField5(st.word, 16, (uint32)spr & 0x1F);
			PutField5(st.word, 11, ((uint32)spr >> 5) & 0x1F);
			return true;
		}
		case Slot::Imm:
			return EncodeImmediateSlot(st, s, text);
		case Slot::FromBase:
			return EncodeFromBaseSlot(st, s, text);
		case Slot::Mem:
			return EncodeMemorySlot(st, text);
		case Slot::Unused:
			break;
		}
		return true;
	}

	void ApplyTie(const TieDef& t, uint32& word)
	{
		switch (t.kind)
		{
		case Tie::None:
		case Tie::SignIs: // only checked when disassembling
			break;
		case Tie::SameField:
			PutField5(word, (uint32)t.b, Field5(word, t.a));
			break;
		case Tie::FieldIs:
			PutField5(word, t.a, (uint32)t.b);
			break;
		case Tie::SprIs:
			PutField5(word, 16, t.b & 0x1F);
			PutField5(word, 11, (t.b >> 5) & 0x1F);
			break;
		}
	}

	/* ---------------------------------------------------------------- */
	/* assembler: fixed layout operands                                  */
	/* ---------------------------------------------------------------- */

	bool OrRegister(AsmState& st, size_t index, uint32 lsb, const char* prefix, sint32 count)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		sint32 reg = 0;
		if (!ParseRegisterLenient(text, prefix, reg) || reg < 0 || reg >= count)
			return Fail(st.io, fmt::format("\'{0}\' is not a valid register operand (must be {1}0 to {1}{2})", text, prefix, count - 1));
		st.word |= (uint32)reg << lsb;
		return true;
	}

	bool OrGpr(AsmState& st, size_t index, uint32 lsb) { return OrRegister(st, index, lsb, "r", 32); }
	bool OrFpr(AsmState& st, size_t index, uint32 lsb) { return OrRegister(st, index, lsb, "f", 32); }
	// CR field operand; with asBitIndex the field number is placed as crN*4
	bool OrCrField(AsmState& st, size_t index, uint32 lsb, bool asBitIndex) { return OrRegister(st, index, lsb + (asBitIndex ? 2 : 0), "cr", 8); }

	// validity, constness, then evaluation (evaluation errors propagate as exceptions)
	template<typename T>
	bool EvaluateConstant(AsmState& st, TExpressionParser<T>& ep, std::string_view expression, T& result)
	{
		if (!ep.IsValidExpression(expression))
			return Fail(st.io, fmt::format("'{}' is not a valid expression", expression));
		if (!ep.IsConstantExpression(expression))
			return Fail(st.io, fmt::format("'{}' does not evaluate to a constant expression", expression));
		result = ep.Evaluate(expression);
		return true;
	}

	// condition register bit: lt/gt/eq/so, optionally as 4*crN+cond
	bool OrCrBit(AsmState& st, size_t index, uint32 lsb)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		TExpressionParser<sint32> ep;
		static constexpr const char* kCondNames[] = { "lt", "gt", "eq", "so" };
		for (sint32 i = 0; i < 4; i++)
			ep.AddConstant(kCondNames[i], i);
		for (sint32 i = 0; i < 8; i++)
			ep.AddConstant(fmt::format("cr{}", i), i);
		sint32 bit = 0;
		if (!EvaluateConstant(st, ep, text, bit))
			return false;
		if (bit < 0 || bit >= 32)
			return Fail(st.io, fmt::format("CR bit operand \'{0}\' evaluated to {1} which is out of range", text, bit));
		PutField5(st.word, lsb, (uint32)bit);
		return true;
	}

	// 16 bit immediate, no range check, relocation when not constant
	bool OrImm16(AsmState& st, size_t index, uint32 lsb)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		double d = 0.0;
		if (EvaluateOrRelocate(st, std::string(text), (uint8)lsb, 16, d) == ImmResult::Relocated)
			return true;
		st.word |= ((uint32)TruncToS32(d) & 0xFFFF) << lsb;
		return true;
	}

	// 0..31 immediate that must evaluate right away
	bool OrImm5(AsmState& st, size_t index, uint32 lsb)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		ExpressionParser ep;
		const sint32 v = TruncToS32(ep.Evaluate(text));
		if (v < 0 || v >= 32)
			return Fail(st.io, fmt::format("\'{}\' is not in range 0-31", text));
		st.word |= ((uint32)v & 0x1F) << lsb;
		return true;
	}

	bool ConstImm5(AsmState& st, size_t index, sint8& value)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		TExpressionParser<sint32> ep;
		sint32 v = 0;
		if (!EvaluateConstant(st, ep, text, v))
			return false;
		if (v < 0 || v >= 32)
			return Fail(st.io, fmt::format("Expression '\'{0}\' which evaluates to {1} is not in range 0-31", text, v));
		value = (sint8)v;
		return true;
	}

	// ".+offset" / ".-offset" relative targets resolve right away
	bool RelativeTarget(const std::string& expression, sint32& distance)
	{
		if (expression.empty() || expression[0] != '.')
			return false;
		ExpressionParser ep;
		try
		{
			distance = TruncToS32(ep.Evaluate("0" + expression.substr(1)));
		}
		catch (std::exception&)
		{
			return false;
		}
		return true;
	}

	bool OrBranchTarget(AsmState& st, size_t index, sint32 minValue, sint32 maxValue, uint32 fieldMask, PPCASM_RELOC relocType)
	{
		std::string_view text;
		if (!RequireArg(st, index, text))
			return false;
		std::string expression(text);
		sint32 distance = 0;
		if (!RelativeTarget(expression, distance))
		{
			AddReloc(st, relocType, expression, 0, 0, 0);
			return true;
		}
		if (distance < minValue || distance > maxValue)
			return Fail(st.io, "Branch target out of range");
		if (distance & 3)
			return Fail(st.io, "Branch target must be aligned to 4");
		st.word |= (uint32)distance & fieldMask;
		return true;
	}

	bool AssembleFixedLayout(AsmState& st, bool hasBranchHint)
	{
		const InstrDef& d = *st.def;
		const size_t argc = st.args.size();
		switch (d.form)
		{
		case Form::Bare:
			return true;
		case Form::CompareReg:
			if (argc == 2)
				return OrGpr(st, 0, 16) && OrGpr(st, 1, 11);
			return OrCrField(st, 0, 21, true) && OrGpr(st, 1, 16) && OrGpr(st, 2, 11);
		case Form::CompareImm:
			if (argc == 2)
				return OrGpr(st, 0, 16) && OrImm16(st, 1, 0);
			return OrCrField(st, 0, 21, true) && OrGpr(st, 1, 16) && OrImm16(st, 2, 0);
		case Form::CompareFp:
			if (argc == 2)
				return OrFpr(st, 0, 16) && OrFpr(st, 1, 11);
			return OrCrField(st, 0, 21, true) && OrFpr(st, 1, 16) && OrFpr(st, 2, 11);
		case Form::ShiftImm:
			return OrGpr(st, 0, 16) && OrGpr(st, 1, 21) && OrImm5(st, 2, 11);
		case Form::Rotate:
			return OrGpr(st, 0, 16) && OrGpr(st, 1, 21) && OrImm5(st, 2, 11) && OrImm5(st, 3, 6) && OrImm5(st, 4, 1);
		case Form::RotateReg:
			return OrGpr(st, 0, 16) && OrGpr(st, 1, 21) && OrGpr(st, 2, 11) && OrImm5(st, 3, 6) && OrImm5(st, 4, 1);
		case Form::RotateRegAlias:
		{
			if (!(OrGpr(st, 0, 16) && OrGpr(st, 1, 21) && OrGpr(st, 2, 11)))
				return false;
			if (d.op == PPCASM_OP_ROTLW || d.op == PPCASM_OP_ROTLW_)
				st.word |= 31u << 1; // MB = 0, ME = 31
			return true;
		}
		case Form::RotateAlias:
		{
			if (!(OrGpr(st, 0, 16) && OrGpr(st, 1, 21)))
				return false;
			// the derived SH/MB/ME are OR-ed in without masking, as uint8 values
			uint8 sh = 0, mb = 0, me = 0;
			sint8 n = 0, b = 0;
			switch (d.op)
			{
			case PPCASM_OP_EXTLWI:
			case PPCASM_OP_EXTLWI_:
				if (!ConstImm5(st, 2, n) || !ConstImm5(st, 3, b))
					return false;
				sh = (uint8)b; mb = 0; me = (uint8)(n - 1);
				break;
			case PPCASM_OP_EXTRWI:
			case PPCASM_OP_EXTRWI_:
				if (!ConstImm5(st, 2, n) || !ConstImm5(st, 3, b))
					return false;
				sh = (uint8)(b + n); mb = (uint8)(32 - n); me = 31;
				break;
			case PPCASM_OP_SLWI:
			case PPCASM_OP_SLWI_:
				if (!ConstImm5(st, 2, n))
					return false;
				sh = (uint8)n; mb = 0; me = (uint8)(31 - n);
				break;
			case PPCASM_OP_SRWI:
			case PPCASM_OP_SRWI_:
				if (!ConstImm5(st, 2, n))
					return false;
				sh = (uint8)(32 - n); mb = (uint8)n; me = 31;
				break;
			case PPCASM_OP_CLRLWI:
			case PPCASM_OP_CLRLWI_:
				if (!ConstImm5(st, 2, n))
					return false;
				sh = 0; mb = (uint8)n; me = 31;
				break;
			case PPCASM_OP_CLRRWI:
			case PPCASM_OP_CLRRWI_:
				if (!ConstImm5(st, 2, n))
					return false;
				sh = 0; mb = 0; me = (uint8)(31 - n);
				break;
			default:
				cemu_assert_debug(false);
				break;
			}
			st.word |= ((uint32)sh << 11) | ((uint32)mb << 6) | ((uint32)me << 1);
			return true;
		}
		case Form::CondBranch:
			if (d.op == PPCASM_OP_BC)
			{
				// bc BO, BI, target
				sint8 bo = 0;
				if (!ConstImm5(st, 0, bo) || !OrCrBit(st, 1, 16) || !OrBranchTarget(st, 2, -32768, 32767, 0xFFFC, PPCASM_RELOC::BRANCH_S16))
					return false;
				st.word |= (uint32)(sint32)bo << 21;
				return true;
			}
			if (argc == 2)
			{
				if (!OrCrField(st, 0, 16, true) || !OrBranchTarget(st, 1, -32768, 32767, 0xFFFC, PPCASM_RELOC::BRANCH_S16))
					return false;
			}
			else if (!OrBranchTarget(st, 0, -32768, 32767, 0xFFFC, PPCASM_RELOC::BRANCH_S16))
				return false;
			// either suffix ('+' or '-') sets the hint bit
			if (hasBranchHint)
				st.word |= 1u << 21;
			return true;
		case Form::Branch:
			return OrBranchTarget(st, 0, -33554432, 33554431, 0x3FFFFFC, PPCASM_RELOC::BRANCH_S26);
		case Form::CondReturn:
			if (argc >= 1)
				return OrCrField(st, 0, 18, false);
			return true;
		case Form::CrLogic:
			return OrCrBit(st, 0, 21) && OrCrBit(st, 1, 16) && OrCrBit(st, 2, 11);
		case Form::Generic:
			break;
		}
		cemu_assert_debug(false);
		return false;
	}

	/* ---------------------------------------------------------------- */
	/* assembler: alignment and data directives                          */
	/* ---------------------------------------------------------------- */

	void SetAlignment(PPCAssemblerInOut* io, uint32 alignment)
	{
		if (io->forceNoAlignment)
			alignment = 1;
		io->alignmentRequirement = alignment;
		const uint32 aligned = alignment == 0 ? io->virtualAddress : (io->virtualAddress + alignment - 1) & ~(alignment - 1);
		io->alignmentPaddingSize = aligned - io->virtualAddress;
		io->virtualAddressAligned = aligned;
	}

	struct DataKind
	{
		PPCASM_RELOC type;
		uint32 size;
	};

	std::optional<DataKind> DataDirective(std::string_view name)
	{
		static const std::pair<const char*, DataKind> kDirectives[] =
		{
			{ ".FLOAT", { PPCASM_RELOC::FLOAT, 4 } },
			{ ".DOUBLE", { PPCASM_RELOC::DOUBLE, 8 } },
			{ ".INT", { PPCASM_RELOC::U32, 4 } }, { ".UINT", { PPCASM_RELOC::U32, 4 } }, { ".PTR", { PPCASM_RELOC::U32, 4 } }, { ".U32", { PPCASM_RELOC::U32, 4 } }, { ".LONG", { PPCASM_RELOC::U32, 4 } },
			{ ".WORD", { PPCASM_RELOC::U16, 2 } }, { ".U16", { PPCASM_RELOC::U16, 2 } }, { ".SHORT", { PPCASM_RELOC::U16, 2 } },
			{ ".BYTE", { PPCASM_RELOC::U8, 1 } }, { ".STRING", { PPCASM_RELOC::U8, 1 } }, { ".U8", { PPCASM_RELOC::U8, 1 } }, { ".CHAR", { PPCASM_RELOC::U8, 1 } },
		};
		for (auto& [n, k] : kDirectives)
			if (name == n)
				return k;
		return std::nullopt;
	}

	// "text" literal with C style escapes. Returns false (error set) on bad input.
	bool DecodeStringLiteral(PPCAssemblerInOut* io, std::string_view literal, std::vector<uint8>& bytes)
	{
		std::string_view s = literal.substr(1, literal.size() - 2);
		size_t i = 0;
		while (i < s.size())
		{
			const char c = s[i++];
			if (c != '\\')
			{
				bytes.push_back((uint8)c);
				continue;
			}
			if (i >= s.size())
				break; // lone trailing backslash is dropped
			char e = s[i++];
			if (e >= 'A' && e <= 'Z')
				e = (char)(e - 'A' + 'a');
			switch (e)
			{
			case '\\': bytes.push_back('\\'); break;
			case 'n': bytes.push_back('\n'); break;
			case 't': bytes.push_back('\t'); break;
			case 'r': bytes.push_back('\r'); break;
			case '"': bytes.push_back('"'); break;
			case '\'': bytes.push_back('\''); break;
			case 'x':
			{
				uint32 value = 0;
				size_t digits = 0;
				while (i < s.size())
				{
					const char h = s[i];
					uint32 v;
					if (h >= '0' && h <= '9') v = (uint32)(h - '0');
					else if (h >= 'a' && h <= 'f') v = (uint32)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') v = (uint32)(h - 'A' + 10);
					else break;
					value = (value << 4) | v;
					digits++;
					i++;
				}
				if (digits == 0)
				{
					io->errorMsg = "String contains invalid hex escape sequence";
					return false;
				}
				bytes.push_back((uint8)value);
				// a hex escape ends the literal: anything after it is ignored
				return true;
			}
			default:
				bytes.push_back('\\'); // unknown escape: keep the backslash, drop the letter
				break;
			}
		}
		return true;
	}

	bool EmitData(AsmState& st, std::string_view directiveName, const DataKind& kind)
	{
		PPCAssemblerInOut* io = st.io;
		if (st.args.empty())
			return Fail(io, fmt::format("Value expected after data directive {}", directiveName));
		SetAlignment(io, kind.size);
		io->outputData.reserve(kind.size * st.args.size());
		for (std::string_view arg : st.args)
		{
			if (!arg.empty() && arg[0] == '"')
			{
				if (kind.type != PPCASM_RELOC::U8)
					return Fail(io, "Strings constants are only allowed in .byte or .string data directives");
				if (arg.size() < 2 || arg.back() != '"')
					return Fail(io, "String constants must end with a quotation mark. Example: \"text\"");
				std::vector<uint8> bytes;
				if (!DecodeStringLiteral(io, arg, bytes))
					return false;
				io->outputData.insert(io->outputData.end(), bytes.begin(), bytes.end());
				io->outputData.push_back(0);
				continue;
			}
			const size_t offset = io->outputData.size();
			io->outputData.resize(offset + kind.size, 0);
			for (size_t i = 0; i < kind.size; i++)
				io->outputData[offset + i] = 0;
			ExpressionParser ep;
			if (!ep.IsConstantExpression(arg))
			{
				AddReloc(st, kind.type, std::string(arg), (uint32)offset, 0, 0);
				continue;
			}
			const double v = ep.Evaluate(arg);
			uint8* dst = io->outputData.data() + offset;
			switch (kind.type)
			{
			case PPCASM_RELOC::U32: *(uint32be*)dst = (uint32)TruncToS64(v); break;
			case PPCASM_RELOC::U16: *(uint16be*)dst = (uint16)TruncToS32(v); break;
			case PPCASM_RELOC::U8: *dst = (uint8)TruncToS32(v); break;
			case PPCASM_RELOC::DOUBLE: *(float64be*)dst = v; break;
			case PPCASM_RELOC::FLOAT: *(float32be*)dst = (float)v; break;
			default: cemu_assert_debug(false); break;
			}
		}
		return true;
	}

	bool EmitAlign(AsmState& st)
	{
		PPCAssemblerInOut* io = st.io;
		if (st.args.size() != 1)
			return Fail(io, ".align directive must have exactly one operand");
		ExpressionParser ep;
		if (!ep.IsConstantExpression(st.args[0]))
			return Fail(io, fmt::format("Expression \'{}\' for .align directive is not a constant", st.args[0]));
		const sint32 alignment = ep.Evaluate<sint32>(st.args[0]);
		if (alignment <= 0 || alignment >= 256)
			return Fail(io, fmt::format("Alignment value \'{}\' is not within the allowed range (1-256)", alignment));
		SetAlignment(io, 1);
		for (uint32 addr = io->virtualAddress; addr % (uint32)alignment != 0; addr++)
			io->outputData.push_back(0);
		return true;
	}

	/* ---------------------------------------------------------------- */
	/* assembler: line splitting                                         */
	/* ---------------------------------------------------------------- */

	inline bool IsBlank(char c) { return c == ' ' || c == '\t'; }

	// odd number of backslashes directly before pos (within the current operand)?
	bool IsEscapedQuote(std::string_view s, size_t operandStart, size_t pos)
	{
		size_t n = 0;
		while (pos > operandStart && s[pos - 1] == '\\')
		{
			n++;
			pos--;
		}
		return (n & 1) != 0;
	}

	const InstrDef* FindByName(std::string_view upperName)
	{
		for (const InstrDef& d : kInstrTable)
			if (upperName == ppcAssembler_getInstructionName(d.op))
				return &d;
		return nullptr;
	}
}

const char* ppcAssembler_getInstructionName(uint32 ppcAsmOp)
{
	for (const OpName& e : kOpNames)
		if ((uint32)e.op == ppcAsmOp)
			return e.name;
	return "UNDEF";
}

void ppcAssembler_disassemble(uint32 virtualAddress, uint32 opcode, PPCDisassembledInstruction* disInstr)
{
	memset(disInstr, 0, sizeof(PPCDisassembledInstruction));
	const auto& matchBits = GetMatchBits();
	const InstrDef* best = nullptr;
	for (size_t i = 0; i < kInstrCount; i++)
	{
		const InstrDef& d = kInstrTable[i];
		if ((opcode & matchBits[i].mask) != matchBits[i].bits)
			continue;
		if ((opcode & d.fixedMask) != d.fixedBits)
			continue;
		if (!RuleHolds(d.rule, opcode))
			continue;
		if (best && d.priority < best->priority)
			continue;
		if (!std::all_of(d.ties.begin(), d.ties.end(), [&](const TieDef& t) { return TieHolds(t, opcode); }))
			continue;
		best = &d; // later rows win ties
	}
	if (!best)
	{
		disInstr->ppcAsmCode = PPCASM_OP_UKN;
		return;
	}
	disInstr->ppcAsmCode = best->op;
	if (best->form == Form::Generic)
	{
		for (size_t i = 0; i < best->slots.size(); i++)
			DecodeSlot(disInstr, best->slots[i], opcode, i);
	}
	else
		DecodeFixedLayout(*best, virtualAddress, opcode, disInstr);
}

bool ppcAssembler_assembleSingleInstruction(char const* text, PPCAssemblerInOut* ctx)
{
	// comments start at '#', anywhere on the line
	std::string_view line(text);
	if (size_t hash = line.find('#'); hash != std::string_view::npos)
		line = line.substr(0, hash);
	while (!line.empty() && IsBlank(line.back()))
		line.remove_suffix(1);
	while (!line.empty() && IsBlank(line.front()))
		line.remove_prefix(1);

	// mnemonic, upper cased, at most 32 characters
	size_t pos = 0;
	std::string name;
	while (pos < line.size() && !IsBlank(line[pos]))
	{
		if (name.size() >= 32)
			return Fail(ctx, "Instruction name exceeds maximum allowed length");
		name.push_back((char)toupper((unsigned char)line[pos]));
		pos++;
	}
	if (name.empty())
		return Fail(ctx, "Instruction name is invalid");

	// branch prediction suffix
	bool hasBranchHint = false;
	if (name.back() == '+')
	{
		hasBranchHint = true;
		name.pop_back();
	}
	if (!name.empty() && name.back() == '-')
	{
		hasBranchHint = true;
		name.pop_back();
	}
	if (name == "BNL")
		name = "BGT";
	else if (name == "BNG")
		name = "BLE";

	// operands: split at commas outside of string literals, trim blanks. The
	// character right after the mnemonic (and after each comma) is skipped.
	AsmState st;
	st.io = ctx;
	bool inString = false;
	while (pos < line.size())
	{
		pos++;
		const size_t start = pos;
		while (pos < line.size())
		{
			if (line[pos] == '"' && !IsEscapedQuote(line, start, pos))
				inString = !inString;
			if (line[pos] == ',' && !inString)
				break;
			pos++;
		}
		size_t b = start, e = pos;
		while (b < line.size() && IsBlank(line[b]))
			b++;
		while (e > b && IsBlank(line[e - 1]))
			e--;
		st.args.push_back(e > b ? line.substr(b, e - b) : std::string_view(line.data() + b, 0));
	}

	if (auto data = DataDirective(name))
		return EmitData(st, name, *data);
	if (name == ".ALIGN")
		return EmitAlign(st);

	const InstrDef* def = FindByName(name);
	if (!def)
		return Fail(ctx, fmt::format("Instruction \'{}\' is unknown or not supported", name));
	st.def = def;
	SetAlignment(ctx, 4);

	uint32 primaryMask, primaryBits;
	PrimaryMatchBits(*def, primaryMask, primaryBits);
	st.word = (primaryBits & primaryMask) | (def->fixedBits & def->fixedMask);

	if (def->form == Form::Generic)
	{
		for (size_t i = 0; i < def->slots.size(); i++)
			if (!EncodeSlot(st, def->slots[i], i))
				return false;
		for (const TieDef& t : def->ties)
			ApplyTie(t, st.word);
	}
	else if (!AssembleFixedLayout(st, hasBranchHint))
		return false;

	ctx->outputData.resize(4);
	ctx->outputData[0] = (uint8)(st.word >> 24);
	ctx->outputData[1] = (uint8)(st.word >> 16);
	ctx->outputData[2] = (uint8)(st.word >> 8);
	ctx->outputData[3] = (uint8)st.word;
	return true;
}

/* ---- self test (debug builds) ---- */

namespace
{
	[[maybe_unused]] void ExpectWord(const char* source, uint32 expected)
	{
		PPCAssemblerInOut io{};
		const bool ok = ppcAssembler_assembleSingleInstruction(source, &io);
		cemu_assert_debug(ok && io.outputData.size() == 4);
		if (!ok || io.outputData.size() != 4)
			return;
		const uint32 word = ((uint32)io.outputData[0] << 24) | ((uint32)io.outputData[1] << 16) | ((uint32)io.outputData[2] << 8) | io.outputData[3];
		cemu_assert_debug(word == expected);
		PPCDisassembledInstruction dis{};
		ppcAssembler_disassemble(0, word, &dis);
		cemu_assert_debug(dis.ppcAsmCode != PPCASM_OP_UKN);
	}

	[[maybe_unused]] void ExpectError(const char* source)
	{
		PPCAssemblerInOut io{};
		cemu_assert_debug(!ppcAssembler_assembleSingleInstruction(source, &io) && !io.errorMsg.empty());
	}

	[[maybe_unused]] void ExpectBytes(const char* source, std::initializer_list<uint8> expected)
	{
		PPCAssemblerInOut io{};
		const bool ok = ppcAssembler_assembleSingleInstruction(source, &io);
		cemu_assert_debug(ok && std::equal(io.outputData.begin(), io.outputData.end(), expected.begin(), expected.end()));
	}
}

void ppcAsmTest()
{
#ifdef CEMU_DEBUG_ASSERT
	// encodings below were taken from the PowerPC architecture manual examples
	ExpectWord("li r3, 0", 0x38600000);
	ExpectWord("li r3, -1", 0x3860FFFF);
	ExpectWord("lis r4, 0x8000", 0x3C808000);
	ExpectWord("addi r1, r1, 0x10", 0x38210010);
	ExpectWord("subi r1, r1, 0x10", 0x3821FFF0);
	ExpectWord("mr r31, r3", 0x7C7F1B78);
	ExpectWord("nop", 0x60000000);
	ExpectWord("blr", 0x4E800020);
	ExpectWord("bctrl", 0x4E800421);
	ExpectWord("mflr r0", 0x7C0802A6);
	ExpectWord("mtctr r12", 0x7D8903A6);
	ExpectWord("stwu r1, -0x20(r1)", 0x9421FFE0);
	ExpectWord("lwz r3, 4(r31)", 0x807F0004);
	ExpectWord("lfs f1, 0(r3)", 0xC0230000);
	ExpectWord("cmpwi cr1, r3, 5", 0x2C830005);
	ExpectWord("cmplw r3, r4", 0x7C032040);
	ExpectWord("bne .+0x10", 0x40820010);
	ExpectWord("b .-4", 0x4BFFFFFC);
	ExpectWord("slwi r3, r4, 2", 0x5483103A);
	ExpectWord("srwi r3, r4, 8", 0x5483C23E);
	ExpectWord("rlwinm r3, r4, 0, 24, 31", 0x5483063E);
	ExpectWord("fadds f1, f2, f3", 0xEC22182A);
	ExpectWord("fmuls f1, f2, f3", 0xEC2200F2);
	ExpectWord("crxor 4*cr1+eq, 4*cr1+eq, 4*cr1+eq", 0x4CC63182);
	ExpectError("li r3, 0x10000");
	ExpectError("addi r32, r1, 0");
	ExpectError("lwz r3, 4");
	ExpectError("frobnicate r1");
	ExpectBytes(".int 0x12345678", { 0x12, 0x34, 0x56, 0x78 });
	ExpectBytes(".short 1, 2", { 0x00, 0x01, 0x00, 0x02 });
	ExpectBytes(".string \"hi\\n\"", { 'h', 'i', '\n', 0 });
#endif
}
