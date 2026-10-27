package ps3

// More of the x86-64 writer (x86emit.go): 64-bit forms for the PPU, whose
// registers are 64 bits wide.

func (a *x86) aluRM64(op, dst, base int, disp int32) {
	a.opRM(true, []byte{byte(op<<3 | 3)}, dst, base, disp)
}
func (a *x86) aluMR64(op, base int, disp int32, src int) {
	a.opRM(true, []byte{byte(op<<3 | 1)}, src, base, disp)
}

// aluMI64: op [base+disp], imm32 sign-extended to 64 bits.
func (a *x86) aluMI64(op, base int, disp int32, v int32) {
	if v >= -128 && v < 128 {
		a.opRM(true, []byte{0x83}, op, base, disp)
		a.byte1(byte(int8(v)))
		return
	}
	a.opRM(true, []byte{0x81}, op, base, disp)
	a.imm32(uint32(v))
}
func (a *x86) unary64(op, r int)         { a.opRR(true, []byte{0xf7}, op, r) }
func (a *x86) imulRR64(dst, src int)     { a.opRR(true, []byte{0x0f, 0xaf}, dst, src) }
func (a *x86) shiftRCL64(op, r int)      { a.opRR(true, []byte{0xd3}, op, r) }
func (a *x86) cmovRR64(cc, dst, src int) { a.opRR(true, []byte{0x0f, 0x40 + byte(cc)}, dst, src) }
func (a *x86) bsr64(dst, src int)        { a.opRR(true, []byte{0x0f, 0xbd}, dst, src) }
func (a *x86) imulRRI64(dst, src int, v int32) {
	a.opRR(true, []byte{0x69}, dst, src)
	a.imm32(uint32(v))
}

// sign extensions to 64 bits
func (a *x86) movsxd(dst, src int) { a.opRR(true, []byte{0x63}, dst, src) }
func (a *x86) movsx8to64(dst, src int) {
	a.opRR(true, []byte{0x0f, 0xbe}, dst, src)
}
func (a *x86) movsx16to64(dst, src int) { a.opRR(true, []byte{0x0f, 0xbf}, dst, src) }

// movImm64 loads v, by the shortest form.
func (a *x86) movImm64(dst int, v uint64) {
	switch {
	case v>>32 == 0:
		a.movRIKeepFlags(dst, uint32(v))
	case int64(v) == int64(int32(v)):
		a.opRR(true, []byte{0xc7}, 0, dst)
		a.imm32(uint32(v))
	default:
		a.movRI64(dst, v)
	}
}

// storeImm64 writes v as 8 bytes at [base+disp].
func (a *x86) storeImm64(base int, disp int32, v uint64) {
	if int64(v) == int64(int32(v)) {
		a.opRM(true, []byte{0xc7}, 0, base, disp)
		a.imm32(uint32(v))
		return
	}
	a.movStoreImm(base, disp, uint32(v))
	a.movStoreImm(base, disp+4, uint32(v>>32))
}

// test [base+disp] with a register, 64 bits
func (a *x86) leaIdx(dst, base, index int) { a.opRMI(true, []byte{0x8d}, dst, base, index, 1, 0) }
