package ps3

// SSE / AVX forms for the SPU recompiler (spujit.go): 128-bit integer and
// single precision instructions on xmm0..xmm15.

// opcode bytes after the 0x0F escape (66 prefix unless noted)
var (
	sseMovdquLoad  = []byte{0x0f, 0x6f} // F3
	sseMovdquStore = []byte{0x0f, 0x7f} // F3
	sseMovdqa      = []byte{0x0f, 0x6f} // 66
	ssePaddd       = []byte{0x0f, 0xfe}
	ssePaddw       = []byte{0x0f, 0xfd}
	ssePaddb       = []byte{0x0f, 0xfc}
	ssePsubd       = []byte{0x0f, 0xfa}
	ssePsubw       = []byte{0x0f, 0xf9}
	ssePsubb       = []byte{0x0f, 0xf8}
	ssePmaxud      = []byte{0x0f, 0x38, 0x3f}
	ssePmaxsd      = []byte{0x0f, 0x38, 0x3d}
	ssePminud      = []byte{0x0f, 0x38, 0x3b}
	ssePminsd      = []byte{0x0f, 0x38, 0x39}
	ssePand        = []byte{0x0f, 0xdb}
	ssePandn       = []byte{0x0f, 0xdf} // dst = ~dst & src
	ssePor         = []byte{0x0f, 0xeb}
	ssePxor        = []byte{0x0f, 0xef}
	ssePcmpeqd     = []byte{0x0f, 0x76}
	ssePcmpeqw     = []byte{0x0f, 0x75}
	ssePcmpeqb     = []byte{0x0f, 0x74}
	ssePcmpgtd     = []byte{0x0f, 0x66}
	ssePcmpgtw     = []byte{0x0f, 0x65}
	ssePcmpgtb     = []byte{0x0f, 0x64}
	ssePshufb      = []byte{0x0f, 0x38, 0x00}
	ssePmulld      = []byte{0x0f, 0x38, 0x40}
	ssePtest       = []byte{0x0f, 0x38, 0x17}
	ssePshufd      = []byte{0x0f, 0x70} // + imm8
	sseMovdTo      = []byte{0x0f, 0x6e} // movd xmm, r32
	sseMovdFrom    = []byte{0x0f, 0x7e} // movd r32, xmm (reg field = xmm)
	// no prefix: packed singles
	sseAddps     = []byte{0x0f, 0x58}
	sseSubps     = []byte{0x0f, 0x5c}
	sseMulps     = []byte{0x0f, 0x59}
	sseDivps     = []byte{0x0f, 0x5e}
	sseCmpps     = []byte{0x0f, 0xc2} // + imm8
	sseAndps     = []byte{0x0f, 0x54}
	sseCvtdq2ps  = []byte{0x0f, 0x5b}
	sseCvttps2dq = []byte{0x0f, 0x5b} // F3
)

// xRR: an SSE op on two xmm registers (dst is the ModRM reg field).
func (a *x86) xRR(pfx byte, op []byte, dst, src int) {
	if pfx != 0 {
		a.byte1(pfx)
	}
	a.rex(false, dst, 0, src)
	a.byte1(op...)
	a.modRR(dst, src)
}

// xRM: an SSE op with a memory operand [base+disp].
func (a *x86) xRM(pfx byte, op []byte, reg, base int, disp int32) {
	if pfx != 0 {
		a.byte1(pfx)
	}
	a.rex(false, reg, 0, base)
	a.byte1(op...)
	a.modMem(reg, base, -1, 1, disp)
}

// xRMI: an SSE op with a memory operand [base+index*scale+disp].
func (a *x86) xRMI(pfx byte, op []byte, reg, base, index, scale int, disp int32) {
	if pfx != 0 {
		a.byte1(pfx)
	}
	a.rex(false, reg, index, base)
	a.byte1(op...)
	a.modMem(reg, base, index, scale, disp)
}

// xRip: an SSE op with a RIP-relative memory operand; it returns the
// position of the disp32, to point at the operand later with ripTo.
// immLen: the bytes after the disp32 (an imm8).
func (a *x86) xRip(pfx byte, op []byte, reg int) int {
	if pfx != 0 {
		a.byte1(pfx)
	}
	a.rex(false, reg, 0, 0)
	a.byte1(op...)
	a.byte1(byte(reg&7)<<3 | 5)
	a.imm32(0)
	return a.pos() - 4
}

// ripTo points the disp32 at at to the absolute address target (immLen
// bytes of the instruction follow the disp32).
func (a *x86) ripTo(at int, immLen int, target uintptr) {
	end := a.base + uintptr(at+4+immLen)
	rel := uint32(int32(int64(target) - int64(end)))
	a.b[at], a.b[at+1], a.b[at+2], a.b[at+3] = byte(rel), byte(rel>>8), byte(rel>>16), byte(rel>>24)
}

// leaRip: lea r64, [rip+disp32]; returns the disp32's position.
func (a *x86) leaRip(r int) int {
	a.rex(true, r, 0, 0)
	a.byte1(0x8d, byte(r&7)<<3|5)
	a.imm32(0)
	return a.pos() - 4
}

func (a *x86) movdquLoad(x, base int, disp int32) { a.xRM(0xf3, sseMovdquLoad, x, base, disp) }
func (a *x86) movdquStore(base int, disp int32, x int) {
	a.xRM(0xf3, sseMovdquStore, x, base, disp)
}
func (a *x86) movdquLoadIdx(x, base, index int) {
	a.xRMI(0xf3, sseMovdquLoad, x, base, index, 1, 0)
}
func (a *x86) movdquStoreIdx(base, index, x int) {
	a.xRMI(0xf3, sseMovdquStore, x, base, index, 1, 0)
}
func (a *x86) movdqa(dst, src int)        { a.xRR(0x66, sseMovdqa, dst, src) }
func (a *x86) pi(op []byte, dst, src int) { a.xRR(0x66, op, dst, src) }
func (a *x86) ps(op []byte, dst, src int) { a.xRR(0, op, dst, src) }

// pshufd / cmpps with an imm8
func (a *x86) pshufd(dst, src int, imm byte) {
	a.xRR(0x66, ssePshufd, dst, src)
	a.byte1(imm)
}
func (a *x86) cmpps(dst, src int, pred byte) {
	a.xRR(0, sseCmpps, dst, src)
	a.byte1(pred)
}

// shifts by an immediate: group 71 (words), 72 (dwords), 73 (qwords and
// whole-register bytes); ext is the /digit.
const (
	shGrpW  = 0x71
	shGrpD  = 0x72
	shGrpDQ = 0x73
	shExtL  = 6 // psll
	shExtR  = 2 // psrl
	shExtA  = 4 // psra
	shExtLB = 7 // pslldq
	shExtRB = 3 // psrldq
)

func (a *x86) pshiftI(grp byte, ext, x int, n uint8) {
	a.byte1(0x66)
	a.rex(false, 0, 0, x)
	a.byte1(0x0f, grp)
	a.modRR(ext, x)
	a.byte1(n)
}

func (a *x86) movdToX32(x, r int) { a.xRR(0x66, sseMovdTo, x, r) }
func (a *x86) movdFromX32(r, x int) {
	a.byte1(0x66)
	a.rex(false, x, 0, r)
	a.byte1(sseMovdFrom...)
	a.modRR(x, r)
}

// vex3: a three-operand VEX.128 instruction in map 0F38 with prefix 66
// (W0): dst = reg, src1 = vvvv, src2 = rm.
func (a *x86) vex38(op byte, dst, src1, src2 int) {
	r := byte(0x80)
	if dst&8 != 0 {
		r = 0
	}
	bb := byte(0x20)
	if src2&8 != 0 {
		bb = 0
	}
	a.byte1(0xc4, r|0x40|bb|0x02, byte(^src1&15)<<3|0x01, op)
	a.modRR(dst, src2)
}

// FMA3 (dst = dst*src1 ± src2 forms, "213")
func (a *x86) vfmadd213ps(dst, s1, s2 int)  { a.vex38(0xa8, dst, s1, s2) }
func (a *x86) vfmsub213ps(dst, s1, s2 int)  { a.vex38(0xaa, dst, s1, s2) }
func (a *x86) vfnmadd213ps(dst, s1, s2 int) { a.vex38(0xac, dst, s1, s2) }
