// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// A small x86-64 machine code encoder covering the instructions the
// recompiler backend uses. Operands are host registers or memory operands of
// the form [base + index * scale + disp].

namespace x64
{
	enum Reg : uint8
	{
		RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
		R8, R9, R10, R11, R12, R13, R14, R15,
	};

	// XMM registers use the same numbering (0-15)
	using Xmm = uint8;

	constexpr uint8 kNoReg = 0xFF;

	struct Mem
	{
		uint8 base{kNoReg};
		uint8 index{kNoReg};
		uint8 scale{1}; // 1, 2, 4 or 8
		sint32 disp{0};
	};

	inline Mem mem(uint8 base, sint32 disp = 0)
	{
		Mem m;
		m.base = base;
		m.disp = disp;
		return m;
	}

	inline Mem mem(uint8 base, uint8 index, uint8 scale, sint32 disp)
	{
		Mem m;
		m.base = base;
		m.index = index;
		m.scale = scale;
		m.disp = disp;
		return m;
	}

	// condition codes (low nibble of Jcc/SETcc/CMOVcc)
	enum Cond : uint8
	{
		CC_O = 0x0, CC_NO = 0x1, CC_B = 0x2, CC_AE = 0x3, CC_E = 0x4, CC_NE = 0x5, CC_BE = 0x6, CC_A = 0x7,
		CC_S = 0x8, CC_NS = 0x9, CC_P = 0xA, CC_NP = 0xB, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF,
	};

	// group 1 ALU operations (the /digit of opcode 0x81/0x83)
	enum AluOp : uint8
	{
		ALU_ADD = 0, ALU_OR = 1, ALU_ADC = 2, ALU_SBB = 3, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7,
	};

	// group 2 shift/rotate operations (the /digit of opcode 0xC1/0xD3)
	enum ShiftOp : uint8
	{
		SH_ROL = 0, SH_ROR = 1, SH_SHL = 4, SH_SHR = 5, SH_SAR = 7,
	};

	class Emitter
	{
	public:
		std::vector<uint8>& Buffer() { return m_code; }
		size_t Size() const { return m_code.size(); }

		void Byte(uint8 v) { m_code.push_back(v); }
		void Word(uint16 v) { Byte((uint8)v); Byte((uint8)(v >> 8)); }
		void Dword(uint32 v) { for (int i = 0; i < 4; i++) Byte((uint8)(v >> (i * 8))); }
		void Qword(uint64 v) { for (int i = 0; i < 8; i++) Byte((uint8)(v >> (i * 8))); }
		void PatchDword(size_t offset, uint32 v) { for (int i = 0; i < 4; i++) m_code[offset + i] = (uint8)(v >> (i * 8)); }

		// ---- generic encoding ----
		// prefix: 0 or a mandatory prefix (0x66, 0xF2, 0xF3). opcode: 1-3 bytes.
		// byteRegs: the r/m or reg operand is an 8-bit register (forces a REX
		// prefix for SPL/BPL/SIL/DIL).

		void EncodeRR(uint8 prefix, bool w, std::initializer_list<uint8> opcode, uint8 reg, uint8 rm, bool byteRegs = false)
		{
			if (prefix)
				Byte(prefix);
			uint8 rex = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);
			if (rex != 0x40 || (byteRegs && ((reg >= 4 && reg < 8) || (rm >= 4 && rm < 8))))
				Byte(rex);
			for (uint8 o : opcode)
				Byte(o);
			Byte(0xC0 | ((reg & 7) << 3) | (rm & 7));
		}

		void EncodeRM(uint8 prefix, bool w, std::initializer_list<uint8> opcode, uint8 reg, const Mem& m, bool byteReg = false)
		{
			if (prefix)
				Byte(prefix);
			uint8 rex = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0);
			if (m.index != kNoReg && (m.index & 8))
				rex |= 2;
			if (m.base != kNoReg && (m.base & 8))
				rex |= 1;
			if (rex != 0x40 || (byteReg && reg >= 4 && reg < 8))
				Byte(rex);
			for (uint8 o : opcode)
				Byte(o);
			EncodeModRMMem(reg & 7, m);
		}

		void EncodeModRMMem(uint8 regField, const Mem& m)
		{
			cemu_assert_debug(m.base != kNoReg);
			uint8 base = m.base & 7;
			bool needSib = m.index != kNoReg || base == 4; // RSP/R12 as base need a SIB byte
			uint8 mod;
			if (m.disp == 0 && base != 5) // RBP/R13 have no disp-less form
				mod = 0;
			else if (m.disp >= -128 && m.disp <= 127)
				mod = 1;
			else
				mod = 2;
			if (needSib)
			{
				Byte((mod << 6) | (regField << 3) | 4);
				uint8 scaleBits = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
				uint8 index = m.index == kNoReg ? 4 : (m.index & 7);
				cemu_assert_debug(m.index != RSP);
				Byte((scaleBits << 6) | (index << 3) | base);
			}
			else
				Byte((mod << 6) | (regField << 3) | base);
			if (mod == 1)
				Byte((uint8)(sint8)m.disp);
			else if (mod == 2)
				Dword((uint32)m.disp);
		}

		// ---- moves ----
		void MOV_rr(uint8 dst, uint8 src, bool w = false) { EncodeRR(0, w, {0x8B}, dst, src); }
		void MOV_rm(uint8 dst, const Mem& m, bool w = false) { EncodeRM(0, w, {0x8B}, dst, m); }
		void MOV_mr(const Mem& m, uint8 src, bool w = false) { EncodeRM(0, w, {0x89}, src, m); }
		void MOV_mr16(const Mem& m, uint8 src) { EncodeRM(0x66, false, {0x89}, src, m); }
		void MOV_mr8(const Mem& m, uint8 src) { EncodeRM(0, false, {0x88}, src, m, true); }
		void MOV_ri32(uint8 dst, uint32 imm)
		{
			if (dst & 8)
				Byte(0x41);
			Byte(0xB8 | (dst & 7));
			Dword(imm);
		}
		void MOV_ri64(uint8 dst, uint64 imm)
		{
			Byte(0x48 | ((dst & 8) ? 1 : 0));
			Byte(0xB8 | (dst & 7));
			Qword(imm);
		}
		void MOV_mi32(const Mem& m, uint32 imm) { EncodeRM(0, false, {0xC7}, 0, m); Dword(imm); }
		void MOVZX_r8(uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, 0xB6}, dst, src, true); }
		void MOVZX_r16(uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, 0xB7}, dst, src); }
		void MOVSX_r8(uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, 0xBE}, dst, src, true); }
		void MOVSX_r16(uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, 0xBF}, dst, src); }
		void MOVZX_m8(uint8 dst, const Mem& m) { EncodeRM(0, false, {0x0F, 0xB6}, dst, m); }
		void MOVZX_m16(uint8 dst, const Mem& m) { EncodeRM(0, false, {0x0F, 0xB7}, dst, m); }
		void MOVSX_m8(uint8 dst, const Mem& m) { EncodeRM(0, false, {0x0F, 0xBE}, dst, m); }
		void MOVSXD_rr(uint8 dst, uint8 src) { EncodeRR(0, true, {0x63}, dst, src); }
		void LEA32(uint8 dst, const Mem& m) { EncodeRM(0, false, {0x8D}, dst, m); }
		void LEA64(uint8 dst, const Mem& m) { EncodeRM(0, true, {0x8D}, dst, m); }
		void CMOV_rr(Cond cc, uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, (uint8)(0x40 | cc)}, dst, src); }

		// ---- arithmetic ----
		void ALU_rr(AluOp op, uint8 dst, uint8 src, bool w = false) { EncodeRR(0, w, {(uint8)((op << 3) | 0x03)}, dst, src); }
		void ALU_rm(AluOp op, uint8 dst, const Mem& m, bool w = false) { EncodeRM(0, w, {(uint8)((op << 3) | 0x03)}, dst, m); }
		void ALU_ri(AluOp op, uint8 dst, uint32 imm, bool w = false)
		{
			if ((sint32)imm >= -128 && (sint32)imm <= 127)
			{
				EncodeRR(0, w, {0x83}, op, dst);
				Byte((uint8)imm);
			}
			else
			{
				EncodeRR(0, w, {0x81}, op, dst);
				Dword(imm);
			}
		}
		void ALU_mi(AluOp op, const Mem& m, uint32 imm)
		{
			if ((sint32)imm >= -128 && (sint32)imm <= 127)
			{
				EncodeRM(0, false, {0x83}, op, m);
				Byte((uint8)imm);
			}
			else
			{
				EncodeRM(0, false, {0x81}, op, m);
				Dword(imm);
			}
		}
		void TEST_rr(uint8 a, uint8 b, bool w = false) { EncodeRR(0, w, {0x85}, b, a); }
		void TEST_ri(uint8 a, uint32 imm) { EncodeRR(0, false, {0xF7}, 0, a); Dword(imm); }
		void NOT(uint8 r, bool w = false) { EncodeRR(0, w, {0xF7}, 2, r); }
		void NEG(uint8 r, bool w = false) { EncodeRR(0, w, {0xF7}, 3, r); }
		void MUL(uint8 r) { EncodeRR(0, false, {0xF7}, 4, r); }
		void DIV(uint8 r) { EncodeRR(0, false, {0xF7}, 6, r); }
		void IDIV(uint8 r) { EncodeRR(0, false, {0xF7}, 7, r); }
		void IMUL_rr(uint8 dst, uint8 src, bool w = false) { EncodeRR(0, w, {0x0F, 0xAF}, dst, src); }
		void IMUL_rri(uint8 dst, uint8 src, uint32 imm) { EncodeRR(0, false, {0x69}, dst, src); Dword(imm); }
		void CDQ() { Byte(0x99); }
		void SHIFT_ri(ShiftOp op, uint8 r, uint8 amount, bool w = false) { EncodeRR(0, w, {0xC1}, op, r); Byte(amount); }
		void SHIFT_rcl(ShiftOp op, uint8 r, bool w = false) { EncodeRR(0, w, {0xD3}, op, r); }
		void ROL16_ri(uint8 r, uint8 amount) { EncodeRR(0x66, false, {0xC1}, SH_ROL, r); Byte(amount); }
		void BSWAP(uint8 r, bool w = false)
		{
			uint8 rex = 0x40 | (w ? 8 : 0) | ((r & 8) ? 1 : 0);
			if (rex != 0x40)
				Byte(rex);
			Byte(0x0F);
			Byte(0xC8 | (r & 7));
		}
		void BSR(uint8 dst, uint8 src) { EncodeRR(0, false, {0x0F, 0xBD}, dst, src); }
		void LZCNT(uint8 dst, uint8 src) { EncodeRR(0xF3, false, {0x0F, 0xBD}, dst, src); }
		void SETcc(Cond cc, uint8 r) { EncodeRR(0, false, {0x0F, (uint8)(0x90 | cc)}, 0, r, true); }
		void LOCK_CMPXCHG_mr(const Mem& m, uint8 src)
		{
			Byte(0xF0);
			EncodeRM(0, false, {0x0F, 0xB1}, src, m);
		}

		// ---- stack and control flow ----
		void PUSH(uint8 r)
		{
			if (r & 8)
				Byte(0x41);
			Byte(0x50 | (r & 7));
		}
		void POP(uint8 r)
		{
			if (r & 8)
				Byte(0x41);
			Byte(0x58 | (r & 7));
		}
		void RET() { Byte(0xC3); }
		void CALL_r(uint8 r) { EncodeRR(0, false, {0xFF}, 2, r); }
		void JMP_r(uint8 r) { EncodeRR(0, false, {0xFF}, 4, r); }
		void JMP_m(const Mem& m) { EncodeRM(0, false, {0xFF}, 4, m); }
		// returns the offset of the rel32 field
		size_t JMP_rel32()
		{
			Byte(0xE9);
			size_t at = Size();
			Dword(0);
			return at;
		}
		size_t Jcc_rel32(Cond cc)
		{
			Byte(0x0F);
			Byte(0x80 | cc);
			size_t at = Size();
			Dword(0);
			return at;
		}
		// makes the rel32 field at 'at' jump to 'target' (both buffer offsets)
		void BindRel32(size_t at, size_t target)
		{
			PatchDword(at, (uint32)(sint32)((sint64)target - (sint64)(at + 4)));
		}

		// ---- SSE ----
		void SSE_rr(uint8 prefix, uint8 op, Xmm dst, Xmm src) { EncodeRR(prefix, false, {0x0F, op}, dst, src); }
		void SSE_rm(uint8 prefix, uint8 op, Xmm dst, const Mem& m) { EncodeRM(prefix, false, {0x0F, op}, dst, m); }

		void MOVAPD(Xmm dst, Xmm src) { SSE_rr(0x66, 0x28, dst, src); }
		void MOVSD_rr(Xmm dst, Xmm src) { SSE_rr(0xF2, 0x10, dst, src); }
		void MOVUPD_rm(Xmm dst, const Mem& m) { SSE_rm(0x66, 0x10, dst, m); }
		void MOVUPD_mr(const Mem& m, Xmm src) { SSE_rm(0x66, 0x11, src, m); }
		void MOVAPS_rm(Xmm dst, const Mem& m) { SSE_rm(0, 0x28, dst, m); }
		void MOVAPS_mr(const Mem& m, Xmm src) { SSE_rm(0, 0x29, src, m); }
		void MOVLPD_rm(Xmm dst, const Mem& m) { SSE_rm(0x66, 0x12, dst, m); }
		void MOVUPS_mr(const Mem& m, Xmm src) { SSE_rm(0, 0x11, src, m); }
		void MOVQ_xr(Xmm dst, uint8 src) { EncodeRR(0x66, true, {0x0F, 0x6E}, dst, src); }
		void MOVQ_rx(uint8 dst, Xmm src) { EncodeRR(0x66, true, {0x0F, 0x7E}, src, dst); }
		void MOVD_xr(Xmm dst, uint8 src) { EncodeRR(0x66, false, {0x0F, 0x6E}, dst, src); }
		void MOVD_rx(uint8 dst, Xmm src) { EncodeRR(0x66, false, {0x0F, 0x7E}, src, dst); }

		void ADDSD(Xmm d, Xmm s) { SSE_rr(0xF2, 0x58, d, s); }
		void MULSD(Xmm d, Xmm s) { SSE_rr(0xF2, 0x59, d, s); }
		void SUBSD(Xmm d, Xmm s) { SSE_rr(0xF2, 0x5C, d, s); }
		void DIVSD(Xmm d, Xmm s) { SSE_rr(0xF2, 0x5E, d, s); }
		void ADDPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x58, d, s); }
		void MULPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x59, d, s); }
		void SUBPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x5C, d, s); }
		void DIVPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x5E, d, s); }
		void CVTSD2SS(Xmm d, Xmm s) { SSE_rr(0xF2, 0x5A, d, s); }
		void CVTSS2SD(Xmm d, Xmm s) { SSE_rr(0xF3, 0x5A, d, s); }
		void CVTPD2PS(Xmm d, Xmm s) { SSE_rr(0x66, 0x5A, d, s); }
		void CVTPS2PD(Xmm d, Xmm s) { SSE_rr(0, 0x5A, d, s); }
		void CVTTSD2SI(uint8 d, Xmm s) { EncodeRR(0xF2, false, {0x0F, 0x2C}, d, s); }
		void UCOMISD(Xmm a, Xmm b) { SSE_rr(0x66, 0x2E, a, b); }
		void CMPSD(Xmm d, Xmm s, uint8 pred) { SSE_rr(0xF2, 0xC2, d, s); Byte(pred); }
		void CMPPD(Xmm d, Xmm s, uint8 pred) { SSE_rr(0x66, 0xC2, d, s); Byte(pred); }
		void ANDPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x54, d, s); }
		void ANDNPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x55, d, s); }
		void ORPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x56, d, s); }
		void XORPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x57, d, s); }
		void ANDPD_m(Xmm d, const Mem& m) { SSE_rm(0x66, 0x54, d, m); }
		void ORPD_m(Xmm d, const Mem& m) { SSE_rm(0x66, 0x56, d, m); }
		void XORPD_m(Xmm d, const Mem& m) { SSE_rm(0x66, 0x57, d, m); }
		void PADDQ_m(Xmm d, const Mem& m) { SSE_rm(0x66, 0xD4, d, m); }
		void PAND_m(Xmm d, const Mem& m) { SSE_rm(0x66, 0xDB, d, m); }
		void UNPCKLPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x14, d, s); }
		void UNPCKHPD(Xmm d, Xmm s) { SSE_rr(0x66, 0x15, d, s); }
		void SHUFPD(Xmm d, Xmm s, uint8 imm) { SSE_rr(0x66, 0xC6, d, s); Byte(imm); }

	private:
		std::vector<uint8> m_code;
	};
}
