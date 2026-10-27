package ps3

// Translating RSX vertex programs to Direct3D vertex shaders, so the GPU
// transforms the vertices (the CPU only fetches their attributes). The
// translation does what runVP does; rsx_vpdxbc_test.go checks the two
// agree.
//
// Only programs whose flow is known when the draw starts are translated:
// static branches (on the branch bits) and unconditional calls, jumps and
// returns are followed here, so the shader is straight-line code. A program
// with a branch that depends on the vertex runs on the CPU as before.
//
// The vertex buffer (t0) holds each vertex's enabled attributes, packed
// (vpShaderKey.slots of them a vertex). cb0 holds:
//
//	[0..511]  the program constants (468 and up read as 0)
//	[512]     viewport scale (x, y, z)
//	[513]     viewport offset (x, y, z)
//	[514]     the targets' size (w, h)
//
// The shader's outputs match gpuVSCode's: the position in Direct3D's clip
// space, outputs 1..15, and (window z, 0, 0, 1/w).

import (
	"fmt"
	"hash/maphash"

	"retropixels/wiiu/d3d"
)

const (
	vxT     = 0   // temporaries 0..63
	vxA     = 64  // attributes 0..15
	vxO     = 80  // outputs 0..15
	vxAR    = 96  // address registers A0, A1 (integers)
	vxCC    = 98  // CC0, CC1
	vxD     = 100 // an operation's result
	vxS     = 101 // sources S0..S2
	vxX     = 104 // scratch 104..107
	vxM     = 108 // a write's condition mask
	vxTemps = 110

	vxConstVP   = 512 // cb0 index of the viewport scale
	vxConstSize = 515
)

// vpShaderKey is what a translated vertex shader depends on.
type vpShaderKey struct {
	hash  uint64 // the traced instructions
	slots uint32 // enabled attribute mask (each one a vec4 in the buffer)
}

var vpHashSeed = maphash.MakeSeed()

// vpTrace follows the program from start as runVP would, with the branch
// bits given: the instructions run, in order (false when the flow depends
// on the vertex).
func vpTrace(prog []vpInst, start int, branchBits uint32) ([]int, bool) {
	var pcs []int
	var stack [8]int
	sp := 0
	pc := start
	for steps := 0; ; steps++ {
		if steps > 2048 || pc < 0 || pc >= len(prog) {
			return nil, false
		}
		in := &prog[pc]
		pcs = append(pcs, pc)
		pc++
		sop := in.scaOp
		if in.condTest && in.cond == 0 {
			sop = 0
		}
		static := func() bool { return (branchBits>>in.brbIndex&1 != 0) == in.brbTrue }
		dyn := func() (taken, ok bool) {
			switch in.cond {
			case 7:
				return true, true
			case 0:
				return false, true
			}
			return false, false
		}
		switch sop {
		case 0x08: // BRA: through the address register
			taken, ok := dyn()
			if !ok || taken {
				return nil, false
			}
		case 0x09: // BRI
			taken, ok := dyn()
			if !ok {
				return nil, false
			}
			if taken {
				pc = in.target
			}
		case 0x0a: // CAL
			taken, ok := dyn()
			if !ok {
				return nil, false
			}
			if taken {
				if sp == len(stack) {
					return pcs, true
				}
				stack[sp] = pc
				sp++
				pc = in.target
			}
		case 0x0c: // RET
			taken, ok := dyn()
			if !ok {
				return nil, false
			}
			if taken {
				if sp == 0 {
					return pcs, true
				}
				sp--
				pc = stack[sp]
			}
		case 0x11: // BRB
			if static() {
				pc = in.target
			}
		case 0x12: // CLB
			if static() {
				if sp == len(stack) {
					return pcs, true
				}
				stack[sp] = pc
				sp++
				pc = in.target
			}
		}
		if in.end {
			return pcs, true
		}
	}
}

// vpTraceHash identifies a trace by its instruction words.
func vpTraceHash(words []uint32, pcs []int) uint64 {
	var h maphash.Hash
	h.SetSeed(vpHashSeed)
	var b [16]byte
	for _, pc := range pcs {
		for k := 0; k < 4; k++ {
			w := words[pc*4+k]
			b[4*k], b[4*k+1], b[4*k+2], b[4*k+3] = byte(w), byte(w>>8), byte(w>>16), byte(w>>24)
		}
		h.Write(b[:])
	}
	return h.Sum64()
}

type vpTr struct {
	s *d3d.Shader
}

func (t *vpTr) ins(op uint32, o ...d3d.Opnd) { t.s.Ins(op, o...) }

// translateVP makes the vertex shader for the traced instructions.
func translateVP(prog []vpInst, pcs []int, slots uint32) (code []byte, err error) {
	t := &vpTr{s: &d3d.Shader{}}
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("vertex program: %v", r)
		}
	}()
	s := t.s
	s.In = []d3d.Sig{{Name: "SV_VertexID", SV: d3d.SVVertexID, CompType: 1, Mask: 1}}
	s.DclCB(0, vxConstSize, true)
	s.DclResource(0, d3d.DimBuffer, d3d.RetFloat)
	s.DclInputSV(0, 1, d3d.SVVertexID)
	s.Out = gpuPSInSig()
	s.DclOutputSV(0, 15, d3d.SVPosition)
	for r := 1; r <= dxPosReg; r++ {
		s.DclOutput(r, 15)
	}
	// registers start as the interpreter's: 0, outputs (0, 0, 0, 1)
	for i := 0; i < vxTemps; i++ {
		t.ins(d3d.OpMov, dR(i).Mask(15), dU(0, 0, 0, 0))
	}
	for i := 0; i < 16; i++ {
		t.ins(d3d.OpMov, dR(vxO+i).Mask(15), dF(0, 0, 0, 1))
	}
	// the attributes
	n := uint32(0)
	for a := uint32(0); a < 16; a++ {
		if slots>>a&1 != 0 {
			n++
		}
	}
	t.ins(d3d.OpImad, dW(vxX, 0), d3d.In(0).C(0), dU(n), dU(0))
	k := uint32(0)
	for a := 0; a < 16; a++ {
		if slots>>a&1 == 0 {
			continue
		}
		t.ins(d3d.OpIadd, dW(vxX, 1), dRC(vxX, 0), dU(k))
		t.ins(d3d.OpLd, dR(vxA+a).Mask(15), dRC(vxX, 1), d3d.Tex(0))
		k++
	}
	for _, pc := range pcs {
		t.inst(&prog[pc])
	}
	t.output()
	t.ins(d3d.OpRet)
	s.Temps(vxTemps)
	return s.Bytes(), nil
}

// src reads source n of in into S<n> (swizzled, abs, neg).
func (t *vpTr) src(in *vpInst, n int) d3d.Opnd {
	sr := &in.src[n]
	var o d3d.Opnd
	switch sr.typ {
	case 1:
		o = dR(vxT + int(sr.tmp))
	case 2:
		o = dR(vxA + int(in.input&15))
	case 3:
		if in.indexConst {
			x := dW(vxX, 2)
			t.ins(d3d.OpIadd, x, dR(vxAR+int(in.addrSel&1)).C(uint32(in.addrSwz)), dU(uint32(in.constant)))
			t.ins(d3d.OpAnd, x, dRC(vxX, 2), dU(511))
			o = d3d.CB(0, 0).RelIndex(1, dRC(vxX, 2))
		} else if in.constant < 468 {
			o = d3d.CB(0, int(in.constant))
		} else {
			o = dF(0, 0, 0, 0)
		}
	default:
		o = dF(0, 0, 0, 0)
	}
	o = o.Swz(uint32(sr.swz[0]), uint32(sr.swz[1]), uint32(sr.swz[2]), uint32(sr.swz[3]))
	if sr.abs {
		o = o.Abs()
	}
	if sr.neg {
		o = o.Neg()
	}
	r := vxS + n
	t.ins(d3d.OpMov, dR(r).Mask(15), o)
	return dR(r)
}

// condMask puts in's per-component condition in vxM.
func (t *vpTr) condMask(in *vpInst) {
	c := dR(vxCC+int(in.condSel&1)).Swz(uint32(in.condSwz[0]), uint32(in.condSwz[1]), uint32(in.condSwz[2]), uint32(in.condSwz[3]))
	z := dF(0, 0, 0, 0)
	m := dR(vxM).Mask(15)
	switch in.cond {
	case 1:
		t.ins(d3d.OpLt, m, c, z)
	case 2:
		t.ins(d3d.OpEq, m, c, z)
	case 3:
		t.ins(d3d.OpGe, m, z, c)
	case 4:
		t.ins(d3d.OpLt, m, z, c)
	case 5:
		t.ins(d3d.OpNe, m, c, z)
	case 6:
		t.ins(d3d.OpGe, m, c, z)
	case 7:
		t.ins(d3d.OpMov, m, dU(0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff))
	default:
		t.ins(d3d.OpMov, m, dU(0, 0, 0, 0))
	}
}

// put writes vxD to register r under mask (and the condition).
func (t *vpTr) put(in *vpInst, r int, mask [4]bool) {
	var m uint32
	for i, b := range mask {
		if b {
			m |= 1 << i
		}
	}
	if m == 0 {
		return
	}
	if in.condTest {
		t.ins(d3d.OpMovc, dR(r).Mask(m), dR(vxM), dR(vxD), dR(r))
	} else {
		t.ins(d3d.OpMov, dR(r).Mask(m), dR(vxD))
	}
}

// writeVec / writeSca store vxD as vpWriteVec / vpWriteSca do.
func (t *vpTr) writeVec(in *vpInst) {
	if in.sat {
		t.ins(d3d.OpMov|d3d.Sat, dR(vxD).Mask(15), dR(vxD))
	}
	if in.condTest {
		t.condMask(in)
	}
	if in.dstTmp != 0x3f {
		t.put(in, vxT+int(in.dstTmp), in.vecMask)
		if !in.vecResult {
			return
		}
	}
	if in.vecResult {
		if in.dst < 16 {
			t.put(in, vxO+int(in.dst), in.vecMask)
		}
		return
	}
	t.put(in, vxCC+int(in.condSel&1), in.vecMask)
}

func (t *vpTr) writeSca(in *vpInst) {
	if in.sat {
		t.ins(d3d.OpMov|d3d.Sat, dR(vxD).Mask(15), dR(vxD))
	}
	if in.condTest {
		t.condMask(in)
	}
	if in.scaDstTmp != 0x3f {
		t.put(in, vxT+int(in.scaDstTmp), in.scaMask)
		return
	}
	if !in.vecResult {
		if in.dst < 16 {
			t.put(in, vxO+int(in.dst), in.scaMask)
		}
		return
	}
	t.put(in, vxCC+int(in.condSel&1), in.scaMask)
}

const vxOne = 0x3f800000

func (t *vpTr) inst(in *vpInst) {
	vop, sop := in.vecOp, in.scaOp
	if in.condTest && in.cond == 0 {
		vop, sop = 0, 0
	}
	D := dR(vxD).Mask(15)
	Dv := dR(vxD)
	X := func(i int) d3d.Opnd { return dR(vxX + i) }
	if vop == 0x0d { // ARL
		a := t.src(in, 0)
		t.ins(d3d.OpRoundNi, dR(vxX).Mask(15), a)
		t.ins(d3d.OpFtoi, dR(vxAR+int(in.dstTmp&1)).Mask(15), X(0))
	} else if vop != 0 {
		a := t.src(in, 0)
		cmp := func(op uint32, swap bool) {
			b := t.src(in, 1)
			if swap {
				t.ins(op, D, b, a)
			} else {
				t.ins(op, D, a, b)
			}
			t.ins(d3d.OpAnd, D, Dv, dU(vxOne, vxOne, vxOne, vxOne))
		}
		switch vop {
		case 0x01:
			t.ins(d3d.OpMov, D, a)
		case 0x02:
			t.ins(d3d.OpMul, D, a, t.src(in, 1))
		case 0x03:
			t.ins(d3d.OpAdd, D, a, t.src(in, 2))
		case 0x04:
			b := t.src(in, 1)
			c := t.src(in, 2)
			t.ins(d3d.OpMad, D, a, b, c)
		case 0x05:
			t.ins(d3d.OpDp3, D, a, t.src(in, 1))
		case 0x06:
			b := t.src(in, 1)
			t.ins(d3d.OpDp3, D, a, b)
			t.ins(d3d.OpAdd, D, Dv, b.C(3))
		case 0x07:
			t.ins(d3d.OpDp4, D, a, t.src(in, 1))
		case 0x08: // DST
			b := t.src(in, 1)
			t.ins(d3d.OpMul, dR(vxD).Mask(2), a, b)
			t.ins(d3d.OpMov, dR(vxD).Mask(4), a)
			t.ins(d3d.OpMov, dR(vxD).Mask(8), b)
			t.ins(d3d.OpMov, dR(vxD).Mask(1), dF(1, 1, 1, 1))
		case 0x09:
			t.ins(d3d.OpMin, D, a, t.src(in, 1))
		case 0x0a:
			t.ins(d3d.OpMax, D, a, t.src(in, 1))
		case 0x0b: // SLT
			cmp(d3d.OpLt, false)
		case 0x0c: // SGE
			cmp(d3d.OpGe, false)
		case 0x0e:
			t.ins(d3d.OpFrc, D, a)
		case 0x0f:
			t.ins(d3d.OpRoundNi, D, a)
		case 0x10: // SEQ
			cmp(d3d.OpEq, false)
		case 0x11:
			t.ins(d3d.OpMov, D, dF(0, 0, 0, 0))
		case 0x12: // SGT: a > b is b < a
			cmp(d3d.OpLt, true)
		case 0x13: // SLE: a <= b is b >= a
			cmp(d3d.OpGe, true)
		case 0x14: // SNE
			cmp(d3d.OpNe, false)
		case 0x15:
			t.ins(d3d.OpMov, D, dF(1, 1, 1, 1))
		case 0x16: // SSG
			t.ins(d3d.OpMov, D, a)
			t.ins(d3d.OpLt, X(0).Mask(15), dF(0, 0, 0, 0), a)
			t.ins(d3d.OpMovc, D, X(0), dF(1, 1, 1, 1), Dv)
			t.ins(d3d.OpLt, X(0).Mask(15), a, dF(0, 0, 0, 0))
			t.ins(d3d.OpMovc, D, X(0), dF(-1, -1, -1, -1), Dv)
		default: // TXL (no vertex textures here) and the rest
			t.ins(d3d.OpMov, D, dF(0, 0, 0, 0))
		}
		t.writeVec(in)
	}
	if sop == 0 {
		return
	}
	if sop == 0x07 { // LIT
		s := t.src(in, 2)
		t.ins(d3d.OpMov, D, dF(1, 0, 0, 1))
		// z: y^clamp(w) when x > 0 and y > 0
		t.ins(d3d.OpMax, dW(vxX, 0), s.C(3), dF(-127.9961))
		t.ins(d3d.OpMin, dW(vxX, 0), dRC(vxX, 0), dF(127.9961))
		t.ins(d3d.OpLog, dW(vxX, 1), s.C(1))
		t.ins(d3d.OpMul, dW(vxX, 1), dRC(vxX, 1), dRC(vxX, 0))
		t.ins(d3d.OpExp, dW(vxX, 1), dRC(vxX, 1))
		t.ins(d3d.OpLt, dW(vxX, 2), dF(0), s.C(0))
		t.ins(d3d.OpLt, dW(vxX, 3), dF(0), s.C(1))
		t.ins(d3d.OpAnd, dW(vxX, 3), dRC(vxX, 3), dRC(vxX, 2))
		t.ins(d3d.OpMovc, dR(vxD).Mask(2), dRC(vxX, 2), s.C(0), Dv)
		t.ins(d3d.OpMovc, dR(vxD).Mask(4), dRC(vxX, 3), dRC(vxX, 1), Dv)
		t.writeSca(in)
		return
	}
	switch sop {
	case 0x08, 0x09, 0x0a, 0x0c, 0x11, 0x12: // flow: followed by vpTrace
		return
	}
	s := t.src(in, 2).C(0)
	x := dW(vxD, 0)
	xv := dRC(vxD, 0)
	switch sop {
	case 0x01:
		t.ins(d3d.OpMov, x, s)
	case 0x02:
		t.ins(d3d.OpDiv, x, dF(1), s)
	case 0x03: // RCC: 1/s, kept away from 0 and infinity
		t.ins(d3d.OpDiv, x, dF(1), s)
		t.ins(d3d.OpAnd, dW(vxX, 0), xv, dU(0x80000000))
		t.ins(d3d.OpMax, x, xv.Abs(), dF(5.42101e-20))
		t.ins(d3d.OpMin, x, xv, dF(1.884467e19))
		t.ins(d3d.OpOr, x, xv, dRC(vxX, 0))
	case 0x04:
		t.ins(d3d.OpRsq, x, s.Abs())
	case 0x05, 0x0e:
		t.ins(d3d.OpExp, x, s)
	case 0x06:
		t.ins(d3d.OpLog, x, s.Abs())
	case 0x0d:
		t.ins(d3d.OpLog, x, s)
	case 0x0f:
		t.ins(d3d.OpSincos, x, d3d.Null(), s)
	case 0x10:
		t.ins(d3d.OpSincos, d3d.Null(), x, s)
	default:
		return
	}
	t.ins(d3d.OpMov, dR(vxD).Mask(14), xv)
	t.writeSca(in)
}

// output turns the program's outputs into the shader's: the position into
// Direct3D's clip space (as gpuRenderer.draw does on the CPU).
func (t *vpTr) output() {
	p := dR(vxO)
	sc, of, sz := d3d.CB(0, vxConstVP), d3d.CB(0, vxConstVP+1), d3d.CB(0, vxConstVP+2)
	X := func(c uint32) d3d.Opnd { return dW(vxX, c) }
	XV := func(c uint32) d3d.Opnd { return dRC(vxX, c) }
	w := p.C(3)
	// x: 2*(x*sx+ox*w)/W - w
	t.ins(d3d.OpMul, X(0), of.C(0), w)
	t.ins(d3d.OpMad, X(0), p.C(0), sc.C(0), XV(0))
	t.ins(d3d.OpMul, X(0), XV(0), dF(2))
	t.ins(d3d.OpDiv, X(0), XV(0), sz.C(0))
	t.ins(d3d.OpAdd, d3d.Out(0).Mask(1), XV(0), w.Neg())
	// y: w - 2*(y*sy+oy*w)/H
	t.ins(d3d.OpMul, X(1), of.C(1), w)
	t.ins(d3d.OpMad, X(1), p.C(1), sc.C(1), XV(1))
	t.ins(d3d.OpMul, X(1), XV(1), dF(2))
	t.ins(d3d.OpDiv, X(1), XV(1), sz.C(1))
	t.ins(d3d.OpAdd, d3d.Out(0).Mask(2), w, XV(1).Neg())
	// z: z*sz+oz*w
	t.ins(d3d.OpMul, X(2), of.C(2), w)
	t.ins(d3d.OpMad, d3d.Out(0).Mask(4), p.C(2), sc.C(2), XV(2))
	t.ins(d3d.OpMov, d3d.Out(0).Mask(8), w)
	for k := 1; k < 16; k++ {
		t.ins(d3d.OpMov, d3d.Out(k).Mask(15), dR(vxO+k))
	}
	// (window z, 0, 0, 1/w), 1/w = 1e6 when w = 0
	t.ins(d3d.OpEq, X(3), w, dF(0))
	t.ins(d3d.OpDiv, X(0), dF(1), w)
	t.ins(d3d.OpMovc, X(0), XV(3), dF(1e6), XV(0))
	t.ins(d3d.OpMul, X(1), p.C(2), XV(0))
	t.ins(d3d.OpMad, d3d.Out(dxPosReg).Mask(1), XV(1), sc.C(2), of.C(2))
	t.ins(d3d.OpMov, d3d.Out(dxPosReg).Mask(6), dF(0, 0, 0, 0))
	t.ins(d3d.OpMov, d3d.Out(dxPosReg).Mask(8), XV(0))
}
