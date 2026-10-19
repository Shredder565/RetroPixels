// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Instruction set helpers for the Espresso (PowerPC 750CL family) used by
// the recompiler front end and tools that inspect guest code.
namespace Espresso
{
	// bit positions inside a 4-bit condition register field
	enum CR_BIT
	{
		CR_BIT_INDEX_LT = 0,
		CR_BIT_INDEX_GT = 1,
		CR_BIT_INDEX_EQ = 2,
		CR_BIT_INDEX_SO = 3,
	};

	// GQR load/store data types for psq_l / psq_st
	enum class PSQ_LOAD_TYPE
	{
		TYPE_F32 = 0,
		TYPE_UNUSED1 = 1,
		TYPE_UNUSED2 = 2,
		TYPE_UNUSED3 = 3,
		TYPE_U8 = 4,
		TYPE_U16 = 5,
		TYPE_S8 = 6,
		TYPE_S16 = 7,
	};

	// bits 0-5 of an instruction word. A trailing underscore marks forms that
	// always record into CR0.
	enum class PrimaryOpcode
	{
		ZERO = 0,
		VIRTUAL_HLE = 1, // emulator specific: calls a host function
		GROUP_4 = 4,     // paired singles
		MULLI = 7,
		SUBFIC = 8,
		CMPLI = 10,
		CMPI = 11,
		ADDIC = 12,
		ADDIC_ = 13,
		ADDI = 14,
		ADDIS = 15,
		BC = 16,
		GROUP_17 = 17,   // sc
		B = 18,
		GROUP_19 = 19,
		RLWIMI = 20,
		RLWINM = 21,
		RLWNM = 23,
		ORI = 24,
		ORIS = 25,
		XORI = 26,
		XORIS = 27,
		ANDI_ = 28,
		ANDIS_ = 29,
		GROUP_31 = 31,
		LWZ = 32,
		LWZU = 33,
		LBZ = 34,
		LBZU = 35,
		STW = 36,
		STWU = 37,
		STB = 38,
		STBU = 39,
		LHZ = 40,
		LHZU = 41,
		LHA = 42,
		LHAU = 43,
		STH = 44,
		STHU = 45,
		LMW = 46,
		STMW = 47,
		LFS = 48,
		LFSU = 49,
		LFD = 50,
		LFDU = 51,
		STFS = 52,
		STFSU = 53,
		STFD = 54,
		STFDU = 55,
		PSQ_L = 56,
		PSQ_LU = 57,
		GROUP_59 = 59,
		PSQ_ST = 60,
		PSQ_STU = 61,
		GROUP_63 = 63,
	};

	// extended opcodes (bits 21-30) of primary opcode 19
	enum class Opcode19
	{
		MCRF = 0,
		BCLR = 16,
		CRNOR = 33,
		RFI = 50,
		CRANDC = 129,
		ISYNC = 150,
		CRXOR = 193,
		CRAND = 257,
		CREQV = 289,
		CRORC = 417,
		CROR = 449,
		BCCTR = 528
	};

	// extended opcodes of primary opcode 31 that callers need to recognise
	enum class Opcode31
	{
		TW = 4,
		MFTB = 371,
	};

	inline PrimaryOpcode GetPrimaryOpcode(uint32 opcode) { return static_cast<PrimaryOpcode>(opcode >> 26); }
	inline Opcode19 GetGroup19Opcode(uint32 opcode) { return static_cast<Opcode19>((opcode >> 1) & 0x3FF); }
	inline Opcode31 GetGroup31Opcode(uint32 opcode) { return static_cast<Opcode31>((opcode >> 1) & 0x3FF); }

	// the 5-bit BO operand of conditional branches
	//   0x10: ignore the CR condition
	//   0x08: branch if the CR bit is set (otherwise if clear)
	//   0x04: do not decrement CTR
	//   0x02: branch if the decremented CTR is zero (otherwise non-zero)
	struct BOField
	{
		BOField() = default;
		BOField(uint8 value) : bo(value) {}

		bool conditionIgnore() const { return (bo & 0x10) != 0; }
		bool conditionInverted() const { return (bo & 0x08) == 0; }
		bool decrementerIgnore() const { return (bo & 0x04) != 0; }
		bool decrementerMustBeZero() const { return (bo & 0x02) != 0; }
		bool branchAlways() { return conditionIgnore() && decrementerIgnore(); }

		uint8 bo;
	};

	// link bit of branch instructions
	inline bool DecodeLK(uint32 opcode)
	{
		return (opcode & 1) != 0;
	}

	namespace detail
	{
		// sign extends the low 'bits' bits of v
		inline uint32 SignExtend(uint32 v, uint32 bits)
		{
			uint32 signBit = 1u << (bits - 1);
			return (v ^ signBit) - signBit;
		}
	}

	// b / ba / bl / bla: 24-bit word offset
	inline void decodeOp_B(uint32 opcode, uint32& LI, bool& AA, bool& LK)
	{
		LI = detail::SignExtend(opcode & 0x03FFFFFC, 26);
		AA = (opcode & 2) != 0;
		LK = DecodeLK(opcode);
	}

	// bc: 14-bit word offset plus BO/BI operands
	inline void decodeOp_BC(uint32 opcode, uint32& BD, BOField& BO, uint32& BI, bool& AA, bool& LK)
	{
		BD = detail::SignExtend(opcode & 0xFFFC, 16);
		BO = BOField((uint8)((opcode >> 21) & 0x1F));
		BI = (opcode >> 16) & 0x1F;
		AA = (opcode & 2) != 0;
		LK = DecodeLK(opcode);
	}

	// bclr / bcctr (the target comes from LR or CTR)
	inline void decodeOp_BCSPR(uint32 opcode, BOField& BO, uint32& BI, bool& LK)
	{
		BO = BOField((uint8)((opcode >> 21) & 0x1F));
		BI = (opcode >> 16) & 0x1F;
		LK = DecodeLK(opcode);
	}

	// addi rD, rA, SIMM
	inline void decodeOp_ADDI(uint32 opcode, uint32& rD, uint32& rA, uint32& imm)
	{
		rD = (opcode >> 21) & 0x1F;
		rA = (opcode >> 16) & 0x1F;
		imm = detail::SignExtend(opcode & 0xFFFF, 16);
	}
}
