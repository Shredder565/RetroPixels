// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Single line PowerPC (Espresso) assembler and table based disassembler,
// used by graphic pack patches and the debugger.

#include <boost/container/small_vector.hpp>

#define PPCASM_OPERAND_COUNT		5

// operand kinds reported by the disassembler
#define PPCASM_OPERAND_TYPE_GPR		0 // r0..r31
#define PPCASM_OPERAND_TYPE_FPR		1 // f0..f31
#define PPCASM_OPERAND_TYPE_SPR		2 // special purpose register number
#define PPCASM_OPERAND_TYPE_IMM		3 // integer immediate
#define PPCASM_OPERAND_TYPE_MEM		4 // displacement(gpr)
#define PPCASM_OPERAND_TYPE_CIMM	5 // code address (branch target)
#define PPCASM_OPERAND_TYPE_CR		6 // cr0..cr7
#define PPCASM_OPERAND_TYPE_CR_BIT	7 // single condition bit, 0..31
#define PPCASM_OPERAND_TYPE_PSQMODE	8 // paired single quantization mode

// Instruction identifiers. The numeric values are visible to callers, keep the order.
// A trailing underscore stands for the '.' (record) form.
enum PPCASM_OP
{
	PPCASM_OP_UKN, PPCASM_OP_ADDI, PPCASM_OP_SUBI, PPCASM_OP_ADDIS, PPCASM_OP_ADDIC,
	PPCASM_OP_ADDIC_, PPCASM_OP_ADD, PPCASM_OP_ADD_, PPCASM_OP_SUBF, PPCASM_OP_SUBF_,
	PPCASM_OP_SUBFC, PPCASM_OP_SUBFC_, PPCASM_OP_SUBFE, PPCASM_OP_SUBFE_, PPCASM_OP_SUBFIC,
	PPCASM_OP_SUB, PPCASM_OP_SUB_, PPCASM_OP_MULLI, PPCASM_OP_MULLW, PPCASM_OP_MULLW_,
	PPCASM_OP_MULHW, PPCASM_OP_MULHW_, PPCASM_OP_MULHWU, PPCASM_OP_MULHWU_, PPCASM_OP_DIVW,
	PPCASM_OP_DIVW_, PPCASM_OP_DIVWU, PPCASM_OP_DIVWU_, PPCASM_OP_AND, PPCASM_OP_AND_,
	PPCASM_OP_ANDC, PPCASM_OP_ANDC_, PPCASM_OP_OR, PPCASM_OP_OR_, PPCASM_OP_ORC,
	PPCASM_OP_XOR, PPCASM_OP_NOR, PPCASM_OP_NOR_, PPCASM_OP_NOT, PPCASM_OP_NOT_,
	PPCASM_OP_NEG, PPCASM_OP_NEG_, PPCASM_OP_ANDI_, PPCASM_OP_ANDIS_, PPCASM_OP_ORI,
	PPCASM_OP_ORIS, PPCASM_OP_XORI, PPCASM_OP_XORIS, PPCASM_OP_CNTLZW, PPCASM_OP_EXTSB,
	PPCASM_OP_EXTSH, PPCASM_OP_CNTLZW_, PPCASM_OP_EXTSB_, PPCASM_OP_EXTSH_, PPCASM_OP_SRAW,
	PPCASM_OP_SRAW_, PPCASM_OP_SRAWI, PPCASM_OP_SRAWI_, PPCASM_OP_SLW, PPCASM_OP_SLW_,
	PPCASM_OP_SRW, PPCASM_OP_SRW_, PPCASM_OP_RLWINM, PPCASM_OP_RLWINM_, PPCASM_OP_RLWIMI,
	PPCASM_OP_RLWIMI_, PPCASM_OP_EXTLWI, PPCASM_OP_EXTLWI_, PPCASM_OP_EXTRWI, PPCASM_OP_EXTRWI_,
	PPCASM_OP_ROTLWI, PPCASM_OP_ROTLWI_, PPCASM_OP_ROTRWI, PPCASM_OP_ROTRWI_, PPCASM_OP_SLWI,
	PPCASM_OP_SLWI_, PPCASM_OP_SRWI, PPCASM_OP_SRWI_, PPCASM_OP_CLRLWI, PPCASM_OP_CLRLWI_,
	PPCASM_OP_CLRRWI, PPCASM_OP_CLRRWI_, PPCASM_OP_RLWNM, PPCASM_OP_RLWNM_, PPCASM_OP_ROTLW,
	PPCASM_OP_ROTLW_, PPCASM_OP_CMPWI, PPCASM_OP_CMPLWI, PPCASM_OP_CMPW, PPCASM_OP_CMPLW,
	PPCASM_OP_MR, PPCASM_OP_MR_, PPCASM_OP_MFSPR, PPCASM_OP_MTSPR, PPCASM_OP_LMW,
	PPCASM_OP_LWZ, PPCASM_OP_LWZU, PPCASM_OP_LWZX, PPCASM_OP_LWZUX, PPCASM_OP_LHZ,
	PPCASM_OP_LHZU, PPCASM_OP_LHZX, PPCASM_OP_LHZUX, PPCASM_OP_LHA, PPCASM_OP_LHAU,
	PPCASM_OP_LHAX, PPCASM_OP_LHAUX, PPCASM_OP_LBZ, PPCASM_OP_LBZU, PPCASM_OP_LBZX,
	PPCASM_OP_LBZUX, PPCASM_OP_STMW, PPCASM_OP_STW, PPCASM_OP_STWU, PPCASM_OP_STWX,
	PPCASM_OP_STWUX, PPCASM_OP_STH, PPCASM_OP_STHU, PPCASM_OP_STHX, PPCASM_OP_STHUX,
	PPCASM_OP_STB, PPCASM_OP_STBU, PPCASM_OP_STBX, PPCASM_OP_STBUX, PPCASM_OP_LWBRX,
	PPCASM_OP_LHBRX, PPCASM_OP_STWBRX, PPCASM_OP_STHBRX, PPCASM_OP_STSWI, PPCASM_OP_LWARX,
	PPCASM_OP_STWCX_, PPCASM_OP_B, PPCASM_OP_BA, PPCASM_OP_BL, PPCASM_OP_BLA,
	PPCASM_OP_BC, PPCASM_OP_BNE, PPCASM_OP_BEQ, PPCASM_OP_BGE, PPCASM_OP_BGT,
	PPCASM_OP_BLT, PPCASM_OP_BLE, PPCASM_OP_BDZ, PPCASM_OP_BDNZ, PPCASM_OP_BLR,
	PPCASM_OP_BLRL, PPCASM_OP_BLTLR, PPCASM_OP_BLELR, PPCASM_OP_BEQLR, PPCASM_OP_BGELR,
	PPCASM_OP_BGTLR, PPCASM_OP_BNELR, PPCASM_OP_BDZLR, PPCASM_OP_BDNZLR, PPCASM_OP_BDZLRL,
	PPCASM_OP_BDNZLRL, PPCASM_OP_BCTR, PPCASM_OP_BCTRL, PPCASM_OP_CROR, PPCASM_OP_CRORC,
	PPCASM_OP_CRXOR, PPCASM_OP_CREQV, PPCASM_OP_CRNOR, PPCASM_OP_CRAND, PPCASM_OP_CRNAND,
	PPCASM_OP_CRANDC, PPCASM_OP_CRSET, PPCASM_OP_CRCLR, PPCASM_OP_CRMOVE, PPCASM_OP_CRNOT,
	PPCASM_OP_LFS, PPCASM_OP_LFSU, PPCASM_OP_LFSX, PPCASM_OP_LFSUX, PPCASM_OP_LFD,
	PPCASM_OP_LFDU, PPCASM_OP_LFDX, PPCASM_OP_LFDUX, PPCASM_OP_STFS, PPCASM_OP_STFSU,
	PPCASM_OP_STFSX, PPCASM_OP_STFSUX, PPCASM_OP_STFD, PPCASM_OP_STFDU, PPCASM_OP_STFDX,
	PPCASM_OP_STFDUX, PPCASM_OP_STFIWX, PPCASM_OP_FMR, PPCASM_OP_FABS, PPCASM_OP_FNEG,
	PPCASM_OP_FRSP, PPCASM_OP_FRSQRTE, PPCASM_OP_FADD, PPCASM_OP_FADDS, PPCASM_OP_FSUB,
	PPCASM_OP_FSUBS, PPCASM_OP_FMUL, PPCASM_OP_FMULS, PPCASM_OP_FDIV, PPCASM_OP_FDIVS,
	PPCASM_OP_FMADD, PPCASM_OP_FMADDS, PPCASM_OP_FNMADD, PPCASM_OP_FNMADDS, PPCASM_OP_FMSUB,
	PPCASM_OP_FMSUBS, PPCASM_OP_FNMSUB, PPCASM_OP_FNMSUBS, PPCASM_OP_FCTIWZ, PPCASM_OP_FCMPU,
	PPCASM_OP_FCMPO, PPCASM_OP_PS_MERGE00, PPCASM_OP_PS_MERGE01, PPCASM_OP_PS_MERGE10, PPCASM_OP_PS_MERGE11,
	PPCASM_OP_PS_ADD, PPCASM_OP_PS_SUB, PPCASM_OP_PS_DIV, PPCASM_OP_PS_MUL, PPCASM_OP_PS_MADD,
	PPCASM_OP_PS_MSUB, PPCASM_OP_PS_NMADD, PPCASM_OP_PS_NMSUB, PPCASM_OP_ISYNC, PPCASM_OP_MFCR,
	PPCASM_OP_MTCRF, PPCASM_OP_MCRF, PPCASM_OP_NOP, PPCASM_OP_LI, PPCASM_OP_LIS,
	PPCASM_OP_MFLR, PPCASM_OP_MTLR, PPCASM_OP_MFCTR, PPCASM_OP_MTCTR,
};

struct PPCDisassemblerOperand
{
	uint8 type;
	uint16 registerIndex;
	union
	{
		sint32 immS32;
		uint32 immU32;
	};
	bool isSignedImm;
	uint8 immWidth; // bits
};

struct PPCDisassembledInstruction
{
	uint32 ppcAsmCode;
	uint8 operandMask; // bit n set = operand[n] is shown
	PPCDisassemblerOperand operand[PPCASM_OPERAND_COUNT];
	bool branchHintBitSet{false}; // conditional branches only
};

// upper case mnemonic, "." for record forms
const char* ppcAssembler_getInstructionName(uint32 ppcAsmOp);

void ppcAssembler_disassemble(uint32 virtualAddress, uint32 opcode, PPCDisassembledInstruction* disInstr);

// work left for the caller when an operand refers to symbols not known yet
enum class PPCASM_RELOC
{
	U32_MASKED_IMM, // immediate bit field inside an instruction word
	BRANCH_S16,
	BRANCH_S26,
	FLOAT,
	DOUBLE,
	U32,
	U16,
	U8,
};

struct PPCAssemblerReloc
{
	PPCAssemblerReloc(PPCASM_RELOC relocType, std::string expression, uint32 byteOffset, uint8 bitOffset, uint8 bitCount) : m_relocType(relocType), m_expression(expression), m_byteOffset(byteOffset), m_bitOffset(bitOffset), m_bitCount(bitCount) {};
	PPCASM_RELOC m_relocType;
	std::string m_expression;
	uint32 m_byteOffset;
	uint8 m_bitOffset;
	uint8 m_bitCount;
	bool m_isApplied{};
	bool isApplied() { return m_isApplied; };
	void setApplied() { m_isApplied = true; };
};

struct PPCAssemblerInOut
{
	// input
	uint32 virtualAddress;
	bool forceNoAlignment{}; // treat every alignment as 1, including .align
	// output
	boost::container::small_vector<uint8, 16> outputData;
	std::vector<PPCAssemblerReloc> list_relocs;
	std::string errorMsg;
	uint32 alignmentRequirement{}; // 0 = none
	uint32 alignmentPaddingSize{}; // padding bytes that go before the output
	uint32 virtualAddressAligned{}; // address the output starts at
};

bool ppcAssembler_assembleSingleInstruction(char const* text, PPCAssemblerInOut* ctx);

// rlwinm style mask for bits MB..ME (IBM numbering), wrapping when MB > ME
static uint32 ppcAssembler_generateMaskRLW(int MB, int ME)
{
	uint32 fromMB = 0xFFFFFFFFu >> MB;
	uint32 toME = 0xFFFFFFFFu << (31 - ME);
	return MB <= ME ? (fromMB & toME) : (fromMB | toME);
}
