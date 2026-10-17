package wiiu

// The Latte shader interpreter: runs the GPU's R700-family shader programs
// (vertex, pixel and fetch shaders) on four lanes at once, the way the GPU
// runs a 2x2 quad of pixels (so texture sampling can work out how far apart
// neighbouring pixels' coordinates are).
//
// A program is a list of control-flow (CF) instructions that start clauses
// of ALU instructions (in groups of up to five, for the x, y, z, w and t
// units), texture fetches or vertex fetches, export results, and branch and
// loop with a stack of lane masks. Shader code is little-endian in memory.

import (
	"math"
	"math/bits"
)

const nl = 4 // lanes

type lanes [nl]uint32
type laneMask [nl]bool

func (m laneMask) any() bool { return m[0] || m[1] || m[2] || m[3] }

// shaderEnv is what a shader reads and writes outside its registers.
type shaderEnv interface {
	// read32 reads GPU memory (little-endian words).
	read32(addr uint32) uint32
	// cfile: the ALU constant file (uniform registers), index 0-255
	cfile(i, ch uint32) uint32
	// kcache: constant buffer bank (0-15), constant index, channel
	kcache(bank, i, ch uint32) uint32
	loopConst(i uint32) uint32
	boolConst(i uint32) bool
	// fetch: a vertex-fetch instruction for the active lanes
	fetch(s *shaderRun, w0, w1, w2 uint32, mask laneMask)
	// tex: a texture-fetch instruction for the active lanes
	tex(s *shaderRun, w0, w1, w2 uint32, mask laneMask)
	// export: type 0 pixel, 1 position, 2 parameter; base = array base
	export(s *shaderRun, typ, base uint32, v *[4]lanes, mask laneMask)
	// fetchShader: where CALL_FS goes
	fetchShader() uint32
}

type loopRec struct {
	start, end   uint32 // body start (after LOOP_START), instruction after LOOP_END
	saved        laneMask
	broken       laneMask
	cont         laneMask
	count        int32
	index, inc   int32
	pushDepth    int
	dx10, active bool
}

// shaderRun is one run of a program over four lanes.
type shaderRun struct {
	env      shaderEnv
	base     uint32 // the program's address
	gpr      [128][4]lanes
	ar       [4]lanes // address register (MOVA)
	pv       [4]lanes
	ps       lanes
	active   laneMask
	pred     laneMask // the predicate bits PRED_SET* leaves
	valid    laneMask
	killed   laneMask
	stack    []laneMask
	loops    []loopRec
	calls    []uint32
	callBase []uint32
	steps    int
	Err      string
	// for pixel shaders: lanes' helper status (outside the triangle)
	helper laneMask
	// kcache set up by the current ALU clause
	kcBank [2]uint32
	kcAddr [2]uint32
	kcMode [2]uint32
	// trace, when set, gets a line per instruction (debugging)
	trace func(string)
	// gradients set by SET_GRADIENTS_H/V
	gradH, gradV [4]lanes
	gradSet      bool
}

func f2u(f float32) uint32 { return math.Float32bits(f) }
func u2f(u uint32) float32 { return math.Float32frombits(u) }

const maxShaderSteps = 1 << 20

// run executes the program at base until END_OF_PROGRAM (or a RETURN from
// the top level).
func (s *shaderRun) run(base uint32) {
	s.base = base
	s.active = s.valid
	pc := uint32(0)
	for {
		s.steps++
		if s.steps > maxShaderSteps {
			s.Err = "shader ran too long"
			return
		}
		addr := s.base + pc*8
		w0 := s.env.read32(addr)
		w1 := s.env.read32(addr + 4)
		pc++
		end, next := s.cf(pc-1, w0, w1)
		if next >= 0 {
			pc = uint32(next)
		}
		if end || s.Err != "" {
			return
		}
	}
}

// cond evaluates a CF instruction's COND field on top of the active lanes.
func (s *shaderRun) cond(c, cfConst uint32) laneMask {
	m := s.active
	switch c {
	case 1: // CF_COND_BOOL
		if !s.env.boolConst(cfConst) {
			m = laneMask{}
		}
	case 2: // CF_COND_NOT_BOOL
		if s.env.boolConst(cfConst) {
			m = laneMask{}
		}
	}
	return m
}

func (s *shaderRun) push() { s.stack = append(s.stack, s.active) }

func (s *shaderRun) pop(n uint32) {
	for ; n > 0; n-- {
		if len(s.stack) == 0 {
			s.active = s.valid
			break
		}
		s.active = s.stack[len(s.stack)-1]
		s.stack = s.stack[:len(s.stack)-1]
	}
	s.maskLoop()
}

// maskLoop keeps lanes that broke out of (or continued) the current loop off.
func (s *shaderRun) maskLoop() {
	if len(s.loops) == 0 {
		return
	}
	l := &s.loops[len(s.loops)-1]
	for i := range s.active {
		if l.broken[i] || l.cont[i] {
			s.active[i] = false
		}
	}
}

func (s *shaderRun) killLanes() {
	for i := range s.active {
		if s.killed[i] {
			s.active[i] = false
		}
	}
}

// CF instructions
const (
	cfNOP              = 0
	cfTEX              = 1
	cfVTX              = 2
	cfVTX_TC           = 3
	cfLOOP_START       = 4
	cfLOOP_END         = 5
	cfLOOP_START_DX10  = 6
	cfLOOP_START_NO_AL = 7
	cfLOOP_CONTINUE    = 8
	cfLOOP_BREAK       = 9
	cfJUMP             = 10
	cfPUSH             = 11
	cfPUSH_ELSE        = 12
	cfELSE             = 13
	cfPOP              = 14
	cfPOP_JUMP         = 15
	cfPOP_PUSH         = 16
	cfPOP_PUSH_ELSE    = 17
	cfCALL             = 18
	cfCALL_FS          = 19
	cfRETURN           = 20
	cfEMIT_VERTEX      = 21
	cfEMIT_CUT         = 22
	cfCUT_VERTEX       = 23
	cfKILL             = 24
	cfWAIT_ACK         = 26
	cfTEX_ACK          = 27
	cfVTX_ACK          = 28
	cfVTX_TC_ACK       = 29

	cfMEM_STREAM0 = 0x20
	cfMEM_SCRATCH = 0x24
	cfMEM_REDUCT  = 0x25
	cfMEM_RING    = 0x26
	cfEXPORT      = 0x27
	cfEXPORT_DONE = 0x28
)

// cf runs one CF instruction; next >= 0 is a jump.
func (s *shaderRun) cf(pc, w0, w1 uint32) (end bool, next int64) {
	next = -1
	if w1>>29&1 != 0 { // ALU clause (CF_INST in bits 29:26 = 8..15; no END_OF_PROGRAM bit)
		endProg := false
		inst := w1 >> 26 & 0xf
		addr := w0 & 0x3fffff
		count := (w1 >> 18 & 0x7f) + 1
		s.kcBank = [2]uint32{w0 >> 22 & 0xf, w0 >> 26 & 0xf}
		s.kcMode = [2]uint32{w0 >> 30 & 3, w1 & 3}
		s.kcAddr = [2]uint32{w1 >> 2 & 0xff, w1 >> 10 & 0xff}
		switch inst {
		case 8: // ALU
			s.aluClause(addr, count)
		case 9: // ALU_PUSH_BEFORE
			s.push()
			s.aluClause(addr, count)
		case 10: // ALU_POP_AFTER
			s.aluClause(addr, count)
			s.pop(1)
		case 11: // ALU_POP2_AFTER
			s.aluClause(addr, count)
			s.pop(2)
		case 13, 14: // ALU_CONTINUE, ALU_BREAK
			before := s.active
			s.aluClause(addr, count)
			if len(s.loops) > 0 {
				l := &s.loops[len(s.loops)-1]
				for i := range before {
					if before[i] && s.active[i] {
						if inst == 14 {
							l.broken[i] = true
						} else {
							l.cont[i] = true
						}
					}
				}
			}
			s.active = before
			s.maskLoop()
		case 15: // ALU_ELSE_AFTER
			s.aluClause(addr, count)
			s.doElse()
		default:
			s.aluClause(addr, count)
		}
		return endProg, -1
	}
	endProg := w1>>21&1 != 0
	inst := w1 >> 23 & 0x7f
	if inst >= cfMEM_STREAM0 {
		s.exportCF(inst, w0, w1)
		return endProg, -1
	}
	addr := w0
	popCount := w1 & 7
	cfConst := w1 >> 3 & 0x1f
	cnd := w1 >> 8 & 3
	count := (w1>>10&7 | (w1>>19&1)<<3) + 1
	switch inst {
	case cfNOP, cfWAIT_ACK, cfTEX_ACK, cfVTX_ACK, cfVTX_TC_ACK:
	case cfTEX:
		s.texClause(addr, count)
	case cfVTX, cfVTX_TC:
		s.vtxClause(addr, count)
	case cfLOOP_START, cfLOOP_START_DX10, cfLOOP_START_NO_AL:
		m := s.cond(cnd, cfConst)
		l := loopRec{start: pc + 1, end: addr, saved: s.active, pushDepth: len(s.stack), dx10: inst == cfLOOP_START_DX10}
		if inst != cfLOOP_START_DX10 {
			lc := s.env.loopConst(cfConst)
			l.count = int32(lc & 0xfff)
			l.index = int32(lc >> 12 & 0xfff)
			l.inc = int32(int8(lc >> 24))
			if l.count == 0 {
				m = laneMask{}
			}
		} else {
			l.count = -1
		}
		if !m.any() {
			s.active = l.saved
			return endProg, int64(addr)
		}
		s.active = m
		s.loops = append(s.loops, l)
	case cfLOOP_END:
		if len(s.loops) == 0 {
			break
		}
		l := &s.loops[len(s.loops)-1]
		if l.count > 0 {
			l.count--
		}
		l.index += l.inc
		var again laneMask
		for i := range again {
			again[i] = l.saved[i] && !l.broken[i] && !s.killed[i]
		}
		l.cont = laneMask{}
		if again.any() && l.count != 0 {
			s.active = again
			s.stack = s.stack[:min(len(s.stack), l.pushDepth)]
			return endProg, int64(addr)
		}
		s.active = l.saved
		s.killLanes()
		s.stack = s.stack[:min(len(s.stack), l.pushDepth)]
		s.loops = s.loops[:len(s.loops)-1]
		s.maskLoop()
	case cfLOOP_BREAK, cfLOOP_CONTINUE:
		m := s.cond(cnd, cfConst)
		if len(s.loops) > 0 {
			l := &s.loops[len(s.loops)-1]
			for i := range m {
				if m[i] {
					if inst == cfLOOP_BREAK {
						l.broken[i] = true
					} else {
						l.cont[i] = true
					}
					s.active[i] = false
				}
			}
		}
		if !s.active.any() {
			// ADDR is the LOOP_END
			s.stack = s.stack[:min(len(s.stack), s.loopDepth())]
			return endProg, int64(addr)
		}
	case cfJUMP:
		m := s.cond(cnd, cfConst)
		if !m.any() {
			s.pop(popCount)
			return endProg, int64(addr)
		}
		s.active = m
	case cfPUSH:
		s.push()
		m := s.cond(cnd, cfConst)
		s.active = m
		if !m.any() {
			s.pop(popCount + 0)
			return endProg, int64(addr)
		}
	case cfPUSH_ELSE:
		s.push()
		s.doElse()
	case cfELSE:
		s.pop(popCount)
		s.doElse()
		if !s.active.any() {
			return endProg, int64(addr)
		}
	case cfPOP:
		s.pop(popCount)
	case cfPOP_JUMP:
		s.pop(popCount)
		if !s.cond(cnd, cfConst).any() {
			return endProg, int64(addr)
		}
	case cfPOP_PUSH:
		s.pop(popCount)
		s.push()
	case cfPOP_PUSH_ELSE:
		s.pop(popCount)
		s.push()
		s.doElse()
	case cfCALL, cfCALL_FS:
		m := s.cond(cnd, cfConst)
		if !m.any() {
			break
		}
		if len(s.calls) > 32 {
			s.Err = "shader calls nest too deep"
			return true, -1
		}
		s.calls = append(s.calls, pc+1)
		s.callBase = append(s.callBase, s.base)
		if inst == cfCALL_FS {
			s.base = s.env.fetchShader()
			return endProg, 0
		}
		return endProg, int64(addr)
	case cfRETURN:
		if len(s.calls) == 0 {
			return true, -1
		}
		r := s.calls[len(s.calls)-1]
		s.base = s.callBase[len(s.callBase)-1]
		s.calls = s.calls[:len(s.calls)-1]
		s.callBase = s.callBase[:len(s.callBase)-1]
		return false, int64(r)
	case cfKILL:
		m := s.cond(cnd, cfConst)
		for i := range m {
			if m[i] {
				s.killed[i] = true
				s.active[i] = false
			}
		}
	case cfEMIT_VERTEX, cfEMIT_CUT, cfCUT_VERTEX:
		// geometry shaders aren't run yet
	default:
		s.Err = "unknown CF instruction " + hex32(inst)
		return true, -1
	}
	return endProg, next
}

func (s *shaderRun) loopDepth() int {
	if len(s.loops) == 0 {
		return 0
	}
	return s.loops[len(s.loops)-1].pushDepth
}

// doElse: lanes that were active before the last push and aren't now.
func (s *shaderRun) doElse() {
	top := s.valid
	if len(s.stack) > 0 {
		top = s.stack[len(s.stack)-1]
	}
	for i := range s.active {
		s.active[i] = top[i] && !s.active[i]
	}
	s.killLanes()
	s.maskLoop()
}

func hex32(v uint32) string {
	const d = "0123456789ABCDEF"
	b := []byte("0x00000000")
	for i := 0; i < 8; i++ {
		b[9-i] = d[v>>(4*i)&15]
	}
	return string(b)
}

// exportCF: EXPORT / EXPORT_DONE and memory writes.
func (s *shaderRun) exportCF(inst, w0, w1 uint32) {
	arrayBase := w0 & 0x1fff
	typ := w0 >> 13 & 3
	gpr := w0 >> 15 & 0x7f
	rel := w0>>22&1 != 0
	burst := (w1 >> 17 & 0xf) + 1
	if inst != cfEXPORT && inst != cfEXPORT_DONE {
		// memory exports (stream out, scratch, ring): not used by what runs yet
		return
	}
	sel := [4]uint32{w1 & 7, w1 >> 3 & 7, w1 >> 6 & 7, w1 >> 9 & 7}
	m := s.active
	if typ == 0 { // pixel exports skip killed pixels
		for i := range m {
			m[i] = m[i] && !s.killed[i] && !s.helper[i]
		}
	}
	for b := uint32(0); b < burst; b++ {
		g := gpr + b
		var v [4]lanes
		for c := 0; c < 4; c++ {
			for l := 0; l < nl; l++ {
				r := g
				if rel {
					r += s.ar[0][l]
				}
				r &= 127
				switch sel[c] {
				case 0, 1, 2, 3:
					v[c][l] = s.gpr[r][sel[c]][l]
				case 4:
					v[c][l] = 0
				case 5:
					v[c][l] = f2u(1)
				default:
					v[c][l] = 0
				}
			}
		}
		s.env.export(s, typ, arrayBase+b, &v, m)
	}
}

// ---- texture and vertex fetch clauses ----

func (s *shaderRun) texClause(addr, count uint32) {
	for i := uint32(0); i < count; i++ {
		a := s.base + (addr+i*2)*8
		w0, w1, w2 := s.env.read32(a), s.env.read32(a+4), s.env.read32(a+8)
		op := w0 & 0x1f
		switch op {
		case 0, 1: // VTX_FETCH / VTX_SEMANTIC in a texture clause
			s.env.fetch(s, w0, w1, w2, s.active)
		case 0x0b, 0x0c: // SET_GRADIENTS_H / V
			s.setGradients(op == 0x0b, w0, w1, w2)
		default:
			s.env.tex(s, w0, w1, w2, s.active)
		}
	}
}

func (s *shaderRun) setGradients(h bool, w0, w1, w2 uint32) {
	src := w0 >> 16 & 0x7f
	sel := [4]uint32{w2 >> 20 & 7, w2 >> 23 & 7, w2 >> 26 & 7, w2 >> 29 & 7}
	var g [4]lanes
	for c := 0; c < 4; c++ {
		for l := 0; l < nl; l++ {
			g[c][l] = s.selVal(src, sel[c], l)
		}
	}
	if h {
		s.gradH = g
	} else {
		s.gradV = g
	}
	s.gradSet = true
}

// selVal: a source select of a fetch (0-3 xyzw, 4 = 0, 5 = 1).
func (s *shaderRun) selVal(gpr, sel uint32, l int) uint32 {
	switch sel {
	case 0, 1, 2, 3:
		return s.gpr[gpr&127][sel][l]
	case 5:
		return f2u(1)
	}
	return 0
}

func (s *shaderRun) vtxClause(addr, count uint32) {
	for i := uint32(0); i < count; i++ {
		a := s.base + (addr+i*2)*8
		w0, w1, w2 := s.env.read32(a), s.env.read32(a+4), s.env.read32(a+8)
		s.env.fetch(s, w0, w1, w2, s.active)
	}
}

// writeFetch writes a fetch's result with its destination selects.
func (s *shaderRun) writeFetch(dst uint32, rel bool, dsel [4]uint32, v *[4]lanes, mask laneMask) {
	for l := 0; l < nl; l++ {
		if !mask[l] {
			continue
		}
		r := dst
		if rel {
			r += s.ar[0][l]
		}
		r &= 127
		for c := 0; c < 4; c++ {
			switch dsel[c] {
			case 0, 1, 2, 3:
				s.gpr[r][c][l] = v[dsel[c]][l]
			case 4:
				s.gpr[r][c][l] = 0
			case 5:
				s.gpr[r][c][l] = f2u(1)
			}
			// 7: masked
		}
	}
}

// ---- ALU clauses ----

type aluOp struct {
	w0, w1  uint32
	op3     bool
	inst    uint32
	unit    int // 0-3 x..w, 4 trans
	dst     uint32
	dstChan uint32
	write   bool
}

// trans-only OP2 instructions
func transOnly(op uint32) bool {
	switch op {
	case 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
		0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79:
		return true
	}
	return false
}

// vector-only (the four-wide) instructions
func vectorOnly(op uint32) bool {
	switch op {
	case 0x50, 0x51, 0x52, 0x53:
		return true
	}
	return false
}

func (s *shaderRun) aluClause(addr, count uint32) {
	slot := uint32(0)
	for slot < count {
		var group [5]aluOp
		n := 0
		var used [5]bool
		litUsed := false
		for {
			if slot >= count || n >= 5 {
				break
			}
			a := s.base + (addr+slot)*8
			w0, w1 := s.env.read32(a), s.env.read32(a+4)
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
			// unit assignment
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
			if w0>>31 != 0 { // LAST
				break
			}
		}
		// literals follow the group
		var lit [4]uint32
		if litUsed {
			a := s.base + (addr+slot)*8
			for i := 0; i < 4; i++ {
				lit[i] = s.env.read32(a + uint32(i)*4)
			}
			// how many literal slots: 1 if only x/y used, else 2
			nlit := uint32(1)
			for _, op := range group[:n] {
				for _, src := range s.srcs(op) {
					if src.sel == 253 && src.ch >= 2 {
						nlit = 2
					}
				}
			}
			slot += nlit
		}
		s.aluGroup(group[:n], &lit)
	}
}

type aluSrc struct {
	sel, ch  uint32
	rel, neg bool
	abs      bool
}

func (s *shaderRun) srcs(op aluOp) []aluSrc {
	w0, w1 := op.w0, op.w1
	a := aluSrc{sel: w0 & 0x1ff, rel: w0>>9&1 != 0, ch: w0 >> 10 & 3, neg: w0>>12&1 != 0}
	b := aluSrc{sel: w0 >> 13 & 0x1ff, rel: w0>>22&1 != 0, ch: w0 >> 23 & 3, neg: w0>>25&1 != 0}
	if op.op3 {
		c := aluSrc{sel: w1 & 0x1ff, rel: w1>>9&1 != 0, ch: w1 >> 10 & 3, neg: w1>>12&1 != 0}
		return []aluSrc{a, b, c}
	}
	a.abs = w1&1 != 0
	b.abs = w1>>1&1 != 0
	return []aluSrc{a, b}
}

// read gets a source operand for every lane.
func (s *shaderRun) read(src aluSrc, indexMode uint32, lit *[4]uint32, pv *[4]lanes, ps *lanes) lanes {
	var out lanes
	for l := 0; l < nl; l++ {
		var off uint32
		if src.rel {
			if indexMode < 4 {
				off = s.ar[indexMode][l]
			} else if len(s.loops) > 0 {
				off = uint32(s.loops[len(s.loops)-1].index)
			}
		}
		sel := src.sel
		var v uint32
		switch {
		case sel < 128:
			v = s.gpr[(sel+off)&127][src.ch][l]
		case sel < 160:
			v = s.kc(0, sel-128+off, src.ch)
		case sel < 192:
			v = s.kc(1, sel-160+off, src.ch)
		case sel == 248:
			v = 0
		case sel == 249:
			v = f2u(1)
		case sel == 250:
			v = 1
		case sel == 251:
			v = 0xffffffff
		case sel == 252:
			v = f2u(0.5)
		case sel == 253:
			v = lit[src.ch]
		case sel == 254:
			v = pv[src.ch][l]
		case sel == 255:
			v = ps[l]
		case sel >= 256:
			v = s.env.cfile((sel-256+off)&255, src.ch)
		}
		if src.abs {
			v &^= 0x80000000
		}
		if src.neg {
			v ^= 0x80000000
		}
		out[l] = v
	}
	return out
}

func (s *shaderRun) kc(set int, i, ch uint32) uint32 {
	if s.kcMode[set] == 0 {
		return 0
	}
	return s.env.kcache(s.kcBank[set], s.kcAddr[set]*16+i, ch)
}

func (s *shaderRun) aluGroup(group []aluOp, lit *[4]uint32) {
	pv := s.pv
	ps := s.ps
	var newPV [4]lanes
	newPS := ps
	type result struct {
		op   aluOp
		v    lanes
		mask laneMask
	}
	var res [5]result
	var nres int
	var newAR [4]lanes
	setAR := [4]bool{}
	// DOT4 / CUBE / MAX4 take their inputs from all four vector slots
	var vec [4]aluOp
	var hasVec [4]bool
	for _, op := range group {
		if !op.op3 && vectorOnly(op.inst) && op.unit < 4 {
			vec[op.unit] = op
			hasVec[op.unit] = true
		}
	}
	predUpdate := false
	var newPred laneMask
	execUpdate := false
	var execMask laneMask
	for _, op := range group {
		srcs := s.srcs(op)
		im := op.w0 >> 26 & 7
		var in [3]lanes
		for i, sr := range srcs {
			in[i] = s.read(sr, im, lit, &pv, &ps)
		}
		var out lanes
		mask := s.active
		switch op.w0 >> 29 & 3 { // PRED_SEL
		case 2:
			for l := range mask {
				mask[l] = mask[l] && !s.pred[l]
			}
		case 3:
			for l := range mask {
				mask[l] = mask[l] && s.pred[l]
			}
		}
		if op.op3 {
			out = s.op3(op.inst, &in)
		} else {
			switch op.inst {
			case 0x50, 0x51, 0x52, 0x53: // DOT4, DOT4_IEEE, CUBE, MAX4
				var a, b [4]lanes
				for c := 0; c < 4; c++ {
					if hasVec[c] {
						sr := s.srcs(vec[c])
						im2 := vec[c].w0 >> 26 & 7
						a[c] = s.read(sr[0], im2, lit, &pv, &ps)
						b[c] = s.read(sr[1], im2, lit, &pv, &ps)
					}
				}
				out = s.vec4(op.inst, op.unit, &a, &b)
			default:
				var pr laneMask
				var ue, up bool
				out, pr, ue, up = s.op2(op, &in, mask)
				if up {
					predUpdate = true
					newPred = pr
				}
				if ue {
					execUpdate = true
					execMask = pr
				}
				if op.inst == 0x15 || op.inst == 0x16 || op.inst == 0x18 || op.inst == 0x60 { // MOVA*
					var ar lanes
					for l := 0; l < nl; l++ {
						switch op.inst {
						case 0x15: // MOVA: round
							ar[l] = uint32(int32(math.Floor(float64(u2f(in[0][l])) + 0.5)))
						case 0x16: // MOVA_FLOOR
							ar[l] = uint32(int32(math.Floor(float64(u2f(in[0][l])))))
						default: // MOVA_INT, MOVA_GPR_INT
							ar[l] = in[0][l]
						}
					}
					newAR[op.dstChan] = ar
					setAR[op.dstChan] = true
				}
			}
		}
		// output modifier and clamp
		if !s.isIntOp(op) {
			omod := uint32(0)
			if !op.op3 {
				omod = op.w1 >> 5 & 3
			}
			clamp := op.w1>>31 != 0
			if omod != 0 || clamp {
				for l := range out {
					f := u2f(out[l])
					switch omod {
					case 1:
						f *= 2
					case 2:
						f *= 4
					case 3:
						f /= 2
					}
					if clamp {
						if f > 1 {
							f = 1
						} else if !(f > 0) {
							f = 0
						}
					}
					out[l] = f2u(f)
				}
			}
		}
		if op.unit < 4 {
			newPV[op.unit] = out
		} else {
			newPS = out
		}
		res[nres] = result{op, out, mask}
		nres++
	}
	// write the results
	for _, r := range res[:nres] {
		if !r.op.write {
			continue
		}
		rel := r.op.w1>>28&1 != 0
		im := r.op.w0 >> 26 & 7
		for l := 0; l < nl; l++ {
			if !r.mask[l] {
				continue
			}
			d := r.op.dst
			if rel {
				if im < 4 {
					d += s.ar[im][l]
				} else if len(s.loops) > 0 {
					d += uint32(s.loops[len(s.loops)-1].index)
				}
			}
			s.gpr[d&127][r.op.dstChan][l] = r.v[l]
		}
	}
	for c := 0; c < 4; c++ {
		if setAR[c] {
			s.ar[c] = newAR[c]
		}
	}
	if predUpdate {
		s.pred = newPred
	}
	if execUpdate {
		for l := range s.active {
			s.active[l] = s.active[l] && execMask[l]
		}
	}
	s.pv = newPV
	s.ps = newPS
}

// isIntOp reports whether op gives an integer result, which OMOD and clamp
// leave alone.
func (s *shaderRun) isIntOp(op aluOp) bool {
	if op.op3 {
		return op.inst >= 0x1c && op.inst <= 0x1e
	}
	i := op.inst
	switch {
	case i >= 0x0c && i <= 0x0f, // SET*_DX10
		i == 0x18 || i == 0x60, // MOVA_INT, MOVA_GPR_INT
		i == 0x1e || i == 0x1f,
		i >= 0x30 && i <= 0x4f, // integer logic, compares, kills, predicates
		i == 0x6b,              // FLT_TO_INT
		i >= 0x70 && i <= 0x79: // shifts, integer multiplies, FLT_TO_UINT
		return true
	}
	return false
}

func fbool(b bool) uint32 {
	if b {
		return f2u(1)
	}
	return 0
}

func ibool(b bool) uint32 {
	if b {
		return 0xffffffff
	}
	return 0
}

// legacy multiply: 0 times anything is 0
func mulLegacy(a, b float32) float32 {
	if a == 0 || b == 0 {
		return 0
	}
	return a * b
}

func (s *shaderRun) op3(inst uint32, in *[3]lanes) lanes {
	var out lanes
	for l := 0; l < nl; l++ {
		a, b, c := u2f(in[0][l]), u2f(in[1][l]), u2f(in[2][l])
		var r float32
		switch inst {
		case 0x0c, 0x0d, 0x0e, 0x0f: // MUL_LIT (and variants)
			r = mulLegacy(a, b)
			if c <= 0 {
				r = 0
			}
		case 0x10, 0x11, 0x12, 0x13: // MULADD
			r = mulLegacy(a, b) + c
			switch inst {
			case 0x11:
				r *= 2
			case 0x12:
				r *= 4
			case 0x13:
				r /= 2
			}
		case 0x14, 0x15, 0x16, 0x17: // MULADD_IEEE
			r = a*b + c
			switch inst {
			case 0x15:
				r *= 2
			case 0x16:
				r *= 4
			case 0x17:
				r /= 2
			}
		case 0x18: // CNDE
			if a == 0 {
				r = b
			} else {
				r = c
			}
		case 0x19: // CNDGT
			if a > 0 {
				r = b
			} else {
				r = c
			}
		case 0x1a: // CNDGE
			if a >= 0 {
				r = b
			} else {
				r = c
			}
		case 0x1c: // CNDE_INT
			if in[0][l] == 0 {
				out[l] = in[1][l]
			} else {
				out[l] = in[2][l]
			}
			continue
		case 0x1d: // CNDGT_INT
			if int32(in[0][l]) > 0 {
				out[l] = in[1][l]
			} else {
				out[l] = in[2][l]
			}
			continue
		case 0x1e: // CNDGE_INT
			if int32(in[0][l]) >= 0 {
				out[l] = in[1][l]
			} else {
				out[l] = in[2][l]
			}
			continue
		default:
			s.Err = "unknown ALU OP3 instruction " + hex32(inst)
		}
		out[l] = f2u(r)
	}
	return out
}

func (s *shaderRun) vec4(inst uint32, unit int, a, b *[4]lanes) lanes {
	var out lanes
	for l := 0; l < nl; l++ {
		switch inst {
		case 0x50: // DOT4
			var sum float32
			for c := 0; c < 4; c++ {
				sum += mulLegacy(u2f(a[c][l]), u2f(b[c][l]))
			}
			out[l] = f2u(sum)
		case 0x51: // DOT4_IEEE
			var sum float32
			for c := 0; c < 4; c++ {
				sum += u2f(a[c][l]) * u2f(b[c][l])
			}
			out[l] = f2u(sum)
		case 0x53: // MAX4
			m := u2f(a[0][l])
			for c := 1; c < 4; c++ {
				m = max(m, u2f(a[c][l]))
			}
			out[l] = f2u(m)
		case 0x52: // CUBE: src0 = (z, z, x, y), src1 = (y, x, z, z) by convention
			x, y, z := u2f(a[2][l]), u2f(a[3][l]), u2f(a[0][l])
			out[l] = f2u(cubeResult(x, y, z, unit))
		}
	}
	return out
}

// cubeResult: CUBE's outputs: x = t coordinate, y = s, z = 2 * major axis, w = face.
func cubeResult(x, y, z float32, unit int) float32 {
	ax, ay, az := abs32(x), abs32(y), abs32(z)
	var sc, tc, ma, face float32
	switch {
	case az >= ax && az >= ay:
		ma = z
		if z >= 0 {
			face, sc, tc = 4, x, -y
		} else {
			face, sc, tc = 5, -x, -y
		}
	case ay >= ax:
		ma = y
		if y >= 0 {
			face, sc, tc = 2, x, z
		} else {
			face, sc, tc = 3, x, -z
		}
	default:
		ma = x
		if x >= 0 {
			face, sc, tc = 0, -z, -y
		} else {
			face, sc, tc = 1, z, -y
		}
	}
	switch unit {
	case 0:
		return tc
	case 1:
		return sc
	case 2:
		return 2 * abs32(ma)
	}
	return face
}

func abs32(f float32) float32 { return float32(math.Abs(float64(f))) }

func floatToInt(f float32) int32 {
	if f != f {
		return 0
	}
	if f >= 2147483647 {
		return math.MaxInt32
	}
	if f <= -2147483648 {
		return math.MinInt32
	}
	return int32(f)
}

func floatToUint(f float32) uint32 {
	if !(f > 0) {
		return 0
	}
	if f >= 4294967295 {
		return math.MaxUint32
	}
	return uint32(f)
}

// op2 runs an OP2 instruction; pred is the new predicate (for PRED_SET*),
// with ue / up saying whether it updates the exec mask / predicate.
func (s *shaderRun) op2(op aluOp, in *[3]lanes, mask laneMask) (out lanes, pred laneMask, ue, up bool) {
	ue = op.w1>>2&1 != 0
	up = op.w1>>3&1 != 0
	predOp := false
	for l := 0; l < nl; l++ {
		x, y := in[0][l], in[1][l]
		a, b := u2f(x), u2f(y)
		ia, ib := int32(x), int32(y)
		var r uint32
		var p bool
		switch op.inst {
		case 0x00:
			r = f2u(a + b)
		case 0x01:
			r = f2u(mulLegacy(a, b))
		case 0x02:
			r = f2u(a * b)
		case 0x03, 0x05:
			r = f2u(fmax(a, b))
		case 0x04, 0x06:
			r = f2u(fmin(a, b))
		case 0x08:
			r = fbool(a == b)
		case 0x09:
			r = fbool(a > b)
		case 0x0a:
			r = fbool(a >= b)
		case 0x0b:
			r = fbool(a != b)
		case 0x0c:
			r = ibool(a == b)
		case 0x0d:
			r = ibool(a > b)
		case 0x0e:
			r = ibool(a >= b)
		case 0x0f:
			r = ibool(a != b)
		case 0x10:
			r = f2u(a - float32(math.Floor(float64(a))))
		case 0x11:
			r = f2u(float32(math.Trunc(float64(a))))
		case 0x12:
			r = f2u(float32(math.Ceil(float64(a))))
		case 0x13:
			r = f2u(float32(math.RoundToEven(float64(a))))
		case 0x14:
			r = f2u(float32(math.Floor(float64(a))))
		case 0x15:
			r = f2u(float32(math.Floor(float64(a) + 0.5)))
		case 0x16:
			r = f2u(float32(math.Floor(float64(a))))
		case 0x18, 0x60:
			r = x
		case 0x19:
			r = x
		case 0x1a: // NOP
			r = 0
		case 0x1e, 0x1f: // PRED_SETGT_UINT, PRED_SETGE_UINT
			if op.inst == 0x1e {
				p = x > y
			} else {
				p = x >= y
			}
			r = fbool(p)
			predOp = true
		case 0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2a, 0x2b: // PRED_SETE..NE (+_PUSH)
			switch op.inst & 3 {
			case 0:
				p = a == b
			case 1:
				p = a > b
			case 2:
				p = a >= b
			case 3:
				p = a != b
			}
			r = fbool(p)
			if !p {
				r = 0
			}
			predOp = true
		case 0x24: // PRED_SET_INV
			p = a == 0
			if a == 0 {
				r = f2u(1)
			} else {
				r = x
			}
			predOp = true
		case 0x25: // PRED_SET_POP
			p = a <= 0
			if p {
				r = 0
			} else {
				r = f2u(a - 1)
			}
			predOp = true
		case 0x26: // PRED_SET_CLR
			p = false
			r = f2u(float32(math.Inf(1)))
			predOp = true
		case 0x27: // PRED_SET_RESTORE
			p = a == 0
			r = x
			predOp = true
		case 0x2c, 0x2d, 0x2e, 0x2f: // KILLE, KILLGT, KILLGE, KILLNE
			var k bool
			switch op.inst {
			case 0x2c:
				k = a == b
			case 0x2d:
				k = a > b
			case 0x2e:
				k = a >= b
			case 0x2f:
				k = a != b
			}
			if k && mask[l] {
				s.killed[l] = true
			}
			r = fbool(k)
		case 0x30:
			r = x & y
		case 0x31:
			r = x | y
		case 0x32:
			r = x ^ y
		case 0x33:
			r = ^x
		case 0x34:
			r = x + y
		case 0x35:
			r = x - y
		case 0x36:
			r = uint32(max(ia, ib))
		case 0x37:
			r = uint32(min(ia, ib))
		case 0x38:
			r = max(x, y)
		case 0x39:
			r = min(x, y)
		case 0x3a:
			r = ibool(x == y)
		case 0x3b:
			r = ibool(ia > ib)
		case 0x3c:
			r = ibool(ia >= ib)
		case 0x3d:
			r = ibool(x != y)
		case 0x3e:
			r = ibool(x > y)
		case 0x3f:
			r = ibool(x >= y)
		case 0x40, 0x41: // KILLGT_UINT, KILLGE_UINT
			k := (op.inst == 0x40 && x > y) || (op.inst == 0x41 && x >= y)
			if k && mask[l] {
				s.killed[l] = true
			}
			r = ibool(k)
		case 0x42, 0x43, 0x44, 0x45, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f: // PRED_SET*_INT (+_PUSH)
			switch op.inst {
			case 0x42, 0x4a:
				p = x == y
			case 0x43, 0x4b:
				p = ia > ib
			case 0x44, 0x4c:
				p = ia >= ib
			case 0x45, 0x4d:
				p = x != y
			case 0x4e:
				p = ia < ib
			case 0x4f:
				p = ia <= ib
			}
			r = ibool(p)
			if p {
				r = 1
			}
			predOp = true
		case 0x46, 0x47, 0x48, 0x49: // KILL*_INT
			var k bool
			switch op.inst {
			case 0x46:
				k = x == y
			case 0x47:
				k = ia > ib
			case 0x48:
				k = ia >= ib
			case 0x49:
				k = x != y
			}
			if k && mask[l] {
				s.killed[l] = true
			}
			r = ibool(k)
		case 0x61: // EXP_IEEE
			r = f2u(float32(math.Exp2(float64(a))))
		case 0x62: // LOG_CLAMPED
			if a == 0 {
				r = f2u(-math.MaxFloat32)
			} else {
				r = f2u(float32(math.Log2(float64(a))))
			}
		case 0x63: // LOG_IEEE
			r = f2u(float32(math.Log2(float64(a))))
		case 0x64, 0x65: // RECIP_CLAMPED, RECIP_FF
			if a == 0 {
				if op.inst == 0x64 {
					r = f2u(float32(math.Copysign(math.MaxFloat32, float64(a))))
				} else {
					r = 0
				}
			} else {
				r = f2u(1 / a)
			}
		case 0x66: // RECIP_IEEE
			r = f2u(1 / a)
		case 0x67, 0x68: // RECIPSQRT_CLAMPED, RECIPSQRT_FF
			if a == 0 {
				if op.inst == 0x67 {
					r = f2u(math.MaxFloat32)
				} else {
					r = 0
				}
			} else {
				r = f2u(float32(1 / math.Sqrt(math.Abs(float64(a)))))
			}
		case 0x69: // RECIPSQRT_IEEE
			r = f2u(float32(1 / math.Sqrt(float64(a))))
		case 0x6a: // SQRT_IEEE
			r = f2u(float32(math.Sqrt(float64(a))))
		case 0x6b: // FLT_TO_INT
			r = uint32(floatToInt(a))
		case 0x6c: // INT_TO_FLT
			r = f2u(float32(ia))
		case 0x6d: // UINT_TO_FLT
			r = f2u(float32(x))
		case 0x6e: // SIN: on R700 the input is in turns (-0.5..0.5)
			r = f2u(float32(math.Sin(2 * math.Pi * float64(a))))
		case 0x6f: // COS
			r = f2u(float32(math.Cos(2 * math.Pi * float64(a))))
		case 0x70:
			r = uint32(ia >> (y & 31))
		case 0x71:
			r = x >> (y & 31)
		case 0x72:
			r = x << (y & 31)
		case 0x73: // MULLO_INT
			r = uint32(ia * ib)
		case 0x74: // MULHI_INT
			r = uint32((int64(ia) * int64(ib)) >> 32)
		case 0x75: // MULLO_UINT
			r = x * y
		case 0x76: // MULHI_UINT
			hi, _ := bits.Mul32(x, y)
			r = hi
		case 0x77: // RECIP_INT
			if ia != 0 {
				r = uint32(int32(0x100000000 / int64(ia)))
			}
		case 0x78: // RECIP_UINT
			if x != 0 {
				r = uint32(0x100000000 / uint64(x))
			}
		case 0x79: // FLT_TO_UINT
			r = floatToUint(a)
		default:
			s.Err = "unknown ALU OP2 instruction " + hex32(op.inst)
		}
		out[l] = r
		pred[l] = p
	}
	if !predOp {
		ue, up = false, false
	}
	return
}

func fmax(a, b float32) float32 {
	if a != a {
		return b
	}
	if b != b {
		return a
	}
	if a > b {
		return a
	}
	return b
}

func fmin(a, b float32) float32 {
	if a != a {
		return b
	}
	if b != b {
		return a
	}
	if a < b {
		return a
	}
	return b
}
