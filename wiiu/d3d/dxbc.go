package d3d

// An assembler for Direct3D shader bytecode (the DXBC container with a
// shader model 5 program in it). The Wii U renderer translates the GPU's
// own shader code straight into this, so it needs no HLSL compiler.
//
// The token layout follows Microsoft's published description of the
// format (d3d11TokenizedProgramFormat.hpp in the Windows SDK).

import (
	"encoding/binary"
	"math"
	"math/bits"
)

// opcodes
const (
	OpAdd       = 0
	OpAnd       = 1
	OpBreak     = 2
	OpBreakc    = 3
	OpContinue  = 7
	OpCut       = 9
	OpContinuec = 8
	OpDerivRtx  = 11
	OpDerivRty  = 12
	OpDiscard   = 13
	OpDiv       = 14
	OpDp2       = 15
	OpDp3       = 16
	OpDp4       = 17
	OpElse      = 18
	OpEmit      = 19
	OpEndif     = 21
	OpEndloop   = 22
	OpEq        = 24
	OpExp       = 25
	OpFrc       = 26
	OpFtoi      = 27
	OpFtou      = 28
	OpGe        = 29
	OpIadd      = 30
	OpIf        = 31
	OpIeq       = 32
	OpIge       = 33
	OpIlt       = 34
	OpImad      = 35
	OpImax      = 36
	OpImin      = 37
	OpImul      = 38
	OpIne       = 39
	OpIneg      = 40
	OpIshl      = 41
	OpIshr      = 42
	OpItof      = 43
	OpLd        = 45
	OpLog       = 47
	OpLoop      = 48
	OpLt        = 49
	OpMad       = 50
	OpMin       = 51
	OpMax       = 52
	OpMov       = 54
	OpMovc      = 55
	OpMul       = 56
	OpNe        = 57
	OpNot       = 59
	OpOr        = 60
	OpResinfo   = 61
	OpRet       = 62
	OpRoundNe   = 64
	OpRoundNi   = 65
	OpRoundPi   = 66
	OpRoundZ    = 67
	OpRsq       = 68
	OpSample    = 69
	OpSampleC   = 70
	OpSampleCLz = 71
	OpSampleL   = 72
	OpSampleD   = 73
	OpSampleB   = 74
	OpSqrt      = 75
	OpSincos    = 77
	OpUdiv      = 78
	OpUlt       = 79
	OpUge       = 80
	OpUmul      = 81
	OpUmax      = 83
	OpUmin      = 84
	OpUshr      = 85
	OpUtof      = 86
	OpXor       = 87

	opDclResource     = 88
	opDclConstBuffer  = 89
	opDclSampler      = 90
	opDclGSOutTopo    = 92
	opDclGSInPrim     = 93
	opDclMaxOutVerts  = 94
	opDclInput        = 95
	opDclInputSgv     = 96
	opDclInputSiv     = 97
	opDclInputPs      = 98
	opDclInputPsSgv   = 99
	opDclInputPsSiv   = 100
	opDclOutput       = 101
	opDclOutputSiv    = 103
	opDclTemps        = 104
	opDclIndexableTmp = 105
	opDclGlobalFlags  = 106

	OpLod        = 108
	OpGather4    = 109
	OpRtxCoarse  = 122
	OpRtxFine    = 123
	OpRtyCoarse  = 124
	OpRtyFine    = 125
	OpRcp        = 129
	OpF32tof16   = 130
	OpF16tof32   = 131
	OpCountbits  = 134
	OpFirstbitHi = 135
	OpFirstbitLo = 136
	OpUbfe       = 138
	OpIbfe       = 139
	OpBfi        = 140
	OpBfrev      = 141
)

// instruction flags (OR them into the op)
const (
	Sat     = 1 << 13 // clamp the result to [0, 1]
	NonZero = 1 << 18 // for if, breakc, discard...: test for non-zero
)

// operand types
const (
	typeTemp      = 0
	typeInput     = 1
	typeOutput    = 2
	typeIndexable = 3
	typeImm32     = 4
	typeSampler   = 6
	typeResource  = 7
	typeCB        = 8
	typeNull      = 13
	typeDepth     = 12
)

// system values
const (
	SVNone      = 0
	SVPosition  = 1
	SVClip      = 2
	SVVertexID  = 6
	SVInstance  = 8
	SVFrontFace = 9
	SVTarget    = 64 // only in signatures
	SVDepth     = 65
)

// resource dimensions
const (
	DimBuffer     = 1
	DimTex1D      = 2
	DimTex2D      = 3
	DimTex3D      = 5
	DimTexCube    = 6
	DimTex2DArray = 8
)

// return types for dcl_resource
const (
	RetUnorm = 1
	RetSnorm = 2
	RetSint  = 3
	RetUint  = 4
	RetFloat = 5
)

// pixel shader input interpolation
const (
	InterpConstant      = 1
	InterpLinear        = 2
	InterpCentroid      = 3
	InterpNoPerspective = 4
)

// Index is one index of an operand: an immediate plus, optionally, a
// register (Rel, a one-component operand).
type Index struct {
	Imm uint32
	Rel *Opnd
}

// Opnd is an instruction operand.
type Opnd struct {
	typ   uint32
	n     int    // components: 0, 1 or 4
	mode  uint32 // 0 mask, 1 swizzle, 2 select one
	comp  uint32 // the mask, swizzle or component
	idx   []Index
	mod   uint32 // 1 neg, 2 abs, 3 both
	value []uint32
	raw   bool // value only, no operand token
}

func reg(typ uint32, i ...uint32) Opnd {
	o := Opnd{typ: typ, n: 4, mode: 1, comp: 0xe4}
	for _, v := range i {
		o.idx = append(o.idx, Index{Imm: v})
	}
	return o
}

func R(i int) Opnd          { return reg(typeTemp, uint32(i)) }
func In(i int) Opnd         { return reg(typeInput, uint32(i)) }
func InV(v, i int) Opnd     { return reg(typeInput, uint32(v), uint32(i)) } // a geometry shader's input: vertex v, register i
func Out(i int) Opnd        { return reg(typeOutput, uint32(i)) }
func CB(slot, i int) Opnd   { return reg(typeCB, uint32(slot), uint32(i)) }
func X(arr, i int) Opnd     { return reg(typeIndexable, uint32(arr), uint32(i)) }
func Tex(i int) Opnd        { return reg(typeResource, uint32(i)) }
func Null() Opnd            { return Opnd{typ: typeNull} }
func OutDepth() Opnd        { o := Opnd{typ: typeDepth, n: 1}; return o }
func Samp(i int) Opnd       { o := reg(typeSampler, uint32(i)); o.n = 0; return o }
func ImmU(v ...uint32) Opnd { return imm(v) }
func ImmI(v ...int32) Opnd {
	u := make([]uint32, len(v))
	for i, x := range v {
		u[i] = uint32(x)
	}
	return imm(u)
}
func ImmF(v ...float32) Opnd {
	u := make([]uint32, len(v))
	for i, x := range v {
		u[i] = math.Float32bits(x)
	}
	return imm(u)
}

func imm(v []uint32) Opnd {
	if len(v) == 1 {
		return Opnd{typ: typeImm32, n: 1, value: v}
	}
	for len(v) < 4 {
		v = append(v, v[len(v)-1])
	}
	return Opnd{typ: typeImm32, n: 4, value: v}
}

// RelIndex makes index k of the operand relative to reg (a one-component
// operand) plus its immediate.
func (o Opnd) RelIndex(k int, r Opnd) Opnd {
	o.idx = append([]Index(nil), o.idx...)
	rr := r
	o.idx[k].Rel = &rr
	return o
}

// Mask sets the write mask (bit 0 = x).
func (o Opnd) Mask(m uint32) Opnd { o.mode, o.comp = 0, m; return o }

// Swz sets a swizzle: four components, 0-3 each.
func (o Opnd) Swz(x, y, z, w uint32) Opnd {
	o.mode, o.comp = 1, x|y<<2|z<<4|w<<6
	return o
}

// C selects one component (as a swizzle of it, which every operand
// position accepts).
func (o Opnd) C(c uint32) Opnd { return o.Swz(c, c, c, c) }

// Sel1 is a single component select (for one-component index operands).
func (o Opnd) Sel1(c uint32) Opnd { o.mode, o.comp = 2, c; return o }

func (o Opnd) Neg() Opnd { o.mod ^= 1; return o }
func (o Opnd) Abs() Opnd { o.mod = 2; return o }

func (o Opnd) encode(out []uint32) []uint32 {
	if o.raw {
		return append(out, o.value...)
	}
	var t uint32
	switch o.n {
	case 1:
		t = 1
	case 4:
		t = 2 | o.mode<<2
		switch o.mode {
		case 0:
			t |= o.comp << 4
		case 1:
			t |= o.comp << 4
		case 2:
			t |= o.comp << 4
		}
	}
	t |= o.typ << 12
	t |= uint32(len(o.idx)) << 20
	for k, ix := range o.idx {
		rep := uint32(0)
		if ix.Rel != nil {
			rep = 2
			if ix.Imm != 0 {
				rep = 3
			}
		}
		t |= rep << (22 + 3*k)
	}
	if o.mod != 0 {
		t |= 1 << 31
	}
	out = append(out, t)
	if o.mod != 0 {
		out = append(out, 1|o.mod<<6)
	}
	for _, ix := range o.idx {
		if ix.Rel == nil || ix.Imm != 0 {
			out = append(out, ix.Imm)
		}
		if ix.Rel != nil {
			r := *ix.Rel
			if r.n == 4 && r.mode == 1 {
				r.mode, r.comp = 2, r.comp&3
			}
			out = r.encode(out)
		}
	}
	return append(out, o.value...)
}

// Sig is one element of an input or output signature.
type Sig struct {
	Name     string
	Index    uint32
	SV       uint32
	CompType uint32 // 1 uint, 2 int, 3 float
	Reg      uint32
	Mask     uint8
}

// Shader builds one program.
type Shader struct {
	Pixel    bool
	Geometry bool
	In, Out  []Sig
	decl     []uint32
	body     []uint32
	temps    int
}

// Ins emits an instruction: a destination (if any) and sources. Scalar
// immediates are widened when the destination has several components.
func (s *Shader) Ins(op uint32, ops ...Opnd) {
	if len(ops) > 1 && ops[0].n == 4 && ops[0].mode == 0 && bits.OnesCount32(ops[0].comp) > 1 && !scalarArgs(op&0x7ff) {
		ops = append([]Opnd(nil), ops...)
		for i := 1; i < len(ops); i++ {
			if ops[i].typ == typeImm32 && ops[i].n == 1 {
				v := ops[i].value[0]
				ops[i] = imm([]uint32{v, v, v, v})
			}
		}
	}
	s.body = instr(s.body, op, nil, ops...)
}

// scalarArgs: instructions with scalar operands after their vectors.
func scalarArgs(op uint32) bool {
	switch op {
	case OpSample, OpSampleB, OpSampleC, OpSampleCLz, OpSampleL, OpSampleD, OpLd, OpResinfo, OpLod, OpGather4:
		return true
	}
	return false
}

// InsTok emits an instruction with extra tokens before its operands (as
// sample offsets need) and no special handling.
func instr(out []uint32, op uint32, ext []uint32, ops ...Opnd) []uint32 {
	start := len(out)
	out = append(out, op)
	out = append(out, ext...)
	if len(ext) > 0 {
		out[start] |= 1 << 31
	}
	for _, o := range ops {
		out = o.encode(out)
	}
	out[start] |= uint32(len(out)-start) << 24
	return out
}

// InsOffset is a sample/ld with a texel offset (u, v, w in -8..7).
func (s *Shader) InsOffset(op uint32, u, v, w int32, ops ...Opnd) {
	if u == 0 && v == 0 && w == 0 {
		s.Ins(op, ops...)
		return
	}
	ext := uint32(1) | uint32(u&15)<<9 | uint32(v&15)<<13 | uint32(w&15)<<17
	s.body = instr(s.body, op, []uint32{ext}, ops...)
}

// TakeBody removes and returns the instructions emitted so far.
func (s *Shader) TakeBody() []uint32 {
	b := s.body
	s.body = nil
	return b
}

// AppendBody adds instructions (from TakeBody).
func (s *Shader) AppendBody(b []uint32) { s.body = append(s.body, b...) }

// Temps sets how many temporary registers the program uses.
func (s *Shader) Temps(n int) {
	if n > s.temps {
		s.temps = n
	}
}

func (s *Shader) DclInput(r int, mask uint32) {
	s.decl = instr(s.decl, opDclInput, nil, In(r).Mask(mask))
}

func (s *Shader) DclInputSV(r int, mask, sv uint32) {
	s.decl = instr(s.decl, opDclInputSgv, nil, In(r).Mask(mask), Opnd{raw: true, value: []uint32{sv}})
}

func (s *Shader) DclInputPS(r int, mask, interp uint32) {
	s.decl = instr(s.decl, opDclInputPs|interp<<11, nil, In(r).Mask(mask))
}

func (s *Shader) DclInputPSSV(r int, mask, interp, sv uint32) {
	op := uint32(opDclInputPsSiv)
	if interp == 0 {
		op = opDclInputPsSgv
	}
	s.decl = instr(s.decl, op|interp<<11, nil, In(r).Mask(mask), Opnd{raw: true, value: []uint32{sv}})
}

func (s *Shader) DclInputPSSGV(r int, mask, sv uint32) {
	s.decl = instr(s.decl, opDclInputPsSgv, nil, In(r).Mask(mask), Opnd{raw: true, value: []uint32{sv}})
}

func (s *Shader) DclOutput(r int, mask uint32) {
	s.decl = instr(s.decl, opDclOutput, nil, Out(r).Mask(mask))
}

func (s *Shader) DclOutputSV(r int, mask, sv uint32) {
	s.decl = instr(s.decl, opDclOutputSiv, nil, Out(r).Mask(mask), Opnd{raw: true, value: []uint32{sv}})
}

func (s *Shader) DclOutputDepth() {
	s.decl = instr(s.decl, opDclOutput, nil, OutDepth())
}

// DclGSInputTriangles and the rest declare what a geometry shader takes
// (triangles, as v[3][r]) and gives (a triangle strip of up to n vertices).
func (s *Shader) DclGSTriangles(n int) {
	s.decl = append(s.decl, opDclGSInPrim|3<<11|1<<24, opDclGSOutTopo|5<<11|1<<24, opDclMaxOutVerts|2<<24, uint32(n))
}

// DclGSInput declares input register r of all three vertices (with a
// system value, when sv isn't SVNone).
func (s *Shader) DclGSInput(r int, mask, sv uint32) {
	if sv == SVNone {
		s.decl = instr(s.decl, opDclInput, nil, InV(3, r).Mask(mask))
		return
	}
	s.decl = instr(s.decl, opDclInputSiv, nil, InV(3, r).Mask(mask), Opnd{raw: true, value: []uint32{sv}})
}

// DclCB declares constant buffer slot with size vec4s; dynamic allows
// register-indexed reads.
func (s *Shader) DclCB(slot, size int, dynamic bool) {
	op := uint32(opDclConstBuffer)
	if dynamic {
		op |= 1 << 11
	}
	s.decl = instr(s.decl, op, nil, CB(slot, size))
}

func (s *Shader) DclSampler(i int, compare bool) {
	op := uint32(opDclSampler)
	if compare {
		op |= 1 << 11
	}
	o := Samp(i)
	s.decl = instr(s.decl, op, nil, o)
}

func (s *Shader) DclResource(i int, dim, ret uint32) {
	r := Tex(i)
	r.n, r.mode, r.comp = 0, 0, 0
	s.decl = instr(s.decl, opDclResource|dim<<11, nil, r, Opnd{raw: true, value: []uint32{ret * 0x1111}})
}

// DclIndexable declares x[arr] with size registers of 4 components.
func (s *Shader) DclIndexable(arr, size int) {
	s.decl = append(s.decl, opDclIndexableTmp|4<<24, uint32(arr), uint32(size), 4)
}

// Bytes builds the finished, signed container.
func (s *Shader) Bytes() []byte {
	prog := []uint32{0x50, 0}
	switch {
	case s.Geometry:
		prog[0] |= 2 << 16
	case !s.Pixel:
		prog[0] |= 1 << 16
	}
	prog = append(prog, s.decl...)
	if s.temps > 0 {
		prog = append(prog, opDclTemps|2<<24, uint32(s.temps))
	}
	prog = append(prog, s.body...)
	prog[1] = uint32(len(prog))
	shex := make([]byte, 4*len(prog))
	for i, w := range prog {
		binary.LittleEndian.PutUint32(shex[i*4:], w)
	}
	chunks := []struct {
		tag  string
		data []byte
	}{{"ISGN", signature(s.In, true)}, {"OSGN", signature(s.Out, false)}, {"SHEX", shex}}
	hdr := 32 + 4*len(chunks)
	size := hdr
	for _, c := range chunks {
		size += 8 + len(c.data)
	}
	b := make([]byte, 0, size)
	b = append(b, "DXBC"...)
	b = append(b, make([]byte, 16)...)
	b = binary.LittleEndian.AppendUint32(b, 1)
	b = binary.LittleEndian.AppendUint32(b, uint32(size))
	b = binary.LittleEndian.AppendUint32(b, uint32(len(chunks)))
	off := hdr
	for _, c := range chunks {
		b = binary.LittleEndian.AppendUint32(b, uint32(off))
		off += 8 + len(c.data)
	}
	for _, c := range chunks {
		b = append(b, c.tag...)
		b = binary.LittleEndian.AppendUint32(b, uint32(len(c.data)))
		b = append(b, c.data...)
	}
	sum := Checksum(b)
	for i, w := range sum {
		binary.LittleEndian.PutUint32(b[4+4*i:], w)
	}
	return b
}

func signature(els []Sig, input bool) []byte {
	head := 8 + 24*len(els)
	var names []byte
	nameOff := map[string]int{}
	for _, e := range els {
		if _, ok := nameOff[e.Name]; !ok {
			nameOff[e.Name] = head + len(names)
			names = append(names, e.Name...)
			names = append(names, 0)
		}
	}
	for len(names)%4 != 0 {
		names = append(names, 0xab)
	}
	b := binary.LittleEndian.AppendUint32(nil, uint32(len(els)))
	b = binary.LittleEndian.AppendUint32(b, 8)
	for _, e := range els {
		sv := e.SV
		if sv >= 64 {
			sv = 0 // SV_Target and SV_Depth are plain in signatures
		}
		ct := e.CompType
		if ct == 0 {
			ct = 3
		}
		b = binary.LittleEndian.AppendUint32(b, uint32(nameOff[e.Name]))
		b = binary.LittleEndian.AppendUint32(b, e.Index)
		b = binary.LittleEndian.AppendUint32(b, sv)
		b = binary.LittleEndian.AppendUint32(b, ct)
		b = binary.LittleEndian.AppendUint32(b, e.Reg)
		rw := e.Mask
		if !input {
			rw = 0 // outputs: the components that are never written
		}
		b = append(b, e.Mask, rw, 0, 0)
	}
	return append(b, names...)
}
