package wiiu

// A small x86-64 machine code writer for the CPU recompiler (jit.go): the
// instructions it needs, encoded by hand from the Intel manuals' tables.

const (
	rAX = iota
	rCX
	rDX
	rBX
	rSP
	rBP
	rSI
	rDI
	r8
	r9
	r10
	r11
	r12
	r13
	r14
	r15
)

// condition codes (the low nibble of Jcc / SETcc / CMOVcc)
const (
	ccO  = 0x0
	ccNO = 0x1
	ccB  = 0x2 // below (unsigned <), carry
	ccAE = 0x3 // above or equal, no carry
	ccE  = 0x4
	ccNE = 0x5
	ccBE = 0x6
	ccA  = 0x7
	ccS  = 0x8
	ccNS = 0x9
	ccP  = 0xa
	ccNP = 0xb
	ccL  = 0xc
	ccGE = 0xd
	ccLE = 0xe
	ccG  = 0xf
)

// ALU group numbers (the /digit of 0x81 and 0x83, and opcode base >> 3)
const (
	aluAdd = 0
	aluOr  = 1
	aluAdc = 2
	aluSbb = 3
	aluAnd = 4
	aluSub = 5
	aluXor = 6
	aluCmp = 7
)

// shift group numbers (/digit of 0xC1 and 0xD3)
const (
	shRol = 0
	shRor = 1
	shShl = 4
	shShr = 5
	shSar = 7
)

type x86 struct {
	b    []byte
	base uintptr // where b[0] will be in memory (for jumps to outside code)
}

func (a *x86) byte1(v ...byte) { a.b = append(a.b, v...) }
func (a *x86) imm32(v uint32) {
	a.b = append(a.b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}
func (a *x86) imm64(v uint64) {
	a.imm32(uint32(v))
	a.imm32(uint32(v >> 32))
}
func (a *x86) pos() int { return len(a.b) }

// rex writes a REX prefix when one is needed (force: for byte registers
// SPL..DIL, which this writer doesn't use, and 64-bit operands).
func (a *x86) rex(w bool, reg, index, base int) {
	v := byte(0x40)
	if w {
		v |= 8
	}
	if reg&8 != 0 {
		v |= 4
	}
	if index&8 != 0 {
		v |= 2
	}
	if base&8 != 0 {
		v |= 1
	}
	if v != 0x40 {
		a.byte1(v)
	}
}

// rexByte: a REX prefix for an instruction on byte register reg (SPL..DIL
// need one even when no bit is set).
func (a *x86) rexByte(reg, base int) {
	v := byte(0x40)
	if reg&8 != 0 {
		v |= 4
	}
	if base&8 != 0 {
		v |= 1
	}
	if v != 0x40 || reg >= 4 && reg < 8 || base >= 4 && base < 8 {
		a.byte1(v)
	}
}

// modRM for register-register.
func (a *x86) modRR(reg, rm int) { a.byte1(0xc0 | byte(reg&7)<<3 | byte(rm&7)) }

// modMem for [base + disp] (index < 0) or [base + index*scale + disp].
func (a *x86) modMem(reg, base, index, scale int, disp int32) {
	r := byte(reg&7) << 3
	mod := byte(0x80)
	small := disp >= -128 && disp < 128
	if disp == 0 && base&7 != rBP {
		mod = 0
	} else if small {
		mod = 0x40
	}
	if index < 0 && base&7 != rSP {
		a.byte1(mod | r | byte(base&7))
	} else {
		a.byte1(mod | r | 4)
		ss := byte(0)
		switch scale {
		case 2:
			ss = 1
		case 4:
			ss = 2
		case 8:
			ss = 3
		}
		idx := byte(4) // none
		if index >= 0 {
			idx = byte(index & 7)
		}
		a.byte1(ss<<6 | idx<<3 | byte(base&7))
	}
	switch mod {
	case 0x40:
		a.byte1(byte(int8(disp)))
	case 0x80:
		a.imm32(uint32(disp))
	}
}

// op with a register and a memory operand [base+disp]
func (a *x86) opRM(w bool, op []byte, reg, base int, disp int32) {
	a.rex(w, reg, 0, base)
	a.byte1(op...)
	a.modMem(reg, base, -1, 1, disp)
}

// op with a register and [base+index*scale+disp]
func (a *x86) opRMI(w bool, op []byte, reg, base, index, scale int, disp int32) {
	a.rex(w, reg, index, base)
	a.byte1(op...)
	a.modMem(reg, base, index, scale, disp)
}

func (a *x86) opRR(w bool, op []byte, reg, rm int) {
	a.rex(w, reg, 0, rm)
	a.byte1(op...)
	a.modRR(reg, rm)
}

// 32-bit moves
func (a *x86) movLoad(dst, base int, disp int32)  { a.opRM(false, []byte{0x8b}, dst, base, disp) }
func (a *x86) movStore(base int, disp int32, src int) { a.opRM(false, []byte{0x89}, src, base, disp) }
func (a *x86) movRR(dst, src int)                 { a.opRR(false, []byte{0x89}, src, dst) }
func (a *x86) movRI(dst int, v uint32) {
	if v == 0 {
		a.aluRR(aluXor, dst, dst)
		return
	}
	a.rex(false, 0, 0, dst)
	a.byte1(0xb8 + byte(dst&7))
	a.imm32(v)
}

// movRIKeepFlags: mov without the xor shortcut (flags are left alone).
func (a *x86) movRIKeepFlags(dst int, v uint32) {
	a.rex(false, 0, 0, dst)
	a.byte1(0xb8 + byte(dst&7))
	a.imm32(v)
}

func (a *x86) movStoreImm(base int, disp int32, v uint32) {
	a.opRM(false, []byte{0xc7}, 0, base, disp)
	a.imm32(v)
}

// 64-bit
func (a *x86) movLoad64(dst, base int, disp int32) { a.opRM(true, []byte{0x8b}, dst, base, disp) }
func (a *x86) movLoad64Idx(dst, base, index, scale int) {
	a.opRMI(true, []byte{0x8b}, dst, base, index, scale, 0)
}
func (a *x86) testRR64(x, y int) { a.opRR(true, []byte{0x85}, y, x) }
func (a *x86) movRI64(dst int, v uint64) {
	a.rex(true, 0, 0, dst)
	a.byte1(0xb8 + byte(dst&7))
	a.imm64(v)
}
func (a *x86) aluRI64(op, dst int, v int32) {
	if v >= -128 && v < 128 {
		a.opRR(true, []byte{0x83}, op, dst)
		a.byte1(byte(int8(v)))
		return
	}
	a.opRR(true, []byte{0x81}, op, dst)
	a.imm32(uint32(v))
}

// loads and stores of [base+index] (index a 32-bit offset zero-extended in
// a 64-bit register)
func (a *x86) load32Idx(dst, base, index int) { a.opRMI(false, []byte{0x8b}, dst, base, index, 1, 0) }
func (a *x86) store32Idx(base, index, src int) {
	a.opRMI(false, []byte{0x89}, src, base, index, 1, 0)
}
func (a *x86) load8Idx(dst, base, index int)  { a.opRMI(false, []byte{0x0f, 0xb6}, dst, base, index, 1, 0) }
func (a *x86) load16Idx(dst, base, index int) { a.opRMI(false, []byte{0x0f, 0xb7}, dst, base, index, 1, 0) }
func (a *x86) store16Idx(base, index, src int) {
	a.byte1(0x66)
	a.opRMI(false, []byte{0x89}, src, base, index, 1, 0)
}
func (a *x86) store8Idx(base, index, src int) {
	v := byte(0x40)
	if src&8 != 0 {
		v |= 4
	}
	if index&8 != 0 {
		v |= 2
	}
	if base&8 != 0 {
		v |= 1
	}
	if v != 0x40 || src >= 4 && src < 8 {
		a.byte1(v)
	}
	a.byte1(0x88)
	a.modMem(src, base, index, 1, 0)
}
func (a *x86) load64Idx(dst, base, index int) { a.opRMI(true, []byte{0x8b}, dst, base, index, 1, 0) }
func (a *x86) store64Idx(base, index, src int) {
	a.opRMI(true, []byte{0x89}, src, base, index, 1, 0)
}

// ALU
func (a *x86) aluRR(op, dst, src int) { a.opRR(false, []byte{byte(op<<3 | 1)}, src, dst) }
func (a *x86) aluRM(op, dst, base int, disp int32) {
	a.opRM(false, []byte{byte(op<<3 | 3)}, dst, base, disp)
}
func (a *x86) aluMR(op, base int, disp int32, src int) {
	a.opRM(false, []byte{byte(op<<3 | 1)}, src, base, disp)
}
func (a *x86) aluRI(op, dst int, v uint32) {
	if int32(v) >= -128 && int32(v) < 128 {
		a.opRR(false, []byte{0x83}, op, dst)
		a.byte1(byte(v))
		return
	}
	a.opRR(false, []byte{0x81}, op, dst)
	a.imm32(v)
}
func (a *x86) aluMI(op, base int, disp int32, v uint32) {
	if int32(v) >= -128 && int32(v) < 128 {
		a.opRM(false, []byte{0x83}, op, base, disp)
		a.byte1(byte(v))
		return
	}
	a.opRM(false, []byte{0x81}, op, base, disp)
	a.imm32(v)
}
func (a *x86) testRR(x, y int) { a.opRR(false, []byte{0x85}, y, x) }
func (a *x86) testMI(base int, disp int32, v uint32) {
	a.opRM(false, []byte{0xf7}, 0, base, disp)
	a.imm32(v)
}
func (a *x86) testRI(r int, v uint32) {
	a.opRR(false, []byte{0xf7}, 0, r)
	a.imm32(v)
}

// unary group F7: 2 not, 3 neg, 4 mul, 5 imul, 6 div, 7 idiv
func (a *x86) unary(op, r int) { a.opRR(false, []byte{0xf7}, op, r) }

func (a *x86) imulRR(dst, src int) { a.opRR(false, []byte{0x0f, 0xaf}, dst, src) }
func (a *x86) imulRRI(dst, src int, v uint32) {
	a.opRR(false, []byte{0x69}, dst, src)
	a.imm32(v)
}

func (a *x86) shiftRI(op, r int, n uint8) {
	if n == 1 {
		a.opRR(false, []byte{0xd1}, op, r)
		return
	}
	a.opRR(false, []byte{0xc1}, op, r)
	a.byte1(n)
}
func (a *x86) shiftRCL(op, r int) { a.opRR(false, []byte{0xd3}, op, r) }
func (a *x86) shiftRI64(op, r int, n uint8) {
	a.opRR(true, []byte{0xc1}, op, r)
	a.byte1(n)
}

func (a *x86) bswap(r int) {
	a.rex(false, 0, 0, r)
	a.byte1(0x0f, 0xc8+byte(r&7))
}

// rol16by8 swaps the two bytes of a 16-bit register.
func (a *x86) rol16by8(r int) {
	a.byte1(0x66)
	a.opRR(false, []byte{0xc1}, shRol, r)
	a.byte1(8)
}

func (a *x86) movzx8(dst, src int) {
	a.rexByte(dst, src)
	a.byte1(0x0f, 0xb6)
	a.modRR(dst, src)
}
func (a *x86) movzx16(dst, src int) { a.opRR(false, []byte{0x0f, 0xb7}, dst, src) }
func (a *x86) movsx8(dst, src int) {
	a.rexByte(dst, src)
	a.byte1(0x0f, 0xbe)
	a.modRR(dst, src)
}
func (a *x86) movsx16(dst, src int) { a.opRR(false, []byte{0x0f, 0xbf}, dst, src) }

func (a *x86) setcc(cc, r int) {
	a.rexByte(0, r)
	a.byte1(0x0f, 0x90+byte(cc))
	a.modRR(0, r)
}
func (a *x86) cmovRR(cc, dst, src int) { a.opRR(false, []byte{0x0f, 0x40 + byte(cc)}, dst, src) }
func (a *x86) bsr(dst, src int)        { a.opRR(false, []byte{0x0f, 0xbd}, dst, src) }
func (a *x86) lea(dst, base int, disp int32) {
	a.opRM(false, []byte{0x8d}, dst, base, disp)
}

// jumps: each returns the position of its rel32 for patch
func (a *x86) jcc(cc int) int {
	a.byte1(0x0f, 0x80+byte(cc))
	a.imm32(0)
	return a.pos() - 4
}
func (a *x86) jmp() int {
	a.byte1(0xe9)
	a.imm32(0)
	return a.pos() - 4
}

// patch points the rel32 at at to the current position.
func (a *x86) patch(at int) { a.patchTo(at, a.pos()) }
func (a *x86) patchTo(at, target int) {
	rel := uint32(target - (at + 4))
	a.b[at], a.b[at+1], a.b[at+2], a.b[at+3] = byte(rel), byte(rel>>8), byte(rel>>16), byte(rel>>24)
}

// jmpAbs jumps to an address outside b (needs base set).
func (a *x86) jmpAbs(target uintptr) {
	a.byte1(0xe9)
	rel := int64(target) - int64(a.base+uintptr(a.pos())+4)
	a.imm32(uint32(int32(rel)))
}
func (a *x86) jccAbs(cc int, target uintptr) {
	a.byte1(0x0f, 0x80+byte(cc))
	rel := int64(target) - int64(a.base+uintptr(a.pos())+4)
	a.imm32(uint32(int32(rel)))
}
func (a *x86) jmpReg(r int) {
	a.rex(false, 0, 0, r)
	a.byte1(0xff)
	a.modRR(4, r)
}
func (a *x86) ret() { a.byte1(0xc3) }

// SSE2 (scalar doubles): op is the byte after 0x0F, prefix F2 (double) or
// F3 (single) or 66.
func (a *x86) sseRM(prefix byte, op byte, xmm, base int, disp int32) {
	a.byte1(prefix)
	a.rex(false, xmm, 0, base)
	a.byte1(0x0f, op)
	a.modMem(xmm, base, -1, 1, disp)
}
func (a *x86) sseRR(prefix byte, op byte, dst, src int) {
	if prefix != 0 {
		a.byte1(prefix)
	}
	a.rex(false, dst, 0, src)
	a.byte1(0x0f, op)
	a.modRR(dst, src)
}
