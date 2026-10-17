package wiiu

// GX2's fixed-function state, set directly (GX2SetBlendControl(...)) or
// worked out ahead into a small structure (GX2InitBlendControlReg(&reg,
// ...)) and set from it later (GX2SetBlendControlReg(&reg)). The
// structure holds the register values; programs don't look inside it.

// stateReg is one kind of state: how many words its structure has, how to
// work out the values from the arguments (ints from argument index ai,
// floats from float argument 0), and how to set them.
type stateReg struct {
	name  string // e.g. "BlendControl"
	words uint32
	calc  func(a func(int) uint32, f func(int) float32) []uint32
	set   func(m *Machine, v []uint32)
}

var stateRegs = []stateReg{
	{"Viewport", 12, func(a func(int) uint32, f func(int) float32) []uint32 {
		x0, y0, w, h, n, fr := f(0), f(1), f(2), f(3), f(4), f(5)
		return []uint32{f2u(w / 2), f2u(x0 + w/2), f2u(-h / 2), f2u(y0 + h/2), f2u((fr - n) / 2), f2u((fr + n) / 2),
			f2u(min(n, fr)), f2u(max(n, fr))}
	}, func(m *Machine, v []uint32) {
		b := &pm4{}
		b.set(regPA_CL_VPORT_XSCALE_0, v[:6]...)
		b.set(regPA_SC_VPORT_ZMIN_0, v[6], v[7])
		m.emit(b)
	}},
	{"Scissor", 2, func(a func(int) uint32, f func(int) float32) []uint32 {
		x0, y0, w, h := a(0), a(1), a(2), a(3)
		return []uint32{x0&0x7fff | (y0&0x7fff)<<16 | 1<<31, (x0+w)&0x7fff | ((y0+h)&0x7fff)<<16}
	}, func(m *Machine, v []uint32) { m.setRegs(regPA_SC_GENERIC_SCISSOR_TL, v[0], v[1]) }},
	{"ColorControl", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		rop, mask, multi, write := a(0), a(1), a(2), a(3)
		if write == 0 {
			rop = 0xaa // leave the target as it is
		}
		return []uint32{b2u(multi != 0)<<1 | 1<<7 | (mask&0xff)<<8 | (rop&0xff)<<16}
	}, func(m *Machine, v []uint32) { m.setRegs(regCB_COLOR_CONTROL, v[0]) }},
	{"BlendControl", 2, func(a func(int) uint32, f func(int) float32) []uint32 {
		// (target, colour src, dst, combine, separate alpha, alpha src, dst, combine)
		return []uint32{a(0), a(1)&0x1f | (a(3)&7)<<5 | (a(2)&0x1f)<<8 |
			(a(5)&0x1f)<<16 | (a(7)&7)<<21 | (a(6)&0x1f)<<24 | b2u(a(4) != 0)<<29}
	}, func(m *Machine, v []uint32) {
		if v[0] <= 7 {
			m.setRegs(regCB_BLEND0_CONTROL+v[0]*4, v[1])
		}
	}},
	{"BlendConstantColor", 4, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{f2u(f(0)), f2u(f(1)), f2u(f(2)), f2u(f(3))}
	}, func(m *Machine, v []uint32) { m.setRegs(regCB_BLEND_RED, v...) }},
	{"DepthStencilControl", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{b2u(a(3) != 0) | b2u(a(0) != 0)<<1 | b2u(a(1) != 0)<<2 | (a(2)&7)<<4 | b2u(a(4) != 0)<<7 |
			(a(5)&7)<<8 | (a(8)&7)<<11 | (a(6)&7)<<14 | (a(7)&7)<<17 |
			(a(9)&7)<<20 | (a(12)&7)<<23 | (a(10)&7)<<26 | (a(11)&7)<<29}
	}, func(m *Machine, v []uint32) { m.setRegs(regDB_DEPTH_CONTROL, v[0]) }},
	{"StencilMask", 2, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{a(2)&0xff | (a(0)&0xff)<<8 | (a(1)&0xff)<<16, a(5)&0xff | (a(3)&0xff)<<8 | (a(4)&0xff)<<16}
	}, func(m *Machine, v []uint32) { m.setRegs(regDB_STENCILREFMASK, v[0], v[1]) }},
	{"PolygonControl", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{(a(1) & 1) | (a(2)&1)<<1 | (a(0)&1)<<2 | (a(3)&1)<<3 |
			(a(4)&7)<<5 | (a(5)&7)<<8 | (a(6)&1)<<11 | (a(7)&1)<<12 | (a(8)&1)<<13}
	}, func(m *Machine, v []uint32) { m.setRegs(regPA_SU_SC_MODE_CNTL, v[0]) }},
	{"PolygonOffset", 5, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{f2u(f(1) * 16), f2u(f(0)), f2u(f(3) * 16), f2u(f(2)), f2u(f(4))}
	}, func(m *Machine, v []uint32) {
		m.setRegs(regPA_SU_POLY_OFFSET_FRONT_SCALE, v[:4]...)
		m.setRegs(regPA_SU_POLY_OFFSET_CLAMP, v[4])
	}},
	{"AlphaTest", 2, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{a(1)&7 | b2u(a(0) != 0)<<3, f2u(f(0))}
	}, func(m *Machine, v []uint32) {
		m.setRegs(regSX_ALPHA_TEST_CONTROL, v[0])
		m.setRegs(regSX_ALPHA_REF, v[1])
	}},
	{"TargetChannelMasks", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		var v uint32
		for i := 0; i < 8; i++ {
			v |= (a(i) & 15) << (4 * i)
		}
		return []uint32{v}
	}, func(m *Machine, v []uint32) { m.setRegs(regCB_TARGET_MASK, v[0]) }},
	{"PointSize", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{uint32(f(1)*8)&0xffff | (uint32(f(0)*8)&0xffff)<<16}
	}, func(m *Machine, v []uint32) { m.setRegs(regPA_SU_POINT_SIZE, v[0]) }},
	{"PointLimits", 1, func(a func(int) uint32, f func(int) float32) []uint32 {
		return []uint32{uint32(f(0)*8)&0xffff | (uint32(f(1)*8)&0xffff)<<16}
	}, func(m *Machine, v []uint32) { m.setRegs(regPA_SU_POINT_MINMAX, v[0]) }},
}

func init() {
	x := "gx2"
	for _, sr := range stateRegs {
		sr := sr
		args := func(m *Machine, cpu *CPU, first int) []uint32 {
			return sr.calc(func(i int) uint32 { return stackArg(m, cpu, first+i) },
				func(i int) float32 { return argF32(cpu, i) })
		}
		// GX2SetX(args): set now
		reg(x, "GX2Set"+sr.name, func(m *Machine, cpu *CPU, t *Thread) { sr.set(m, args(m, cpu, 0)) })
		// GX2InitXReg(&reg, args): work it out into reg
		reg(x, "GX2Init"+sr.name+"Reg", func(m *Machine, cpu *CPU, t *Thread) {
			p := arg(cpu, 0)
			v := args(m, cpu, 1)
			for i := uint32(0); i < sr.words; i++ {
				var w uint32
				if int(i) < len(v) {
					w = v[i]
				}
				m.Mem.Write32(p+i*4, w)
			}
		})
		// GX2SetXReg(&reg): set what Init worked out
		reg(x, "GX2Set"+sr.name+"Reg", func(m *Machine, cpu *CPU, t *Thread) {
			p := arg(cpu, 0)
			v := make([]uint32, sr.words)
			for i := range v {
				v[i] = m.Mem.Read32(p + uint32(i)*4)
			}
			sr.set(m, v)
		})
	}
	reg(x, "GX2SetDepthOnlyControl", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegs(regDB_DEPTH_CONTROL, b2u(arg(cpu, 0) != 0)<<1|b2u(arg(cpu, 1) != 0)<<2|(arg(cpu, 2)&7)<<4)
	})
	reg(x, "GX2SetCullOnlyControl", func(m *Machine, cpu *CPU, t *Thread) {
		m.setRegBits(regPA_SU_SC_MODE_CNTL, 7, (arg(cpu, 1)&1)|(arg(cpu, 2)&1)<<1|(arg(cpu, 0)&1)<<2)
	})
	reg(x, "GX2SetAlphaToMask", func(m *Machine, cpu *CPU, t *Thread) {})
}
