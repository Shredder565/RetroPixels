package wiiu

// Tiling apertures: a window where the CPU writes a tiled surface as if it
// were linear (row after row); the GPU's memory controller tiles the data
// on its way through. Here the window is a linear buffer, tiled into the
// surface when the program lets go of it (GX2FreeTilingAperture).

type aperture struct {
	buf         uint32
	lay         surfLayout
	w, h, slice uint32
	endian      uint32
}

func init() {
	x := "gx2"
	alloc := func(m *Machine, cpu *CPU, s, level, slice, endian, hOut, aOut uint32) {
		gs := m.readSurface(s)
		lay, w, h := gs.level(level)
		bpp := lay.BPP / 8
		size := lay.Pitch * h * bpp
		if size == 0 || lay.Base == 0 {
			ret(cpu, 0)
			return
		}
		buf := m.heaps.allocSystem(m, size, 0x100)
		// what's there already shows through
		ap := &aperture{buf: buf, lay: lay, w: w, h: h, slice: slice, endian: endian}
		ap.each(m, func(lin, tiled uint32) { m.Mem.WriteBytes(lin, ap.swap(m.readN(tiled, bpp))) })
		if m.apertures == nil {
			m.apertures = map[uint32]*aperture{}
		}
		m.nextAperture++
		id := m.nextAperture
		m.apertures[id] = ap
		if hOut != 0 {
			m.Mem.Write32(hOut, id)
		}
		if aOut != 0 {
			m.Mem.Write32(aOut, buf)
		}
	}
	reg(x, "GX2AllocateTilingApertureEx", func(m *Machine, cpu *CPU, t *Thread) {
		alloc(m, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5))
	})
	reg(x, "GX2AllocateTilingAperture", func(m *Machine, cpu *CPU, t *Thread) {
		// (surface, level, slice, endian, handle out, address out)
		alloc(m, cpu, arg(cpu, 0), arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4), arg(cpu, 5))
	})
	reg(x, "GX2FreeTilingAperture", func(m *Machine, cpu *CPU, t *Thread) {
		ap := m.apertures[arg(cpu, 0)]
		if ap == nil {
			return
		}
		delete(m.apertures, arg(cpu, 0))
		bpp := ap.lay.BPP / 8
		lo, hi := ^uint32(0), uint32(0)
		ap.each(m, func(lin, tiled uint32) {
			b := make([]byte, bpp)
			m.Mem.ReadBytes(lin, b)
			m.Mem.WriteBytes(tiled, ap.swap(b))
			lo, hi = min(lo, tiled), max(hi, tiled+bpp)
		})
		m.heaps.get(m.heaps.System).expFree(ap.buf)
		if hi > lo {
			m.gpuWritten(lo, hi-lo)
		}
	})
}

// each visits every element: its address in the window and in the surface.
func (ap *aperture) each(m *Machine, f func(lin, tiled uint32)) {
	bpp := ap.lay.BPP / 8
	for y := uint32(0); y < ap.h; y++ {
		for x := uint32(0); x < ap.w; x++ {
			f(ap.buf+(y*ap.lay.Pitch+x)*bpp, ap.lay.addr(x, y, ap.slice))
		}
	}
}

// swap applies the window's byte swapping (it's its own inverse).
func (ap *aperture) swap(b []byte) []byte {
	unit := 0
	switch ap.endian {
	case 1:
		unit = 2
	case 2:
		unit = 4
	case 3: // the format's own
		switch len(b) {
		case 2:
			unit = 2
		case 4, 8, 16:
			unit = 4
		}
	}
	if unit == 0 {
		return b
	}
	for i := 0; i+unit <= len(b); i += unit {
		for j := 0; j < unit/2; j++ {
			b[i+j], b[i+unit-1-j] = b[i+unit-1-j], b[i+j]
		}
	}
	return b
}

func (m *Machine) readN(a, n uint32) []byte {
	b := make([]byte, n)
	m.Mem.ReadBytes(a, b)
	return b
}
