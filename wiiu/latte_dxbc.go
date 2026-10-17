package wiiu

// Translating Latte (R700) shader programs into Direct3D 11 shader bytecode,
// so a graphics card can run them.
//
// The translation follows the shader interpreter (latte_shader.go) closely,
// with one big difference: the interpreter runs four lanes together and
// keeps a stack of lane masks, while a Direct3D shader is written for one
// lane and the graphics card handles divergence itself. So the lane masks
// become a single "active" flag (and a stack of them), every clause runs
// under "if active", and jumps (which the GPU only takes when no lane is
// active) become structured ifs over the instructions they skip.
//
// Register use in the generated program:
//
//	r0-r127   the GPRs (or x0[128] when the program indexes them)
//	r128      x active, y killed, z predicate
//	r129      the address register (MOVA)
//	r130-133  PV and PS, double buffered
//	r134-135  gradients for SAMPLE_G
//	r136      x stack depth
//	r137-152  scratch
//	r153+     loop state, two registers per nesting level
//	r170+     exports, copied to the outputs at the end
//	x1[64]    the mask stack

import (
	"fmt"
	"math"

	"retropixels/wiiu/d3d"
)

const (
	dxACT   = 128
	dxAR    = 129
	dxPV0   = 130 // and 131
	dxPS0   = 132 // and 133
	dxGH    = 134
	dxGV    = 135
	dxDEPTH = 136
	dxS     = 137 // 16 scratch registers
	dxLOOP  = 153
	dxOUT   = 170

	dxMaxLoops = 8
	dxStackLen = 64
)

// constant buffer slots
const (
	dxCBSys   = 0 // system values (dxSys*)
	dxCBFile  = 1 // the ALU constant file (256 registers)
	dxCBBank0 = 2 // kcache banks, in the order the program uses them
)

// system constant buffer layout (vec4 registers)
const (
	dxSysDraw    = 0  // x index offset, y base vertex, z start instance
	dxSysBool    = 1  // x bool constants, y alpha reference
	dxSysLoop    = 2  // 8 registers: 32 loop constants
	dxSysVport   = 10 // x/y: a, b of x' = a*x + b*w; z/w the same for y
	dxSysVportZ  = 11 // x/y for z
	dxSysBufSize = 12 // 4 registers: 16 vertex buffer sizes
	dxSysSize    = 16
)

// dxTexKey is what a texture fetch's translation depends on.
type dxTexKey struct {
	valid  bool
	dim    uint8 // the resource's DIM field
	intFmt bool
	sel    [4]uint8
	depth  bool // a depth format, sampled with a comparison
}

// dxFetchKey is what a vertex fetch's translation depends on.
type dxFetchKey struct {
	stride uint32
	word2  uint32 // format fields, for fetches that take them from the resource
}

// dxPSIn says where a pixel shader input GPR comes from.
type dxPSIn struct {
	reg      int // the vertex shader output register, or -1 for def
	def      [4]uint32
	flat     bool
	linear   bool // no perspective correction
	centroid bool
}

// dxConfig is the GPU state a translation needs.
type dxConfig struct {
	ps       bool
	read32   func(addr uint32) uint32
	fsAddr   uint32
	texInfo  func(res uint32) dxTexKey
	vbufInfo func(buf uint32) dxFetchKey
	vtxSem   [32]uint32
	// vertex shaders: how many parameter outputs (the pixel shader must
	// agree)
	nParams int
	vte     uint32
	// pixel shaders
	psIn       []dxPSIn
	posGPR     int32
	faceGPR    int32
	faceChan   uint32
	faceBits   bool
	alphaOn    bool
	alphaFunc  uint32
	multiWrite bool
	// testing: put vertex i's position at pixel (i, 0) of a testPoints-wide
	// target, output the program's position as parameter 0 (and its
	// parameters after it), and load R1..R(testGPRs) from a buffer
	testPoints int
	testGPRs   int
}

// dxTestBuf is the pseudo vertex buffer the test inputs come from.
const dxTestBuf = 0xffff

// dxSRV is what goes in a shader resource slot.
type dxSRV struct {
	vbuf bool // a vertex buffer (else a texture)
	id   uint32
}

// dxProgram is a translated shader.
type dxProgram struct {
	code     []byte
	srv      []dxSRV
	samplers []uint32
	banks    []uint32
	colors   uint8 // pixel shaders: colour exports written
	depth    bool
	texDeps  map[uint32]dxTexKey
	vbufDeps map[uint32]dxFetchKey
}

type dxLoopState struct {
	dx10 bool
}

type dxTr struct {
	cfg     *dxConfig
	s       *d3d.Shader
	base    uint32 // address of the program being read
	indexed bool   // GPRs in x0
	err     string
	needIdx bool
	// static state
	loops   []dxLoopState
	group   int // group counter (PV/PS buffers)
	vec4At  int // group+1 whose DOT4/MAX4 result is in scratch 5
	pvSet   [2][4]bool
	psSet   [2]bool
	calls   int
	maxGPR  int
	exports map[uint32]bool
	// kcache state of the current ALU clause
	kcBank, kcMode, kcAddr [2]uint32
	// resource bookkeeping
	srvSlot  map[dxSRV]int
	sampSlot map[uint32]int
	bankSlot map[uint32]int
	prog     *dxProgram
	gradOK   bool
}

type dxRestart struct{}

// translateDX translates the program at addr.
func translateDX(cfg *dxConfig, addr uint32) (p *dxProgram, err error) {
	for _, indexed := range []bool{false, true} {
		p, err = translateDXMode(cfg, addr, indexed)
		if err == nil || err.Error() != "restart" {
			return
		}
	}
	return
}

func translateDXMode(cfg *dxConfig, addr uint32, indexed bool) (p *dxProgram, err error) {
	t := &dxTr{cfg: cfg, indexed: indexed, base: addr,
		srvSlot: map[dxSRV]int{}, sampSlot: map[uint32]int{}, bankSlot: map[uint32]int{},
		exports: map[uint32]bool{}}
	t.prog = &dxProgram{texDeps: map[uint32]dxTexKey{}, vbufDeps: map[uint32]dxFetchKey{}}
	t.s = &d3d.Shader{Pixel: cfg.ps}
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(dxRestart); ok {
				err = fmt.Errorf("restart")
				return
			}
			if s, ok := r.(string); ok {
				err = fmt.Errorf("%s", s)
				return
			}
			panic(r)
		}
	}()
	t.prologue()
	end := t.cfRange(0, 1<<20, false)
	if end != dxEndProgram && end != dxEndReturn {
		t.fail("program ran off its end")
	}
	t.epilogue()
	t.declare()
	if t.err != "" {
		return nil, fmt.Errorf("%s", t.err)
	}
	t.prog.code = t.s.Bytes()
	return t.prog, nil
}

func (t *dxTr) fail(f string, a ...any) { panic(fmt.Sprintf(f, a...)) }

// ---- small emit helpers ----

func (t *dxTr) ins(op uint32, ops ...d3d.Opnd) { t.s.Ins(op, ops...) }

func dxR(i int) d3d.Opnd            { return d3d.R(i) }
func dxRC(i int, c uint32) d3d.Opnd { return d3d.R(i).C(c) }
func dxW(i int, c uint32) d3d.Opnd  { return d3d.R(i).Mask(1 << c) }
func dxU(v uint32) d3d.Opnd         { return d3d.ImmU(v) }
func dxF(v float32) d3d.Opnd        { return d3d.ImmF(v) }

// scratch register k, x component
func sx(k int) d3d.Opnd  { return dxRC(dxS+k, 0) }
func sxw(k int) d3d.Opnd { return dxW(dxS+k, 0) }

var (
	actv   = dxRC(dxACT, 0)
	actvW  = dxW(dxACT, 0)
	killd  = dxRC(dxACT, 1)
	killdW = dxW(dxACT, 1)
	pred   = dxRC(dxACT, 2)
	predW  = dxW(dxACT, 2)
	depthR = dxRC(dxDEPTH, 0)
	depthW = dxW(dxDEPTH, 0)
)

func (t *dxTr) ifNZ(o d3d.Opnd) { t.ins(d3d.OpIf|d3d.NonZero, o) }
func (t *dxTr) ifZ(o d3d.Opnd)  { t.ins(d3d.OpIf, o) }
func (t *dxTr) els()            { t.ins(d3d.OpElse) }
func (t *dxTr) endif()          { t.ins(d3d.OpEndif) }

func sysCB(i int, c uint32) d3d.Opnd { return d3d.CB(dxCBSys, i).C(c) }

// gpr operands
func (t *dxTr) gprSrc(r uint32, ch uint32, rel d3d.Opnd, hasRel bool) d3d.Opnd {
	if hasRel && !t.indexed {
		panic(dxRestart{})
	}
	if int(r&127) > t.maxGPR {
		t.maxGPR = int(r & 127)
	}
	if t.indexed {
		o := d3d.X(0, int(r&127))
		if hasRel {
			o = o.RelIndex(1, rel)
		}
		return o.C(ch)
	}
	return dxRC(int(r&127), ch)
}

func (t *dxTr) gprDst(r uint32, mask uint32, rel d3d.Opnd, hasRel bool) d3d.Opnd {
	if hasRel && !t.indexed {
		panic(dxRestart{})
	}
	if int(r&127) > t.maxGPR {
		t.maxGPR = int(r & 127)
	}
	if t.indexed {
		o := d3d.X(0, int(r&127))
		if hasRel {
			o = o.RelIndex(1, rel)
		}
		return o.Mask(mask)
	}
	return d3d.R(int(r & 127)).Mask(mask)
}

// the whole GPR as a vec4 source
func (t *dxTr) gprVec(r uint32, rel d3d.Opnd, hasRel bool) d3d.Opnd {
	o := t.gprSrc(r, 0, rel, hasRel)
	return o.Swz(0, 1, 2, 3)
}

// arIndex is the relative index register for index mode im.
func (t *dxTr) arIndex(im uint32) d3d.Opnd {
	if im < 4 {
		return dxRC(dxAR, im)
	}
	if len(t.loops) == 0 {
		return d3d.ImmU(0) // no loop: index 0
	}
	return dxRC(t.loopReg(), 1)
}

func (t *dxTr) loopReg() int  { return dxLOOP + 2*(len(t.loops)-1) }
func (t *dxTr) loopMask() int { return dxLOOP + 2*(len(t.loops)-1) + 1 }

// ---- the prologue and epilogue ----

func (t *dxTr) prologue() {
	t.ins(d3d.OpMov, dxR(dxACT).Mask(15), d3d.ImmU(0xffffffff, 0, 0, 0))
	t.ins(d3d.OpMov, dxR(dxAR).Mask(15), d3d.ImmU(0, 0, 0, 0))
	t.ins(d3d.OpMov, depthW, dxU(0))
	for i := 0; i < 2; i++ {
		t.ins(d3d.OpMov, dxR(dxPV0+i).Mask(15), dxU(0))
		t.ins(d3d.OpMov, dxR(dxPS0+i).Mask(15), dxU(0))
	}
	// outputs default to zero
	for i := 0; i < 34; i++ {
		t.ins(d3d.OpMov, dxR(dxOUT+i).Mask(15), dxU(0))
	}
	// GPR setup comes after we know how many there are: the program is
	// assembled with a placeholder and the init inserted in declare()
}

// gprInit writes the GPRs' starting values (emitted before the program
// body by declare).
func (t *dxTr) gprInit() *d3d.Shader {
	s := &d3d.Shader{}
	n := t.maxGPR + 1
	set := func(r int, mask uint32, v d3d.Opnd) {
		if t.indexed {
			s.Ins(d3d.OpMov, d3d.X(0, r).Mask(mask), v)
		} else {
			s.Ins(d3d.OpMov, d3d.R(r).Mask(mask), v)
		}
	}
	for r := 0; r < n; r++ {
		set(r, 15, dxU(0))
	}
	if !t.cfg.ps {
		// R0 = (vertex index + offset, 0, 0, instance)
		s.Ins(d3d.OpIadd, dxW(dxS, 0), d3d.In(0).C(0), sysCB(dxSysDraw, 0))
		set(0, 1, sx(0))
		set(0, 8, d3d.In(1).C(0))
		if n := t.cfg.testGPRs; n > 0 {
			slot := t.srv(dxSRV{vbuf: true, id: dxTestBuf})
			for r := 1; r <= n && r <= t.maxGPR; r++ {
				for c := uint32(0); c < 4; c++ {
					s.Ins(d3d.OpImad, dxW(dxS, 1), d3d.In(0).C(0), dxU(uint32(4*n)), dxU(uint32(4*(r-1))+c))
					s.Ins(d3d.OpLd, dxW(dxS, 2), dxRC(dxS, 1), d3d.Tex(slot).C(0))
					set(r, 1<<c, dxRC(dxS, 2))
				}
			}
		}
		return s
	}
	for i, in := range t.cfg.psIn {
		if i > t.maxGPR {
			break
		}
		if in.reg < 0 {
			set(i, 15, d3d.ImmU(in.def[0], in.def[1], in.def[2], in.def[3]))
		} else {
			set(i, 15, d3d.In(in.reg))
		}
	}
	if p := t.cfg.posGPR; p >= 0 && int(p) <= t.maxGPR {
		set(int(p), 15, d3d.In(0))
	}
	if f := t.cfg.faceGPR; f >= 0 && int(f) <= t.maxGPR {
		inReg := t.cfg.nParams + 1
		if t.cfg.faceBits {
			set(int(f), 1<<t.cfg.faceChan, d3d.In(inReg).C(0))
		} else {
			s.Ins(d3d.OpMovc, dxW(dxS, 0), d3d.In(inReg).C(0), dxF(1), dxF(-1))
			set(int(f), 1<<t.cfg.faceChan, sx(0))
		}
	}
	return s
}

// psInRegs: the input registers the pixel shader reads.
func (t *dxTr) psInRegs() []int {
	var r []int
	for _, in := range t.cfg.psIn {
		if in.reg >= 0 {
			r = append(r, in.reg)
		}
	}
	return r
}

func (t *dxTr) epilogue() {
	if t.cfg.ps {
		// alpha test on colour 0
		if t.cfg.alphaOn {
			a := dxRC(dxOUT, 3)
			ref := sysCB(dxSysBool, 1)
			f := t.cfg.alphaFunc
			switch f {
			case 0: // never
				t.ins(d3d.OpDiscard|d3d.NonZero, dxU(1))
			case 7: // always
			default:
				t.compare(f, sxw(0), a, ref)
				t.ins(d3d.OpDiscard, sx(0))
			}
		}
		for c := uint32(0); c < 8; c++ {
			src := c
			if t.cfg.multiWrite {
				src = 0
			}
			if t.exports[src] {
				t.prog.colors |= 1 << c
				t.ins(d3d.OpMov, d3d.Out(int(c)).Mask(15), dxR(dxOUT+int(src)))
			}
		}
		if t.exports[61] {
			t.prog.depth = true
			t.ins(d3d.OpMov, d3d.OutDepth(), dxRC(dxOUT+8, 0))
		}
		return
	}
	// vertex shaders: the position, through the viewport mapping
	pos := dxOUT
	if n := t.cfg.testPoints; n > 0 {
		// x = (2*(i+0.5)/n - 1), y = 0.5
		t.ins(d3d.OpUtof, sxw(0), d3d.In(0).C(0))
		t.ins(d3d.OpAdd, sxw(0), sx(0), dxF(0.5))
		t.ins(d3d.OpMad, d3d.Out(0).Mask(1), sx(0), dxF(2/float32(n)), dxF(-1))
		t.ins(d3d.OpMov, d3d.Out(0).Mask(14), d3d.ImmF(0, 0, 0.5, 1))
	} else {
		w := dxRC(pos, 3)
		if t.cfg.vte>>8&1 != 0 { // x, y already divided
			w = dxF(1)
		}
		t.ins(d3d.OpMov, d3d.Out(0).Mask(8), w)
		t.ins(d3d.OpMul, sxw(0), dxRC(pos, 0), sysCB(dxSysVport, 0))
		t.ins(d3d.OpMad, d3d.Out(0).Mask(1), w, sysCB(dxSysVport, 1), sx(0))
		t.ins(d3d.OpMul, sxw(0), dxRC(pos, 1), sysCB(dxSysVport, 2))
		t.ins(d3d.OpMad, d3d.Out(0).Mask(2), w, sysCB(dxSysVport, 3), sx(0))
		z := dxRC(pos, 2)
		if t.cfg.vte>>9&1 != 0 && t.cfg.vte>>8&1 == 0 { // z already divided
			t.ins(d3d.OpMul, sxw(1), z, w)
			z = sx(1)
		}
		t.ins(d3d.OpMul, sxw(0), z, sysCB(dxSysVportZ, 0))
		t.ins(d3d.OpMad, d3d.Out(0).Mask(4), w, sysCB(dxSysVportZ, 1), sx(0))
	}
	for i := 0; i < t.cfg.nParams; i++ {
		src := dxOUT + 2 + i
		if t.cfg.testPoints > 0 {
			src = dxOUT + 1 + i
			if i == 0 {
				src = dxOUT
			}
		}
		t.ins(d3d.OpMov, d3d.Out(1+i).Mask(15), dxR(src))
	}
}

// declare adds the declarations and signatures, and puts the GPR setup in
// front of the body.
func (t *dxTr) declare() {
	s := t.s
	body := s.TakeBody()
	init := t.gprInit()
	s.AppendBody(init.TakeBody())
	s.AppendBody(body)
	s.Ins(d3d.OpRet)
	s.Temps(dxOUT + 34)
	if t.indexed {
		s.DclIndexable(0, 128)
	}
	s.DclIndexable(1, dxStackLen)
	// constant buffers
	s.DclCB(dxCBSys, dxSysSize, false)
	s.DclCB(dxCBFile, 256, true)
	for i := range t.prog.banks {
		s.DclCB(dxCBBank0+i, 4096, true)
	}
	// samplers and resources
	for i, id := range t.prog.samplers {
		_ = id
		s.DclSampler(i, false)
	}
	for i, r := range t.prog.srv {
		if r.vbuf {
			s.DclResource(i, d3d.DimBuffer, d3d.RetUint)
			continue
		}
		k := t.prog.texDeps[r.id]
		ret := uint32(d3d.RetFloat)
		if k.intFmt {
			ret = d3d.RetUint
		}
		s.DclResource(i, dxTexDim(k.dim), ret)
	}
	if !t.cfg.ps {
		s.In = []d3d.Sig{
			{Name: "SV_VertexID", SV: d3d.SVVertexID, CompType: 1, Reg: 0, Mask: 1},
			{Name: "SV_InstanceID", SV: d3d.SVInstance, CompType: 1, Reg: 1, Mask: 1},
		}
		s.DclInputSV(0, 1, d3d.SVVertexID)
		s.DclInputSV(1, 1, d3d.SVInstance)
		s.Out = dxVSOutSig(t.cfg.nParams)
		s.DclOutputSV(0, 15, d3d.SVPosition)
		for i := 0; i < t.cfg.nParams; i++ {
			s.DclOutput(1+i, 15)
		}
		return
	}
	// pixel shader inputs: the position, then every parameter register in
	// the vertex shader's order (so the signatures line up), then the face
	s.In = []d3d.Sig{{Name: "SV_Position", SV: d3d.SVPosition, Mask: 15}}
	if t.cfg.posGPR >= 0 {
		s.DclInputPSSV(0, 15, d3d.InterpNoPerspective, d3d.SVPosition)
	}
	regs := map[int]dxPSIn{}
	maxReg := 0
	for _, in := range t.cfg.psIn {
		if in.reg >= 0 {
			regs[in.reg] = in
			maxReg = max(maxReg, in.reg)
		}
	}
	for r := 1; r <= t.cfg.nParams; r++ {
		s.In = append(s.In, d3d.Sig{Name: "TEXCOORD", Index: uint32(r - 1), Reg: uint32(r), Mask: 15})
		in, ok := regs[r]
		if !ok {
			continue
		}
		mode := uint32(d3d.InterpLinear)
		switch {
		case in.flat:
			mode = d3d.InterpConstant
		case in.linear && in.centroid:
			mode = 5 // linear noperspective centroid
		case in.linear:
			mode = d3d.InterpNoPerspective
		case in.centroid:
			mode = d3d.InterpCentroid
		}
		s.DclInputPS(r, 15, mode)
	}
	if t.cfg.faceGPR >= 0 {
		r := t.cfg.nParams + 1
		s.In = append(s.In, d3d.Sig{Name: "SV_IsFrontFace", SV: d3d.SVFrontFace, CompType: 1, Reg: uint32(r), Mask: 1})
		s.DclInputPSSGV(r, 1, d3d.SVFrontFace)
	}
	for c := 0; c < 8; c++ {
		if t.prog.colors>>c&1 != 0 {
			s.Out = append(s.Out, d3d.Sig{Name: "SV_Target", Index: uint32(c), SV: d3d.SVTarget, Reg: uint32(c), Mask: 15})
			s.DclOutput(c, 15)
		}
	}
	if t.prog.depth {
		s.Out = append(s.Out, d3d.Sig{Name: "SV_Depth", SV: d3d.SVDepth, Reg: 0xffffffff, Mask: 1})
		s.DclOutputDepth()
	}
}

// dxVSOutSig is the vertex shader output signature for n parameters.
func dxVSOutSig(n int) []d3d.Sig {
	sig := []d3d.Sig{{Name: "SV_Position", SV: d3d.SVPosition, Mask: 15}}
	for i := 0; i < n; i++ {
		sig = append(sig, d3d.Sig{Name: "TEXCOORD", Index: uint32(i), Reg: uint32(1 + i), Mask: 15})
	}
	return sig
}

func dxTexDim(dim uint8) uint32 {
	switch dim {
	case 0:
		return d3d.DimTex1D
	case 2:
		return d3d.DimTex3D
	case 3:
		return d3d.DimTexCube
	case 4:
		return 7 // 1D array
	case 5, 7:
		return d3d.DimTex2DArray
	case 6:
		return 4 // 2D MSAA
	}
	return d3d.DimTex2D
}

// ---- control flow ----

const (
	dxEndRange   = iota // reached the end of the range
	dxEndProgram        // END_OF_PROGRAM
	dxEndReturn         // RETURN
	dxEndLoop           // LOOP_END (returned to the loop's translator)
)

func (t *dxTr) cfWords(pc uint32) (uint32, uint32) {
	a := t.base + pc*8
	return t.cfg.read32(a), t.cfg.read32(a + 4)
}

// cfInfo decodes a CF instruction: whether it's an ALU clause, its
// instruction, and END_OF_PROGRAM.
func cfDecode(w0, w1 uint32) (alu bool, inst uint32, eop bool) {
	if w1>>29&1 != 0 {
		return true, w1 >> 26 & 0xf, false
	}
	return false, w1 >> 23 & 0x7f, w1>>21&1 != 0
}

// nests reports whether the CF instructions in [from, to) only jump within
// [from, to] and keep their loops inside, so they can be put in an if.
func (t *dxTr) nests(from, to uint32) bool {
	if to < from || to-from > 4096 {
		return false
	}
	depth := 0
	for pc := from; pc < to; pc++ {
		w0, w1 := t.cfWords(pc)
		alu, inst, eop := cfDecode(w0, w1)
		if alu || inst >= cfMEM_STREAM0 {
			continue
		}
		switch inst {
		case cfJUMP, cfPUSH, cfELSE, cfPOP_JUMP:
			if w0 > to || w0 <= pc {
				return false
			}
		case cfLOOP_START, cfLOOP_START_DX10, cfLOOP_START_NO_AL:
			depth++
		case cfLOOP_END:
			depth--
			if depth < 0 {
				return false
			}
		case cfRETURN:
			return false
		}
		if eop {
			return false
		}
	}
	return depth == 0
}

// cfRange translates CF instructions from pc up to end. It returns how it
// stopped; inLoop says a LOOP_END ends it.
func (t *dxTr) cfRange(pc, end uint32, inLoop bool) int {
	for pc < end {
		w0, w1 := t.cfWords(pc)
		alu, inst, eop := cfDecode(w0, w1)
		if alu {
			t.aluCF(inst, w0, w1)
			pc++
			continue
		}
		if inst >= cfMEM_STREAM0 {
			t.exportCF(inst, w0, w1)
			pc++
			if eop {
				return dxEndProgram
			}
			continue
		}
		addr := w0
		popCount := w1 & 7
		cfConst := w1 >> 3 & 0x1f
		cnd := w1 >> 8 & 3
		count := (w1>>10&7 | (w1>>19&1)<<3) + 1
		next := pc + 1
		switch inst {
		case cfNOP, cfWAIT_ACK, cfTEX_ACK, cfVTX_ACK, cfVTX_TC_ACK:
		case cfTEX:
			t.texClause(addr, count)
		case cfVTX, cfVTX_TC:
			t.vtxClause(addr, count)
		case cfLOOP_START, cfLOOP_START_DX10, cfLOOP_START_NO_AL:
			next = t.loop(pc, inst, cnd, cfConst)
		case cfLOOP_END:
			if inLoop {
				return dxEndLoop
			}
			t.fail("LOOP_END outside a loop")
		case cfLOOP_BREAK, cfLOOP_CONTINUE:
			if len(t.loops) == 0 {
				break
			}
			m := t.cond(cnd, cfConst)
			lm := t.loopMask()
			c := uint32(1)
			if inst == cfLOOP_CONTINUE {
				c = 2
			}
			t.ins(d3d.OpOr, dxW(lm, c), dxRC(lm, c), m)
			t.ins(d3d.OpNot, sxw(1), m)
			t.ins(d3d.OpAnd, actvW, actv, sx(1))
		case cfJUMP:
			m := t.cond(cnd, cfConst)
			if addr > pc && addr <= end && t.nests(pc+1, addr) {
				t.ins(d3d.OpMov, sxw(2), m)
				t.ifZ(sx(2))
				t.pop(popCount)
				t.els()
				t.ins(d3d.OpMov, actvW, sx(2))
				if r := t.cfRange(pc+1, addr, false); r != dxEndRange {
					t.fail("jump range ended early")
				}
				t.endif()
				next = addr
			} else {
				t.ins(d3d.OpMov, actvW, m)
			}
		case cfPUSH:
			t.push()
			m := t.cond(cnd, cfConst)
			t.ins(d3d.OpMov, actvW, m)
			if addr > pc && addr <= end && t.nests(pc+1, addr) {
				t.ifZ(actv)
				t.pop(popCount)
				t.els()
				if r := t.cfRange(pc+1, addr, false); r != dxEndRange {
					t.fail("push range ended early")
				}
				t.endif()
				next = addr
			}
		case cfPUSH_ELSE:
			t.push()
			t.doElse()
		case cfELSE:
			t.pop(popCount)
			t.doElse()
			if addr > pc && addr <= end && t.nests(pc+1, addr) {
				t.ifNZ(actv)
				if r := t.cfRange(pc+1, addr, false); r != dxEndRange {
					t.fail("else range ended early")
				}
				t.endif()
				next = addr
			}
		case cfPOP:
			t.pop(popCount)
		case cfPOP_JUMP:
			t.pop(popCount)
			m := t.cond(cnd, cfConst)
			if addr > pc && addr <= end && t.nests(pc+1, addr) {
				t.ins(d3d.OpMov, sxw(2), m)
				t.ifNZ(sx(2))
				if r := t.cfRange(pc+1, addr, false); r != dxEndRange {
					t.fail("pop-jump range ended early")
				}
				t.endif()
				next = addr
			}
		case cfPOP_PUSH:
			t.pop(popCount)
			t.push()
		case cfPOP_PUSH_ELSE:
			t.pop(popCount)
			t.push()
			t.doElse()
		case cfCALL, cfCALL_FS:
			if t.calls > 8 {
				t.fail("shader calls nest too deep")
			}
			m := t.cond(cnd, cfConst)
			t.ins(d3d.OpMov, sxw(2), m)
			t.ifNZ(sx(2))
			saved := t.base
			start := addr
			if inst == cfCALL_FS {
				t.base = t.cfg.fsAddr
				start = 0
			}
			t.calls++
			r := t.cfRange(start, 1<<20, false)
			t.calls--
			t.base = saved
			if r == dxEndProgram {
				t.fail("subroutine ends the program")
			}
			t.endif()
		case cfRETURN:
			if t.calls > 0 {
				return dxEndReturn
			}
			return dxEndReturn
		case cfKILL:
			m := t.cond(cnd, cfConst)
			t.kill(m)
		case cfEMIT_VERTEX, cfEMIT_CUT, cfCUT_VERTEX:
		default:
			t.fail("unknown CF instruction %#x", inst)
		}
		if eop {
			return dxEndProgram
		}
		pc = next
	}
	return dxEndRange
}

// cond puts a CF instruction's condition (on top of the active flag) in
// scratch 0 and returns it.
func (t *dxTr) cond(c, cfConst uint32) d3d.Opnd {
	switch c {
	case 1, 2:
		t.ins(d3d.OpUshr, sxw(0), sysCB(dxSysBool, 0), dxU(cfConst))
		t.ins(d3d.OpAnd, sxw(0), sx(0), dxU(1))
		if c == 1 {
			t.ins(d3d.OpIne, sxw(0), sx(0), dxU(0))
		} else {
			t.ins(d3d.OpIeq, sxw(0), sx(0), dxU(0))
		}
		t.ins(d3d.OpAnd, sxw(0), sx(0), actv)
	default:
		t.ins(d3d.OpMov, sxw(0), actv)
	}
	return sx(0)
}

func (t *dxTr) push() {
	// x1[depth] = active; depth++ (the stack can't grow past its end)
	t.ins(d3d.OpUmin, sxw(3), depthR, dxU(dxStackLen-1))
	t.ins(d3d.OpMov, d3d.X(1, 0).RelIndex(1, sx(3)).Mask(1), actv)
	t.ins(d3d.OpIadd, depthW, sx(3), dxU(1))
}

func (t *dxTr) pop(n uint32) {
	if n == 0 {
		t.maskLoop()
		return
	}
	// if depth >= n { depth -= n; active = x1[depth] } else { depth = 0; active = true }
	t.ins(d3d.OpUge, sxw(3), depthR, dxU(n))
	t.ifNZ(sx(3))
	t.ins(d3d.OpIadd, depthW, depthR, d3d.ImmI(-int32(n)))
	t.ins(d3d.OpMov, actvW, d3d.X(1, 0).RelIndex(1, depthR).C(0))
	t.els()
	t.ins(d3d.OpMov, depthW, dxU(0))
	t.ins(d3d.OpMov, actvW, dxU(0xffffffff))
	t.endif()
	t.maskLoop()
}

// maskLoop keeps lanes that broke out of or continued the current loop off.
func (t *dxTr) maskLoop() {
	if len(t.loops) == 0 {
		return
	}
	lm := t.loopMask()
	t.ins(d3d.OpOr, sxw(4), dxRC(lm, 1), dxRC(lm, 2))
	t.ins(d3d.OpNot, sxw(4), sx(4))
	t.ins(d3d.OpAnd, actvW, actv, sx(4))
}

func (t *dxTr) killLanes() {
	t.ins(d3d.OpNot, sxw(4), killd)
	t.ins(d3d.OpAnd, actvW, actv, sx(4))
}

// doElse: active = top of stack and not active.
func (t *dxTr) doElse() {
	t.ins(d3d.OpMov, sxw(3), dxU(0xffffffff))
	t.ifNZ(depthR)
	t.ins(d3d.OpIadd, sxw(4), depthR, d3d.ImmI(-1))
	t.ins(d3d.OpMov, sxw(3), d3d.X(1, 0).RelIndex(1, sx(4)).C(0))
	t.endif()
	t.ins(d3d.OpNot, sxw(4), actv)
	t.ins(d3d.OpAnd, actvW, sx(3), sx(4))
	t.killLanes()
	t.maskLoop()
}

func (t *dxTr) kill(m d3d.Opnd) {
	t.ins(d3d.OpOr, killdW, killd, m)
	t.ins(d3d.OpNot, sxw(4), m)
	t.ins(d3d.OpAnd, actvW, actv, sx(4))
	if t.cfg.ps {
		t.ins(d3d.OpDiscard|d3d.NonZero, killd)
	}
}

// findLoopEnd finds the LOOP_END that matches the LOOP_START at pc.
func (t *dxTr) findLoopEnd(pc uint32) uint32 {
	depth := 0
	for p := pc + 1; p < pc+4096; p++ {
		w0, w1 := t.cfWords(p)
		alu, inst, eop := cfDecode(w0, w1)
		if !alu && inst < cfMEM_STREAM0 {
			switch inst {
			case cfLOOP_START, cfLOOP_START_DX10, cfLOOP_START_NO_AL:
				depth++
			case cfLOOP_END:
				if depth == 0 {
					return p
				}
				depth--
			}
		}
		if eop {
			break
		}
	}
	t.fail("loop without an end")
	return 0
}

// loop translates a loop starting at pc and returns the CF instruction
// after its LOOP_END.
func (t *dxTr) loop(pc, inst, cnd, cfConst uint32) uint32 {
	if len(t.loops) >= dxMaxLoops {
		t.fail("loops nest too deep")
	}
	endPC := t.findLoopEnd(pc)
	m := t.cond(cnd, cfConst)
	t.ins(d3d.OpMov, sxw(5), m)
	t.loops = append(t.loops, dxLoopState{dx10: inst == cfLOOP_START_DX10})
	lr, lm := t.loopReg(), t.loopMask()
	// saved, broken, cont, push depth
	t.ins(d3d.OpMov, dxW(lm, 0), actv)
	t.ins(d3d.OpMov, dxR(lm).Mask(6), dxU(0))
	t.ins(d3d.OpMov, dxW(lm, 3), depthR)
	if inst != cfLOOP_START_DX10 {
		lc := sysCB(dxSysLoop+int(cfConst/4), cfConst%4)
		t.ins(d3d.OpAnd, dxW(lr, 0), lc, dxU(0xfff))
		t.ins(d3d.OpUshr, dxW(lr, 1), lc, dxU(12))
		t.ins(d3d.OpAnd, dxW(lr, 1), dxRC(lr, 1), dxU(0xfff))
		t.ins(d3d.OpIshr, dxW(lr, 2), lc, dxU(24))
		t.ins(d3d.OpIne, sxw(6), dxRC(lr, 0), dxU(0))
		t.ins(d3d.OpAnd, sxw(5), sx(5), sx(6))
	} else {
		t.ins(d3d.OpMov, dxR(lr).Mask(7), d3d.ImmI(-1, 0, 0, 0))
	}
	t.ins(d3d.OpMov, dxW(lr, 3), dxU(0))
	t.ifNZ(sx(5))
	t.ins(d3d.OpMov, actvW, sx(5))
	t.ins(d3d.OpLoop)
	if r := t.cfRange(pc+1, endPC+1, true); r != dxEndLoop {
		t.fail("loop body ended early")
	}
	// LOOP_END
	t.ins(d3d.OpIlt, sxw(6), dxU(0), dxRC(lr, 0))
	t.ins(d3d.OpIadd, sxw(7), dxRC(lr, 0), d3d.ImmI(-1))
	t.ins(d3d.OpMovc, dxW(lr, 0), sx(6), sx(7), dxRC(lr, 0))
	t.ins(d3d.OpIadd, dxW(lr, 1), dxRC(lr, 1), dxRC(lr, 2))
	// again = saved & !broken & !killed, and the loop count isn't spent
	t.ins(d3d.OpNot, sxw(6), dxRC(lm, 1))
	t.ins(d3d.OpAnd, sxw(6), sx(6), dxRC(lm, 0))
	t.ins(d3d.OpNot, sxw(7), killd)
	t.ins(d3d.OpAnd, sxw(6), sx(6), sx(7))
	t.ins(d3d.OpMov, dxW(lm, 2), dxU(0))
	t.ins(d3d.OpIne, sxw(7), dxRC(lr, 0), dxU(0))
	t.ins(d3d.OpAnd, sxw(7), sx(6), sx(7))
	// a safety limit on iterations
	t.ins(d3d.OpIadd, dxW(lr, 3), dxRC(lr, 3), dxU(1))
	t.ins(d3d.OpUlt, sxw(8), dxRC(lr, 3), dxU(1<<16))
	t.ins(d3d.OpAnd, sxw(7), sx(7), sx(8))
	t.ins(d3d.OpBreakc, sx(7))
	t.ins(d3d.OpMov, actvW, sx(6))
	t.ins(d3d.OpUmin, depthW, depthR, dxRC(lm, 3))
	t.ins(d3d.OpEndloop)
	t.ins(d3d.OpMov, actvW, dxRC(lm, 0))
	t.killLanes()
	t.ins(d3d.OpUmin, depthW, depthR, dxRC(lm, 3))
	t.els()
	t.ins(d3d.OpMov, actvW, dxRC(lm, 0))
	t.endif()
	t.loops = t.loops[:len(t.loops)-1]
	t.maskLoop()
	return endPC + 1
}

// ---- exports ----

func (t *dxTr) exportCF(inst, w0, w1 uint32) {
	if inst != cfEXPORT && inst != cfEXPORT_DONE {
		return // memory exports
	}
	arrayBase := w0 & 0x1fff
	typ := w0 >> 13 & 3
	gpr := w0 >> 15 & 0x7f
	rel := w0>>22&1 != 0
	burst := (w1 >> 17 & 0xf) + 1
	sel := [4]uint32{w1 & 7, w1 >> 3 & 7, w1 >> 6 & 7, w1 >> 9 & 7}
	t.ifNZ(actv)
	for b := uint32(0); b < burst; b++ {
		base := arrayBase + b
		out := -1
		switch {
		case t.cfg.ps && typ == 0 && base < 8:
			out = int(base)
		case t.cfg.ps && typ == 0 && base == 61:
			out = 8
		case !t.cfg.ps && typ == 1 && base == 60:
			out = 0
		case !t.cfg.ps && typ == 1 && base == 61:
			out = 1
		case !t.cfg.ps && typ == 2 && base < 32:
			out = 2 + int(base)
		}
		if out < 0 {
			continue
		}
		if t.cfg.ps {
			t.exports[base] = true
		}
		for c := uint32(0); c < 4; c++ {
			var v d3d.Opnd
			switch sel[c] {
			case 0, 1, 2, 3:
				v = t.gprSrc(gpr+b, sel[c], dxRC(dxAR, 0), rel)
			case 5:
				v = dxF(1)
			default:
				v = dxU(0)
			}
			t.ins(d3d.OpMov, dxW(dxOUT+out, c), v)
		}
	}
	t.endif()
}

// ---- ALU clauses ----

func (t *dxTr) aluCF(inst, w0, w1 uint32) {
	addr := w0 & 0x3fffff
	count := (w1 >> 18 & 0x7f) + 1
	t.kcBank = [2]uint32{w0 >> 22 & 0xf, w0 >> 26 & 0xf}
	t.kcMode = [2]uint32{w0 >> 30 & 3, w1 & 3}
	t.kcAddr = [2]uint32{w1 >> 2 & 0xff, w1 >> 10 & 0xff}
	switch inst {
	case 9: // ALU_PUSH_BEFORE
		t.push()
		t.aluClause(addr, count)
	case 10: // ALU_POP_AFTER
		t.aluClause(addr, count)
		t.pop(1)
	case 11: // ALU_POP2_AFTER
		t.aluClause(addr, count)
		t.pop(2)
	case 13, 14: // ALU_CONTINUE, ALU_BREAK
		t.ins(d3d.OpMov, sxw(9), actv)
		t.aluClause(addr, count)
		if len(t.loops) > 0 {
			lm := t.loopMask()
			c := uint32(2)
			if inst == 14 {
				c = 1
			}
			t.ins(d3d.OpAnd, sxw(10), sx(9), actv)
			t.ins(d3d.OpOr, dxW(lm, c), dxRC(lm, c), sx(10))
		}
		t.ins(d3d.OpMov, actvW, sx(9))
		t.maskLoop()
	case 15: // ALU_ELSE_AFTER
		t.aluClause(addr, count)
		t.doElse()
	default:
		t.aluClause(addr, count)
	}
}

func (t *dxTr) aluClause(addr, count uint32) {
	nIf := 1
	t.ifNZ(actv)
	slot := uint32(0)
	for slot < count {
		var group [5]aluOp
		n := 0
		var used [5]bool
		litUsed := false
		for slot < count && n < 5 {
			a := t.base + (addr+slot)*8
			w0, w1 := t.cfg.read32(a), t.cfg.read32(a+4)
			slot++
			op := aluOp{w0: w0, w1: w1}
			op.op3 = w1>>15&7 != 0
			if op.op3 {
				op.inst = w1 >> 13 & 0x1f
				op.write = true
			} else {
				op.inst = w1 >> 7 & 0x7ff
				op.write = w1>>4&1 != 0
			}
			op.dst = w1 >> 21 & 0x7f
			op.dstChan = w1 >> 29 & 3
			ch := int(op.dstChan)
			if !op.op3 && transOnly(op.inst) {
				op.unit = 4
			} else if used[ch] {
				op.unit = 4
			} else {
				op.unit = ch
			}
			used[op.unit] = true
			if w0&0x1ff == 253 || w0>>13&0x1ff == 253 || (op.op3 && w1&0x1ff == 253) {
				litUsed = true
			}
			group[n] = op
			n++
			if w0>>31 != 0 {
				break
			}
		}
		var lit [4]uint32
		if litUsed {
			a := t.base + (addr+slot)*8
			for i := 0; i < 4; i++ {
				lit[i] = t.cfg.read32(a + uint32(i)*4)
			}
			nlit := uint32(1)
			for _, op := range group[:n] {
				for _, src := range (&shaderRun{}).srcs(op) {
					if src.sel == 253 && src.ch >= 2 {
						nlit = 2
					}
				}
			}
			slot += nlit
		}
		if t.aluGroup(group[:n], &lit) {
			t.ifNZ(actv)
			nIf++
		}
	}
	for ; nIf > 0; nIf-- {
		t.endif()
	}
}

// aluSrc returns an operand for an ALU source. Integer ops (bits) get the
// neg and abs flags applied as bit operations in scratch register k.
func (t *dxTr) aluSrc(src aluSrc, im uint32, lit *[4]uint32, k int, bitsMods bool) d3d.Opnd {
	sel := src.sel
	var o d3d.Opnd
	rel := t.arIndex(im)
	isConst := false
	var cv uint32
	switch {
	case sel < 128:
		o = t.gprSrc(sel, src.ch, rel, src.rel)
	case sel < 192:
		set := 0
		i := sel - 128
		if sel >= 160 {
			set, i = 1, sel-160
		}
		if t.kcMode[set] == 0 {
			isConst, cv = true, 0
			break
		}
		slot := t.bank(t.kcBank[set])
		o = d3d.CB(slot, int(t.kcAddr[set]*16+i))
		if src.rel {
			o = o.RelIndex(1, rel)
		}
		o = o.C(src.ch)
	case sel == 248:
		isConst, cv = true, 0
	case sel == 249:
		isConst, cv = true, f2u(1)
	case sel == 250:
		isConst, cv = true, 1
	case sel == 251:
		isConst, cv = true, 0xffffffff
	case sel == 252:
		isConst, cv = true, f2u(0.5)
	case sel == 253:
		isConst, cv = true, lit[src.ch]
	case sel == 254:
		prev := (t.group + 1) & 1
		if !t.pvSet[prev][src.ch] {
			isConst, cv = true, 0
		} else {
			o = dxRC(dxPV0+prev, src.ch)
		}
	case sel == 255:
		prev := (t.group + 1) & 1
		if !t.psSet[prev] {
			isConst, cv = true, 0
		} else {
			o = dxRC(dxPS0+prev, 0)
		}
	case sel >= 256:
		o = d3d.CB(dxCBFile, int(sel-256)&255)
		if src.rel {
			o = o.RelIndex(1, rel)
		}
		o = o.C(src.ch)
	default:
		isConst, cv = true, 0
	}
	if isConst {
		if src.abs {
			cv &^= 0x80000000
		}
		if src.neg {
			cv ^= 0x80000000
		}
		return dxU(cv)
	}
	if !src.abs && !src.neg {
		return o
	}
	if !bitsMods {
		if src.abs {
			o = o.Abs()
		}
		if src.neg {
			o = o.Neg()
		}
		return o
	}
	cur := o
	if src.abs {
		t.ins(d3d.OpAnd, sxw(k), cur, dxU(0x7fffffff))
		cur = sx(k)
	}
	if src.neg {
		t.ins(d3d.OpXor, sxw(k), cur, dxU(0x80000000))
		cur = sx(k)
	}
	return cur
}

// bank gives the constant buffer slot of a kcache bank.
func (t *dxTr) bank(b uint32) int {
	if s, ok := t.bankSlot[b]; ok {
		return s
	}
	s := dxCBBank0 + len(t.prog.banks)
	if s > 13 {
		t.fail("too many constant buffers")
	}
	t.bankSlot[b] = s
	t.prog.banks = append(t.prog.banks, b)
	return s
}

// aluGroup translates one instruction group; it reports whether the
// group changed the active flag (so the clause must test it again).
func (t *dxTr) aluGroup(group []aluOp, lit *[4]uint32) bool {
	t.group++
	cur := t.group & 1
	pv := dxPV0 + cur
	ps := dxPS0 + cur
	var newSet [4]bool
	newPS := false
	var vec [4]aluOp
	var hasVec [4]bool
	for _, op := range group {
		if !op.op3 && vectorOnly(op.inst) && op.unit < 4 {
			vec[op.unit] = op
			hasVec[op.unit] = true
		}
	}
	type pending struct {
		op   aluOp
		res  d3d.Opnd
		kind int // 0 plain, 1 pred, 2 kill, 3 mova
	}
	var todo []pending
	execUpdate, predUpdate := false, false
	predSrc := d3d.Opnd{}
	for _, op := range group {
		var dst d3d.Opnd
		var res d3d.Opnd
		if op.unit < 4 {
			dst = dxW(pv, uint32(op.unit))
			res = dxRC(pv, uint32(op.unit))
			newSet[op.unit] = true
		} else {
			dst = dxW(ps, 0)
			res = dxRC(ps, 0)
			newPS = true
		}
		im := op.w0 >> 26 & 7
		kind := 0
		if op.op3 {
			srcs := (&shaderRun{}).srcs(op)
			bitsOp := op.inst >= 0x1c && op.inst <= 0x1e
			var in [3]d3d.Opnd
			for i, s := range srcs {
				in[i] = t.aluSrc(s, im, lit, i, bitsOp)
			}
			t.op3(op.inst, dst, res, in)
		} else if vectorOnly(op.inst) {
			var a, b [4]d3d.Opnd
			for c := 0; c < 4; c++ {
				if hasVec[c] {
					sr := (&shaderRun{}).srcs(vec[c])
					im2 := vec[c].w0 >> 26 & 7
					a[c] = t.aluSrc(sr[0], im2, lit, 2*c, false)
					b[c] = t.aluSrc(sr[1], im2, lit, 2*c+1, false)
				} else {
					a[c], b[c] = dxU(0), dxU(0)
				}
			}
			t.vec4(op.inst, op.unit, dst, res, &a, &b)
		} else {
			srcs := (&shaderRun{}).srcs(op)
			bitsOp := dxBitsOp(op.inst)
			var in [3]d3d.Opnd
			for i, s := range srcs {
				in[i] = t.aluSrc(s, im, lit, i, bitsOp)
			}
			kind = t.op2(op, dst, res, in)
			if kind == 1 {
				ue := op.w1>>2&1 != 0
				up := op.w1>>3&1 != 0
				if ue || up {
					// the predicate result is in scratch 12+unit
					predSrc = sx(12 + min(op.unit, 3))
					if op.unit == 4 {
						t.ins(d3d.OpMov, sxw(11), sx(12+3))
						predSrc = sx(11)
					}
					// keep it past the group: copy into its own place
					t.ins(d3d.OpMov, dxW(dxS+15, 0), predSrc)
					predSrc = sx(15)
					execUpdate = execUpdate || ue
					predUpdate = predUpdate || up
				}
			}
		}
		// output modifier and clamp
		if !(&shaderRun{}).isIntOp(op) {
			omod := uint32(0)
			if !op.op3 {
				omod = op.w1 >> 5 & 3
			}
			switch omod {
			case 1:
				t.ins(d3d.OpMul, dst, res, dxF(2))
			case 2:
				t.ins(d3d.OpMul, dst, res, dxF(4))
			case 3:
				t.ins(d3d.OpMul, dst, res, dxF(0.5))
			}
			if op.w1>>31 != 0 {
				t.ins(d3d.OpMov|d3d.Sat, dst, res)
			}
		}
		todo = append(todo, pending{op, res, kind})
	}
	// zero the PV channels this group didn't set
	var zero uint32
	for c := 0; c < 4; c++ {
		if !newSet[c] {
			zero |= 1 << c
		}
	}
	if !newPS {
		// PS keeps its value through groups without a trans instruction
		prev := (t.group + 1) & 1
		if t.psSet[prev] {
			t.ins(d3d.OpMov, dxW(ps, 0), dxRC(dxPS0+prev, 0))
		}
		newPS = t.psSet[prev]
	}
	t.pvSet[cur] = newSet
	t.psSet[cur] = newPS
	_ = zero
	// write the results
	for _, p := range todo {
		ps := p.op.w0 >> 29 & 3
		if ps >= 2 {
			if ps == 3 {
				t.ifNZ(pred)
			} else {
				t.ifZ(pred)
			}
		}
		if p.kind == 2 {
			// kills: the result is 1.0 / ~0 when the lane dies
			t.ins(d3d.OpIne, sxw(11), p.res, dxU(0))
			t.kill(sx(11))
		}
		if p.op.write {
			rel := p.op.w1>>28&1 != 0
			im := p.op.w0 >> 26 & 7
			t.ins(d3d.OpMov, t.gprDst(p.op.dst, 1<<p.op.dstChan, t.arIndex(im), rel), p.res)
		}
		if ps >= 2 {
			t.endif()
		}
	}
	// the address register
	for _, op := range group {
		if op.op3 {
			continue
		}
		switch op.inst {
		case 0x15, 0x16: // MOVA, MOVA_FLOOR: the result is already rounded
			r := dxRC(pv, uint32(op.unit))
			if op.unit == 4 {
				r = dxRC(ps, 0)
			}
			t.ins(d3d.OpFtoi, dxW(dxAR, op.dstChan), r)
		case 0x18, 0x60: // MOVA_INT
			r := dxRC(pv, uint32(op.unit))
			if op.unit == 4 {
				r = dxRC(ps, 0)
			}
			t.ins(d3d.OpMov, dxW(dxAR, op.dstChan), r)
		}
	}
	if predUpdate {
		t.ins(d3d.OpMov, predW, predSrc)
	}
	if execUpdate {
		t.ins(d3d.OpAnd, actvW, actv, predSrc)
		return true
	}
	return false
}

// dxBitsOp: OP2 instructions whose sources are plain bits (so neg and abs
// work on the bits).
func dxBitsOp(i uint32) bool {
	switch {
	case i == 0x18 || i == 0x19 || i == 0x60 || i == 0x1e || i == 0x1f,
		i >= 0x30 && i <= 0x4f && !(i >= 0x2c && i <= 0x2f),
		i == 0x6c || i == 0x6d,
		i >= 0x70 && i <= 0x78:
		return true
	}
	return false
}

// mulHi: the high 32 bits of a*b, from 16-bit pieces (some Direct3D
// implementations, Wine's among them, don't do the 64-bit multiplies).
func (t *dxTr) mulHi(dst, res, a, b d3d.Opnd, signed bool) {
	t.ins(d3d.OpAnd, sxw(6), a, dxU(0xffff))             // al
	t.ins(d3d.OpUshr, sxw(7), a, dxU(16))                // ah
	t.ins(d3d.OpAnd, sxw(8), b, dxU(0xffff))             // bl
	t.ins(d3d.OpUshr, sxw(9), b, dxU(16))                // bh
	t.ins(d3d.OpImul, d3d.Null(), sxw(10), sx(6), sx(8)) // ll
	t.ins(d3d.OpUshr, sxw(10), sx(10), dxU(16))
	t.ins(d3d.OpImul, d3d.Null(), sxw(6), sx(6), sx(9)) // lh
	t.ins(d3d.OpImul, d3d.Null(), sxw(8), sx(7), sx(8)) // hl
	t.ins(d3d.OpImul, d3d.Null(), sxw(7), sx(7), sx(9)) // hh
	t.ins(d3d.OpAnd, sxw(9), sx(6), dxU(0xffff))
	t.ins(d3d.OpIadd, sxw(10), sx(10), sx(9))
	t.ins(d3d.OpAnd, sxw(9), sx(8), dxU(0xffff))
	t.ins(d3d.OpIadd, sxw(10), sx(10), sx(9)) // mid
	t.ins(d3d.OpUshr, sxw(6), sx(6), dxU(16))
	t.ins(d3d.OpUshr, sxw(8), sx(8), dxU(16))
	t.ins(d3d.OpUshr, sxw(10), sx(10), dxU(16))
	t.ins(d3d.OpIadd, sxw(7), sx(7), sx(6))
	t.ins(d3d.OpIadd, sxw(7), sx(7), sx(8))
	t.ins(d3d.OpIadd, dst, sx(7), sx(10))
	if signed {
		// subtract b if a < 0, and a if b < 0
		t.ins(d3d.OpIshr, sxw(6), a, dxU(31))
		t.ins(d3d.OpAnd, sxw(6), sx(6), b)
		t.ins(d3d.OpIneg, sxw(6), sx(6))
		t.ins(d3d.OpIadd, dst, res, sx(6))
		t.ins(d3d.OpIshr, sxw(6), b, dxU(31))
		t.ins(d3d.OpAnd, sxw(6), sx(6), a)
		t.ins(d3d.OpIneg, sxw(6), sx(6))
		t.ins(d3d.OpIadd, dst, res, sx(6))
	}
}

// mulLegacy: d = a*b, or 0 if either is 0.
func (t *dxTr) mulLegacy(dst, res, a, b d3d.Opnd) {
	t.ins(d3d.OpMul, dst, a, b)
	t.ins(d3d.OpEq, sxw(6), a, dxF(0))
	t.ins(d3d.OpEq, sxw(7), b, dxF(0))
	t.ins(d3d.OpOr, sxw(6), sx(6), sx(7))
	t.ins(d3d.OpMovc, dst, sx(6), dxU(0), res)
}

// compare writes ~0 to dst when "a f b" holds, for the GPU's compare
// functions (0 never, 1 less, 2 equal, 3 less-equal, 4 greater, 5
// not-equal, 6 greater-equal, 7 always).
func (t *dxTr) compare(f uint32, dst, a, b d3d.Opnd) {
	switch f {
	case 0:
		t.ins(d3d.OpMov, dst, dxU(0))
	case 1:
		t.ins(d3d.OpLt, dst, a, b)
	case 2:
		t.ins(d3d.OpEq, dst, a, b)
	case 3:
		t.ins(d3d.OpGe, dst, b, a)
	case 4:
		t.ins(d3d.OpLt, dst, b, a)
	case 5:
		t.ins(d3d.OpNe, dst, a, b)
	case 6:
		t.ins(d3d.OpGe, dst, a, b)
	default:
		t.ins(d3d.OpMov, dst, dxU(0xffffffff))
	}
}

func (t *dxTr) op3(inst uint32, dst, res d3d.Opnd, in [3]d3d.Opnd) {
	a, b, c := in[0], in[1], in[2]
	switch inst {
	case 0x0c, 0x0d, 0x0e, 0x0f: // MUL_LIT
		t.mulLegacy(dst, res, a, b)
		t.ins(d3d.OpGe, sxw(6), dxF(0), c)
		t.ins(d3d.OpMovc, dst, sx(6), dxU(0), res)
	case 0x10, 0x11, 0x12, 0x13: // MULADD
		t.mulLegacy(dst, res, a, b)
		t.ins(d3d.OpAdd, dst, res, c)
		t.scaleMuladd(inst-0x10, dst, res)
	case 0x14, 0x15, 0x16, 0x17: // MULADD_IEEE
		t.ins(d3d.OpMul, dst, a, b)
		t.ins(d3d.OpAdd, dst, res, c)
		t.scaleMuladd(inst-0x14, dst, res)
	case 0x18: // CNDE
		t.ins(d3d.OpEq, sxw(6), a, dxF(0))
		t.ins(d3d.OpMovc, dst, sx(6), b, c)
	case 0x19: // CNDGT
		t.ins(d3d.OpLt, sxw(6), dxF(0), a)
		t.ins(d3d.OpMovc, dst, sx(6), b, c)
	case 0x1a: // CNDGE
		t.ins(d3d.OpGe, sxw(6), a, dxF(0))
		t.ins(d3d.OpMovc, dst, sx(6), b, c)
	case 0x1c: // CNDE_INT
		t.ins(d3d.OpMovc, dst, a, c, b)
	case 0x1d: // CNDGT_INT
		t.ins(d3d.OpIlt, sxw(6), dxU(0), a)
		t.ins(d3d.OpMovc, dst, sx(6), b, c)
	case 0x1e: // CNDGE_INT
		t.ins(d3d.OpIge, sxw(6), a, dxU(0))
		t.ins(d3d.OpMovc, dst, sx(6), b, c)
	default:
		t.fail("unknown ALU OP3 instruction %#x", inst)
	}
}

func (t *dxTr) scaleMuladd(m uint32, dst, res d3d.Opnd) {
	switch m {
	case 1:
		t.ins(d3d.OpMul, dst, res, dxF(2))
	case 2:
		t.ins(d3d.OpMul, dst, res, dxF(4))
	case 3:
		t.ins(d3d.OpMul, dst, res, dxF(0.5))
	}
}

func (t *dxTr) vec4(inst uint32, unit int, dst, res d3d.Opnd, a, b *[4]d3d.Opnd) {
	if inst != 0x52 {
		// every slot of a DOT4 or MAX4 gets the same result: work it out once
		if t.vec4At == t.group+1 {
			t.ins(d3d.OpMov, dst, sx(5))
			return
		}
		t.vec4At = t.group + 1
		defer t.ins(d3d.OpMov, sxw(5), res)
	}
	switch inst {
	case 0x50, 0x51: // DOT4, DOT4_IEEE: summed in order
		for c := 0; c < 4; c++ {
			d := dxW(dxS+12+c%2, 0)
			r := sx(12 + c%2)
			if inst == 0x50 {
				t.mulLegacy(d, r, a[c], b[c])
			} else {
				t.ins(d3d.OpMul, d, a[c], b[c])
			}
			if c == 0 {
				t.ins(d3d.OpMov, dst, r)
			} else {
				t.ins(d3d.OpAdd, dst, res, r)
			}
		}
	case 0x53: // MAX4
		t.ins(d3d.OpMax, dst, a[0], a[1])
		t.ins(d3d.OpMax, dst, res, a[2])
		t.ins(d3d.OpMax, dst, res, a[3])
	case 0x52: // CUBE: src0 = (z, z, x, y)
		x, y, z := a[2], a[3], a[0]
		t.cube(unit, dst, x, y, z)
	}
}

// cube computes one output of CUBE (see cubeResult).
func (t *dxTr) cube(unit int, dst, x, y, z d3d.Opnd) {
	// scratch: 6 |x|, 7 |y|, 8 |z|, 9 z-major, 10 y-major
	t.ins(d3d.OpMov, sxw(6), x.Abs())
	t.ins(d3d.OpMov, sxw(7), y.Abs())
	t.ins(d3d.OpMov, sxw(8), z.Abs())
	t.ins(d3d.OpGe, sxw(9), sx(8), sx(6))
	t.ins(d3d.OpGe, sxw(10), sx(8), sx(7))
	t.ins(d3d.OpAnd, sxw(9), sx(9), sx(10))
	t.ins(d3d.OpGe, sxw(10), sx(7), sx(6))
	switch unit {
	case 0: // tc: z-major -y; y-major (y>=0 ? z : -z); x-major -y
		t.ins(d3d.OpGe, sxw(11), y, dxF(0))
		t.ins(d3d.OpMovc, sxw(11), sx(11), z, z.Neg())
		t.ins(d3d.OpMov, sxw(13), y.Neg())
		t.ins(d3d.OpMovc, sxw(11), sx(10), sx(11), sx(13))
		t.ins(d3d.OpMovc, dst, sx(9), sx(13), sx(11))
	case 1: // sc: z-major (z>=0 ? x : -x); y-major x; x-major (x>=0 ? -z : z)
		t.ins(d3d.OpGe, sxw(11), x, dxF(0))
		t.ins(d3d.OpMovc, sxw(11), sx(11), z.Neg(), z)
		t.ins(d3d.OpMovc, sxw(11), sx(10), x, sx(11))
		t.ins(d3d.OpGe, sxw(13), z, dxF(0))
		t.ins(d3d.OpMovc, sxw(13), sx(13), x, x.Neg())
		t.ins(d3d.OpMovc, dst, sx(9), sx(13), sx(11))
	case 2: // 2 * |major axis|
		t.ins(d3d.OpMovc, sxw(11), sx(10), sx(7), sx(6))
		t.ins(d3d.OpMovc, sxw(11), sx(9), sx(8), sx(11))
		t.ins(d3d.OpAdd, dst, sx(11), sx(11))
	default: // face
		t.ins(d3d.OpGe, sxw(11), x, dxF(0))
		t.ins(d3d.OpMovc, sxw(11), sx(11), dxF(0), dxF(1))
		t.ins(d3d.OpGe, sxw(13), y, dxF(0))
		t.ins(d3d.OpMovc, sxw(13), sx(13), dxF(2), dxF(3))
		t.ins(d3d.OpMovc, sxw(11), sx(10), sx(13), sx(11))
		t.ins(d3d.OpGe, sxw(13), z, dxF(0))
		t.ins(d3d.OpMovc, sxw(13), sx(13), dxF(4), dxF(5))
		t.ins(d3d.OpMovc, dst, sx(9), sx(13), sx(11))
	}
}

// op2 translates an OP2 instruction. It returns 1 for predicate setters
// (whose condition is left in scratch 12+unit), 2 for kills.
func (t *dxTr) op2(op aluOp, dst, res d3d.Opnd, in [3]d3d.Opnd) int {
	a, b := in[0], in[1]
	if op.inst >= 0x08 && op.inst <= 0x0b {
		defer t.ins(d3d.OpAnd, dst, res, dxF(1))
	}
	pu := 12 + min(op.unit, 3)
	pw, pr := sxw(pu), sx(pu)
	one := dxF(1)
	boolF := func(cond d3d.Opnd) { t.ins(d3d.OpAnd, dst, cond, one) }
	switch i := op.inst; i {
	case 0x00:
		t.ins(d3d.OpAdd, dst, a, b)
	case 0x01:
		t.mulLegacy(dst, res, a, b)
	case 0x02:
		t.ins(d3d.OpMul, dst, a, b)
	case 0x03, 0x05:
		t.ins(d3d.OpMax, dst, a, b)
	case 0x04, 0x06:
		t.ins(d3d.OpMin, dst, a, b)
	case 0x08, 0x0c:
		t.ins(d3d.OpEq, dst, a, b)
	case 0x09, 0x0d:
		t.ins(d3d.OpLt, dst, b, a)
	case 0x0a, 0x0e:
		t.ins(d3d.OpGe, dst, a, b)
	case 0x0b, 0x0f:
		t.ins(d3d.OpNe, dst, a, b)
	case 0x10:
		t.ins(d3d.OpFrc, dst, a)
	case 0x11:
		t.ins(d3d.OpRoundZ, dst, a)
	case 0x12:
		t.ins(d3d.OpRoundPi, dst, a)
	case 0x13:
		t.ins(d3d.OpRoundNe, dst, a)
	case 0x14, 0x16:
		t.ins(d3d.OpRoundNi, dst, a)
	case 0x15:
		t.ins(d3d.OpAdd, dst, a, dxF(0.5))
		t.ins(d3d.OpRoundNi, dst, res)
	case 0x18, 0x19, 0x60:
		t.ins(d3d.OpMov, dst, a)
	case 0x1a:
		t.ins(d3d.OpMov, dst, dxU(0))
	case 0x1e, 0x1f: // PRED_SETGT_UINT, PRED_SETGE_UINT
		if i == 0x1e {
			t.ins(d3d.OpUlt, pw, b, a)
		} else {
			t.ins(d3d.OpUge, pw, a, b)
		}
		boolF(pr)
		return 1
	case 0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2a, 0x2b:
		switch i & 3 {
		case 0:
			t.ins(d3d.OpEq, pw, a, b)
		case 1:
			t.ins(d3d.OpLt, pw, b, a)
		case 2:
			t.ins(d3d.OpGe, pw, a, b)
		case 3:
			t.ins(d3d.OpNe, pw, a, b)
		}
		boolF(pr)
		return 1
	case 0x24: // PRED_SET_INV
		t.ins(d3d.OpEq, pw, a, dxF(0))
		t.ins(d3d.OpMovc, dst, pr, one, a)
		return 1
	case 0x25: // PRED_SET_POP
		t.ins(d3d.OpGe, pw, dxF(0), a)
		t.ins(d3d.OpAdd, sxw(6), a, dxF(-1))
		t.ins(d3d.OpMovc, dst, pr, dxU(0), sx(6))
		return 1
	case 0x26: // PRED_SET_CLR
		t.ins(d3d.OpMov, pw, dxU(0))
		t.ins(d3d.OpMov, dst, dxU(0x7f800000))
		return 1
	case 0x27: // PRED_SET_RESTORE
		t.ins(d3d.OpEq, pw, a, dxF(0))
		t.ins(d3d.OpMov, dst, a)
		return 1
	case 0x2c, 0x2d, 0x2e, 0x2f: // KILL*
		switch i {
		case 0x2c:
			t.ins(d3d.OpEq, sxw(6), a, b)
		case 0x2d:
			t.ins(d3d.OpLt, sxw(6), b, a)
		case 0x2e:
			t.ins(d3d.OpGe, sxw(6), a, b)
		case 0x2f:
			t.ins(d3d.OpNe, sxw(6), a, b)
		}
		t.ins(d3d.OpAnd, dst, sx(6), one)
		return 2
	case 0x30:
		t.ins(d3d.OpAnd, dst, a, b)
	case 0x31:
		t.ins(d3d.OpOr, dst, a, b)
	case 0x32:
		t.ins(d3d.OpXor, dst, a, b)
	case 0x33:
		t.ins(d3d.OpNot, dst, a)
	case 0x34:
		t.ins(d3d.OpIadd, dst, a, b)
	case 0x35:
		t.ins(d3d.OpIneg, sxw(6), b)
		t.ins(d3d.OpIadd, dst, a, sx(6))
	case 0x36:
		t.ins(d3d.OpImax, dst, a, b)
	case 0x37:
		t.ins(d3d.OpImin, dst, a, b)
	case 0x38:
		t.ins(d3d.OpUmax, dst, a, b)
	case 0x39:
		t.ins(d3d.OpUmin, dst, a, b)
	case 0x3a:
		t.ins(d3d.OpIeq, dst, a, b)
	case 0x3b:
		t.ins(d3d.OpIlt, dst, b, a)
	case 0x3c:
		t.ins(d3d.OpIge, dst, a, b)
	case 0x3d:
		t.ins(d3d.OpIne, dst, a, b)
	case 0x3e:
		t.ins(d3d.OpUlt, dst, b, a)
	case 0x3f:
		t.ins(d3d.OpUge, dst, a, b)
	case 0x40, 0x41: // KILLGT_UINT, KILLGE_UINT
		if i == 0x40 {
			t.ins(d3d.OpUlt, dst, b, a)
		} else {
			t.ins(d3d.OpUge, dst, a, b)
		}
		return 2
	case 0x42, 0x43, 0x44, 0x45, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f:
		switch i {
		case 0x42, 0x4a:
			t.ins(d3d.OpIeq, pw, a, b)
		case 0x43, 0x4b:
			t.ins(d3d.OpIlt, pw, b, a)
		case 0x44, 0x4c:
			t.ins(d3d.OpIge, pw, a, b)
		case 0x45, 0x4d:
			t.ins(d3d.OpIne, pw, a, b)
		case 0x4e:
			t.ins(d3d.OpIlt, pw, a, b)
		case 0x4f:
			t.ins(d3d.OpIge, pw, b, a)
		}
		t.ins(d3d.OpAnd, dst, pr, dxU(1))
		return 1
	case 0x46, 0x47, 0x48, 0x49: // KILL*_INT
		switch i {
		case 0x46:
			t.ins(d3d.OpIeq, dst, a, b)
		case 0x47:
			t.ins(d3d.OpIlt, dst, b, a)
		case 0x48:
			t.ins(d3d.OpIge, dst, a, b)
		case 0x49:
			t.ins(d3d.OpIne, dst, a, b)
		}
		return 2
	case 0x61:
		t.ins(d3d.OpExp, dst, a)
	case 0x62: // LOG_CLAMPED
		t.ins(d3d.OpLog, dst, a)
		t.negNaN(dst, a, res)
		t.ins(d3d.OpEq, sxw(6), a, dxF(0))
		t.ins(d3d.OpMovc, dst, sx(6), dxF(-math.MaxFloat32), res)
	case 0x63:
		t.ins(d3d.OpLog, dst, a)
		t.negNaN(dst, a, res)
	case 0x64, 0x65, 0x66: // RECIP_CLAMPED, RECIP_FF, RECIP_IEEE
		t.ins(d3d.OpDiv, dst, one, a)
		if i != 0x66 {
			t.ins(d3d.OpEq, sxw(6), a, dxF(0))
			z := dxU(0)
			if i == 0x64 {
				// copysign(FLT_MAX, a)
				t.ins(d3d.OpMov, sxw(7), a) // a's modifiers are float ones
				t.ins(d3d.OpAnd, sxw(7), sx(7), dxU(0x80000000))
				t.ins(d3d.OpOr, sxw(7), sx(7), dxU(f2u(math.MaxFloat32)))
				z = sx(7)
			}
			t.ins(d3d.OpMovc, dst, sx(6), z, res)
		}
	case 0x67, 0x68: // RECIPSQRT_CLAMPED, RECIPSQRT_FF
		t.ins(d3d.OpRsq, dst, a.Abs())
		t.ins(d3d.OpEq, sxw(6), a, dxF(0))
		z := dxU(0)
		if i == 0x67 {
			z = dxF(math.MaxFloat32)
		}
		t.ins(d3d.OpMovc, dst, sx(6), z, res)
	case 0x69:
		t.ins(d3d.OpRsq, dst, a)
		t.negNaN(dst, a, res)
		// -0 (and negative denormals, which flush to it) give -inf
		t.ins(d3d.OpMov, sxw(6), a) // a's modifiers are float ones
		t.ins(d3d.OpIadd, sxw(6), sx(6), dxU(0x80000000))
		t.ins(d3d.OpUlt, sxw(6), sx(6), dxU(0x800000))
		t.ins(d3d.OpMovc, dst, sx(6), dxU(0xff800000), res)
	case 0x6a:
		t.ins(d3d.OpSqrt, dst, a)
		t.negNaN(dst, a, res)
	case 0x6b:
		t.ins(d3d.OpFtoi, dst, a)
	case 0x6c:
		t.ins(d3d.OpItof, dst, a)
	case 0x6d:
		t.ins(d3d.OpUtof, dst, a)
	case 0x6e, 0x6f: // SIN, COS of turns, reduced to -0.5..0.5 first
		t.ins(d3d.OpRoundNe, sxw(6), a)
		t.ins(d3d.OpAdd, sxw(6), a, sx(6).Neg())
		t.ins(d3d.OpMul, sxw(6), sx(6), dxF(2*math.Pi))
		if i == 0x6e {
			t.ins(d3d.OpSincos, dst, d3d.Null(), sx(6))
		} else {
			t.ins(d3d.OpSincos, d3d.Null(), dst, sx(6))
		}
	case 0x70:
		t.ins(d3d.OpIshr, dst, a, b)
	case 0x71:
		t.ins(d3d.OpUshr, dst, a, b)
	case 0x72:
		t.ins(d3d.OpIshl, dst, a, b)
	case 0x73:
		t.ins(d3d.OpImul, d3d.Null(), dst, a, b)
	case 0x74:
		t.mulHi(dst, res, a, b, true)
	case 0x75:
		t.ins(d3d.OpUmul, d3d.Null(), dst, a, b)
	case 0x76:
		t.mulHi(dst, res, a, b, false)
	case 0x77, 0x78: // RECIP_INT, RECIP_UINT: 2^32 / x
		x := a
		if i == 0x77 {
			// |x|, sign kept in scratch 8
			t.ins(d3d.OpIshr, sxw(8), a, dxU(31))
			t.ins(d3d.OpIneg, sxw(9), a)
			t.ins(d3d.OpImax, sxw(9), a, sx(9))
			x = sx(9)
		}
		// q, r = 0xffffffff / x; result q + (r == x-1)
		t.ins(d3d.OpUdiv, sxw(6), sxw(7), dxU(0xffffffff), x)
		t.ins(d3d.OpIadd, sxw(10), x, d3d.ImmI(-1))
		t.ins(d3d.OpIeq, sxw(10), sx(7), sx(10))
		t.ins(d3d.OpAnd, sxw(10), sx(10), dxU(1))
		t.ins(d3d.OpIadd, dst, sx(6), sx(10))
		if i == 0x77 {
			t.ins(d3d.OpIneg, sxw(10), res)
			t.ins(d3d.OpMovc, dst, sx(8), sx(10), res)
		}
		t.ins(d3d.OpMovc, dst, x, res, dxU(0))
	case 0x79:
		t.ins(d3d.OpFtou, dst, a)
	default:
		t.fail("unknown ALU OP2 instruction %#x", op.inst)
	}
	return 0
}

// negNaN makes dst NaN where a is below zero (log and square roots of
// negatives aren't NaN on every host).
func (t *dxTr) negNaN(dst, a, res d3d.Opnd) {
	t.ins(d3d.OpLt, sxw(6), a, dxF(0))
	t.ins(d3d.OpMovc, dst, sx(6), dxU(0x7fc00000), res)
}
