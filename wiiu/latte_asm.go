package wiiu

// A small assembler for Latte (R700) shader code. GX2 needs it to build
// fetch shaders (GX2InitFetchShaderEx), and the tests use it to write
// vertex and pixel shaders.
//
// A program is laid out as the GPU wants it: the control-flow instructions
// first, then the ALU, texture and vertex clauses they point at.

type shaderAsm struct {
	cf      [][2]uint32 // control flow
	clauses []clause
}

type clause struct {
	alu   bool
	words []uint32 // ALU: 2 per slot; fetch: 4 per instruction
	cfIdx int      // the CF instruction that points at it
}

// alu source selects
const (
	srcKC0     = 128
	srcKC1     = 160
	srcZero    = 248
	srcOne     = 249
	srcIntOne  = 250
	srcIntMOne = 251
	srcHalf    = 252
	srcLiteral = 253
	srcPV      = 254
	srcPS      = 255
	srcCfile   = 256
)

// aluSrcSpec is one ALU operand: sel (a GPR 0-127 or a special select),
// channel, negate, absolute value.
type aluSrcSpec struct {
	sel, ch  uint32
	neg, abs bool
}

func gprSrc(r, ch uint32) aluSrcSpec { return aluSrcSpec{sel: r, ch: ch} }
func cfileSrc(i, ch uint32) aluSrcSpec {
	return aluSrcSpec{sel: srcCfile + i, ch: ch}
}
func litSrc(ch uint32) aluSrcSpec { return aluSrcSpec{sel: srcLiteral, ch: ch} }
func specialSrc(sel uint32) aluSrcSpec { return aluSrcSpec{sel: sel} }
func (s aluSrcSpec) negate() aluSrcSpec { s.neg = !s.neg; return s }

// aluInst is one slot of an ALU group.
type aluInst struct {
	op3     bool
	inst    uint32
	src     [3]aluSrcSpec
	dst     uint32
	dstChan uint32
	write   bool
	clamp   bool
	omod    uint32
	predSel uint32
	upExec  bool
	upPred  bool
}

func op2(inst, dst, ch uint32, a, b aluSrcSpec) aluInst {
	return aluInst{inst: inst, dst: dst, dstChan: ch, write: true, src: [3]aluSrcSpec{a, b}}
}

func op3i(inst, dst, ch uint32, a, b, c aluSrcSpec) aluInst {
	return aluInst{op3: true, inst: inst, dst: dst, dstChan: ch, write: true, src: [3]aluSrcSpec{a, b, c}}
}

func encSrc(s aluSrcSpec) (sel, ch, neg uint32) {
	return s.sel & 0x1ff, s.ch & 3, b2u(s.neg)
}

func (a aluInst) words(last bool) (uint32, uint32) {
	s0, c0, n0 := encSrc(a.src[0])
	s1, c1, n1 := encSrc(a.src[1])
	w0 := s0 | c0<<10 | n0<<12 | s1<<13 | c1<<23 | n1<<25 | (a.predSel&3)<<29 | b2u(last)<<31
	var w1 uint32
	if a.op3 {
		s2, c2, n2 := encSrc(a.src[2])
		w1 = s2 | c2<<10 | n2<<12 | (a.inst&0x1f)<<13
	} else {
		w1 = b2u(a.src[0].abs) | b2u(a.src[1].abs)<<1 | b2u(a.upExec)<<2 | b2u(a.upPred)<<3 |
			b2u(a.write)<<4 | (a.omod&3)<<5 | (a.inst&0x7ff)<<7
	}
	w1 |= (a.dst&0x7f)<<21 | (a.dstChan&3)<<29 | b2u(a.clamp)<<31
	return w0, w1
}

// aluGroup encodes one instruction group with its literals.
func aluGroupWords(group []aluInst, lits []uint32) []uint32 {
	var w []uint32
	for i, a := range group {
		w0, w1 := a.words(i == len(group)-1)
		w = append(w, w0, w1)
	}
	if len(lits) > 0 {
		l := append([]uint32{}, lits...)
		for len(l)%2 != 0 || len(l) == 0 {
			l = append(l, 0)
		}
		w = append(w, l...)
	}
	return w
}

// ALU adds an ALU clause made of groups (each a slice of instructions with
// optional literals); kcache sets up constant buffer banks.
type aluGroupSpec struct {
	insts []aluInst
	lits  []uint32
}

type kcacheSpec struct{ bank, mode, addr uint32 }

func (p *shaderAsm) ALU(cfInst uint32, kc [2]kcacheSpec, groups ...aluGroupSpec) {
	var words []uint32
	for _, g := range groups {
		words = append(words, aluGroupWords(g.insts, g.lits)...)
	}
	idx := len(p.cf)
	slots := uint32(len(words) / 2)
	w0 := (kc[0].bank&0xf)<<22 | (kc[1].bank&0xf)<<26 | (kc[0].mode&3)<<30
	w1 := kc[1].mode&3 | (kc[0].addr&0xff)<<2 | (kc[1].addr&0xff)<<10 | ((slots-1)&0x7f)<<18 | (cfInst&0xf)<<26 | 1<<31
	p.cf = append(p.cf, [2]uint32{w0, w1})
	p.clauses = append(p.clauses, clause{alu: true, words: words, cfIdx: idx})
}

// fetch instructions

type vtxSpec struct {
	semantic   bool // VTX_SEMANTIC (dst from SQ_VTX_SEMANTIC) instead of VTX_FETCH
	semanticID uint32
	fetchType  uint32 // 0 vertex, 1 instance, 2 no index offset
	buffer     uint32
	srcGPR     uint32
	srcSel     uint32
	dstGPR     uint32
	dstSel     [4]uint32
	useConst   bool
	format     uint32
	numFormat  uint32 // 0 norm, 1 int, 2 scaled
	signed     bool
	offset     uint32
	endian     uint32
	megaCount  uint32
}

func (v vtxSpec) words() []uint32 {
	inst := uint32(0)
	if v.semantic {
		inst = 1
	}
	mega := v.megaCount
	if mega == 0 {
		mega = max(fmtBytes(v.format), 1)
	}
	w0 := inst | (v.fetchType&3)<<5 | (v.buffer&0xff)<<8 | (v.srcGPR&0x7f)<<16 | (v.srcSel&3)<<24 | ((mega-1)&0x3f)<<26
	var w1 uint32
	if v.semantic {
		w1 = v.semanticID & 0xff
	} else {
		w1 = v.dstGPR & 0x7f
	}
	w1 |= (v.dstSel[0]&7)<<9 | (v.dstSel[1]&7)<<12 | (v.dstSel[2]&7)<<15 | (v.dstSel[3]&7)<<18 |
		b2u(v.useConst)<<21 | (v.format&0x3f)<<22 | (v.numFormat&3)<<28 | b2u(v.signed)<<30
	w2 := v.offset&0xffff | (v.endian&3)<<16 | 1<<19 // MEGA_FETCH
	return []uint32{w0, w1, w2, 0}
}

type texSpec struct {
	inst       uint32 // 0x10 SAMPLE, ...
	resource   uint32
	sampler    uint32
	srcGPR     uint32
	srcSel     [4]uint32
	dstGPR     uint32
	dstSel     [4]uint32
	normalized [4]bool
	lodBias    int32
	offset     [3]int32
}

func (t texSpec) words() []uint32 {
	w0 := t.inst&0x1f | (t.resource&0xff)<<8 | (t.srcGPR&0x7f)<<16
	w1 := t.dstGPR&0x7f | (t.dstSel[0]&7)<<9 | (t.dstSel[1]&7)<<12 | (t.dstSel[2]&7)<<15 | (t.dstSel[3]&7)<<18 |
		uint32(t.lodBias&0x7f)<<21 | b2u(t.normalized[0])<<28 | b2u(t.normalized[1])<<29 | b2u(t.normalized[2])<<30 | b2u(t.normalized[3])<<31
	w2 := uint32(t.offset[0]&0x1f) | uint32(t.offset[1]&0x1f)<<5 | uint32(t.offset[2]&0x1f)<<10 | (t.sampler&0x1f)<<15 |
		(t.srcSel[0]&7)<<20 | (t.srcSel[1]&7)<<23 | (t.srcSel[2]&7)<<26 | (t.srcSel[3]&7)<<29
	return []uint32{w0, w1, w2, 0}
}

func (p *shaderAsm) fetchClause(cfInst uint32, ins [][]uint32) {
	var words []uint32
	for _, w := range ins {
		words = append(words, w...)
	}
	n := uint32(len(ins))
	idx := len(p.cf)
	w1 := ((n-1)&7)<<10 | ((n-1)>>3&1)<<19 | cfInst<<23 | 1<<31
	p.cf = append(p.cf, [2]uint32{0, w1})
	p.clauses = append(p.clauses, clause{words: words, cfIdx: idx})
}

func (p *shaderAsm) VTX(ins ...vtxSpec) {
	var w [][]uint32
	for _, v := range ins {
		w = append(w, v.words())
	}
	p.fetchClause(cfVTX, w)
}

func (p *shaderAsm) TEX(ins ...texSpec) {
	var w [][]uint32
	for _, t := range ins {
		w = append(w, t.words())
	}
	p.fetchClause(cfTEX, w)
}

// CF adds a plain control-flow instruction.
func (p *shaderAsm) CF(inst, addr, cond, cfConst, popCount uint32) int {
	w1 := popCount&7 | (cfConst&0x1f)<<3 | (cond&3)<<8 | inst<<23 | 1<<31
	p.cf = append(p.cf, [2]uint32{addr, w1})
	return len(p.cf) - 1
}

// Export: typ 0 pixel, 1 position, 2 parameter; sel are the channel selects
// (0-3 xyzw, 4 zero, 5 one, 7 masked).
func (p *shaderAsm) Export(done bool, typ, base, gpr, burst uint32, sel [4]uint32) {
	inst := uint32(cfEXPORT)
	if done {
		inst = cfEXPORT_DONE
	}
	w0 := base&0x1fff | (typ&3)<<13 | (gpr&0x7f)<<15
	w1 := sel[0]&7 | (sel[1]&7)<<3 | (sel[2]&7)<<6 | (sel[3]&7)<<9 | ((max(burst, 1)-1)&0xf)<<17 | inst<<23 | 1<<31
	p.cf = append(p.cf, [2]uint32{w0, w1})
}

// End marks the last CF instruction as the end of the program.
func (p *shaderAsm) End() {
	if len(p.cf) == 0 {
		p.CF(cfNOP, 0, 0, 0, 0)
	}
	p.cf[len(p.cf)-1][1] |= 1 << 21
}

// Words lays the program out: CF instructions, then the clauses (fetch
// clauses aligned to 16 bytes).
func (p *shaderAsm) Words() []uint32 {
	cf := make([][2]uint32, len(p.cf))
	copy(cf, p.cf)
	out := make([]uint32, 0, len(cf)*2)
	pos := uint32(len(cf)) // in 64-bit units
	var body []uint32
	for _, c := range p.clauses {
		if !c.alu && pos%2 != 0 {
			body = append(body, 0, 0)
			pos++
		}
		cf[c.cfIdx][0] = cf[c.cfIdx][0]&^0x3fffff | pos
		if !c.alu {
			cf[c.cfIdx][0] = pos
		}
		body = append(body, c.words...)
		pos += uint32(len(c.words) / 2)
	}
	for _, w := range cf {
		out = append(out, w[0], w[1])
	}
	return append(out, body...)
}

// Bytes gives the program as the GPU reads it (little-endian words).
func (p *shaderAsm) Bytes() []byte {
	w := p.Words()
	b := make([]byte, len(w)*4)
	for i, v := range w {
		b[i*4], b[i*4+1], b[i*4+2], b[i*4+3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
	}
	return b
}
