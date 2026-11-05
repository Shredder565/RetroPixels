package ctr

// The geometry shader stage. When a draw uses it, the vertex shader's
// outputs aren't drawn: they're gathered up and handed to the second
// shader unit, which makes the triangles itself (SETEMIT says which of
// three vertex slots the next EMIT fills, and whether that completes a
// triangle and with which winding). The outputs reach the geometry shader
// in one of three ways (register 0x252's low byte):
//
//   - points: a vertex's outputs (or a few vertices') become its input
//     registers;
//   - fixed primitives: a fixed number of vertices go into its float
//     uniforms, from a starting one;
//   - variable primitives: the index list says how many vertices each
//     primitive has; the count goes to uniform 0 and the vertices after
//     it (whole for the first ones, only their first output after that).

type gsPipeline struct {
	mode      uint32
	buf       [16]vec4 // points: the inputs gathered so far
	n         int      // vec4s gathered
	wantCount bool     // variable: the next index is a vertex count
	main      int      // variable: vertices still to take whole
	left      int      // variable: vertices still to take
	slots     [3]gpuVertex
}

func (g *gpu) gsStart() {
	p := &g.gsp
	p.mode = g.reg[0x252] & 0xff
	p.n = 0
	p.wantCount = p.mode == 1
}

// gsCount starts a variable primitive of n vertices.
func (g *gpu) gsCount(n uint32) {
	p := &g.gsp
	f := float32(n)
	g.gs.f[0] = vec4{f, f, f, f}
	p.n = 1
	p.main = int(g.reg[0x254]&0xff) + 1
	p.left = int(n)
	p.wantCount = n == 0
}

// gsSubmit hands one vertex's outputs to the geometry stage.
func (g *gpu) gsSubmit(o *vsOutputs) {
	p := &g.gsp
	vsN := min(int(g.reg[0x24a]&0xf)+1, 16)
	switch p.mode {
	case 0: // points
		inN := int(g.reg[0x289]&0xf) + 1
		for k := 0; k < vsN && p.n < len(p.buf); k++ {
			p.buf[p.n] = o.r[k]
			p.n++
		}
		if p.n >= inN {
			var u shaderUnit
			m := uint64(g.reg[0x28b]) | uint64(g.reg[0x28c])<<32
			for k := 0; k < inN; k++ {
				u.in[m>>(4*k)&0xf] = p.buf[k]
			}
			p.n = 0
			g.runGS(&u)
		}
	case 1: // variable primitives
		if p.main > 0 {
			for k := 0; k < vsN && p.n < len(g.gs.f); k++ {
				g.gs.f[p.n] = o.r[k]
				p.n++
			}
			p.main--
		} else if p.n < len(g.gs.f) {
			g.gs.f[p.n] = o.r[0]
			p.n++
		}
		p.left--
		if p.left <= 0 {
			p.wantCount = true
			var u shaderUnit
			g.runGS(&u)
		}
	case 2: // fixed primitives
		start := int(g.reg[0x252] >> 16 & 0xff)
		verts := int(g.reg[0x252]>>8&0xf) + 1
		for k := 0; k < vsN; k++ {
			if i := start + p.n; i < len(g.gs.f) {
				g.gs.f[i] = o.r[k]
			}
			p.n++
		}
		if p.n >= vsN*verts {
			p.n = 0
			var u shaderUnit
			g.runGS(&u)
		}
	default:
		g.warn("this geometry shader mode")
	}
}

// runGS runs the geometry shader; its EMITs make the triangles.
func (g *gpu) runGS(u *shaderUnit) {
	p := &g.gsp
	mask := g.reg[0x28d]
	u.emit = func(u *shaderUnit, winding, prim bool, id int) {
		if id > 2 {
			return
		}
		o := packOutputs(u, mask)
		p.slots[id] = g.vertex(&o)
		if prim {
			if winding {
				g.triangle(&p.slots[1], &p.slots[0], &p.slots[2])
			} else {
				g.triangle(&p.slots[0], &p.slots[1], &p.slots[2])
			}
		}
	}
	u.run(&g.gs)
	g.gs.b |= 1 << 15 // tells the program it has run before
}
