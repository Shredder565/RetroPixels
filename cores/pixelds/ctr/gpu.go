package ctr

import "math"

// gpu is the PICA200: command lists write its registers; vertex data is
// loaded from memory (or sent through registers), run through the vertex
// shader, put together into triangles, clipped, and drawn by the
// rasterizer in raster.go into the colour and depth buffers in memory.
type gpu struct {
	m   *Machine
	reg [0x400]uint32

	vs, gs shaderSetup

	// fixed attributes and immediate-mode vertices
	fixed     [16]vec4
	fixIdx    uint32
	fixBuf    [4]uint32
	fixN      int
	immAttr   int
	immVertex [16]vec4

	luts           [24][256]uint32
	fogLUT         [128]uint32
	fogIdx         uint32
	lutIdx, lutSel uint32

	asm      assembler
	warned   map[string]bool
	jumpTo   uint32
	jumpSize uint32
	jumped   bool
	draws    int
}

func newGPU(m *Machine) *gpu { return &gpu{m: m, warned: map[string]bool{}} }

func (g *gpu) warn(what string) {
	if !g.warned[what] {
		g.warned[what] = true
		g.m.logf("GPU: %s isn't done yet", what)
	}
}

// writeReg writes a register through a byte mask and does what writing
// it does.
func (g *gpu) writeReg(id, v, mask uint32) {
	if id >= uint32(len(g.reg)) {
		return
	}
	v = g.reg[id]&^mask | v&mask
	g.reg[id] = v
	switch {
	case id == 0x22e || id == 0x22f:
		g.draw(id == 0x22f)
	case id == 0x232:
		g.fixIdx = v & 0xf
		g.fixN = 0
		g.immAttr = 0
	case id >= 0x233 && id <= 0x235:
		g.fixBuf[g.fixN] = v
		g.fixN++
		if g.fixN == 3 {
			g.fixN = 0
			a := unpack4f24(g.fixBuf)
			if g.fixIdx == 0xf { // immediate mode: one attribute of a vertex
				g.immVertex[g.immAttr] = a
				g.immAttr++
				if g.immAttr >= g.numAttribs() {
					g.immAttr = 0
					g.immediateVertex()
				}
			} else if g.fixIdx < 12 {
				g.fixed[g.fixIdx] = a
			}
		}
	case id == 0x23c || id == 0x23d: // jump to another command buffer
		ch := id - 0x23c
		g.jumpTo = g.reg[0x23a+ch] << 3
		g.jumpSize = g.reg[0x238+ch] << 3
		g.jumped = true
	case id == 0x0e6:
		g.fogIdx = v & 0x7f
	case id >= 0x0e8 && id <= 0x0ef:
		g.fogLUT[g.fogIdx&0x7f] = v
		g.fogIdx++
	case id == 0x1c5:
		g.lutIdx, g.lutSel = v&0xff, v>>8&0x1f
	case id >= 0x1c8 && id <= 0x1cf:
		if g.lutSel < 24 {
			g.luts[g.lutSel][g.lutIdx&0xff] = v
		}
		g.lutIdx++
	case id == 0x25f:
		g.asm.reset()
	case id >= 0x280 && id < 0x2b0:
		g.shaderReg(&g.gs, id-0x280, v)
	case id >= 0x2b0 && id < 0x2e0:
		g.shaderReg(&g.vs, id-0x2b0, v)
		if g.reg[0x244]&1 != 0 && (id < 0x2b9 || id >= 0x2c0) { // the geometry shader shares the vertex shader's program and uniforms
			g.shaderReg(&g.gs, id-0x2b0, v)
		}
	}
}

// shaderReg: the registers of a shader unit, from its first one.
func (g *gpu) shaderReg(s *shaderSetup, r, v uint32) {
	switch {
	case r == 0x00:
		s.b = uint16(v)
	case r >= 0x01 && r <= 0x04:
		s.i[r-1] = [3]uint8{uint8(v), uint8(v >> 8), uint8(v >> 16)}
	case r == 0x0a:
		s.entry = v & 0xffff
	case r == 0x10:
		s.fIdx = v & 0xff
		s.fF32 = v>>31 != 0
		s.fN = 0
	case r >= 0x11 && r <= 0x18:
		s.writeUniform(v)
	case r == 0x1b:
		s.codeIdx = v & 0xfff
	case r >= 0x1c && r <= 0x23:
		if s.codeIdx < 4096 {
			s.code[s.codeIdx] = v
		}
		s.codeIdx++
	case r == 0x25:
		s.swzIdx = v & 0x1ff
	case r >= 0x26 && r <= 0x2d:
		if s.swzIdx < 512 {
			s.swz[s.swzIdx] = v
		}
		s.swzIdx++
	}
}

func (g *gpu) readReg(id uint32) uint32 {
	if id < uint32(len(g.reg)) {
		return g.reg[id]
	}
	return 0
}

// runCommandList runs a command list at physical address pa: each
// command is a parameter word then a header (register, byte mask, extra
// parameter count, consecutive flag). A write to a jump register
// continues in another buffer.
func (g *gpu) runCommandList(pa, size uint32) {
	for jumps := 0; jumps < 1024; jumps++ {
		buf := g.m.mem.physSlice(pa, size)
		if buf == nil {
			return
		}
		word := func(i uint32) uint32 {
			return uint32(buf[i]) | uint32(buf[i+1])<<8 | uint32(buf[i+2])<<16 | uint32(buf[i+3])<<24
		}
		g.jumped = false
		for i := uint32(0); i+8 <= size && !g.jumped; {
			param, hdr := word(i), word(i+4)
			i += 8
			id := hdr & 0x3ff
			mask := expandMask(hdr >> 16 & 15)
			extra := hdr >> 20 & 0xff
			seq := hdr&(1<<31) != 0
			g.writeReg(id, param, mask)
			for n := uint32(0); n < extra && i+4 <= size; n++ {
				if seq {
					id++
				}
				g.writeReg(id, word(i), mask)
				i += 4
			}
			if extra&1 != 0 { // keep 8-byte alignment
				i += 4
			}
		}
		if !g.jumped {
			return
		}
		pa, size = g.jumpTo, g.jumpSize
	}
}

func expandMask(m uint32) uint32 {
	var r uint32
	for i := uint32(0); i < 4; i++ {
		if m>>i&1 != 0 {
			r |= 0xff << (8 * i)
		}
	}
	return r
}

// ---------------- vertices ----------------

// A vertex after the shader: its outputs by meaning (see outSem).
type gpuVertex struct {
	a [24]float32
	// screen position, after the viewport
	sx, sy, sz, invW float32
}

// What the shader's outputs mean (the output map registers' values).
const (
	semPos   = 0x00 // x y z w
	semQuat  = 0x04
	semColor = 0x08 // r g b a
	semTC0   = 0x0c // u v
	semTC1   = 0x0e
	semTC0W  = 0x10
	semView  = 0x12 // x y z
	semTC2   = 0x16
)

func (g *gpu) numAttribs() int { return int(g.reg[0x202]>>28) + 1 }

// shade runs the vertex shader on the attributes and maps its outputs.
func (g *gpu) shade(attrs *[16]vec4) gpuVertex {
	var u shaderUnit
	n := int(g.reg[0x2b9]&0xf) + 1
	vmap := uint64(g.reg[0x2bb]) | uint64(g.reg[0x2bc])<<32
	for k := 0; k < n && k < 16; k++ {
		u.in[vmap>>(4*k)&0xf] = attrs[k]
	}
	u.run(&g.vs)
	var v gpuVertex
	v.a[semPos+3] = 1
	outMask := g.reg[0x2bd]
	nOut := int(g.reg[0x04f] & 7)
	k := 0
	for r := 0; r < 16 && k < nOut; r++ {
		if outMask>>r&1 == 0 {
			continue
		}
		sem := g.reg[0x050+k]
		for c := 0; c < 4; c++ {
			s := sem >> (8 * c) & 0x1f
			if s < 24 {
				v.a[s] = u.out[r][c]
			}
		}
		k++
	}
	return v
}

func (g *gpu) immediateVertex() {
	attrs := g.immVertex
	v := g.shade(&attrs)
	g.asm.mode = g.reg[0x25e] >> 8 & 3
	g.asm.add(v, g.triangle)
}

// loadVertex reads vertex number idx from the vertex arrays.
func (g *gpu) loadVertex(idx uint32, attrs *[16]vec4) {
	mem := g.m.mem
	base := g.reg[0x200] << 3
	n := g.numAttribs()
	fixedMask := g.reg[0x202] >> 16 & 0xfff
	for i := 0; i < n && i < 12; i++ {
		if fixedMask>>i&1 != 0 {
			attrs[i] = g.fixed[i]
		} else {
			attrs[i] = vec4{0, 0, 0, 1}
		}
	}
	for j := uint32(0); j < 12; j++ {
		off := g.reg[0x203+3*j]
		c1 := g.reg[0x204+3*j]
		c2 := g.reg[0x205+3*j]
		count := c2 >> 28
		stride := c2 >> 16 & 0xff
		if count == 0 {
			continue
		}
		addr := base + off + idx*stride
		for k := uint32(0); k < count; k++ {
			var a uint32
			if k < 8 {
				a = c1 >> (4 * k) & 0xf
			} else {
				a = c2 >> (4 * (k - 8)) & 0xf
			}
			if a >= 12 { // padding
				addr += 4 * (a - 11)
				continue
			}
			var f uint32
			if a < 8 {
				f = g.reg[0x201] >> (4 * a) & 0xf
			} else {
				f = g.reg[0x202] >> (4 * (a - 8)) & 0xf
			}
			typ, comps := f&3, f>>2+1
			size := [4]uint32{1, 1, 2, 4}[typ]
			addr = (addr + size - 1) &^ (size - 1)
			v := vec4{0, 0, 0, 1}
			pa := addr
			for c := uint32(0); c < comps; c++ {
				p := mem.physSlice(pa, size)
				if p == nil {
					break
				}
				switch typ {
				case 0:
					v[c] = float32(int8(p[0]))
				case 1:
					v[c] = float32(p[0])
				case 2:
					v[c] = float32(int16(uint16(p[0]) | uint16(p[1])<<8))
				case 3:
					v[c] = math.Float32frombits(uint32(p[0]) | uint32(p[1])<<8 | uint32(p[2])<<16 | uint32(p[3])<<24)
				}
				pa += size
			}
			addr += size * comps
			if fixedMask>>a&1 == 0 {
				attrs[a] = v
			}
		}
	}
}

// draw runs a DrawArrays (or DrawElements with indexed).
func (g *gpu) draw(indexed bool) {
	g.draws++
	if g.reg[0x229]&3 != 0 || g.reg[0x244]&1 != 0 && g.reg[0x229]&2 != 0 {
		g.warn("the geometry shader")
	}
	count := g.reg[0x228]
	first := g.reg[0x22a]
	ib := g.reg[0x227]
	ibAddr := g.reg[0x200]<<3 + ib&0x0fffffff
	big := ib>>31 != 0
	g.asm.reset()
	g.asm.mode = g.reg[0x25e] >> 8 & 3
	cache := map[uint32]gpuVertex{}
	var attrs [16]vec4
	for i := uint32(0); i < count; i++ {
		idx := first + i
		if indexed {
			if big {
				p := g.m.mem.physSlice(ibAddr+2*i, 2)
				if p == nil {
					return
				}
				idx = uint32(p[0]) | uint32(p[1])<<8
			} else {
				p := g.m.mem.physSlice(ibAddr+i, 1)
				if p == nil {
					return
				}
				idx = uint32(p[0])
			}
			if v, ok := cache[idx]; ok {
				g.asm.add(v, g.triangle)
				continue
			}
		}
		g.loadVertex(idx, &attrs)
		v := g.shade(&attrs)
		if indexed {
			cache[idx] = v
		}
		g.asm.add(v, g.triangle)
	}
}

// assembler puts vertices together into triangles: lists, strips, fans.
type assembler struct {
	mode  uint32 // 0 list, 1 strip, 2 fan, 3 list (geometry shader)
	buf   [2]gpuVertex
	n     int
	ready bool
}

func (a *assembler) reset() { a.n, a.ready = 0, false }

func (a *assembler) add(v gpuVertex, tri func(a, b, c *gpuVertex)) {
	switch a.mode {
	case 1, 2:
		if a.ready {
			tri(&a.buf[0], &a.buf[1], &v)
		}
		a.buf[a.n] = v
		if a.n == 1 {
			a.ready = true
		}
		if a.mode == 1 {
			a.n ^= 1
		} else {
			a.n = 1
		}
	default:
		if a.n < 2 {
			a.buf[a.n] = v
			a.n++
			return
		}
		tri(&a.buf[0], &a.buf[1], &v)
		a.n = 0
	}
}
