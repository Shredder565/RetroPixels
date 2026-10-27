package ps3

// The PPU recompiler's vector (VMX) instructions, in SSE. A vector register
// is a Vec: word 0 in lane 0, each word's bytes mirrored on the host (as in
// spujit.go). Vector floating point flushes denormals to zero (VSCR[NJ]),
// so those instructions run with the host's denormals-are-zero modes on,
// switched around each one.

import (
	"encoding/binary"
	"math"
	"unsafe"
)

func vr(r uint32) int32 { return offVR + int32(r)*16 }

var offVSCR = int32(unsafe.Offsetof(PPUState{}.VSCR))

func (b *jitBlock) constIdx(v [16]byte) int {
	for i, p := range b.pool {
		if p == v {
			return i
		}
	}
	b.pool = append(b.pool, v)
	return len(b.pool) - 1
}

// xc: op x, [constant v].
func (b *jitBlock) xc(pfx byte, op []byte, x int, v [16]byte) {
	at := b.a.xRip(pfx, op, x)
	b.refs = append(b.refs, poolRef{at, 0, b.constIdx(v)})
}

func (b *jitBlock) ldc(x int, v [16]byte)             { b.xc(0x66, sseMovdqa, x, v) }
func (b *jitBlock) pcst(op []byte, x int, v [16]byte) { b.xc(0x66, op, x, v) }

// tab: lea rdx, the shuffle table at off.
func (b *jitBlock) tab(off int) {
	at := b.a.leaRip(rDX)
	b.a.ripTo(at, 0, b.j.base+uintptr(off))
}

func (b *jitBlock) vld(x int, r uint32) { b.a.movdquLoad(x, rBX, vr(r)) }
func (b *jitBlock) vst(r uint32, x int) { b.a.movdquStore(rBX, vr(r), x) }

// njOn / njOff switch the host's denormal flushing on and off.
func (b *jitBlock) mxcsr(v uint32) {
	at := b.a.xRip(0, []byte{0x0f, 0xae}, 2) // ldmxcsr
	b.refs = append(b.refs, poolRef{at, 0, b.constIdx(splatW(v))})
}
func (b *jitBlock) njOn()  { b.mxcsr(0x9fc0) }
func (b *jitBlock) njOff() { b.mxcsr(0x1f80) }

// beShuffle: the pshufb mask for a vector byte permutation (result byte k
// = source byte perm[k], or zero for -1), in host positions.
func beShuffle(perm [16]int) (m [16]byte) {
	for j := 0; j < 16; j++ {
		p := perm[hostPos(j)]
		if p < 0 {
			m[j] = 0x80
		} else {
			m[j] = byte(hostPos(p))
		}
	}
	return
}

// shuffle2: x0 = bytes of a||b by perm (0..31, -1 zero); uses x0..x2.
func (b *jitBlock) shuffle2(ra, rb uint32, perm [16]int) {
	var pa, pb [16]int
	for k, p := range perm {
		pa[k], pb[k] = -1, -1
		if p >= 0 && p < 16 {
			pa[k] = p
		} else if p >= 16 {
			pb[k] = p - 16
		}
	}
	b.vld(0, ra)
	b.vld(1, rb)
	b.pcst(ssePshufb, 0, beShuffle(pa))
	b.pcst(ssePshufb, 1, beShuffle(pb))
	b.a.pi(ssePor, 0, 1)
}

// vmx translates a primary opcode 4 instruction.
func (b *jitBlock) vmx(inst uint32) bool {
	a := b.a
	if !spuJitOK {
		return false
	}
	d, ra, rb, rc := inst>>21&31, inst>>16&31, inst>>11&31, inst>>6&31
	bin := func(op []byte, pfx byte, swap bool) {
		b.vld(0, ra)
		b.vld(1, rb)
		if swap {
			a.xRR(pfx, op, 1, 0)
			b.vst(d, 1)
		} else {
			a.xRR(pfx, op, 0, 1)
			b.vst(d, 0)
		}
	}
	if x := inst & 0x3f; x >= 32 {
		switch x {
		case 42: // vsel: c ? b : a
			b.vld(0, ra)
			b.vld(1, rb)
			b.vld(2, rc)
			a.pi(ssePand, 1, 2)
			a.pi(ssePandn, 2, 0)
			a.pi(ssePor, 1, 2)
			b.vst(d, 1)
		case 43: // vperm
			b.vld(0, ra)
			b.vld(1, rb)
			b.vld(2, rc)
			a.movdqa(3, 2)
			b.pcst(ssePand, 3, splatW(0x0f0f0f0f))
			b.pcst(ssePxor, 3, splatW(0x03030303))
			a.pi(ssePshufb, 0, 3)
			a.pi(ssePshufb, 1, 3)
			b.pcst(ssePand, 2, splatW(0x10101010))
			b.pcst(ssePcmpeqb, 2, splatW(0x10101010))
			a.pi(ssePand, 1, 2)
			a.pi(ssePandn, 2, 0)
			a.pi(ssePor, 1, 2)
			b.vst(d, 1)
		case 44: // vsldoi
			sh := int(inst >> 6 & 15)
			var perm [16]int
			for k := range perm {
				perm[k] = k + sh
			}
			b.shuffle2(ra, rb, perm)
			b.vst(d, 0)
		case 46, 47: // vmaddfp a*c + b, vnmsubfp -(a*c - b)
			if !spuJitFMA {
				return false
			}
			b.vld(0, ra)
			b.vld(1, rc)
			b.vld(2, rb)
			b.njOn()
			if x == 46 {
				a.vfmadd213ps(0, 1, 2)
			} else {
				a.vfmsub213ps(0, 1, 2)
			}
			b.njOff()
			if x == 47 {
				b.xc(0, []byte{0x0f, 0x57}, 0, splatW(0x80000000)) // xorps: negate
			}
			b.vst(d, 0)
		default:
			return false
		}
		return true
	}
	switch inst & 0x3ff {
	case 134, 902, 646, 70, 838, 582, 6, 774, 518, 198, 454, 710:
		return b.vcmp(inst, d, ra, rb)
	}
	switch inst & 0x7ff {
	case 0:
		bin(ssePaddb, 0x66, false)
	case 64:
		bin(ssePaddw, 0x66, false)
	case 128:
		bin(ssePaddd, 0x66, false)
	case 1024:
		bin(ssePsubb, 0x66, false)
	case 1088:
		bin(ssePsubw, 0x66, false)
	case 1152:
		bin(ssePsubd, 0x66, false)
	case 1028:
		bin(ssePand, 0x66, false)
	case 1092: // vandc: a & ~b
		bin(ssePandn, 0x66, true)
	case 1156:
		bin(ssePor, 0x66, false)
	case 1220:
		bin(ssePxor, 0x66, false)
	case 1284: // vnor
		b.vld(0, ra)
		b.vld(1, rb)
		a.pi(ssePor, 0, 1)
		a.pi(ssePcmpeqd, 1, 1)
		a.pi(ssePxor, 0, 1)
		b.vst(d, 0)
	case 130: // vmaxuw
		bin(ssePmaxud, 0x66, false)
	case 386: // vmaxsw
		bin(ssePmaxsd, 0x66, false)
	case 642: // vminuw
		bin(ssePminud, 0x66, false)
	case 898: // vminsw
		bin(ssePminsd, 0x66, false)
	case 10, 74: // vaddfp vsubfp
		b.vld(0, ra)
		b.vld(1, rb)
		b.njOn()
		if inst&0x7ff == 10 {
			a.ps(sseAddps, 0, 1)
		} else {
			a.ps(sseSubps, 0, 1)
		}
		b.njOff()
		b.vst(d, 0)
	case 266: // vrefp
		b.vld(1, rb)
		b.ldc(0, splatW(0x3f800000))
		b.njOn()
		a.ps(sseDivps, 0, 1)
		b.njOff()
		b.vst(d, 0)
	case 652: // vspltw
		b.vld(0, rb)
		a.pshufd(0, 0, byte(ra&3)*0x55)
		b.vst(d, 0)
	case 588, 524: // vsplth, vspltb
		var perm [16]int
		for k := range perm {
			if inst&0x7ff == 588 {
				perm[k] = 2*int(ra&7) + k&1
			} else {
				perm[k] = int(ra & 15)
			}
		}
		b.vld(0, rb)
		b.pcst(ssePshufb, 0, beShuffle(perm))
		b.vst(d, 0)
	case 908, 844, 780: // vspltisw vspltish vspltisb
		v := uint32(int32(ra<<27) >> 27)
		switch inst & 0x7ff {
		case 844:
			v = v & 0xffff * 0x10001
		case 780:
			v = v & 0xff * 0x01010101
		}
		for k := int32(0); k < 4; k++ {
			a.movStoreImm(rBX, vr(d)+4*k, v)
		}
	case 140, 396, 12, 268, 76, 332: // vmrghw vmrglw vmrghb vmrglb vmrghh vmrglh
		var perm [16]int
		lo := 0
		switch inst & 0x7ff {
		case 396, 268, 332:
			lo = 8
		}
		size := map[uint32]int{140: 4, 396: 4, 12: 1, 268: 1, 76: 2, 332: 2}[inst&0x7ff]
		for k := 0; k < 16; k++ {
			e := k / size // element of the result
			src := lo + e/2*size + k%size
			if e&1 != 0 {
				src += 16
			}
			perm[k] = src
		}
		b.shuffle2(ra, rb, perm)
		b.vst(d, 0)
	case 388, 644, 900, 132: // vslw vsrw vsraw vrlw
		if !spuJitAVX2 {
			return false
		}
		b.vld(0, ra)
		b.vld(1, rb)
		b.pcst(ssePand, 1, splatW(31))
		switch inst & 0x7ff {
		case 388:
			a.vex38(0x47, 2, 0, 1)
		case 644:
			a.vex38(0x45, 2, 0, 1)
		case 900:
			a.vex38(0x46, 2, 0, 1)
		default:
			b.ldc(3, splatW(32))
			a.pi(ssePsubd, 3, 1)
			a.vex38(0x47, 2, 0, 1)
			a.vex38(0x45, 4, 0, 3)
			a.pi(ssePor, 2, 4)
		}
		b.vst(d, 2)
	case 842: // vcfsx: float(b) / 2^a
		b.vld(0, rb)
		a.ps(sseCvtdq2ps, 0, 0)
		if ra != 0 {
			b.xc(0, sseMulps, 0, splatW(math.Float32bits(float32(math.Ldexp(1, -int(ra))))))
		}
		b.vst(d, 0)
	case 970: // vctsxs: int(b * 2^a), saturating (setting VSCR[SAT])
		b.vld(0, rb)
		if ra != 0 {
			b.xc(0, sseMulps, 0, splatW(math.Float32bits(float32(math.Ldexp(1, int(ra))))))
		}
		a.movdqa(1, 0)
		a.xRR(0xf3, sseCvttps2dq, 1, 0)
		b.ldc(2, splatW(0x4f000000)) // 2^31 <= x: 0x7fffffff
		a.cmpps(2, 0, 2)
		a.pi(ssePxor, 1, 2)
		a.movdqa(3, 0)
		a.cmpps(3, 3, 7) // ordered (NaN -> 0)
		a.pi(ssePand, 1, 3)
		b.vst(d, 1)
		// saturated: too big, NaN, or below -2^31
		b.ldc(4, splatW(0xcf000000))
		a.movdqa(5, 0)
		a.cmpps(5, 4, 1) // x < -2^31
		a.pi(ssePor, 5, 2)
		a.pi(ssePcmpeqd, 6, 6)
		a.pi(ssePxor, 3, 6)
		a.pi(ssePor, 5, 3)
		a.ps([]byte{0x0f, 0x50}, rAX, 5) // movmskps eax, xmm5
		a.testRR(rAX, rAX)
		skip := a.jcc(ccE)
		a.aluMI(aluOr, rBX, offVSCR, 1)
		a.patch(skip)
	default:
		return false
	}
	return true
}

// vcmp: the vector compares; the record forms set CR6 (8 all true, 2 all
// false).
func (b *jitBlock) vcmp(inst, d, ra, rb uint32) bool {
	a := b.a
	b.vld(0, ra)
	b.vld(1, rb)
	xo := inst & 0x3ff
	unsigned := func(op []byte, bias uint32) {
		b.pcst(ssePxor, 0, splatW(bias))
		b.pcst(ssePxor, 1, splatW(bias))
		a.pi(op, 0, 1)
	}
	switch xo {
	case 134:
		a.pi(ssePcmpeqd, 0, 1)
	case 902:
		a.pi(ssePcmpgtd, 0, 1)
	case 646:
		unsigned(ssePcmpgtd, 0x80000000)
	case 70:
		a.pi(ssePcmpeqw, 0, 1)
	case 838:
		a.pi(ssePcmpgtw, 0, 1)
	case 582:
		unsigned(ssePcmpgtw, 0x80008000)
	case 6:
		a.pi(ssePcmpeqb, 0, 1)
	case 774:
		a.pi(ssePcmpgtb, 0, 1)
	case 518:
		unsigned(ssePcmpgtb, 0x80808080)
	case 198, 454, 710: // eq, ge (b <= a), gt (b < a)
		b.njOn()
		switch xo {
		case 198:
			a.cmpps(0, 1, 0)
		case 454:
			a.cmpps(1, 0, 2)
			a.movdqa(0, 1)
		default:
			a.cmpps(1, 0, 1)
			a.movdqa(0, 1)
		}
		b.njOff()
	default:
		return false
	}
	b.vst(d, 0)
	if inst&0x400 != 0 {
		a.xRR(0x66, []byte{0x0f, 0xd7}, rAX, 0) // pmovmskb: a bit per byte
		a.movRI(rCX, 0)
		a.aluRI(aluCmp, rAX, 0xffff)
		a.movRIKeepFlags(rDX, 8)
		a.cmovRR(ccE, rCX, rDX)
		a.testRR(rAX, rAX)
		a.movRIKeepFlags(rDX, 2)
		a.cmovRR(ccE, rCX, rDX)
		a.shiftRI(shShl, rCX, 4)
		a.movLoad(rDX, rBX, offCR)
		a.aluRI(aluAnd, rDX, ^uint32(0xf0))
		a.aluRR(aluOr, rDX, rCX)
		a.movStore(rBX, offCR, rDX)
	}
	return true
}

// lvlrx: lvlx / lvrx (the bytes from EA to the end of its quadword, to the
// left; or from the quadword's start to EA, to the right).
func (b *jitBlock) lvlrx(right bool, d, ra, rb uint32) {
	a := b.a
	b.eaX(ra, rb)
	a.movRR(r10, rCX)
	a.aluRI(aluAnd, r10, 15)
	a.aluRI(aluAnd, rCX, ^uint32(15))
	b.hostAddr(16, false)
	a.movdquLoadIdx(0, rDX, rAX)
	b.tab(tabBswap)
	a.xRM(0x66, ssePshufb, 0, rDX, 0)
	if right { // shift right by 16 - n bytes
		a.movRI(rCX, 16)
		a.aluRR(aluSub, rCX, r10)
		a.movRR(r10, rCX)
		b.tab(tabRotqmby)
	} else {
		b.tab(tabShlqby)
	}
	a.shiftRI(shShl, r10, 4)
	a.xRMI(0x66, ssePshufb, 0, rDX, r10, 1, 0)
	b.vst(d, 0)
}

var _ = binary.LittleEndian
