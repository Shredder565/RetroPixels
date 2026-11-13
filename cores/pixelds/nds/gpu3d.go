package nds

// gpu3D is the DS's 3D engine: the geometry engine here (commands,
// matrices, lighting, clipping, polygon lists) and the rasterizer in
// render3d.go. Commands run as soon as they're written, so the command
// FIFO always reads as empty.

import (
	"bytes"
	"compress/flate"
	"encoding/gob"
)

type mat [16]int64 // row-major, 20.12 fixed point

var identity = mat{0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0x1000}

func (a *mat) mul(b *mat) (r mat) {
	for i := 0; i < 4; i++ {
		for j := 0; j < 4; j++ {
			var s int64
			for k := 0; k < 4; k++ {
				s += a[i*4+k] * b[k*4+j]
			}
			r[i*4+j] = s >> 12
		}
	}
	return
}

// Vertex is a vertex in clip space (x, y, z, w) with its colour (0-31) and
// texture coordinates (texels × 16).
type Vertex struct {
	X, Y, Z, W float64
	R, G, B    float64
	S, T       float64
}

// Polygon is one polygon of the list, in clip space after clipping.
type Polygon struct {
	V       []Vertex
	Attr    uint32
	Tex     uint32
	Pal     uint32
	Strip   bool
	Seq     int
	Facing  bool // front facing
	Opaque  bool
	YMin    float64
	YMax    float64
	WBuffer bool
}

// g3dState is everything the 3D engine keeps, saved in save states.
type g3dState struct {
	Disp3DCnt uint32
	Regs      [0x50]uint16 // 0x4000320-0x40003bf as halfwords from 0x320
	GXIRQ     uint32       // GXSTAT bits 30-31

	MtxMode            uint32
	Proj, Pos, Vec, Tx mat
	Clip               mat
	ProjStack, TxStack mat
	PosStack, VecStack [32]mat
	ProjSP, PosSP      int
	TxSP               int
	StackErr           bool

	Color          [3]int32
	S, T           int32 // current texture coordinates (raw, 12.4)
	RawS, RawT     int32
	LastV          [3]int64
	PolyAttr       uint32 // as written
	CurAttr        uint32 // latched at BEGIN_VTXS
	TexParam, Pltt uint32
	Diffuse        uint32
	Ambient        uint32
	Specular       uint32
	Emission       uint32
	ShineOn        bool
	Shine          [128]uint8
	LightVec       [4][3]int32
	LightCol       [4]uint32
	PrimType       int
	Prim           []Vertex
	PrimCount      int
	Viewport       [4]int
	BoxResult      bool
	PosResult      [4]int32
	VecResult      [3]int16

	Polys      []Polygon // the list being built
	Verts      int
	Ready      []Polygon // after SWAP_BUFFERS, shown from the next VBlank
	ReadyFlags uint32
	HaveReady  bool
	Render     []Polygon // the list being drawn
	RenderFlag uint32

	Queue     []uint8 // packed FIFO commands still waiting for parameters
	Params    []uint32
	PortCmd   uint8
	PortParam []uint32
}

type gpu3D struct {
	m         *Machine
	s         g3dState
	busyUntil int64

	// the picture (render3d.go), drawn at sc times the DS's resolution
	// (W×H pixels); out is always the DS's own 256×192 (what the 2D engine
	// and display capture see), hi the whole picture when sc > 1
	sc, W, H int
	color    []uint32 // bits 0-14 colour, 16-20 alpha
	depth    []uint32
	attr     []uint32 // see render3d.go
	stencil  []bool
	out      [256 * 192]uint32
	hi       []uint32

	tex texCache // texture packs (texpack.go)
}

// setScale draws the 3D picture at n times the DS's resolution.
func (g *gpu3D) setScale(n int) {
	n = max(1, min(n, 8))
	g.sc, g.W, g.H = n, 256*n, 192*n
	g.color = make([]uint32, g.W*g.H)
	g.depth = make([]uint32, g.W*g.H)
	g.attr = make([]uint32, g.W*g.H)
	g.stencil = make([]bool, g.W*g.H)
	g.hi = nil
	if n > 1 {
		g.hi = make([]uint32, g.W*g.H)
	}
}

func newGPU3D(m *Machine) *gpu3D {
	g := &gpu3D{m: m, busyUntil: never}
	g.s.Proj, g.s.Pos, g.s.Vec, g.s.Tx = identity, identity, identity, identity
	g.s.Clip = identity
	g.s.Viewport = [4]int{0, 0, 255, 191}
	g.setScale(1)
	return g
}

var paramCount = func() (t [256]int) {
	for c, n := range map[int]int{
		0x10: 1, 0x12: 1, 0x13: 1, 0x14: 1, 0x16: 16, 0x17: 12, 0x18: 16, 0x19: 12, 0x1a: 9,
		0x1b: 3, 0x1c: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1, 0x26: 1,
		0x27: 1, 0x28: 1, 0x29: 1, 0x2a: 1, 0x2b: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1,
		0x34: 32, 0x40: 1, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1,
	} {
		t[c] = n
	}
	return
}()

func (g *gpu3D) reg16(a uint32) uint16 { return g.s.Regs[(a-0x04000320)/2] }

func (g *gpu3D) read32(a uint32) uint32 {
	s := &g.s
	switch {
	case a == 0x04000060:
		return s.Disp3DCnt
	case a >= 0x04000320 && a < 0x040003c0:
		return uint32(g.reg16(a)) | uint32(g.reg16(a+2))<<16
	case a == 0x04000600:
		v := uint32(0x06000000) // FIFO empty, less than half full
		if s.BoxResult {
			v |= 2
		}
		v |= uint32(s.PosSP&31) << 8
		v |= uint32(s.ProjSP&1) << 13
		if s.StackErr {
			v |= 1 << 15
		}
		return v | s.GXIRQ<<30
	case a == 0x04000604:
		return uint32(len(s.Polys)) | uint32(s.Verts)<<16
	case a >= 0x04000620 && a < 0x04000630:
		return uint32(s.PosResult[(a-0x04000620)/4])
	case a >= 0x04000630 && a < 0x04000636:
		i := (a - 0x04000630) / 2
		v := uint32(uint16(s.VecResult[i]))
		if i < 2 {
			v |= uint32(uint16(s.VecResult[i+1])) << 16
		}
		return v
	case a >= 0x04000640 && a < 0x04000680:
		return uint32(s.Clip[(a-0x04000640)/4])
	case a >= 0x04000680 && a < 0x040006a4:
		i := (a - 0x04000680) / 4
		return uint32(s.Vec[i/3*4+i%3])
	}
	return 0
}

func (g *gpu3D) write32(a, v uint32) {
	s := &g.s
	switch {
	case a == 0x04000060:
		s.Disp3DCnt = s.Disp3DCnt&0x3000&^v | v&0x4fff
		return
	case a >= 0x04000320 && a < 0x040003c0:
		g.write16(a, uint16(v))
		g.write16(a+2, uint16(v>>16))
		return
	case a >= 0x04000400 && a < 0x04000440: // GXFIFO and its mirrors (games write it with STM)
		g.fifoWrite(v)
		return
	case a >= 0x04000440 && a < 0x04000600:
		g.portWrite(uint8((a-0x04000400)/4), v)
		return
	case a == 0x04000600:
		if v&(1<<15) != 0 {
			s.StackErr = false
			s.ProjSP &= 1
		}
		s.GXIRQ = v >> 30
		g.checkIRQ()
		return
	}
}

// The registers from 0x320 to 0x3bf are kept as halfwords.
func (g *gpu3D) write16(a uint32, v uint16) {
	switch {
	case a == 0x04000060:
		g.write32(a, g.s.Disp3DCnt&0xffff0000|uint32(v))
	case a >= 0x04000320 && a < 0x040003c0:
		g.s.Regs[(a-0x04000320)/2] = v
	case a == 0x04000602:
		g.write32(0x04000600, uint32(v)<<16)
	case a >= 0x04000400 && a < 0x04000600:
		g.write32(a&^3, uint32(v)) // halfword command writes are rare
	}
}

func (g *gpu3D) write8(a uint32, v uint8) {
	switch {
	case a >= 0x04000320 && a < 0x040003c0:
		i := (a - 0x04000320) / 2
		sh := a & 1 * 8
		g.s.Regs[i] = g.s.Regs[i]&^(0xff<<sh) | uint16(v)<<sh
	case a == 0x04000060 || a == 0x04000061:
		sh := a & 1 * 8
		g.write32(0x04000060, g.s.Disp3DCnt&^(0xff<<sh)|uint32(v)<<sh)
	case a == 0x04000603:
		g.write32(0x04000600, uint32(v)<<24)
	}
}

func (g *gpu3D) checkIRQ() {
	if g.s.GXIRQ != 0 { // FIFO less than half full / empty: always
		g.m.irq9.raise(irqGXFIFO)
	}
}

func (g *gpu3D) waiting() bool { return false }
func (g *gpu3D) run()          {}

// fifoWrite takes a word sent to GXFIFO: packed commands, then their
// parameters.
func (g *gpu3D) fifoWrite(v uint32) {
	s := &g.s
	if len(s.Queue) == 0 {
		s.Queue = append(s.Queue[:0], uint8(v), uint8(v>>8), uint8(v>>16), uint8(v>>24))
		// trailing zero bytes are no-ops
		for len(s.Queue) > 1 && s.Queue[len(s.Queue)-1] == 0 {
			s.Queue = s.Queue[:len(s.Queue)-1]
		}
		s.Params = s.Params[:0]
		g.runQueue()
		return
	}
	s.Params = append(s.Params, v)
	g.runQueue()
}

func (g *gpu3D) runQueue() {
	s := &g.s
	for len(s.Queue) > 0 {
		c := s.Queue[0]
		if len(s.Params) < paramCount[c] {
			return
		}
		g.exec(c, s.Params)
		s.Params = s.Params[:0]
		s.Queue = s.Queue[1:]
	}
}

func (g *gpu3D) portWrite(c uint8, v uint32) {
	s := &g.s
	if s.PortCmd != c {
		s.PortCmd = c
		s.PortParam = s.PortParam[:0]
	}
	n := paramCount[c]
	if n == 0 {
		g.exec(c, nil)
		return
	}
	s.PortParam = append(s.PortParam, v)
	if len(s.PortParam) >= n {
		g.exec(c, s.PortParam)
		s.PortParam = s.PortParam[:0]
	}
}

func s16(v uint32) int64 { return int64(int16(v)) }
func s10(v uint32) int64 { return int64(int32(v<<22) >> 22) }

func (g *gpu3D) updateClip() { g.s.Clip = g.s.Pos.mul(&g.s.Proj) }

// apply multiplies the current matrix (or matrices) by t: cur = t × cur.
func (g *gpu3D) apply(t *mat, scale bool) {
	s := &g.s
	switch s.MtxMode {
	case 0:
		s.Proj = t.mul(&s.Proj)
	case 1:
		s.Pos = t.mul(&s.Pos)
	case 2:
		s.Pos = t.mul(&s.Pos)
		if !scale {
			s.Vec = t.mul(&s.Vec)
		}
	case 3:
		s.Tx = t.mul(&s.Tx)
		return
	}
	g.updateClip()
}

func (g *gpu3D) load(t *mat) {
	s := &g.s
	switch s.MtxMode {
	case 0:
		s.Proj = *t
	case 1:
		s.Pos = *t
	case 2:
		s.Pos, s.Vec = *t, *t
	case 3:
		s.Tx = *t
		return
	}
	g.updateClip()
}

func matFrom(p []uint32, rows, cols int) mat {
	m := identity
	k := 0
	for r := 0; r < rows; r++ {
		for c := 0; c < cols; c++ {
			m[r*4+c] = int64(int32(p[k]))
			k++
		}
	}
	return m
}

func (g *gpu3D) exec(c uint8, p []uint32) {
	s := &g.s
	switch c {
	case 0x10:
		s.MtxMode = p[0] & 3
	case 0x11: // push
		switch s.MtxMode {
		case 0:
			if s.ProjSP > 0 {
				s.StackErr = true
			}
			s.ProjStack = s.Proj
			s.ProjSP = 1
		case 3:
			s.TxStack = s.Tx
			s.TxSP = 1
		default:
			if s.PosSP >= 31 {
				s.StackErr = true
			}
			s.PosStack[s.PosSP&31] = s.Pos
			s.VecStack[s.PosSP&31] = s.Vec
			s.PosSP = (s.PosSP + 1) & 63
		}
	case 0x12: // pop
		switch s.MtxMode {
		case 0:
			if s.ProjSP == 0 {
				s.StackErr = true
			}
			s.ProjSP = 0
			s.Proj = s.ProjStack
			g.updateClip()
		case 3:
			s.TxSP = 0
			s.Tx = s.TxStack
		default:
			n := int(int32(p[0]<<26) >> 26)
			sp := s.PosSP - n
			if sp < 0 || sp >= 31 {
				s.StackErr = true
			}
			s.PosSP = sp & 63
			s.Pos = s.PosStack[s.PosSP&31]
			s.Vec = s.VecStack[s.PosSP&31]
			g.updateClip()
		}
	case 0x13: // store
		switch s.MtxMode {
		case 0:
			s.ProjStack = s.Proj
		case 3:
			s.TxStack = s.Tx
		default:
			i := int(p[0] & 31)
			if i == 31 {
				s.StackErr = true
			}
			s.PosStack[i] = s.Pos
			s.VecStack[i] = s.Vec
		}
	case 0x14: // restore
		switch s.MtxMode {
		case 0:
			s.Proj = s.ProjStack
			g.updateClip()
		case 3:
			s.Tx = s.TxStack
		default:
			i := int(p[0] & 31)
			if i == 31 {
				s.StackErr = true
			}
			s.Pos = s.PosStack[i]
			s.Vec = s.VecStack[i]
			g.updateClip()
		}
	case 0x15:
		g.load(&identity)
	case 0x16:
		m := matFrom(p, 4, 4)
		g.load(&m)
	case 0x17:
		m := matFrom(p, 4, 3)
		g.load(&m)
	case 0x18:
		m := matFrom(p, 4, 4)
		g.apply(&m, false)
	case 0x19:
		m := matFrom(p, 4, 3)
		g.apply(&m, false)
	case 0x1a:
		m := matFrom(p, 3, 3)
		g.apply(&m, false)
	case 0x1b:
		m := identity
		m[0], m[5], m[10] = int64(int32(p[0])), int64(int32(p[1])), int64(int32(p[2]))
		g.apply(&m, true)
	case 0x1c:
		m := identity
		m[12], m[13], m[14] = int64(int32(p[0])), int64(int32(p[1])), int64(int32(p[2]))
		g.apply(&m, false)
	case 0x20:
		s.Color = [3]int32{int32(p[0] & 31), int32(p[0] >> 5 & 31), int32(p[0] >> 10 & 31)}
	case 0x21:
		g.normal(p[0])
	case 0x22:
		s.RawS, s.RawT = int32(int16(p[0])), int32(int16(p[0]>>16))
		s.S, s.T = s.RawS, s.RawT
		if s.TexParam>>30 == 1 {
			m := &s.Tx
			S, T := int64(s.RawS), int64(s.RawT)
			s.S = int32((S*m[0] + T*m[4] + m[8] + m[12]) >> 12)
			s.T = int32((S*m[1] + T*m[5] + m[9] + m[13]) >> 12)
		}
	case 0x23:
		g.vertex(s16(p[0]), s16(p[0]>>16), s16(p[1]))
	case 0x24:
		g.vertex(s10(p[0])<<6, s10(p[0]>>10)<<6, s10(p[0]>>20)<<6)
	case 0x25:
		g.vertex(s16(p[0]), s16(p[0]>>16), s.LastV[2])
	case 0x26:
		g.vertex(s16(p[0]), s.LastV[1], s16(p[0]>>16))
	case 0x27:
		g.vertex(s.LastV[0], s16(p[0]), s16(p[0]>>16))
	case 0x28:
		// the differences are in the vertex's own 1/4096 units (±0.125)
		g.vertex(int64(int16(s.LastV[0]+s10(p[0]))), int64(int16(s.LastV[1]+s10(p[0]>>10))), int64(int16(s.LastV[2]+s10(p[0]>>20))))
	case 0x29:
		s.PolyAttr = p[0]
	case 0x2a:
		s.TexParam = p[0]
	case 0x2b:
		s.Pltt = p[0] & 0x1fff
	case 0x30:
		s.Diffuse, s.Ambient = p[0]&0x7fff, p[0]>>16&0x7fff
		if p[0]&0x8000 != 0 {
			s.Color = [3]int32{int32(p[0] & 31), int32(p[0] >> 5 & 31), int32(p[0] >> 10 & 31)}
		}
	case 0x31:
		s.Specular, s.Emission = p[0]&0x7fff, p[0]>>16&0x7fff
		s.ShineOn = p[0]&0x8000 != 0
	case 0x32:
		i := p[0] >> 30
		x, y, z := s10(p[0]), s10(p[0]>>10), s10(p[0]>>20)
		m := &s.Vec
		s.LightVec[i] = [3]int32{
			int32((x*m[0] + y*m[4] + z*m[8]) >> 12),
			int32((x*m[1] + y*m[5] + z*m[9]) >> 12),
			int32((x*m[2] + y*m[6] + z*m[10]) >> 12),
		}
	case 0x33:
		s.LightCol[p[0]>>30] = p[0] & 0x7fff
	case 0x34:
		for i, w := range p {
			for k := 0; k < 4; k++ {
				s.Shine[i*4+k] = uint8(w >> (8 * k))
			}
		}
	case 0x40:
		s.PrimType = int(p[0] & 3)
		s.Prim = s.Prim[:0]
		s.PrimCount = 0
		s.CurAttr = s.PolyAttr
	case 0x41:
	case 0x50:
		s.Ready = append(s.Ready[:0], s.Polys...)
		s.ReadyFlags = p[0] & 3
		s.HaveReady = true
		s.Polys = s.Polys[:0]
		s.Verts = 0
	case 0x60:
		x1, y1, x2, y2 := int(p[0]&0xff), int(p[0]>>8&0xff), int(p[0]>>16&0xff), int(p[0]>>24&0xff)
		s.Viewport = [4]int{x1, y1, x2, y2}
	case 0x70:
		g.boxTest(p)
	case 0x71:
		x, y, z := s16(p[0]), s16(p[0]>>16), s16(p[1])
		s.LastV = [3]int64{x, y, z}
		m := &s.Clip
		for i := 0; i < 4; i++ {
			s.PosResult[i] = int32((x*m[i] + y*m[4+i] + z*m[8+i] + 0x1000*m[12+i]) >> 12)
		}
	case 0x72:
		x, y, z := s10(p[0]), s10(p[0]>>10), s10(p[0]>>20)
		m := &s.Vec
		for i := 0; i < 3; i++ {
			v := (x*m[i] + y*m[4+i] + z*m[8+i]) >> 9
			s.VecResult[i] = int16(int32(v<<20) >> 20)
		}
	}
	if c >= 0x10 {
		g.checkIRQ()
	}
}

func (g *gpu3D) boxTest(p []uint32) {
	s := &g.s
	x, y, z := s16(p[0]), s16(p[0]>>16), s16(p[1])
	w, h, d := s16(p[1]>>16), s16(p[2]), s16(p[2]>>16)
	m := &s.Clip
	var out [6]int // corners outside each plane
	for i := 0; i < 8; i++ {
		cx, cy, cz := x, y, z
		if i&1 != 0 {
			cx += w
		}
		if i&2 != 0 {
			cy += h
		}
		if i&4 != 0 {
			cz += d
		}
		var c [4]int64
		for k := 0; k < 4; k++ {
			c[k] = cx*m[k] + cy*m[4+k] + cz*m[8+k] + 0x1000*m[12+k]
		}
		for k := 0; k < 3; k++ {
			if c[k] > c[3] {
				out[k*2]++
			}
			if c[k] < -c[3] {
				out[k*2+1]++
			}
		}
	}
	s.BoxResult = true
	for _, n := range out {
		if n == 8 {
			s.BoxResult = false
		}
	}
}

// normal lights the vertex colour from a NORMAL command.
func (g *gpu3D) normal(v uint32) {
	s := &g.s
	nx, ny, nz := s10(v), s10(v>>10), s10(v>>20)
	if s.TexParam>>30 == 2 {
		m := &s.Tx
		s.S = int32((nx*m[0]+ny*m[4]+nz*m[8])>>21) + s.RawS
		s.T = int32((nx*m[1]+ny*m[5]+nz*m[9])>>21) + s.RawT
	}
	m := &s.Vec
	n := [3]int64{
		(nx*m[0] + ny*m[4] + nz*m[8]) >> 12,
		(nx*m[1] + ny*m[5] + nz*m[9]) >> 12,
		(nx*m[2] + ny*m[6] + nz*m[10]) >> 12,
	}
	ch := func(c uint32, i int) int64 { return int64(c >> (5 * i) & 31) }
	var col [3]int64
	for i := range col {
		col[i] = ch(s.Emission, i)
	}
	for l := 0; l < 4; l++ {
		if s.CurAttrLive()&(1<<l) == 0 {
			continue
		}
		L := s.LightVec[l]
		dot := (int64(L[0])*n[0] + int64(L[1])*n[1] + int64(L[2])*n[2]) >> 9
		diff := -dot
		if diff < 0 {
			diff = 0
		}
		if diff > 512 {
			diff = 512
		}
		hx, hy, hz := int64(L[0])/2, int64(L[1])/2, (int64(L[2])-512)/2
		sh := -((hx*n[0] + hy*n[1] + hz*n[2]) >> 9)
		if sh < 0 {
			sh = 0
		}
		if sh > 512 {
			sh = 512
		}
		sh = sh * sh >> 9
		if s.ShineOn {
			idx := sh >> 2
			if idx > 127 {
				idx = 127
			}
			sh = int64(s.Shine[idx]) << 1
		}
		for i := range col {
			lc := ch(s.LightCol[l], i)
			col[i] += ch(s.Specular, i)*lc*sh>>14 + ch(s.Diffuse, i)*lc*diff>>14 + ch(s.Ambient, i)*lc>>5
		}
	}
	for i := range col {
		if col[i] > 31 {
			col[i] = 31
		}
		s.Color[i] = int32(col[i])
	}
}

// CurAttrLive: the lights in use are those of the last POLYGON_ATTR, which
// takes effect for lighting at once.
func (s *g3dState) CurAttrLive() uint32 { return s.PolyAttr }

func (g *gpu3D) vertex(x, y, z int64) {
	s := &g.s
	s.LastV = [3]int64{x, y, z}
	if s.TexParam>>30 == 3 {
		m := &s.Tx
		s.S = int32((x*m[0]+y*m[4]+z*m[8])>>24) + s.RawS
		s.T = int32((x*m[1]+y*m[5]+z*m[9])>>24) + s.RawT
	}
	m := &s.Clip
	var c [4]int64
	for k := 0; k < 4; k++ {
		c[k] = (x*m[k] + y*m[4+k] + z*m[8+k] + 0x1000*m[12+k]) >> 12
	}
	v := Vertex{
		X: float64(c[0]), Y: float64(c[1]), Z: float64(c[2]), W: float64(c[3]),
		R: float64(s.Color[0]), G: float64(s.Color[1]), B: float64(s.Color[2]),
		S: float64(s.S), T: float64(s.T),
	}
	s.Prim = append(s.Prim, v)
	s.PrimCount++
	switch s.PrimType {
	case 0:
		if len(s.Prim) == 3 {
			g.addPoly(s.Prim, false)
			s.Prim = s.Prim[:0]
		}
	case 1:
		if len(s.Prim) == 4 {
			g.addPoly(s.Prim, false)
			s.Prim = s.Prim[:0]
		}
	case 2:
		if len(s.Prim) == 3 {
			if s.PrimCount&1 == 0 { // every other triangle is wound the other way
				g.addPoly([]Vertex{s.Prim[1], s.Prim[0], s.Prim[2]}, true)
			} else {
				g.addPoly(s.Prim, true)
			}
			s.Prim = append(s.Prim[:0], s.Prim[1], s.Prim[2])
		}
	case 3:
		if len(s.Prim) == 4 {
			g.addPoly([]Vertex{s.Prim[0], s.Prim[1], s.Prim[3], s.Prim[2]}, true)
			s.Prim = append(s.Prim[:0], s.Prim[2], s.Prim[3])
		}
	}
}

func lerpV(a, b Vertex, t float64) Vertex {
	l := func(x, y float64) float64 { return x + (y-x)*t }
	return Vertex{l(a.X, b.X), l(a.Y, b.Y), l(a.Z, b.Z), l(a.W, b.W), l(a.R, b.R), l(a.G, b.G), l(a.B, b.B), l(a.S, b.S), l(a.T, b.T)}
}

// clipPlane keeps the part of the polygon where d(v) >= 0.
func clipPlane(in []Vertex, d func(v *Vertex) float64) []Vertex {
	if len(in) == 0 {
		return in
	}
	out := make([]Vertex, 0, len(in)+2)
	for i := range in {
		a, b := in[i], in[(i+1)%len(in)]
		da, db := d(&a), d(&b)
		if da >= 0 {
			out = append(out, a)
		}
		if (da >= 0) != (db >= 0) {
			out = append(out, lerpV(a, b, da/(da-db)))
		}
	}
	return out
}

func (g *gpu3D) addPoly(vs []Vertex, strip bool) {
	s := &g.s
	if len(s.Polys) >= 2048 || s.Verts+len(vs) > 6144 {
		s.Disp3DCnt |= 1 << 13 // polygon list overflow
		return
	}
	attr := s.CurAttr
	// facing, from the clip-space vertices: the sign of the triangle's
	// normal against the eye
	a, b, c := vs[0], vs[1], vs[2]
	area := 0.0
	{
		// homogeneous normal: (v1 - v0) × (v2 - v0) · v0, using x, y, w
		x1, y1, w1 := b.X-a.X, b.Y-a.Y, b.W-a.W
		x2, y2, w2 := c.X-a.X, c.Y-a.Y, c.W-a.W
		nx := y1*w2 - w1*y2
		ny := w1*x2 - x1*w2
		nw := x1*y2 - y1*x2
		area = nx*a.X + ny*a.Y + nw*a.W
		if len(vs) == 4 && area == 0 {
			d := vs[3]
			x1, y1, w1 = c.X-a.X, c.Y-a.Y, c.W-a.W
			x2, y2, w2 = d.X-a.X, d.Y-a.Y, d.W-a.W
			nx = y1*w2 - w1*y2
			ny = w1*x2 - x1*w2
			nw = x1*y2 - y1*x2
			area = nx*a.X + ny*a.Y + nw*a.W
		}
	}
	front := area > 0
	if area != 0 {
		if front && attr&(1<<7) == 0 || !front && attr&(1<<6) == 0 {
			return
		}
	}
	far := attr&(1<<12) != 0
	poly := append([]Vertex(nil), vs...)
	if !far {
		for i := range poly {
			if poly[i].Z > poly[i].W {
				return
			}
		}
	}
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W + v.Z }) // near
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W - v.Z })
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W + v.X })
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W - v.X })
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W + v.Y })
	poly = clipPlane(poly, func(v *Vertex) float64 { return v.W - v.Y })
	if len(poly) < 3 {
		return
	}
	alpha := attr >> 16 & 31
	p := Polygon{V: poly, Attr: attr, Tex: s.TexParam, Pal: s.Pltt, Strip: strip, Seq: len(s.Polys), Facing: front}
	texAlpha := s.TexParam>>26&7 == 1 || s.TexParam>>26&7 == 6
	p.Opaque = (alpha == 31 || alpha == 0) && !texAlpha
	s.Polys = append(s.Polys, p)
	s.Verts += len(poly)
}

// vblank: a list finished with SWAP_BUFFERS becomes the one drawn.
func (g *gpu3D) vblank() {
	s := &g.s
	if s.HaveReady {
		s.Render = append(s.Render[:0], s.Ready...)
		s.RenderFlag = s.ReadyFlags
		s.HaveReady = false
	}
}

func (g *gpu3D) saveState() []byte {
	var buf bytes.Buffer
	zw, _ := flate.NewWriter(&buf, flate.BestSpeed)
	if err := gob.NewEncoder(zw).Encode(&g.s); err != nil {
		return nil
	}
	zw.Close()
	return buf.Bytes()
}

func (g *gpu3D) loadState(b []byte) error {
	if len(b) == 0 {
		return nil
	}
	var s g3dState
	if err := gob.NewDecoder(flate.NewReader(bytes.NewReader(b))).Decode(&s); err != nil {
		return err
	}
	g.s = s
	g.renderFrame()
	return nil
}
