// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
// Definitions shared by the Espresso interpreter and the code that looks at
// guest CPU state from the outside (recompiler front end, coreinit thread
// contexts, the debugger). Register numbers and instruction field layouts
// follow the IBM PowerPC 750CL user manual.

#include "Cafe/HW/Espresso/PPCState.h"

// ---------------------------------------------------------------------------
// Special purpose register numbers as encoded in mfspr/mtspr (spr field with
// its two 5-bit halves already swapped back).

#define SPR_XER		1
#define SPR_LR		8
#define SPR_CTR		9
#define SPR_DEC		22
#define SPR_SDR1	25
#define SPR_SRR0	26
#define SPR_SRR1	27
#define SPR_TBL		268		// read form (mftb)
#define SPR_TBU		269		// read form (mftb)
#define SPR_PVR		287
#define SPR_UGQR0	896
#define SPR_UGQR1	897
#define SPR_UGQR2	898
#define SPR_UGQR3	899
#define SPR_UGQR4	900
#define SPR_UGQR5	901
#define SPR_UGQR6	902
#define SPR_UGQR7	903
#define SPR_GQR0	912
#define SPR_GQR1	913
#define SPR_GQR2	914
#define SPR_GQR3	915
#define SPR_GQR4	916
#define SPR_GQR5	917
#define SPR_GQR6	918
#define SPR_GQR7	919
#define SPR_HID2	920
#define SPR_DMAU	922
#define SPR_DMAL	923
#define SPR_SCR		947
#define SPR_UPIR	1007
#define SPR_HID0	1008
#define SPR_HID1	1009
#define SPR_FPECR	1022

// condition register: bit position inside a 4-bit field (cr[] stores one bit per byte, cr0.LT first)
#define CR_BIT_LT	0
#define CR_BIT_GT	1
#define CR_BIT_EQ	2
#define CR_BIT_SO	3

// XER bit numbers (LSB = 0). CA, OV and SO live in PPCInterpreter_t::xer_ca/xer_ov/xer_so
#define XER_BIT_CA	29
#define XER_BIT_OV	30
#define XER_BIT_SO	31

// FPSCR bits
#define FPSCR_VXSNAN	(1 << 24)
#define FPSCR_VXVC		(1 << 19)
#define FPSCR_VE		(1 << 7)

// MSR bits
#define MSR_POW		(1 << 18)
#define MSR_ILE		(1 << 16)
#define MSR_EE		(1 << 15)
#define MSR_PR		(1 << 14)
#define MSR_FP		(1 << 13)
#define MSR_ME		(1 << 12)
#define MSR_FE0		(1 << 11)
#define MSR_SE		(1 << 10)
#define MSR_BE		(1 << 9)
#define MSR_FE1		(1 << 8)
#define MSR_IP		(1 << 6)
#define MSR_IR		(1 << 5)
#define MSR_DR		(1 << 4)
#define MSR_PM		(1 << 2)
#define MSR_RI		(1 << 1)
#define MSR_LE		(1 << 0)

// no-op placeholder for consistency checks on reserved instruction fields
#define PPC_ASSERT(cond)

// ---------------------------------------------------------------------------
// Instruction word fields. Bit numbers below are IBM style (bit 0 = MSB).

#define PPC_OPC_RC	1			// record bit (bit 31)
#define PPC_OPC_LK	1			// link bit (bit 31, branches)
#define PPC_OPC_AA	(1 << 1)	// absolute address bit (bit 30, branches)
#define PPC_OPC_OE	(1 << 10)	// overflow enable bit (bit 21, XO form)

// extracts __bitCount bits that end at IBM bit position __index
#define PPC_getBits(__value, __index, __bitCount) (((__value) >> (31 - (__index))) & ((1 << (__bitCount)) - 1))

inline uint32 ppcOpField_D(uint32 op) { return (op >> 21) & 0x1F; } // rD / rS / frD / BO / crbD
inline uint32 ppcOpField_A(uint32 op) { return (op >> 16) & 0x1F; } // rA / frA / BI / crbA
inline uint32 ppcOpField_B(uint32 op) { return (op >> 11) & 0x1F; } // rB / frB / SH / NB / crbB
inline uint32 ppcOpField_C(uint32 op) { return (op >> 6) & 0x1F; }  // frC / MB
inline uint32 ppcOpField_E(uint32 op) { return (op >> 1) & 0x1F; }  // ME
inline uint32 ppcOpSImm16(uint32 op) { return (uint32)(sint32)(sint16)(uint16)op; }

// the macros below assign the decoded fields to caller supplied variables
#define PPC_OPC_TEMPL_A(opc, rD, rA, rB, rC) { rD = ppcOpField_D(opc); rA = ppcOpField_A(opc); rB = ppcOpField_B(opc); rC = ppcOpField_C(opc); }
#define PPC_OPC_TEMPL_B(opc, BO, BI, BD) { BO = ppcOpField_D(opc); BI = ppcOpField_A(opc); BD = ppcOpSImm16((opc) & 0xFFFC); }
#define PPC_OPC_TEMPL_D_SImm(opc, rD, rA, imm) { rD = ppcOpField_D(opc); rA = ppcOpField_A(opc); imm = ppcOpSImm16(opc); }
#define PPC_OPC_TEMPL_D_UImm(opc, rD, rA, imm) { rD = ppcOpField_D(opc); rA = ppcOpField_A(opc); imm = (opc) & 0xFFFF; }
#define PPC_OPC_TEMPL_D_Shift16(opc, rD, rA, imm) { rD = ppcOpField_D(opc); rA = ppcOpField_A(opc); imm = (opc) << 16; }
#define PPC_OPC_TEMPL_I(opc, LI) { LI = (uint32)(((sint32)((opc) << 6)) >> 6) & ~3u; }
#define PPC_OPC_TEMPL_M(opc, rS, rA, SH, MB, ME) { rS = ppcOpField_D(opc); rA = ppcOpField_A(opc); SH = ppcOpField_B(opc); MB = ppcOpField_C(opc); ME = ppcOpField_E(opc); }
#define PPC_OPC_TEMPL_X(opc, rS, rA, rB) { rS = ppcOpField_D(opc); rA = ppcOpField_A(opc); rB = ppcOpField_B(opc); }
#define PPC_OPC_TEMPL_XFX(opc, rS, CRM) { rS = ppcOpField_D(opc); CRM = ((opc) >> 12) & 0xFF; }
#define PPC_OPC_TEMPL_XO(opc, rS, rA, rB) PPC_OPC_TEMPL_X(opc, rS, rA, rB)
#define PPC_OPC_TEMPL_XL(opc, BO, BI, BD) { BO = ppcOpField_D(opc); BI = ppcOpField_A(opc); BD = ppcOpField_B(opc); }
#define PPC_OPC_TEMPL_XFL(opc, rB, FM) { rB = ppcOpField_B(opc); FM = ((opc) >> 17) & 0xFF; }

// declare and decode rD/rA/rB (or crbD/crbA/crbB) from a variable named 'opcode'
#define PPC_OPC_TEMPL3_XO() sint32 rD = (sint32)ppcOpField_D(opcode), rA = (sint32)ppcOpField_A(opcode), rB = (sint32)ppcOpField_B(opcode)
#define PPC_OPC_TEMPL_X_CR() sint32 crD = (sint32)ppcOpField_D(opcode), crA = (sint32)ppcOpField_A(opcode), crB = (sint32)ppcOpField_B(opcode)

// ---------------------------------------------------------------------------
// Condition register helpers

inline uint8 ppc_getCRBit(PPCInterpreter_t* hCPU, uint32 bitIndex)
{
	return hCPU->cr[bitIndex];
}

inline void ppc_setCRBit(PPCInterpreter_t* hCPU, uint32 bitIndex, uint8 value)
{
	hCPU->cr[bitIndex] = value;
}

// CR0 from a 32-bit result (Rc=1 forms): LT/GT/EQ from the signed value, SO copied from XER
inline void ppc_update_cr0(PPCInterpreter_t* hCPU, uint32 result)
{
	uint8 negative = (uint8)(result >> 31);
	uint8 zero = result == 0 ? 1 : 0;
	hCPU->cr[CR_BIT_LT] = negative;
	hCPU->cr[CR_BIT_GT] = (uint8)((negative | zero) ^ 1);
	hCPU->cr[CR_BIT_EQ] = zero;
	hCPU->cr[CR_BIT_SO] = hCPU->xer_so;
}

// packs the 32 CR bits into the architectural 32-bit layout (cr0.LT = MSB)
inline uint32 ppc_getCR(PPCInterpreter_t* hCPU)
{
	uint32 packed = 0;
	for (uint32 bit = 0; bit < 32; bit++)
		packed |= (uint32)(hCPU->cr[bit] != 0 ? 1 : 0) << (31 - bit);
	return packed;
}

inline void ppc_setCR(PPCInterpreter_t* hCPU, uint32 packed)
{
	for (uint32 bit = 0; bit < 32; bit++)
		hCPU->cr[bit] = (uint8)((packed >> (31 - bit)) & 1);
}

// mtcrf field mask (bit 7 = cr0) -> mask over the cr[] bit indices (cr0.LT = bit 0)
inline uint32 ppc_MTCRFMaskToCRBitMask(const uint32 mtcrfMask)
{
	uint32 bitMask = 0;
	for (uint32 field = 0; field < 8; field++)
	{
		if ((mtcrfMask >> (7 - field)) & 1)
			bitMask |= 0xFu << (field * 4);
	}
	return bitMask;
}

// ---------------------------------------------------------------------------
// Integer helpers

// rotate mask with ones from bit MB to bit ME (IBM numbering), wrapping when MB > ME
inline uint32 ppc_mask(int MB, int ME)
{
	uint32 fromMB = 0xFFFFFFFFu >> MB;
	uint32 toME = 0xFFFFFFFFu << (31 - ME);
	return (MB <= ME) ? (fromMB & toME) : (fromMB | toME);
}

// ---------------------------------------------------------------------------
// Espresso estimate instructions, shared with the recompiler (called from generated code)

ATTR_MS_ABI double fres_espresso(double input);
ATTR_MS_ABI double frsqrte_espresso(double input);
