package ps3

// Translating RSX fragment programs to Direct3D pixel shaders (shader model
// 5 bytecode, built with the Wii U core's assembler), so draws can run on
// the host GPU. The translation does what fpProg.run does, instruction by
// instruction; rsx_dxbc_test.go checks the two agree.
//
// Constants that RSX programs carry inline (games patch them between draws)
// become constant-buffer reads, so a program whose constants change keeps
// its shader. cb0 holds:
//
//	[0]          x: alpha test reference
//	[1+6u]       texture unit u: coordinate scale (x, y)
//	[2+6u..5+6u] rows mapping the host texel (r, g, b, a) to the RSX one
//	[6+6u]       added after the rows (remapped constant channels)
//	[97+k]       the program's constant k
//
// The vertex stage is a pass-through (vertices are transformed on the CPU,
// see gpuVSCode): register k (1..15) is vertex program output k, and
// register 16 holds window z and 1/w for the fragment position.

import (
	"fmt"
	"math"

	"retropixels/wiiu/d3d"
)

const (
	dxFPConst   = 97 // cb0 index of constant 0
	dxFPTexBase = 1
	dxFPTexSize = 6

	dxH     = 48  // H registers follow R0..R47
	dxCC    = 96  // CC0, CC1
	dxD     = 98  // an instruction's result
	dxS0    = 99  // sources, S0..S2
	dxT     = 102 // scratch 102..105
	dxCond  = 106
	dxWPos  = 107
	dxCnt   = 108 // loop counters, one per depth
	dxTemps = dxCnt + 8

	dxPosReg = 16 // the extra vertex output: (window z, 0, 0, 1/w)
)

// fpKey is what a translated pixel shader depends on besides the program.
type fpKey struct {
	shape    uint64
	out32    bool
	fixedOut bool   // the target holds 0..1 (alpha is clamped for the test)
	alphaFn  uint32 // 0 = no alpha test
	mrt      uint8  // more colour targets (R2/H4, R3/H6, R4/H8 to SV_Target1..3)
}

type fpTr struct {
	s     *d3d.Shader
	p     *fpProg
	k     fpKey
	depth int // loop nesting
	err   error
}

func dR(i int) d3d.Opnd                      { return d3d.R(i) }
func dRC(i int, c uint32) d3d.Opnd           { return d3d.R(i).C(c) }
func dW(i int, c uint32) d3d.Opnd            { return d3d.R(i).Mask(1 << c) }
func dF(v ...float32) d3d.Opnd               { return d3d.ImmF(v...) }
func dU(v ...uint32) d3d.Opnd                { return d3d.ImmU(v...) }
func (t *fpTr) ins(op uint32, o ...d3d.Opnd) { t.s.Ins(op, o...) }

// translateFP makes the pixel shader for p.
func translateFP(p *fpProg, k fpKey) (code []byte, err error) {
	t := &fpTr{s: &d3d.Shader{Pixel: true}, p: p, k: k}
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("fragment program: %v", r)
		}
	}()
	t.body()
	if t.err != nil {
		return nil, t.err
	}
	t.declare()
	return t.s.Bytes(), nil
}

func (t *fpTr) declare() {
	s := t.s
	body := s.TakeBody()
	// every register starts at 0, as the interpreter's do
	for i := 0; i < dxD; i++ {
		s.Ins(d3d.OpMov, dR(i).Mask(15), dF(0, 0, 0, 0))
	}
	if t.p.inputs&1 != 0 {
		s.Ins(d3d.OpMov, dR(dxWPos).Mask(3), d3d.In(0))
		s.Ins(d3d.OpMov, dR(dxWPos).Mask(4), d3d.In(dxPosReg).C(0))
		s.Ins(d3d.OpMov, dR(dxWPos).Mask(8), d3d.In(dxPosReg).C(3))
	}
	s.AppendBody(body)
	s.Temps(dxTemps)
	s.DclCB(0, dxFPConst+max(t.p.consts, 1), false)
	for u := 0; u < 16; u++ {
		if t.p.usesTex>>u&1 != 0 {
			s.DclSampler(u, false)
		}
	}
	for u := 0; u < 16; u++ {
		if t.p.usesTex>>u&1 != 0 {
			s.DclResource(u, d3d.DimTex2D, d3d.RetFloat)
		}
	}
	s.In = gpuPSInSig()
	if t.p.inputs&1 != 0 {
		s.DclInputPSSV(0, 15, d3d.InterpNoPerspective, d3d.SVPosition)
	}
	for in := 1; in < 16; in++ {
		if t.p.inputs>>in&1 != 0 && fpInputToVP[in] > 0 {
			s.DclInputPS(fpInputToVP[in], 15, d3d.InterpLinear)
		}
	}
	if t.p.inputs&1 != 0 {
		s.DclInputPS(dxPosReg, 15, d3d.InterpNoPerspective)
	}
	s.Out = []d3d.Sig{{Name: "SV_Target", SV: d3d.SVTarget, Mask: 15}}
	s.DclOutput(0, 15)
	for k := 1; k <= int(t.k.mrt); k++ {
		s.Out = append(s.Out, d3d.Sig{Name: "SV_Target", Index: uint32(k), Reg: uint32(k), SV: d3d.SVTarget, Mask: 15})
		s.DclOutput(k, 15)
	}
}

// gpuPSInSig: the position, then outputs 1..15 and the extra register.
func gpuPSInSig() []d3d.Sig {
	sig := []d3d.Sig{{Name: "SV_Position", SV: d3d.SVPosition, Mask: 15}}
	for r := 1; r <= dxPosReg; r++ {
		sig = append(sig, d3d.Sig{Name: "TEXCOORD", Index: uint32(r), Reg: uint32(r), Mask: 15})
	}
	return sig
}

// gpuVSCode is the vertex shader: vertex i's 17 registers are elements
// 17i.. of buffer t0 (register 0 the position in Direct3D's clip space).
func gpuVSCode() []byte {
	s := &d3d.Shader{}
	s.In = []d3d.Sig{{Name: "SV_VertexID", SV: d3d.SVVertexID, CompType: 1, Mask: 1}}
	s.DclResource(0, d3d.DimBuffer, d3d.RetFloat)
	s.DclInputSV(0, 1, d3d.SVVertexID)
	s.Out = gpuPSInSig()
	s.DclOutputSV(0, 15, d3d.SVPosition)
	for r := 1; r <= dxPosReg; r++ {
		s.DclOutput(r, 15)
	}
	s.Ins(d3d.OpImad, dW(0, 0), d3d.In(0).C(0), dU(dxPosReg+1), dU(0))
	for r := 0; r <= dxPosReg; r++ {
		s.Ins(d3d.OpIadd, dW(1, 0), dRC(0, 0), dU(uint32(r)))
		s.Ins(d3d.OpLd, d3d.Out(r).Mask(15), dRC(1, 0), d3d.Tex(0))
	}
	s.Ins(d3d.OpRet)
	s.Temps(2)
	return s.Bytes()
}

// src gives source n of in, read into S<n> (swizzled and modified).
func (t *fpTr) src(in *fpInst, n int) d3d.Opnd {
	s := &in.src[n]
	var o d3d.Opnd
	switch s.typ {
	case 0:
		if s.half {
			o = dR(dxH + int(s.tmp))
		} else {
			o = dR(int(s.tmp))
		}
	case 1:
		o = t.input(in.input)
	case 2:
		if in.cidx < 0 {
			o = dF(0, 0, 0, 0)
		} else {
			o = d3d.CB(0, dxFPConst+in.cidx)
		}
	default:
		o = dF(0, 0, 0, 0)
	}
	o = o.Swz(uint32(s.swz[0]), uint32(s.swz[1]), uint32(s.swz[2]), uint32(s.swz[3]))
	if s.abs {
		o = o.Abs()
	}
	r := dxS0 + n
	switch s.prec {
	case 4:
		t.ins(d3d.OpMov|d3d.Sat, dR(r).Mask(15), o)
	case 2, 3:
		lim := float32(2)
		if s.prec == 3 {
			lim = 1
		}
		t.ins(d3d.OpMov, dR(r).Mask(15), o)
		t.ins(d3d.OpEq, dR(dxCond).Mask(15), dR(r), dR(r)) // NaN becomes 0
		t.ins(d3d.OpMax, dR(r).Mask(15), dR(r), dF(-lim))
		t.ins(d3d.OpMin, dR(r).Mask(15), dR(r), dF(lim))
		t.ins(d3d.OpAnd, dR(r).Mask(15), dR(r), dR(dxCond))
	default:
		t.ins(d3d.OpMov, dR(r).Mask(15), o)
	}
	if s.neg {
		return dR(r).Neg()
	}
	return dR(r)
}

func (t *fpTr) input(n uint8) d3d.Opnd {
	switch {
	case n == 0:
		return dR(dxWPos)
	case n == 14:
		return dF(1, 1, 1, 1)
	case fpInputToVP[n] > 0:
		return d3d.In(fpInputToVP[n])
	}
	return dF(0, 0, 0, 0)
}

// cond sets dxCond.x (all bits) when in's condition holds on any
// component, as condOK does.
func (t *fpTr) cond(in *fpInst) {
	if in.cond == 7 {
		t.ins(d3d.OpMov, dW(dxCond, 0), dU(0xffffffff))
		return
	}
	t.condMask(in, dxCond)
	t.ins(d3d.OpOr, dW(dxCond, 0), dRC(dxCond, 0), dRC(dxCond, 1))
	t.ins(d3d.OpOr, dW(dxCond, 0), dRC(dxCond, 0), dRC(dxCond, 2))
	t.ins(d3d.OpOr, dW(dxCond, 0), dRC(dxCond, 0), dRC(dxCond, 3))
}

// condMask puts testCond's per-component result in register r.
func (t *fpTr) condMask(in *fpInst, r int) {
	c := dR(dxCC+int(in.condReg&1)).Swz(uint32(in.condSwz[0]), uint32(in.condSwz[1]), uint32(in.condSwz[2]), uint32(in.condSwz[3]))
	z := dF(0, 0, 0, 0)
	switch in.cond {
	case 1:
		t.ins(d3d.OpLt, dR(r).Mask(15), c, z)
	case 2:
		t.ins(d3d.OpEq, dR(r).Mask(15), c, z)
	case 3:
		t.ins(d3d.OpGe, dR(r).Mask(15), z, c)
	case 4:
		t.ins(d3d.OpLt, dR(r).Mask(15), z, c)
	case 5:
		t.ins(d3d.OpNe, dR(r).Mask(15), c, z)
	case 6:
		t.ins(d3d.OpGe, dR(r).Mask(15), c, z)
	case 7:
		t.ins(d3d.OpMov, dR(r).Mask(15), dU(0xffffffff))
	default:
		t.ins(d3d.OpMov, dR(r).Mask(15), dU(0))
	}
}

// block markers: what to emit before instruction i
type fpBlock struct {
	loop          bool
	elseAt, endAt int
	elseDone      bool
	ret           bool
}

func (t *fpTr) body() {
	p := t.p
	var stack []fpBlock
	close := func(i int) {
		for len(stack) > 0 {
			b := &stack[len(stack)-1]
			if !b.loop && !b.elseDone && b.elseAt == i && b.elseAt < b.endAt {
				t.ins(d3d.OpElse)
				b.elseDone = true
				continue
			}
			if b.endAt != i {
				return
			}
			if b.loop {
				t.ins(d3d.OpIadd, dW(dxCnt+t.depth-1, 0), dRC(dxCnt+t.depth-1, 0), dU(0xffffffff))
				t.ins(d3d.OpEndloop)
				t.ins(d3d.OpEndif)
				t.depth--
			} else {
				t.ins(d3d.OpEndif)
			}
			stack = stack[:len(stack)-1]
		}
	}
	for i := range p.inst {
		close(i)
		in := &p.inst[i]
		if in.flow {
			switch in.op {
			case 0x42: // IFE
				t.cond(in)
				t.ins(d3d.OpIf|d3d.NonZero, dRC(dxCond, 0))
				end := in.endAt
				if end <= i {
					end = len(p.inst)
				}
				stack = append(stack, fpBlock{elseAt: in.elseAt, endAt: end})
			case 0x43, 0x44: // LOOP, REP
				n := 0
				if in.loopEnd > in.loopStart {
					n = (in.loopEnd-in.loopStart-1)/max(in.loopInc, 1) + 1
				}
				t.cond(in)
				if n == 0 {
					t.ins(d3d.OpMov, dW(dxCond, 0), dU(0))
				}
				t.ins(d3d.OpIf|d3d.NonZero, dRC(dxCond, 0))
				if t.depth >= 8 {
					panic("loops nested too deep")
				}
				t.depth++
				cnt := dxCnt + t.depth - 1
				t.ins(d3d.OpMov, dW(cnt, 0), dU(uint32(n)))
				t.ins(d3d.OpLoop)
				t.ins(d3d.OpBreakc, dRC(cnt, 0)) // break when it reaches 0
				end := in.endAt
				if end <= i {
					end = len(p.inst)
				}
				stack = append(stack, fpBlock{loop: true, endAt: end})
			case 0x40: // BRK
				if t.depth > 0 {
					t.cond(in)
					t.ins(d3d.OpBreakc|d3d.NonZero, dRC(dxCond, 0))
				}
			case 0x45: // RET
				t.cond(in)
				t.ins(d3d.OpIf|d3d.NonZero, dRC(dxCond, 0))
				t.output()
				t.ins(d3d.OpRet)
				t.ins(d3d.OpEndif)
			}
			if in.end {
				break
			}
			continue
		}
		switch in.op {
		case 0x00, 0x3d, 0x3e:
		case 0x12: // KIL
			t.cond(in)
			t.ins(d3d.OpDiscard|d3d.NonZero, dRC(dxCond, 0))
		default:
			t.op(in)
		}
		if in.end {
			break
		}
	}
	close(len(p.inst))
	for len(stack) > 0 { // unclosed blocks
		b := stack[len(stack)-1]
		if b.loop {
			t.ins(d3d.OpIadd, dW(dxCnt+t.depth-1, 0), dRC(dxCnt+t.depth-1, 0), dU(0xffffffff))
			t.ins(d3d.OpEndloop)
			t.depth--
		}
		t.ins(d3d.OpEndif)
		stack = stack[:len(stack)-1]
	}
	t.output()
	t.ins(d3d.OpRet)
}

// output writes the colour (and does the alpha test).
func (t *fpTr) output() {
	col := dR(0)
	if !t.k.out32 {
		col = dR(dxH)
	}
	if t.k.alphaFn != 0 && t.k.alphaFn != 0x207 {
		a := dW(dxT, 0)
		if t.k.fixedOut {
			t.ins(d3d.OpMov|d3d.Sat, a, col.C(3))
		} else {
			t.ins(d3d.OpMov, a, col.C(3))
		}
		av, ref := dRC(dxT, 0), d3d.CB(0, 0).C(0)
		m := dW(dxT, 1)
		mv := dRC(dxT, 1)
		kill := true // discard when the comparison is true
		switch t.k.alphaFn {
		case 0x200: // never
			t.ins(d3d.OpMov, m, dU(0xffffffff))
		case 0x201: // less: pass when a < ref
			t.ins(d3d.OpLt, m, av, ref)
			kill = false
		case 0x202:
			t.ins(d3d.OpEq, m, av, ref)
			kill = false
		case 0x203:
			t.ins(d3d.OpGe, m, ref, av)
			kill = false
		case 0x204:
			t.ins(d3d.OpLt, m, ref, av)
			kill = false
		case 0x205:
			t.ins(d3d.OpNe, m, av, ref)
			kill = false
		case 0x206:
			t.ins(d3d.OpGe, m, av, ref)
			kill = false
		default:
			t.ins(d3d.OpMov, m, dU(0))
		}
		if kill {
			t.ins(d3d.OpDiscard|d3d.NonZero, mv)
		} else {
			t.ins(d3d.OpDiscard, mv)
		}
	}
	t.ins(d3d.OpMov, d3d.Out(0).Mask(15), col)
	for k := 1; k <= int(t.k.mrt); k++ {
		c := dR(1 + k) // R2, R3, R4
		if !t.k.out32 {
			c = dR(dxH + 2 + 2*k) // H4, H6, H8
		}
		if t.k.fixedOut {
			t.ins(d3d.OpMov|d3d.Sat, d3d.Out(k).Mask(15), c)
		} else {
			t.ins(d3d.OpMov, d3d.Out(k).Mask(15), c)
		}
	}
}

// op translates one arithmetic or texture instruction.
func (t *fpTr) op(in *fpInst) {
	D := dR(dxD)
	T := func(c uint32) d3d.Opnd { return dRC(dxT, c) }
	TW := func(m uint32) d3d.Opnd { return dR(dxT).Mask(m) }
	n := fpReads(in.op)
	var s [3]d3d.Opnd
	for k := 0; k < n && k < 3; k++ {
		s[k] = t.src(in, k)
	}
	s0, s1, s2 := s[0], s[1], s[2]
	S := func(k int, c uint32) d3d.Opnd { return s[k].C(c) }
	one := dF(1, 1, 1, 1)
	switch in.op {
	case 0x01:
		t.ins(d3d.OpMov, D.Mask(15), s0)
	case 0x02:
		t.ins(d3d.OpMul, D.Mask(15), s0, s1)
	case 0x03:
		t.ins(d3d.OpAdd, D.Mask(15), s0, s1)
	case 0x04:
		t.ins(d3d.OpMad, D.Mask(15), s0, s1, s2)
	case 0x05:
		t.ins(d3d.OpDp3, D.Mask(15), s0, s1)
	case 0x06:
		t.ins(d3d.OpDp4, D.Mask(15), s0, s1)
	case 0x38:
		t.ins(d3d.OpDp2, D.Mask(15), s0, s1)
	case 0x07: // DST
		t.ins(d3d.OpMov, D.Mask(15), one)
		t.ins(d3d.OpMul, dW(dxD, 1), S(0, 1), S(1, 1))
		t.ins(d3d.OpMov, dW(dxD, 2), S(0, 2))
		t.ins(d3d.OpMov, dW(dxD, 3), S(1, 3))
	case 0x08:
		t.ins(d3d.OpMin, D.Mask(15), s0, s1)
	case 0x09:
		t.ins(d3d.OpMax, D.Mask(15), s0, s1)
	case 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f:
		switch in.op {
		case 0x0a:
			t.ins(d3d.OpLt, D.Mask(15), s0, s1)
		case 0x0b:
			t.ins(d3d.OpGe, D.Mask(15), s0, s1)
		case 0x0c:
			t.ins(d3d.OpGe, D.Mask(15), s1, s0)
		case 0x0d:
			t.ins(d3d.OpLt, D.Mask(15), s1, s0)
		case 0x0e:
			t.ins(d3d.OpNe, D.Mask(15), s0, s1)
		case 0x0f:
			t.ins(d3d.OpEq, D.Mask(15), s0, s1)
		}
		t.ins(d3d.OpAnd, D.Mask(15), D, dU(0x3f800000))
	case 0x10:
		t.ins(d3d.OpFrc, D.Mask(15), s0)
	case 0x11:
		t.ins(d3d.OpRoundNi, D.Mask(15), s0)
	case 0x15:
		t.ins(d3d.OpDerivRtx, D.Mask(15), s0)
	case 0x16:
		t.ins(d3d.OpDerivRty, D.Mask(15), s0)
	case 0x1a: // RCP
		t.ins(d3d.OpDiv, D.Mask(15), one, S(0, 0))
	case 0x1b: // RSQ
		t.ins(d3d.OpRsq, D.Mask(15), S(0, 0).Abs())
	case 0x1c:
		t.ins(d3d.OpExp, D.Mask(15), S(0, 0))
	case 0x1d:
		t.ins(d3d.OpLog, D.Mask(15), S(0, 0))
	case 0x1e: // LIT
		t.ins(d3d.OpMax, TW(8), S(0, 3), dF(-127.9961))
		t.ins(d3d.OpMin, TW(8), T(3), dF(127.9961))
		t.ins(d3d.OpLog, TW(4), S(0, 1))
		t.ins(d3d.OpMul, TW(4), T(2), T(3))
		t.ins(d3d.OpExp, TW(4), T(2))
		t.ins(d3d.OpLt, TW(1), dF(0), S(0, 0))
		t.ins(d3d.OpLt, TW(2), dF(0), S(0, 1))
		t.ins(d3d.OpAnd, TW(2), T(1), T(0))
		t.ins(d3d.OpMov, D.Mask(15), one)
		t.ins(d3d.OpAnd, dW(dxD, 1), S(0, 0), T(0))
		t.ins(d3d.OpAnd, dW(dxD, 2), T(2), T(1))
	case 0x3c: // LIF
		t.ins(d3d.OpExp, TW(1), S(0, 3))
		t.ins(d3d.OpLt, TW(2), dF(0), S(0, 1))
		t.ins(d3d.OpMov, D.Mask(15), one)
		t.ins(d3d.OpMov, dW(dxD, 1), S(0, 1))
		t.ins(d3d.OpAnd, dW(dxD, 2), T(0), T(1))
	case 0x1f: // LRP: s0*s1 + (1-s0)*s2
		t.ins(d3d.OpAdd, dR(dxT).Mask(15), one, s0.Neg())
		t.ins(d3d.OpMul, dR(dxT).Mask(15), dR(dxT), s2)
		t.ins(d3d.OpMad, D.Mask(15), s0, s1, dR(dxT))
	case 0x20:
		t.ins(d3d.OpMov, D.Mask(15), one)
	case 0x21:
		t.ins(d3d.OpMov, D.Mask(15), dF(0, 0, 0, 0))
	case 0x22: // COS
		t.ins(d3d.OpSincos, d3d.Null(), D, S(0, 0))
	case 0x23: // SIN
		t.ins(d3d.OpSincos, D.Mask(15), d3d.Null(), S(0, 0))
	case 0x39: // NRM
		t.ins(d3d.OpDp3, TW(1), s0, s0)
		t.ins(d3d.OpNe, TW(2), T(0), dF(0))
		t.ins(d3d.OpRsq, TW(1), T(0))
		t.ins(d3d.OpMul, D.Mask(15), s0.Swz(0, 1, 2, 2), T(0))
		t.ins(d3d.OpMovc, D.Mask(15), T(1), D, dF(0, 0, 0, 0))
	case 0x26: // POW
		t.ins(d3d.OpLog, TW(1), S(0, 0))
		t.ins(d3d.OpMul, TW(1), T(0), S(1, 0))
		t.ins(d3d.OpExp, D.Mask(15), T(0))
	case 0x3a: // DIV
		t.ins(d3d.OpDiv, D.Mask(15), s0, S(1, 0))
	case 0x3b: // DIVSQ
		t.ins(d3d.OpRsq, TW(1), S(1, 0).Abs())
		t.ins(d3d.OpMul, D.Mask(15), s0, T(0))
	case 0x36: // REFL
		t.ins(d3d.OpDp4, TW(1), s0, s1)
		t.ins(d3d.OpAdd, TW(1), T(0), T(0))
		t.ins(d3d.OpMad, D.Mask(15), T(0).Neg(), s1, s0)
	case 0x2e: // DP2A
		t.ins(d3d.OpDp2, TW(1), s0, s1)
		t.ins(d3d.OpAdd, D.Mask(15), T(0), S(2, 0))
	case 0x24: // PK2
		t.ins(d3d.OpF32tof16, TW(3), s0)
		t.ins(d3d.OpIshl, TW(2), T(1), dU(16))
		t.ins(d3d.OpOr, TW(1), T(0), T(1))
		t.ins(d3d.OpMov, D.Mask(15), T(0))
	case 0x25: // UP2
		t.ins(d3d.OpAnd, TW(1), S(0, 0), dU(0xffff))
		t.ins(d3d.OpUshr, TW(2), S(0, 0), dU(16))
		t.ins(d3d.OpF16tof32, D.Mask(15), dR(dxT).Swz(0, 1, 0, 1))
	case 0x13: // PK4
		t.ins(d3d.OpMax, dR(dxT).Mask(15), s0, dF(-1))
		t.ins(d3d.OpMin, dR(dxT).Mask(15), dR(dxT), dF(1))
		t.ins(d3d.OpMul, dR(dxT).Mask(15), dR(dxT), dF(127))
		t.ins(d3d.OpRoundNe, dR(dxT).Mask(15), dR(dxT))
		t.ins(d3d.OpFtoi, dR(dxT).Mask(15), dR(dxT))
		t.ins(d3d.OpAnd, dR(dxT).Mask(15), dR(dxT), dU(0xff))
		t.pack4()
	case 0x14: // UP4
		t.ins(d3d.OpIbfe, dR(dxT).Mask(15), dU(8, 8, 8, 8), dU(0, 8, 16, 24), S(0, 0))
		t.ins(d3d.OpItof, dR(dxT).Mask(15), dR(dxT))
		t.ins(d3d.OpMul, dR(dxT).Mask(15), dR(dxT), dF(1.0/127))
		t.ins(d3d.OpMax, D.Mask(15), dR(dxT), dF(-1))
	case 0x27, 0x2c: // PKB, PKG
		t.ins(d3d.OpMov|d3d.Sat, dR(dxT).Mask(15), s0)
		t.ins(d3d.OpMul, dR(dxT).Mask(15), dR(dxT), dF(255))
		t.ins(d3d.OpRoundNe, dR(dxT).Mask(15), dR(dxT))
		t.ins(d3d.OpFtou, dR(dxT).Mask(15), dR(dxT))
		t.pack4()
	case 0x28, 0x2d: // UPB, UPG
		t.ins(d3d.OpUbfe, dR(dxT).Mask(15), dU(8, 8, 8, 8), dU(0, 8, 16, 24), S(0, 0))
		t.ins(d3d.OpUtof, dR(dxT).Mask(15), dR(dxT))
		t.ins(d3d.OpMul, D.Mask(15), dR(dxT), dF(1.0/255))
	case 0x29: // PK16
		t.ins(d3d.OpMov|d3d.Sat, TW(3), s0)
		t.ins(d3d.OpMul, TW(3), dR(dxT), dF(65535))
		t.ins(d3d.OpRoundNe, TW(3), dR(dxT))
		t.ins(d3d.OpFtou, TW(3), dR(dxT))
		t.ins(d3d.OpIshl, TW(2), T(1), dU(16))
		t.ins(d3d.OpOr, TW(1), T(0), T(1))
		t.ins(d3d.OpMov, D.Mask(15), T(0))
	case 0x2a: // UP16
		t.ins(d3d.OpAnd, TW(1), S(0, 0), dU(0xffff))
		t.ins(d3d.OpUshr, TW(2), S(0, 0), dU(16))
		t.ins(d3d.OpUtof, TW(3), dR(dxT))
		t.ins(d3d.OpMul, TW(3), dR(dxT), dF(1.0/65535))
		t.ins(d3d.OpMov, D.Mask(15), dR(dxT).Swz(0, 1, 0, 1))
	case 0x17, 0x18, 0x19, 0x2f, 0x31: // TEX, TXP, TXD, TXL, TXB
		t.tex(in, s0)
	default:
		t.ins(d3d.OpMov, D.Mask(15), dF(0, 0, 0, 0))
	}
	t.write(in)
}

// pack4 ORs T's four bytes (x lowest) into every component of D.
func (t *fpTr) pack4() {
	t.ins(d3d.OpIshl, dR(dxT).Mask(15), dR(dxT), dU(0, 8, 16, 24))
	t.ins(d3d.OpOr, dW(dxT, 0), dRC(dxT, 0), dRC(dxT, 1))
	t.ins(d3d.OpOr, dW(dxT, 0), dRC(dxT, 0), dRC(dxT, 2))
	t.ins(d3d.OpOr, dW(dxT, 0), dRC(dxT, 0), dRC(dxT, 3))
	t.ins(d3d.OpMov, dR(dxD).Mask(15), dRC(dxT, 0))
}

func (t *fpTr) tex(in *fpInst, c d3d.Opnd) {
	u := int(in.tex & 15)
	C := dR(dxT + 1)
	t.ins(d3d.OpMov, C.Mask(15), c)
	if in.op == 0x18 { // TXP: divide by w unless it's 0
		t.ins(d3d.OpNe, dW(dxT+2, 0), dRC(dxT+1, 3), dF(0))
		t.ins(d3d.OpDiv, dR(dxT+3).Mask(15), C, dRC(dxT+1, 3))
		t.ins(d3d.OpMovc, C.Mask(7), dRC(dxT+2, 0), dR(dxT+3), C)
	}
	base := dxFPTexBase + dxFPTexSize*u
	t.ins(d3d.OpMul, C.Mask(3), C, d3d.CB(0, base))
	if t.p.usesTex>>u&1 == 0 {
		t.ins(d3d.OpMov, dR(dxT).Mask(15), dF(0, 0, 0, 0))
	} else {
		t.ins(d3d.OpSampleL, dR(dxT).Mask(15), C.Swz(0, 1, 0, 0), d3d.Tex(u), d3d.Samp(u), dF(0))
	}
	for ch := 0; ch < 4; ch++ {
		t.ins(d3d.OpDp4, dW(dxD, uint32(ch)), dR(dxT), d3d.CB(0, base+1+ch))
	}
	t.ins(d3d.OpAdd, dR(dxD).Mask(15), dR(dxD), d3d.CB(0, base+5))
	if in.bx2 && (in.op == 0x17 || in.op == 0x18) {
		t.ins(d3d.OpMad, dR(dxD).Mask(15), dR(dxD), dF(2), dF(-1))
	}
}

// write stores D the way fpRun.write does.
func (t *fpTr) write(in *fpInst) {
	D := dR(dxD)
	if in.scale != 1 {
		t.ins(d3d.OpMul, D.Mask(15), D, dF(in.scale))
	}
	switch {
	case in.sat:
		t.ins(d3d.OpMov|d3d.Sat, D.Mask(15), D)
	case in.prec == 2 || in.prec == 3:
		lim := float32(2)
		if in.prec == 3 {
			lim = 1
		}
		t.ins(d3d.OpEq, dR(dxT).Mask(15), D, D)
		t.ins(d3d.OpMax, D.Mask(15), D, dF(-lim))
		t.ins(d3d.OpMin, D.Mask(15), D, dF(lim))
		t.ins(d3d.OpAnd, D.Mask(15), D, dR(dxT))
	case in.prec == 1:
		t.halfClamp()
	}
	if in.dstHalf {
		t.halfClamp()
	}
	var mask uint32
	for i, b := range in.mask {
		if b {
			mask |= 1 << i
		}
	}
	if mask == 0 {
		return
	}
	cond := in.cond != 7
	if cond {
		t.condMask(in, dxCond)
	}
	put := func(r int) {
		if cond {
			t.ins(d3d.OpMovc, dR(r).Mask(mask), dR(dxCond), D, dR(r))
		} else {
			t.ins(d3d.OpMov, dR(r).Mask(mask), D)
		}
	}
	if in.setCond {
		put(dxCC + int(in.condSet&1))
	}
	if in.noDest {
		return
	}
	if in.dstHalf {
		put(dxH + int(in.dst&47))
	} else {
		put(int(in.dst & 47))
	}
}

// halfClamp keeps D in the half range (NaN becomes 0), like fpClamp(1).
func (t *fpTr) halfClamp() {
	D := dR(dxD)
	t.ins(d3d.OpEq, dR(dxT).Mask(15), D, D) // false for NaN
	t.ins(d3d.OpMax, D.Mask(15), D, dF(-65504))
	t.ins(d3d.OpMin, D.Mask(15), D, dF(65504))
	t.ins(d3d.OpAnd, D.Mask(15), D, dR(dxT))
}

// fpTexParams fills cb0's block for unit u from the texture as uploaded:
// the host texel is (r, g, b, a) in hostOrder's RSX channels.
func fpConstBlock(p *fpProg, alphaRef float32, tex *[16]gpuTexParams) []float32 {
	cb := make([]float32, 4*(dxFPConst+max(p.consts, 1)))
	cb[0] = alphaRef
	for u := 0; u < 16; u++ {
		if p.usesTex>>u&1 == 0 {
			continue
		}
		tp := &tex[u]
		b := 4 * (dxFPTexBase + dxFPTexSize*u)
		copy(cb[b:], tp.scale[:])
		for r := 0; r < 4; r++ {
			copy(cb[b+4+4*r:], tp.rows[r][:])
		}
		copy(cb[b+20:], tp.add[:])
	}
	for i := range p.inst {
		in := &p.inst[i]
		if in.cidx >= 0 {
			copy(cb[4*(dxFPConst+in.cidx):], in.constant[:])
		}
	}
	return cb
}

// gpuTexParams is how a shader reads one texture unit.
type gpuTexParams struct {
	scale [4]float32
	rows  [4][4]float32 // RSX R, G, B, A from the host texel (r, g, b, a)
	add   [4]float32
}

// remapParams builds the rows for a texture whose host texel holds the
// RSX channels A, R, G, B (index 0..3, as rsxTexture.texel returns them)
// at host positions pos[ch] (-1: the channel isn't stored; it reads as
// fill[ch]), then applies the texture's remap register.
func remapParams(pos [4]int, fill [4]float32, remap uint32) (rows [4][4]float32, add [4]float32) {
	// argb[ch] = host[pos[ch]] or fill[ch]
	type lin struct {
		w [4]float32
		c float32
	}
	var argb [4]lin
	for ch := 0; ch < 4; ch++ {
		if pos[ch] >= 0 {
			argb[ch].w[pos[ch]] = 1
		} else {
			argb[ch].c = fill[ch]
		}
	}
	out := argb
	if remap != 0xAAE4 && remap != 0 {
		for ch := 0; ch < 4; ch++ {
			switch remap >> (8 + 2*ch) & 3 {
			case 0:
				out[ch] = lin{}
			case 1:
				out[ch] = lin{c: 1}
			default:
				out[ch] = argb[remap>>(2*ch)&3]
			}
		}
	}
	// RSX result order R, G, B, A = argb 1, 2, 3, 0
	for i, ch := range [4]int{1, 2, 3, 0} {
		rows[i] = out[ch].w
		add[i] = out[ch].c
	}
	return
}

var _ = math.Inf
