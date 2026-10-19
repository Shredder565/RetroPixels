// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// R600-family (GPU7) shader ISA constants shared by the shader parsers and decompilers.

// export array base values with a fixed meaning
#define GPU7_DECOMPILER_CF_EXPORT_BASE_POSITION	(60)
#define GPU7_DECOMPILER_CF_EXPORT_POINT_SIZE	(61)

// control flow opcodes (CF_INST field, bits 23..29 of the second CF word)
#define GPU7_CF_INST_NOP				(0x00)
#define GPU7_CF_INST_TEX				(0x01)
#define GPU7_CF_INST_VTX				(0x02) // vertex fetch clause, GPU7 only uses it in GS copy programs
#define GPU7_CF_INST_LOOP_END			(0x05)
#define GPU7_CF_INST_LOOP_START_DX10	(0x06)
#define GPU7_CF_INST_LOOP_START_NO_AL	(0x07)
#define GPU7_CF_INST_LOOP_BREAK			(0x09)
#define GPU7_CF_INST_JUMP				(0x0A)
#define GPU7_CF_INST_ELSE				(0x0D)
#define GPU7_CF_INST_POP				(0x0E)
#define GPU7_CF_INST_CALL				(0x12)
#define GPU7_CF_INST_CALL_FS			(0x13)
#define GPU7_CF_INST_RETURN				(0x14)
#define GPU7_CF_INST_EMIT_VERTEX		(0x15) // geometry programs only
#define GPU7_CF_INST_MEM_STREAM0_WRITE	(0x20) // transform feedback, buffer 0
#define GPU7_CF_INST_MEM_STREAM1_WRITE	(0x21) // transform feedback, buffer 1
#define GPU7_CF_INST_MEM_RING_WRITE		(0x26) // ES->GS / GS->VS ring
#define GPU7_CF_INST_EXPORT				(0x27)
#define GPU7_CF_INST_EXPORT_DONE		(0x28)

// ALU clause opcodes use a shorter field whose values overlap the ones above,
// so they are tagged with an extra bit to keep every CF opcode value unique
#define GPU7_CF_INST_ALU_MASK			(0x10000)
#define GPU7_CF_INST_ALU				(GPU7_CF_INST_ALU_MASK | 0x08)
#define GPU7_CF_INST_ALU_PUSH_BEFORE	(GPU7_CF_INST_ALU_MASK | 0x09)
#define GPU7_CF_INST_ALU_POP_AFTER		(GPU7_CF_INST_ALU_MASK | 0x0A)
#define GPU7_CF_INST_ALU_POP2_AFTER		(GPU7_CF_INST_ALU_MASK | 0x0B)
#define GPU7_CF_INST_ALU_BREAK			(GPU7_CF_INST_ALU_MASK | 0x0E)
#define GPU7_CF_INST_ALU_ELSE_AFTER		(GPU7_CF_INST_ALU_MASK | 0x0F)

// relative addressing index selects of ALU operands
#define GPU7_INDEX_AR_X					(0)
#define GPU7_INDEX_AR_Y					(1)
#define GPU7_INDEX_AR_Z					(2)
#define GPU7_INDEX_AR_W					(3)

// texture/fetch clause opcodes
#define GPU7_TEX_INST_VFETCH				(0x00)
#define GPU7_TEX_INST_MEM					(0x02)
#define GPU7_TEX_INST_LD					(0x03)
#define GPU7_TEX_INST_GET_TEXTURE_RESINFO	(0x04)
#define GPU7_TEX_INST_GET_COMP_TEX_LOD		(0x06)
#define GPU7_TEX_INST_GET_GRADIENTS_H		(0x07)
#define GPU7_TEX_INST_GET_GRADIENTS_V		(0x08)
#define GPU7_TEX_INST_SET_GRADIENTS_H		(0x0B)
#define GPU7_TEX_INST_SET_GRADIENTS_V		(0x0C)
#define GPU7_TEX_INST_SET_CUBEMAP_INDEX		(0x0E)
#define GPU7_TEX_INST_FETCH4				(0x0F)
#define GPU7_TEX_INST_SAMPLE				(0x10) // hardware LOD
#define GPU7_TEX_INST_SAMPLE_L				(0x11) // explicit LOD in src.w
#define GPU7_TEX_INST_SAMPLE_LB				(0x12) // LOD bias
#define GPU7_TEX_INST_SAMPLE_LZ				(0x13) // LOD 0
#define GPU7_TEX_INST_SAMPLE_G				(0x14) // explicit gradients
#define GPU7_TEX_INST_SAMPLE_C				(0x18) // depth compare, hardware LOD
#define GPU7_TEX_INST_SAMPLE_C_L			(0x19) // depth compare, explicit LOD
#define GPU7_TEX_INST_SAMPLE_C_LZ			(0x1B) // depth compare, LOD 0

// coordinate type of a texture instruction
#define GPU7_TEX_UNNORMALIZED				(0)

// ALU operand selects:
//   0..127 GPR, 128..159 constant bank 0, 160..191 constant bank 1,
//   248..252 inline constants, 253 literal, 254 previous vector, 255 previous scalar,
//   256..511 uniform register file
#define GPU7_ALU_SRC_IS_GPR(__v)			((__v) >= 0 && (__v) <= 127)
#define GPU7_ALU_SRC_IS_CBANK0(__v)			((__v) >= 128 && (__v) <= 159)
#define GPU7_ALU_SRC_IS_CBANK1(__v)			((__v) >= 160 && (__v) <= 191)
#define GPU7_ALU_SRC_IS_CONST_0F(__v)		((__v) == 248) // 0.0
#define GPU7_ALU_SRC_IS_CONST_1F(__v)		((__v) == 249) // 1.0
#define GPU7_ALU_SRC_IS_CONST_1I(__v)		((__v) == 250) // 1
#define GPU7_ALU_SRC_IS_CONST_M1I(__v)		((__v) == 251) // -1
#define GPU7_ALU_SRC_IS_CONST_0_5F(__v)		((__v) == 252) // 0.5
#define GPU7_ALU_SRC_IS_ANY_CONST(__v)		((__v) >= 248 && (__v) <= 252)
#define GPU7_ALU_SRC_IS_LITERAL(__v)		((__v) == 253)
#define GPU7_ALU_SRC_IS_PV(__v)				((__v) == 254)
#define GPU7_ALU_SRC_IS_PS(__v)				((__v) == 255)
#define GPU7_ALU_SRC_IS_CFILE(__v)			((__v) >= 256 && (__v) <= 511)

#define GPU7_ALU_SRC_GET_GPR_INDEX(__v)		(__v)
#define GPU7_ALU_SRC_GET_CBANK0_INDEX(__v)	((__v) - 128)
#define GPU7_ALU_SRC_GET_CBANK1_INDEX(__v)	((__v) - 160)
#define GPU7_ALU_SRC_GET_CFILE_INDEX(__v)	((__v) - 256)
