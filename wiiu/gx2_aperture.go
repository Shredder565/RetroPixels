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
		ap.copy(m, false)
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
		lo, hi := ap.copy(m, true)
		m.heaps.get(m.heaps.System).expFree(ap.buf)
		if hi > lo {
			m.gpuWritten(lo, hi-lo)
		}
	})
}

// copy moves the surface into the window (or, back, the window into the
// surface), a row at a time, and returns the range of surface written.
func (ap *aperture) copy(m *Machine, back bool) (lo, hi uint32) {
	bpp := ap.lay.BPP / 8
	lo = ^uint32(0)
	row := make([]byte, ap.w*bpp)
	all := m.apertureAddrs(ap)
	for y := uint32(0); y < ap.h; y++ {
		lin := ap.buf + y*ap.lay.Pitch*bpp
		addrs := all[y*ap.w : (y+1)*ap.w]
		if back {
			m.Mem.ReadBytes(lin, row)
			ap.swap(row, bpp)
			for x, a := range addrs {
				m.Mem.WriteBytes(a, row[uint32(x)*bpp:uint32(x+1)*bpp])
				lo, hi = min(lo, a), max(hi, a+bpp)
			}
		} else {
			for x, a := range addrs {
				m.Mem.ReadBytes(a, row[uint32(x)*bpp:uint32(x+1)*bpp])
			}
			ap.swap(row, bpp)
			m.Mem.WriteBytes(lin, row)
		}
	}
	return lo, hi
}

// swap applies the window's byte swapping to elements of bpp bytes (it's
// its own inverse).
func (ap *aperture) swap(b []byte, bpp uint32) {
	unit := 0
	switch ap.endian {
	case 1:
		unit = 2
	case 2:
		unit = 4
	case 3: // the format's own
		switch bpp {
		case 2:
			unit = 2
		case 4, 8, 16:
			unit = 4
		}
	}
	if unit == 0 {
		return
	}
	for i := 0; i+unit <= len(b); i += unit {
		for j := 0; j < unit/2; j++ {
			b[i+j], b[i+unit-1-j] = b[i+unit-1-j], b[i+j]
		}
	}
}

// apertureKey names the element addresses of one surface level and slice.
type apertureKey struct {
	lay         surfLayout
	slice, w, h uint32
}

// apertureAddrs returns where each element of the aperture's surface is,
// row by row. Programs usually open the same surface every frame, so the
// last answer is kept.
func (m *Machine) apertureAddrs(ap *aperture) []uint32 {
	k := apertureKey{ap.lay, ap.slice, ap.w, ap.h}
	if m.apAddrs != nil && m.apAddrsKey == k {
		return m.apAddrs
	}
	a := make([]uint32, ap.w*ap.h)
	for y := uint32(0); y < ap.h; y++ {
		ap.lay.rowAddrs(0, y, ap.slice, a[y*ap.w:(y+1)*ap.w])
	}
	m.apAddrs, m.apAddrsKey = a, k
	return a
}
