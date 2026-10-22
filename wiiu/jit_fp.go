package wiiu

// The recompiler's floating point and paired single instructions. They do
// exactly what fpu.go does: each half of a pair is a double, single
// results are rounded to single and back, and single loads and stores of
// NaNs (which keep their payload bit for bit) go to the interpreter, as do
// the rare cases fpu.go works out in software (fmas's halfway case).
//
// The multiply-adds need the host's FMA instructions (as Go's math.FMA
// uses); without them those run in the interpreter.

import "math"

var jitFMA = jitSupported && cpuHasFMA()

func fpr(r uint32) int32 { return offFPR + int32(r)*8 }
func ps1(r uint32) int32 { return offPS1 + int32(r)*8 }

const (
	x0 = iota
	x1
	x2
	x3
	x4
	x5
	x6
	x7
)

// nanSlow sends a NaN result to the interpreter, which makes NaNs the
// PowerPC's way (fpu.go's pick).
func (b *jitBlock) nanSlow(x int) {
	b.a.ucomisd(x, x)
	b.slowIf(ccP)
}

// rsX rounds xmm x to single precision (and back to a double).
func (b *jitBlock) rsX(x int) {
	b.a.cvtsd2ss(x)
	b.a.cvtss2sd(x)
}

// force25X rounds xmm x's mantissa to 25 bits the way force25 does
// (clobbers RAX, RCX).
func (b *jitBlock) force25X(x int) {
	a := b.a
	a.movqFromX(rAX, x)
	a.movRR64(rCX, rAX)
	a.shiftRI64(shShr, rCX, 52)
	a.aluRI(aluAnd, rCX, 0x7ff)
	a.aluRI(aluCmp, rCX, 0x7ff)
	skip := a.jcc(ccE)
	a.movRR64(rCX, rAX)
	a.aluRI64(aluAnd, rCX, 0x8000000)
	a.aluRI64(aluAnd, rAX, -0x8000000) // 0xfffffffff8000000
	a.aluRR64(aluAdd, rAX, rCX)
	a.movqToX(x, rAX)
	a.patch(skip)
}

// negX flips xmm x's sign (clobbers RAX, xmm7).
func (b *jitBlock) negX(x int) {
	a := b.a
	a.movRI64(rAX, 1<<63)
	a.movqToX(x7, rAX)
	a.xorpd(x, x7)
}

// fmasX: dst = a*c + b in one rounding to single, as fmas does: dst holds
// b on entry, x the a and y the c. The halfway case goes to the
// interpreter. Clobbers RAX, RCX.
func (b *jitBlock) fmasX(dst, x, y int) {
	a := b.a
	a.vfmadd231sd(dst, x, y)
	b.nanSlow(dst)
	// bits & 0x1fffffff == 0x10000000 (and not inf / NaN): the interpreter
	a.movqFromX(rAX, dst)
	a.movRR(rCX, rAX)
	a.aluRI(aluAnd, rCX, 0x1fffffff)
	a.aluRI(aluCmp, rCX, 0x10000000)
	ok := a.jcc(ccNE)
	a.movRR64(rCX, rAX)
	a.shiftRI64(shShr, rCX, 52)
	a.aluRI(aluAnd, rCX, 0x7ff)
	a.aluRI(aluCmp, rCX, 0x7ff)
	b.slowIf(ccNE)
	a.patch(ok)
	b.rsX(dst)
}

// f32 loads: EAX = single bits -> xmm x, NaNs to the interpreter
// (clobbers ECX)
func (b *jitBlock) singleToX(x int) {
	a := b.a
	a.movRR(rCX, rAX)
	a.aluRI(aluAnd, rCX, 0x7f800000)
	a.aluRI(aluCmp, rCX, 0x7f800000)
	ok := a.jcc(ccNE)
	a.testRI(rAX, 0x7fffff)
	b.slowIf(ccNE)
	a.patch(ok)
	a.movdToX(x, rAX)
	a.cvtss2sd(x)
}

// xmm x -> single bits in reg r, NaNs to the interpreter (x is changed)
func (b *jitBlock) xToSingle(r, x int) {
	a := b.a
	a.ucomisd(x, x)
	b.slowIf(ccP)
	a.cvtsd2ss(x)
	a.movdFromX(r, x)
}

// fcmpX compares xmm x with y into CR field f and FPSCR[FPCC].
func (b *jitBlock) fcmpX(f uint32, x, y int) {
	a := b.a
	a.ucomisd(x, y)
	a.movRIKeepFlags(rCX, 1)
	j1 := a.jcc(ccP)
	a.movRIKeepFlags(rCX, 8)
	j2 := a.jcc(ccB)
	a.movRIKeepFlags(rCX, 2)
	j3 := a.jcc(ccE)
	a.movRIKeepFlags(rCX, 4)
	a.patch(j1)
	a.patch(j2)
	a.patch(j3)
	// FPSCR[FPCC]
	a.movRR(rAX, rCX)
	a.shiftRI(shShl, rAX, 12)
	a.movLoad(rDX, rBX, offFPSCR)
	a.aluRI(aluAnd, rDX, ^uint32(0xf000))
	a.aluRR(aluOr, rDX, rAX)
	a.movStore(rBX, offFPSCR, rDX)
	// CR field
	sh := uint8(28 - 4*f)
	if sh != 0 {
		a.shiftRI(shShl, rCX, sh)
	}
	a.movLoad(rDX, rBX, offCR)
	a.aluRI(aluAnd, rDX, ^(uint32(0xf) << sh))
	a.aluRR(aluOr, rDX, rCX)
	a.movStore(rBX, offCR, rDX)
}

// selX: dst = x >= 0 ? y : z (dst may be any of them; clobbers xmm7)
func (b *jitBlock) selX(dst, x, y, z int) {
	a := b.a
	a.xorpd(x7, x7)
	a.ucomisd(x, x7)
	a.movapd(dst, z)
	lt := a.jcc(ccB) // below or unordered
	a.movapd(dst, y)
	a.patch(lt)
}

// fpLoad loads at ECX (with the update register in R8) into FPR d.
func (b *jitBlock) fpLoad(double bool, d uint32) {
	a := b.a
	if double {
		b.hostAddr(8)
		a.load64Idx(rAX, rDX, rAX)
		a.bswap64(rAX)
		a.movStore64(rBX, fpr(d), rAX)
		return
	}
	b.load(4, false)
	b.singleToX(x0)
	a.movsdStore(rBX, fpr(d), x0)
	a.movsdStore(rBX, ps1(d), x0)
}

// fpStore stores FPR s at ECX (keeps R10).
func (b *jitBlock) fpStore(kind int, s uint32) {
	a := b.a
	switch kind {
	case 0: // single
		a.movsdLoad(x0, rBX, fpr(s))
		b.xToSingle(r8, x0)
		b.store(4)
	case 1: // double
		a.movLoad64Disp(r8, rBX, fpr(s))
		b.hostAddr(8)
		a.bswap64(r8)
		a.store64Idx(rDX, rAX, r8)
	case 2: // stfiwx: the low word
		a.movLoad(r8, rBX, fpr(s))
		b.store(4)
	}
}

// fpMem: lfs(u) lfd(u) stfs(u) stfd(u) and the X forms.
func (b *jitBlock) fpMem(load bool, kind int, d, ra uint32, update bool, ea func()) bool {
	a := b.a
	if update && ra == 0 {
		return false
	}
	ea()
	if update {
		a.movRR(r10, rCX)
	}
	if load {
		b.fpLoad(kind == 1, d)
	} else {
		a.movRR(r10, rCX)
		// (the stores use ECX as the address and keep R10)
		b.fpStore(kind, d)
	}
	if update {
		a.movStore(rBX, gpr(ra), r10)
	}
	return true
}

// psq: psq_l / psq_st with a float GQR (others in the interpreter). EA is in
// ECX; w: one value (the second is 1.0 on loads).
func (b *jitBlock) psq(load bool, d, w, i uint32) {
	a := b.a
	gq := offGQR + int32(i)*4
	if load {
		a.testMI(rBX, gq, 7<<16)
	} else {
		a.testMI(rBX, gq, 7)
	}
	b.slowIf(ccNE)
	n := uint32(8)
	if w != 0 {
		n = 4
	}
	if load {
		b.hostAddr(n)
		a.opRMI(true, []byte{0x8d}, rSI, rDX, rAX, 1, 0) // lea rsi, [rdx+rax]
		a.movLoad(rAX, rSI, 0)
		a.bswap(rAX)
		b.singleToX(x0)
		if w == 0 {
			a.movLoad(rAX, rSI, 4)
			a.bswap(rAX)
			b.singleToX(x1)
		} else {
			a.movRI64(rAX, math.Float64bits(1))
			a.movqToX(x1, rAX)
		}
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x1)
		return
	}
	a.movsdLoad(x0, rBX, fpr(d))
	b.xToSingle(r8, x0)
	if w == 0 {
		a.movsdLoad(x1, rBX, ps1(d))
		b.xToSingle(r11, x1)
	}
	b.hostAddr(n)
	a.opRMI(true, []byte{0x8d}, rSI, rDX, rAX, 1, 0)
	a.bswap(r8)
	a.movStore(rSI, 0, r8)
	if w == 0 {
		a.bswap(r11)
		a.movStore(rSI, 4, r11)
	}
}

// fpInst translates the floating point instructions (primary opcodes 4,
// 48..63).
func (b *jitBlock) fpInst(inst uint32) bool {
	a := b.a
	op := inst >> 26
	d := inst >> 21 & 31
	ra := inst >> 16 & 31
	rb := inst >> 11 & 31
	rc := inst >> 6 & 31
	simm := uint32(int32(int16(inst)))
	eaD := func() { b.eaD(ra, simm) }
	switch op {
	case 48, 49:
		return b.fpMem(true, 0, d, ra, op == 49, eaD)
	case 50, 51:
		return b.fpMem(true, 1, d, ra, op == 51, eaD)
	case 52, 53:
		return b.fpMem(false, 0, d, ra, op == 53, eaD)
	case 54, 55:
		return b.fpMem(false, 1, d, ra, op == 55, eaD)
	case 56, 57, 60, 61: // psq_l(u), psq_st(u)
		update := op == 57 || op == 61
		if update && ra == 0 {
			return false
		}
		off := uint32(int32(inst<<20) >> 20)
		if ra != 0 || update {
			b.eaD(ra, off)
		} else {
			a.movRI(rCX, off)
		}
		a.movRR(r10, rCX)
		b.psq(op < 60, d, inst>>15&1, inst>>12&7)
		if update {
			a.movStore(rBX, gpr(ra), r10)
		}
		return true
	case 59:
		return b.fp59(inst, d, ra, rb, rc)
	case 63:
		return b.fp63(inst, d, ra, rb, rc)
	case 4:
		return b.fpPS(inst, d, ra, rb, rc)
	}
	return false
}

func (b *jitBlock) fp59(inst, d, ra, rb, rc uint32) bool {
	a := b.a
	if inst&1 != 0 {
		return false
	}
	xo := inst >> 1 & 31
	switch xo {
	case 18, 20, 21: // fdivs, fsubs, fadds
		ops := map[uint32]byte{18: sseDiv, 20: sseSub, 21: sseAdd}
		a.movsdLoad(x0, rBX, fpr(ra))
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(ops[xo], x0, x1)
	case 22: // fsqrts
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(sseSqrt, x0, x1)
	case 24: // fres
		a.movRI64(rAX, math.Float64bits(1))
		a.movqToX(x0, rAX)
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(sseDiv, x0, x1)
	case 25: // fmuls
		a.movsdLoad(x1, rBX, fpr(rc))
		b.force25X(x1)
		a.movsdLoad(x0, rBX, fpr(ra))
		a.sd(sseMul, x0, x1)
	case 28, 29, 30, 31: // fmsubs, fmadds, fnmsubs, fnmadds
		if !jitFMA {
			return false
		}
		a.movsdLoad(x2, rBX, fpr(rc))
		b.force25X(x2)
		a.movsdLoad(x1, rBX, fpr(ra))
		a.movsdLoad(x0, rBX, fpr(rb))
		if xo == 28 || xo == 30 {
			b.negX(x0)
		}
		b.fmasX(x0, x1, x2)
		if xo >= 30 {
			b.negX(x0)
		}
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x0)
		return true
	default:
		return false
	}
	b.nanSlow(x0)
	b.rsX(x0)
	a.movsdStore(rBX, fpr(d), x0)
	a.movsdStore(rBX, ps1(d), x0)
	return true
}

func (b *jitBlock) fp63(inst, d, ra, rb, rc uint32) bool {
	a := b.a
	if inst&1 != 0 {
		return false
	}
	switch inst >> 1 & 31 {
	case 18, 20, 21: // fdiv, fsub, fadd
		ops := map[uint32]byte{18: sseDiv, 20: sseSub, 21: sseAdd}
		a.movsdLoad(x0, rBX, fpr(ra))
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(ops[inst>>1&31], x0, x1)
		b.nanSlow(x0)
		a.movsdStore(rBX, fpr(d), x0)
		return true
	case 22: // fsqrt
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(sseSqrt, x0, x1)
		b.nanSlow(x0)
		a.movsdStore(rBX, fpr(d), x0)
		return true
	case 23: // fsel
		a.movsdLoad(x1, rBX, fpr(ra))
		a.movsdLoad(x2, rBX, fpr(rc))
		a.movsdLoad(x3, rBX, fpr(rb))
		b.selX(x0, x1, x2, x3)
		a.movsdStore(rBX, fpr(d), x0)
		return true
	case 25: // fmul
		a.movsdLoad(x0, rBX, fpr(ra))
		a.movsdLoad(x1, rBX, fpr(rc))
		a.sd(sseMul, x0, x1)
		b.nanSlow(x0)
		a.movsdStore(rBX, fpr(d), x0)
		return true
	case 26: // frsqrte
		a.movsdLoad(x1, rBX, fpr(rb))
		a.sd(sseSqrt, x1, x1)
		a.movRI64(rAX, math.Float64bits(1))
		a.movqToX(x0, rAX)
		a.sd(sseDiv, x0, x1)
		b.nanSlow(x0)
		a.movsdStore(rBX, fpr(d), x0)
		return true
	case 28, 29, 30, 31: // fmsub, fmadd, fnmsub, fnmadd
		if !jitFMA {
			return false
		}
		xo := inst >> 1 & 31
		a.movsdLoad(x1, rBX, fpr(ra))
		a.movsdLoad(x2, rBX, fpr(rc))
		a.movsdLoad(x0, rBX, fpr(rb))
		if xo == 28 || xo == 30 {
			b.negX(x0)
		}
		a.vfmadd231sd(x0, x1, x2)
		b.nanSlow(x0)
		if xo >= 30 {
			b.negX(x0)
		}
		a.movsdStore(rBX, fpr(d), x0)
		return true
	}
	switch inst >> 1 & 0x3ff {
	case 0, 32: // fcmpu, fcmpo
		a.movsdLoad(x0, rBX, fpr(ra))
		a.movsdLoad(x1, rBX, fpr(rb))
		b.fcmpX(d>>2, x0, x1)
	case 12: // frsp
		a.movsdLoad(x0, rBX, fpr(rb))
		b.rsX(x0)
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x0)
	case 15: // fctiwz
		a.movsdLoad(x0, rBX, fpr(rb))
		a.movRI64(rAX, math.Float64bits(2147483647))
		a.movqToX(x1, rAX)
		a.movRIKeepFlags(rCX, 0x7fffffff)
		a.ucomisd(x0, x1)
		big := a.jcc(ccAE) // >= 2147483647 (not NaN)
		a.cvttsd2si(rCX, x0)
		a.patch(big)
		a.movRI64(rAX, 0xfff8000000000000)
		a.aluRR64(aluOr, rAX, rCX) // (ECX's upper half is 0)
		a.movStore64(rBX, fpr(d), rAX)
	case 40, 72, 136, 264: // fneg, fmr, fnabs, fabs
		a.movLoad64Disp(rAX, rBX, fpr(rb))
		switch inst >> 1 & 0x3ff {
		case 40:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluXor, rAX, rCX)
		case 136:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluOr, rAX, rCX)
		case 264:
			a.movRI64(rCX, 1<<63-1)
			a.aluRR64(aluAnd, rAX, rCX)
		}
		a.movStore64(rBX, fpr(d), rAX)
	default:
		return false
	}
	return true
}

// fpPS: the paired single arithmetic (and psq_lx / psq_stx).
func (b *jitBlock) fpPS(inst, d, ra, rb, rc uint32) bool {
	a := b.a
	xo5 := inst >> 1 & 31
	xo10 := inst >> 1 & 0x3ff
	if xo := inst >> 1 & 63; xo == 6 || xo == 7 || xo == 38 || xo == 39 { // psq_lx, psq_stx, psq_lux, psq_stux
		update := xo >= 38
		if update && ra == 0 {
			return false
		}
		b.eaX(ra, rb)
		a.movRR(r10, rCX)
		b.psq(xo&1 == 0, d, inst>>10&1, inst>>7&7)
		if update {
			a.movStore(rBX, gpr(ra), r10)
		}
		return true
	}
	if inst&1 != 0 {
		return false
	}
	// x1/x2 = a0/a1, x3/x4 = b0/b1, x5/x6 = c0/c1
	ld := func(x int, off int32) { a.movsdLoad(x, rBX, off) }
	setRS := func() { // results in x0 / x1 -> d, rounded
		b.nanSlow(x0)
		b.nanSlow(x1)
		b.rsX(x0)
		b.rsX(x1)
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x1)
	}
	switch xo5 {
	case 18, 20, 21: // ps_div, ps_sub, ps_add
		ops := map[uint32]byte{18: sseDiv, 20: sseSub, 21: sseAdd}
		ld(x0, fpr(ra))
		ld(x1, ps1(ra))
		ld(x3, fpr(rb))
		ld(x4, ps1(rb))
		a.sd(ops[xo5], x0, x3)
		a.sd(ops[xo5], x1, x4)
		setRS()
		return true
	case 25: // ps_mul
		ld(x5, fpr(rc))
		b.force25X(x5)
		ld(x6, ps1(rc))
		b.force25X(x6)
		ld(x0, fpr(ra))
		ld(x1, ps1(ra))
		a.sd(sseMul, x0, x5)
		a.sd(sseMul, x1, x6)
		setRS()
		return true
	case 12, 13: // ps_muls0, ps_muls1
		if xo5 == 12 {
			ld(x5, fpr(rc))
		} else {
			ld(x5, ps1(rc))
		}
		b.force25X(x5)
		ld(x0, fpr(ra))
		ld(x1, ps1(ra))
		a.sd(sseMul, x0, x5)
		a.sd(sseMul, x1, x5)
		setRS()
		return true
	case 10, 11: // ps_sum0, ps_sum1
		ld(x2, fpr(ra))
		ld(x3, ps1(rb))
		a.sd(sseAdd, x2, x3)
		if xo5 == 10 {
			a.movapd(x0, x2)
			ld(x1, ps1(rc))
		} else {
			ld(x0, fpr(rc))
			a.movapd(x1, x2)
		}
		setRS()
		return true
	case 23: // ps_sel
		ld(x1, fpr(ra))
		ld(x2, ps1(ra))
		ld(x3, fpr(rb))
		ld(x4, ps1(rb))
		ld(x5, fpr(rc))
		ld(x6, ps1(rc))
		b.selX(x0, x1, x5, x3)
		b.selX(x1, x2, x6, x4)
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x1)
		return true
	case 24: // ps_res
		ld(x3, fpr(rb))
		ld(x4, ps1(rb))
		a.movRI64(rAX, math.Float64bits(1))
		a.movqToX(x0, rAX)
		a.movqToX(x1, rAX)
		a.sd(sseDiv, x0, x3)
		a.sd(sseDiv, x1, x4)
		setRS()
		return true
	case 26: // ps_rsqrte
		ld(x3, fpr(rb))
		ld(x4, ps1(rb))
		a.sd(sseSqrt, x3, x3)
		a.sd(sseSqrt, x4, x4)
		a.movRI64(rAX, math.Float64bits(1))
		a.movqToX(x0, rAX)
		a.movqToX(x1, rAX)
		a.sd(sseDiv, x0, x3)
		a.sd(sseDiv, x1, x4)
		setRS()
		return true
	case 14, 15, 28, 29, 30, 31: // ps_madds0/1, ps_msub, ps_madd, ps_nmsub, ps_nmadd
		if !jitFMA {
			return false
		}
		switch xo5 {
		case 14:
			ld(x5, fpr(rc))
			b.force25X(x5)
			a.movapd(x6, x5)
		case 15:
			ld(x5, ps1(rc))
			b.force25X(x5)
			a.movapd(x6, x5)
		default:
			ld(x5, fpr(rc))
			b.force25X(x5)
			ld(x6, ps1(rc))
			b.force25X(x6)
		}
		ld(x1, fpr(ra))
		ld(x2, ps1(ra))
		ld(x0, fpr(rb))
		ld(x3, ps1(rb))
		if xo5 == 28 || xo5 == 30 {
			b.negX(x0)
			b.negX(x3)
		}
		b.fmasX(x0, x1, x5)
		b.fmasX(x3, x2, x6)
		if xo5 >= 30 {
			b.negX(x0)
			b.negX(x3)
		}
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x3)
		return true
	}
	switch xo10 {
	case 0, 32, 64, 96: // ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
		if xo10 < 64 {
			ld(x0, fpr(ra))
			ld(x1, fpr(rb))
		} else {
			ld(x0, ps1(ra))
			ld(x1, ps1(rb))
		}
		b.fcmpX(d>>2, x0, x1)
	case 40, 72, 136, 264: // ps_neg, ps_mr, ps_nabs, ps_abs
		a.movLoad64Disp(rAX, rBX, fpr(rb))
		a.movLoad64Disp(rDX, rBX, ps1(rb))
		switch xo10 {
		case 40:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluXor, rAX, rCX)
			a.aluRR64(aluXor, rDX, rCX)
		case 136:
			a.movRI64(rCX, 1<<63)
			a.aluRR64(aluOr, rAX, rCX)
			a.aluRR64(aluOr, rDX, rCX)
		case 264:
			a.movRI64(rCX, 1<<63-1)
			a.aluRR64(aluAnd, rAX, rCX)
			a.aluRR64(aluAnd, rDX, rCX)
		}
		a.movStore64(rBX, fpr(d), rAX)
		a.movStore64(rBX, ps1(d), rDX)
	case 528, 560, 592, 624: // ps_merge00, 01, 10, 11
		if xo10 < 592 {
			ld(x0, fpr(ra))
		} else {
			ld(x0, ps1(ra))
		}
		if xo10 == 528 || xo10 == 592 {
			ld(x1, fpr(rb))
			b.rsX(x1)
		} else {
			ld(x1, ps1(rb))
		}
		a.movsdStore(rBX, fpr(d), x0)
		a.movsdStore(rBX, ps1(d), x1)
	default:
		return false
	}
	return true
}
