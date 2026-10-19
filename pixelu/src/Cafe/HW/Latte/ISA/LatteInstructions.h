// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"

// Decoding helpers for R7xx shader microcode: control flow (CF) words,
// vertex fetch clause instructions and ALU clause instructions.
// Every class is a thin view over the raw little-endian instruction words.

namespace Latte
{
	using GPRType = uint8;
};

namespace LatteISA
{
	// bits [shift, shift+width) of a word
	constexpr uint32 Bits(uint32 word, uint32 shift, uint32 width)
	{
		return (word >> shift) & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u));
	}

	constexpr bool Bit(uint32 word, uint32 bit)
	{
		return ((word >> bit) & 1u) != 0;
	}

	// replaces bits [shift, shift+width) of word with value (value is not masked)
	inline void Put(uint32& word, uint32 shift, uint32 width, uint32 value)
	{
		const uint32 mask = ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u)) << shift;
		word = (word & ~mask) | (value << shift);
	}
};

/* -------------------------------------------------------------------------
   Control flow instructions (64 bit: word0 = CF_DWORD0, word1 = CF_DWORD1)
------------------------------------------------------------------------- */

class LatteCFInstruction
{
public:
	enum class CF_COND
	{
		CF_COND_ACTIVE = 0,
		CF_COND_FALSE = 1,
		CF_COND_BOOL = 2,
		CF_COND_NOT_BOOL = 3,
	};

	// 7-bit CF_INST for the normal encodings, ALU clauses use a 4-bit
	// CF_INST at bit 26 and are tagged here with MASK_ALU
	enum OPCODE
	{
		INST_NOP = 0x00,
		INST_TEX = 0x01,
		INST_VTX = 0x02,
		INST_VTX_TC = 0x03,
		INST_LOOP_START = 0x04,
		INST_LOOP_END = 0x05,
		INST_LOOP_START_DX10 = 0x06,
		INST_LOOP_BREAK = 0x09,
		INST_JUMP = 0x0A,
		INST_ELSE = 0x0D,
		INST_POP = 0x0E,
		INST_ELSE_AFTER = 0x0F,
		INST_CALL = 0x12,
		INST_CALL_FS = 0x13,
		INST_RETURN = 0x14,
		INST_EMIT_VERTEX = 0x15,
		INST_MEM_STREAM0_WRITE = 0x20,
		INST_MEM_STREAM1_WRITE = 0x21,
		INST_MEM_STREAM2_WRITE = 0x22,
		INST_MEM_STREAM3_WRITE = 0x23,
		INST_MEM_RING_WRITE = 0x26,
		INST_EXPORT = 0x27,
		INST_EXPORT_DONE = 0x28,

		MASK_ALU = 0x40,
		INST_ALU = 0x48,
		INST_ALU_PUSH_BEFORE = 0x49,
		INST_ALU_POP_AFTER = 0x4A,
		INST_ALU_POP2_AFTER = 0x4B,
		INST_ALU_CONTINUE = 0x4D,
		INST_ALU_BREAK = 0x4E,
		INST_ALU_ELSE_AFTER = 0x4F,
	};

	OPCODE getField_Opcode() const
	{
		// the 7-bit field overlaps the ALU encoding; values with the top bit
		// set are ALU clauses and take their opcode from bits 26-29
		const uint32 wide = LatteISA::Bits(word1, 23, 7);
		if (wide < 0x40)
			return static_cast<OPCODE>(wide);
		return static_cast<OPCODE>(LatteISA::Bits(word1, 26, 4) | OPCODE::MASK_ALU);
	}

	bool getField_END_OF_PROGRAM() const
	{
		cemu_assert_debug((getField_Opcode() & OPCODE::MASK_ALU) == 0);
		return LatteISA::Bit(word1, 21);
	}

	const class LatteCFInstruction_ALU* getParser_ALU() const
	{
		cemu_assert_debug((getField_Opcode() & OPCODE::MASK_ALU) != 0);
		return reinterpret_cast<const LatteCFInstruction_ALU*>(this);
	}

	// view for the MEM_* and EXPORT* opcodes
	const class LatteCFInstruction_EXPORT_IMPORT* getParser_EXPORT() const
	{
		return reinterpret_cast<const LatteCFInstruction_EXPORT_IMPORT*>(this);
	}

	// returns the encoding view if the opcode belongs to it, otherwise nullptr
	template<typename TCFEncoding>
	const TCFEncoding* getParserIfOpcodeMatch() const
	{
		if (!TCFEncoding::MatchesOpcode(getField_Opcode()))
			return nullptr;
		return reinterpret_cast<const TCFEncoding*>(this);
	}

	// only non-ALU opcodes; clears the low four bits of CF_INST and ORs in the opcode
	void setField_Opcode(OPCODE opcode)
	{
		cemu_assert_debug((static_cast<uint32>(opcode) & static_cast<uint32>(OPCODE::MASK_ALU)) == 0);
		word1 = (word1 & ~(0xFu << 23)) | (static_cast<uint32>(opcode) << 23);
	}

protected:
	uint32 word0;
	uint32 word1;
};

// CF_DWORD0/CF_DWORD1: clause fetches, calls, loops, jumps and stack ops
class LatteCFInstruction_DEFAULT : public LatteCFInstruction
{
public:
	LatteCFInstruction_DEFAULT()
	{
		word0 = 0;
		word1 = 0;
	}

	static bool MatchesOpcode(const OPCODE opcode)
	{
		switch (opcode)
		{
		case OPCODE::INST_NOP:
		case OPCODE::INST_TEX:
		case OPCODE::INST_VTX:
		case OPCODE::INST_VTX_TC:
		case OPCODE::INST_LOOP_START:
		case OPCODE::INST_LOOP_END:
		case OPCODE::INST_LOOP_START_DX10:
		case OPCODE::INST_JUMP:
		case OPCODE::INST_ELSE:
		case OPCODE::INST_POP:
		case OPCODE::INST_CALL:
		case OPCODE::INST_CALL_FS:
		case OPCODE::INST_RETURN:
		case OPCODE::INST_EMIT_VERTEX:
			return true;
		default:
			return false;
		}
	}

	// clause or jump target in bytes (stored in 64-bit units)
	uint32 getField_ADDR() const { return word0 << 3; }
	uint32 getField_POP_COUNT() const { return LatteISA::Bits(word1, 0, 3); }
	uint32 getField_CF_CONST() const { return LatteISA::Bits(word1, 3, 5); }
	CF_COND getField_COND() const { return static_cast<CF_COND>(LatteISA::Bits(word1, 8, 2)); }

	// instruction count of the clause: 3 bits at 10 plus the R7xx extension bit at 19
	uint32 getField_COUNT() const
	{
		return (LatteISA::Bits(word1, 10, 3) | (LatteISA::Bits(word1, 19, 1) << 3)) + 1;
	}

	uint32 getField_CALL_COUNT() const { return LatteISA::Bits(word1, 13, 6); }
	uint32 getField_VALID_PIXEL_MODE() const { return LatteISA::Bits(word1, 22, 1); }
	uint32 getField_WHOLE_QUAD_MODE() const { return LatteISA::Bits(word1, 30, 1); }
	uint32 getField_BARRIER() const { return LatteISA::Bits(word1, 31, 1); }

	// fetch clause bytes of a VTX/VTX_TC instruction (16 bytes per fetch)
	std::span<uint8> getClauseCode(std::span<uint8> programCode) const
	{
		const uint32 offset = getField_ADDR();
		const uint32 length = getField_COUNT() * 16;
		cemu_assert_debug(getField_Opcode() == LatteCFInstruction::INST_VTX || getField_Opcode() == LatteCFInstruction::INST_VTX_TC);
		cemu_assert_debug(offset <= programCode.size());
		cemu_assert_debug(programCode.size() - offset >= length);
		return programCode.subspan(offset, length);
	}

	void setField_ADDR(uint32 addrInBytes)
	{
		word0 = addrInBytes >> 3;
	}

	void setField_COUNT(uint32 count)
	{
		cemu_assert_debug(count > 0 && count <= 16);
		const uint32 stored = count - 1;
		LatteISA::Put(word1, 10, 3, stored & 7);
		LatteISA::Put(word1, 19, 1, (stored >> 3) & 1);
	}

	void setField_BARRIER(bool isEnabled)
	{
		LatteISA::Put(word1, 31, 1, isEnabled ? 1u : 0u);
	}
};

// CF_ALLOC_EXPORT_DWORD0 + CF_ALLOC_EXPORT_DWORD1_BUF or _SWIZ
// word1 bits 0-15 hold either the buffer fields or the swizzle fields
class LatteCFInstruction_EXPORT_IMPORT : public LatteCFInstruction
{
public:
	static bool MatchesOpcode(const OPCODE opcode)
	{
		switch (opcode)
		{
		case OPCODE::INST_MEM_STREAM0_WRITE:
		case OPCODE::INST_MEM_STREAM1_WRITE:
		case OPCODE::INST_MEM_STREAM2_WRITE:
		case OPCODE::INST_MEM_STREAM3_WRITE:
		case OPCODE::INST_MEM_RING_WRITE:
		case OPCODE::INST_EXPORT:
		case OPCODE::INST_EXPORT_DONE:
			return true;
		default:
			return false;
		}
	}

	enum EXPORT_TYPE : uint8
	{
		PIXEL = 0,
		POSITION = 1,
		PARAMETER = 2,
		UNUSED_VAL = 3
	};

	enum COMPSEL : uint8
	{
		X,
		Y,
		Z,
		W,
		CONST_0F,
		CONST_1F,
		RESERVED,
		MASKED
	};

	uint32 getField_ARRAY_BASE() const { return LatteISA::Bits(word0, 0, 13); }
	EXPORT_TYPE getField_TYPE() const { return static_cast<EXPORT_TYPE>(LatteISA::Bits(word0, 13, 2)); }
	uint32 getField_RW_GPR() const { return LatteISA::Bits(word0, 15, 7); }
	bool getField_RW_REL() const { return LatteISA::Bit(word0, 22); } // RW_GPR is indexed
	uint32 getField_INDEX_GPR() const { return LatteISA::Bits(word0, 23, 7); }
	uint8 getField_ELEM_SIZE() const { return static_cast<uint8>(LatteISA::Bits(word0, 30, 2) + 1); }
	uint8 getField_BURST_COUNT() const { return static_cast<uint8>(LatteISA::Bits(word1, 17, 4) + 1); }

	// a non-zero component mask means the BUF variant of word1
	bool isEncodingBUF() const
	{
		return LatteISA::Bits(word1, 12, 4) != 0;
	}

	// SWIZ variant
	COMPSEL getSwizField_SEL_X() const { cemu_assert_debug(!isEncodingBUF()); return static_cast<COMPSEL>(LatteISA::Bits(word1, 0, 3)); }
	COMPSEL getSwizField_SEL_Y() const { cemu_assert_debug(!isEncodingBUF()); return static_cast<COMPSEL>(LatteISA::Bits(word1, 3, 3)); }
	COMPSEL getSwizField_SEL_Z() const { cemu_assert_debug(!isEncodingBUF()); return static_cast<COMPSEL>(LatteISA::Bits(word1, 6, 3)); }
	COMPSEL getSwizField_SEL_W() const { cemu_assert_debug(!isEncodingBUF()); return static_cast<COMPSEL>(LatteISA::Bits(word1, 9, 3)); }

	// BUF variant
	uint32 getBufField_ARRAY_SIZE() const
	{
		cemu_assert_debug(isEncodingBUF());
		return LatteISA::Bits(word1, 0, 12);
	}

	uint32 getBufField_COMP_MASK() const
	{
		cemu_assert_debug(isEncodingBUF());
		return LatteISA::Bits(word1, 12, 4);
	}

	bool getValidPixelMode() const { return LatteISA::Bit(word1, 22); }
	bool getWholeQuadMode() const { return LatteISA::Bit(word1, 30); }
};

// CF_ALU_DWORD0 + CF_ALU_DWORD1
class LatteCFInstruction_ALU : public LatteCFInstruction
{
public:
	static bool MatchesOpcode(const OPCODE opcode)
	{
		switch (opcode)
		{
		case OPCODE::INST_ALU:
		case OPCODE::INST_ALU_PUSH_BEFORE:
		case OPCODE::INST_ALU_POP_AFTER:
		case OPCODE::INST_ALU_POP2_AFTER:
		case OPCODE::INST_ALU_CONTINUE:
		case OPCODE::INST_ALU_BREAK:
		case OPCODE::INST_ALU_ELSE_AFTER:
			return true;
		default:
			return false;
		}
	}

	uint32 getField_ADDR() const { return LatteISA::Bits(word0, 0, 22); } // in 64-bit units
	uint32 getField_KCACHE_BANK0() const { return LatteISA::Bits(word0, 22, 4); }
	uint32 getField_KCACHE_BANK1() const { return LatteISA::Bits(word0, 26, 4); }
	uint32 getField_KCACHE_ADDR0() const { return LatteISA::Bits(word1, 2, 8); }
	uint32 getField_KCACHE_ADDR1() const { return LatteISA::Bits(word1, 10, 8); }
	uint32 getField_COUNT() const { return LatteISA::Bits(word1, 18, 7) + 1; }
	bool getField_USES_WATERFALL() const { return LatteISA::Bit(word1, 25); }
};

static_assert(sizeof(LatteCFInstruction) == 8);
static_assert(sizeof(LatteCFInstruction_DEFAULT) == 8);
static_assert(sizeof(LatteCFInstruction_EXPORT_IMPORT) == 8);
static_assert(sizeof(LatteCFInstruction_ALU) == 8);

/* -------------------------------------------------------------------------
   Vertex fetch clause instruction (128 bit, VTX_DWORD0..2 + padding)
------------------------------------------------------------------------- */

class LatteClauseInstruction_VTX
{
public:
	LatteClauseInstruction_VTX()
	{
		word0 = 0;
		word1 = 0;
		word2 = 0;
		word3 = 0;
	}

	enum class VTX_INST
	{
		_VTX_INST_FETCH = 0,
		_VTX_INST_SEMANTIC = 1,
		_VTX_INST_MEM = 2
	};

	enum class SRC_SEL
	{
		SEL_X = 0,
		SEL_Y = 1,
		SEL_Z = 2,
		SEL_W = 3,
	};

	enum class DST_SEL
	{
		SEL_X = 0,
		SEL_Y = 1,
		SEL_Z = 2,
		SEL_W = 3,
		SEL_0 = 4,
		SEL_1 = 5,
		SEL_RESERVED = 6,
		SEL_MASK = 7,
	};

	enum class NUM_FORMAT_ALL
	{
		NUM_FORMAT_NORM = 0, // normalized to [0,1] or [-1,1]
		NUM_FORMAT_INT = 1,
		NUM_FORMAT_SCALED = 2,
	};

	enum class FORMAT_COMP
	{
		COMP_UNSIGNED = 0,
		COMP_SIGNED = 1,
	};

	enum class SRF_MODE
	{
		SRF_MODE_ZERO_CLAMP_MINUS_ONE = 0,
		SRF_MODE_NO_ZERO = 1,
	};

	// word0
	VTX_INST getField_VTX_INST() const { return static_cast<VTX_INST>(LatteISA::Bits(word0, 0, 5)); }
	LatteConst::VertexFetchType2 getField_FETCH_TYPE() const { return static_cast<LatteConst::VertexFetchType2>(LatteISA::Bits(word0, 5, 2)); }
	uint32 getField_BUFFER_ID() const { return LatteISA::Bits(word0, 8, 8); }
	uint32 getField_SRC_GPR() const { return LatteISA::Bits(word0, 16, 7); }
	bool getField_SRC_REL() const { return LatteISA::Bit(word0, 23); }
	SRC_SEL getField_SRC_SEL_X() const { return static_cast<SRC_SEL>(LatteISA::Bits(word0, 24, 2)); }

	// word1 (common part of the GPR and SEM variants)
	DST_SEL getField_DST_SEL(uint32 index) const
	{
		cemu_assert_debug(index <= 3);
		return static_cast<DST_SEL>(LatteISA::Bits(word1, 9 + index * 3, 3));
	}

	bool getField_USE_CONST_FIELDS() const { return LatteISA::Bit(word1, 21); }
	Latte::E_HWFMT getField_DATA_FORMAT() const { return static_cast<Latte::E_HWFMT>(LatteISA::Bits(word1, 22, 6)); }
	NUM_FORMAT_ALL getField_NUM_FORMAT_ALL() const { return static_cast<NUM_FORMAT_ALL>(LatteISA::Bits(word1, 28, 2)); }
	FORMAT_COMP getField_FORMAT_COMP_ALL() const { return static_cast<FORMAT_COMP>(LatteISA::Bits(word1, 30, 1)); }
	// PixelU has always decoded SRF_MODE_ALL from bit 30 as well
	SRF_MODE getField_SRF_MODE_ALL() const { return static_cast<SRF_MODE>(LatteISA::Bits(word1, 30, 1)); }

	// word1, SEM variant
	uint32 getFieldSEM_SEMANTIC_ID() const { return LatteISA::Bits(word1, 0, 8); }

	// word2
	uint32 getField_OFFSET() const { return LatteISA::Bits(word2, 0, 16); }
	LatteConst::VertexFetchEndianMode getField_ENDIAN_SWAP() const { return static_cast<LatteConst::VertexFetchEndianMode>(LatteISA::Bits(word2, 16, 2)); }
	bool getField_CONST_BUF_NO_STRIDE() const { return LatteISA::Bit(word2, 18); }

	// setters (values are not masked, callers pass in-range values)
	LatteClauseInstruction_VTX& setField_VTX_INST(VTX_INST inst)
	{
		LatteISA::Put(word0, 0, 5, static_cast<uint32>(inst));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_FETCH_TYPE(LatteConst::VertexFetchType2 fetchType)
	{
		LatteISA::Put(word0, 5, 2, static_cast<uint32>(fetchType));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_BUFFER_ID(uint32 bufferId)
	{
		LatteISA::Put(word0, 8, 8, bufferId);
		return *this;
	}

	LatteClauseInstruction_VTX& setField_SRC_GPR(uint32 srcGPR)
	{
		LatteISA::Put(word0, 16, 7, srcGPR);
		return *this;
	}

	LatteClauseInstruction_VTX& setField_SRC_REL(bool isRel)
	{
		LatteISA::Put(word0, 23, 1, isRel ? 1u : 0u);
		return *this;
	}

	LatteClauseInstruction_VTX& setField_SRC_SEL_X(SRC_SEL srcSel)
	{
		LatteISA::Put(word0, 24, 2, static_cast<uint32>(srcSel));
		return *this;
	}

	LatteClauseInstruction_VTX& setFieldSEM_SEMANTIC_ID(uint32 semanticId)
	{
		cemu_assert_debug(semanticId <= 0xFF);
		LatteISA::Put(word1, 0, 8, semanticId);
		return *this;
	}

	LatteClauseInstruction_VTX& setField_DST_SEL(uint32 index, DST_SEL dstSel)
	{
		cemu_assert_debug(index <= 3);
		LatteISA::Put(word1, 9 + index * 3, 3, static_cast<uint32>(dstSel));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_DATA_FORMAT(Latte::E_HWFMT fetchFormat)
	{
		LatteISA::Put(word1, 22, 6, static_cast<uint32>(fetchFormat));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_NUM_FORMAT_ALL(NUM_FORMAT_ALL nfa)
	{
		LatteISA::Put(word1, 28, 2, static_cast<uint32>(nfa));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_FORMAT_COMP_ALL(FORMAT_COMP componentFormat)
	{
		LatteISA::Put(word1, 30, 1, static_cast<uint32>(componentFormat));
		return *this;
	}

	LatteClauseInstruction_VTX& setField_OFFSET(uint32 offset)
	{
		cemu_assert_debug(offset < 0x10000);
		LatteISA::Put(word2, 0, 16, offset);
		return *this;
	}

	LatteClauseInstruction_VTX& setField_ENDIAN_SWAP(LatteConst::VertexFetchEndianMode endianSwap)
	{
		LatteISA::Put(word2, 16, 2, static_cast<uint32>(endianSwap));
		return *this;
	}

protected:
	uint32 word0;
	uint32 word1;
	uint32 word2;
	uint32 word3; // padding to 128 bit
};

static_assert(sizeof(LatteClauseInstruction_VTX) == 16);

/* -------------------------------------------------------------------------
   ALU clause instructions (64 bit: ALU_DWORD0 + ALU_DWORD1_OP2/OP3)
------------------------------------------------------------------------- */

class LatteClauseInstruction_ALU
{
public:
	// ALU_INST of the OP2 encoding (only the ones PixelU decodes by name)
	enum OPCODE_OP2
	{
		ADD = 0x00,
		MUL = 0x01,
		MUL_IEEE = 0x02,
		MOV = 0x19,
		DOT4 = 0x50,
		CUBE = 0x52,
		EXP_IEEE = 0x61,
		LOG_CLAMPED = 0x62,
		LOG_IEEE = 0x63,
		RECIP_CLAMPED = 0x64,
		RECIP_FF = 0x65,
		RECIP_IEEE = 0x66,
		RECIPSQRT_CLAMPED = 0x67,
		RECIPSQRT_FF = 0x68,
		RECIPSQRT_IEEE = 0x69,
		SQRT_IEEE = 0x6A,
		FLT_TO_INT = 0x6B,
		INT_TO_FLOAT = 0x6C,
		UINT_TO_FLOAT = 0x6D,
		SIN = 0x6E,
		COS = 0x6F,
		ASHR_INT = 0x70,
		LSHR_INT = 0x71,
		LSHL_INT = 0x72,
		MULLO_INT = 0x73,
		MULLO_UINT = 0x75,
		FLT_TO_UINT = 0x79,
	};

	// OP3 instructions have a 5-bit opcode at bit 13 with a value of 8 or more
	bool isOP3() const
	{
		return LatteISA::Bits(word1, 13, 5) >= 8;
	}

	OPCODE_OP2 getOP2Code() const
	{
		return static_cast<OPCODE_OP2>(LatteISA::Bits(word1, 7, 11));
	}

	bool isLastInGroup() const
	{
		return LatteISA::Bit(word0, 31);
	}

	const class LatteClauseInstruction_ALU_OP2* getOP2Instruction() const
	{
		return reinterpret_cast<const LatteClauseInstruction_ALU_OP2*>(this);
	}

protected:
	const uint32 word0 = 0;
	const uint32 word1 = 0;
};

// ALU source operand selector (9 bit)
class LatteALUSrcSel
{
public:
	LatteALUSrcSel(const uint16 op) : m_op(op) {};

	bool isGPR() const { return m_op < 128; };
	bool isAnyConst() const { return m_op >= 248 && m_op <= 252; }; // 0.0, 1.0, 1 int, -1 int, 0.5
	bool isConst_0F() const { return m_op == 248; };
	bool isLiteral() const { return m_op == 253; };
	bool isCFile() const { return m_op >= 256; }; // constant file (uniform register)

	uint32 getGPR() const { return m_op; };
	uint32 getCFile() const { return m_op & 0xFF; };

private:
	const uint16 m_op;
};

class LatteClauseInstruction_ALU_OP2 : public LatteClauseInstruction_ALU
{
public:
	// ALU_DWORD0
	const LatteALUSrcSel getSrc0Sel() const { return LatteALUSrcSel(static_cast<uint16>(LatteISA::Bits(word0, 0, 9))); }
	bool isSrc0Rel() const { return LatteISA::Bit(word0, 9); }
	const uint8 getSrc0Chan() const { return static_cast<uint8>(LatteISA::Bits(word0, 10, 2)); }
	bool isSrc0Neg() const { return LatteISA::Bit(word0, 12); }
	const LatteALUSrcSel getSrc1Sel() const { return LatteALUSrcSel(static_cast<uint16>(LatteISA::Bits(word0, 13, 9))); }
	bool isSrc1Rel() const { return LatteISA::Bit(word0, 22); }
	const uint8 getSrc1Chan() const { return static_cast<uint8>(LatteISA::Bits(word0, 23, 2)); }
	bool isSrc1Neg() const { return LatteISA::Bit(word0, 25); }
	uint8 getIndexMode() const { return static_cast<uint8>(LatteISA::Bits(word0, 26, 3)); }

	// ALU_DWORD1_OP2
	bool getWriteMask() const { return LatteISA::Bit(word1, 4); }
	uint8 getOMod() const { return static_cast<uint8>(LatteISA::Bits(word1, 5, 2)); }
	uint32 getDestGpr() const { return LatteISA::Bits(word1, 21, 7); }
	bool getDestRel() const { return LatteISA::Bit(word1, 28); }
	uint32 getDestElem() const { return LatteISA::Bits(word1, 29, 2); }
	bool getDestClamp() const { return LatteISA::Bit(word1, 31); }

	// instructions that only the scalar (t) unit can execute
	bool isTranscedentalUnit() const
	{
		switch (getOP2Code())
		{
		case EXP_IEEE:
		case LOG_CLAMPED:
		case LOG_IEEE:
		case RECIP_CLAMPED:
		case RECIP_FF:
		case RECIP_IEEE:
		case RECIPSQRT_CLAMPED:
		case RECIPSQRT_FF:
		case RECIPSQRT_IEEE:
		case SQRT_IEEE:
		case FLT_TO_INT:
		case FLT_TO_UINT:
		case INT_TO_FLOAT:
		case UINT_TO_FLOAT:
		case SIN:
		case COS:
		case MULLO_INT:
		case MULLO_UINT:
			return true;
		default:
			return false;
		}
	}
};

static_assert(sizeof(LatteClauseInstruction_ALU) == 8);
