package wiiu

// Latte surface operations: colour and depth buffers as the registers
// describe them, and the clears and copies GX2 asks for.

import (
	"encoding/binary"
	"math"
)

// cbDesc is a colour buffer, from its CB_COLOR* registers.
type cbDesc struct {
	lay    surfLayout
	format uint32
	kind   numKind
	slice  uint32
	ok     bool
}

func splitSwizzle(phys, mode uint32) (base, swizzle uint32) {
	if isMacroTiled(mode) {
		return phys &^ 0x700, phys & 0x700
	}
	return phys, 0
}

func cbFromRegs(base, size, info, view uint32) cbDesc {
	pitch := (size&0x3ff + 1) * 8
	tiles := (size>>10&0xfffff + 1)
	height := tiles * 64 / pitch
	format := info >> 2 & 0x3f
	mode := info >> 8 & 0xf
	b, sw := splitSwizzle(base<<8, mode)
	d := cbDesc{format: format, kind: numKindFromCB(info >> 12), slice: view & 0x7ff, ok: base != 0 && format != 0}
	d.lay = surfLayout{Base: b, Pitch: pitch, Height: height, Slices: 1, BPP: formatBits(format), Mode: mode, Samples: 1, Swizzle: sw}
	if d.lay.BPP == 0 {
		d.ok = false
	}
	return d
}

// cb returns colour buffer i as the registers set it.
func (g *Latte) cb(i int) cbDesc {
	o := uint32(i) * 4
	return cbFromRegs(g.reg(regCB_COLOR0_BASE+o), g.reg(regCB_COLOR0_SIZE+o), g.reg(regCB_COLOR0_INFO+o), g.reg(regCB_COLOR0_VIEW+o))
}

// dbDesc is a depth buffer.
type dbDesc struct {
	lay    surfLayout
	format uint32 // DB_DEPTH_INFO.FORMAT: 1 16, 2/3 24 (+8 stencil), 4/5 24 float, 6 32F, 7 32F + 8 stencil
	slice  uint32
	ok     bool
}

func dbFromRegs(base, size, info, view uint32) dbDesc {
	pitch := (size&0x3ff + 1) * 8
	tiles := (size>>10&0xfffff + 1)
	height := tiles * 64 / pitch
	format := info & 7
	mode := info >> 15 & 0xf
	b, sw := splitSwizzle(base<<8, mode)
	bpp := uint32(32)
	switch format {
	case 1:
		bpp = 16
	case 7:
		bpp = 64
	}
	d := dbDesc{format: format, slice: view & 0x7ff, ok: base != 0 && format != 0}
	d.lay = surfLayout{Base: b, Pitch: pitch, Height: height, Slices: 1, BPP: bpp, Mode: mode, Samples: 1, Depth: true, Swizzle: sw}
	return d
}

func (g *Latte) db() dbDesc {
	return dbFromRegs(g.reg(regDB_DEPTH_BASE), g.reg(regDB_DEPTH_SIZE), g.reg(regDB_DEPTH_INFO), g.reg(regDB_DEPTH_VIEW))
}

func (d *dbDesc) hasStencil() bool {
	return d.format == 2 || d.format == 3 || d.format == 5 || d.format == 7
}

// readDepth gives the depth (0..1) and stencil at an element's address.
func (g *Latte) readDepth(d *dbDesc, a uint32) (float32, uint8) {
	m := g.mem
	switch d.format {
	case 1:
		return float32(bswap16(m.Read16(a))) / 65535, 0
	case 6:
		return u2f(bswap32(m.Read32(a))), 0
	case 7:
		return u2f(bswap32(m.Read32(a))), m.Read8(a + 4)
	}
	v := bswap32(m.Read32(a))
	return float32(v&0xffffff) / 0xffffff, uint8(v >> 24)
}

func (g *Latte) writeDepth(d *dbDesc, a uint32, z float32, s uint8, wz, ws bool) {
	m := g.mem
	switch d.format {
	case 1:
		if wz {
			m.Write16(a, bswap16(uint16(math.Floor(float64(clamp01(z))*65535+0.5))))
		}
	case 6:
		if wz {
			m.Write32(a, bswap32(f2u(z)))
		}
	case 7:
		if wz {
			m.Write32(a, bswap32(f2u(z)))
		}
		if ws {
			m.Write8(a+4, s)
		}
	default:
		v := bswap32(m.Read32(a))
		if wz {
			v = v&0xff000000 | uint32(math.Floor(float64(clamp01(z))*0xffffff+0.5))
		}
		if ws {
			v = v&0xffffff | uint32(s)<<24
		}
		m.Write32(a, bswap32(v))
	}
}

// sliceBytes: the size of one slice of a layout.
func (s *surfLayout) sliceBytes() uint32 {
	return (s.Pitch*s.Height*s.BPP*max(s.Samples, 1) + 7) / 8
}

// rowAddrs fills out with the addresses of elements x0.. of row y.
func (s *surfLayout) rowAddrs(x0, y, z uint32, out []uint32) {
	// inside a micro tile of a thin, single-sample surface of up to 32 bits
	// per element, addresses only move within one 256-byte group
	fast := s.BPP <= 32 && tileThickness(s.Mode) == 1 && max(s.Samples, 1) == 1 &&
		s.Mode != tileLinearGeneral && s.Mode != tileLinearAligned && s.Mode != tileLinearSpecial
	if !fast {
		for i := range out {
			out[i] = s.addr(x0+uint32(i), y, z)
		}
		return
	}
	tileX := ^uint32(0)
	var tileBase uint32
	for i := range out {
		x := x0 + uint32(i)
		if x>>3 != tileX {
			tileX = x >> 3
			tx := x &^ 7
			ty := y &^ 7
			tileBase = s.addr(tx, ty, z) - pixelIndexInMicroTile(0, 0, z, s.BPP, s.Mode, s.Depth)*s.BPP/8
		}
		out[i] = tileBase + pixelIndexInMicroTile(x&7, y&7, z, s.BPP, s.Mode, s.Depth)*s.BPP/8
	}
}

// fillSlice sets every element of slice z to the bytes in px.
func (g *Latte) fillSlice(lay *surfLayout, z uint32, px []byte) {
	if lay.Mode == tileLinearGeneral || lay.Mode == tileLinearAligned || lay.Mode == tileLinearSpecial ||
		lay.BPP%8 != 0 || 2048%len(px) != 0 || tileThickness(lay.Mode) > 1 {
		// element by element
		addrs := make([]uint32, lay.Pitch)
		for y := uint32(0); y < lay.Height; y++ {
			lay.rowAddrs(0, y, z, addrs)
			for _, a := range addrs {
				g.mem.WriteBytes(a, px)
			}
		}
		return
	}
	// a slice of a tiled surface covers one contiguous range
	n := lay.sliceBytes()
	start := lay.Base + n*z
	buf := make([]byte, 4096)
	for i := range buf {
		buf[i] = px[i%len(px)]
	}
	for o := uint32(0); o < n; o += uint32(len(buf)) {
		g.mem.WriteBytes(start+o, buf[:min(uint32(len(buf)), n-o)])
	}
}

func (g *Latte) clearColor(cb cbDesc, c [4]float32) {
	if !cb.ok {
		return
	}
	g.tex.invalidate(surfRange(&cb.lay, cb.slice))
	if g.hw != nil {
		if g.hw.clearColor(&cb, c) {
			return
		}
		g.hw.invalidate(surfRange(&cb.lay, cb.slice))
	}
	px := make([]byte, fmtBytes(cb.format))
	v := [4]uint32{f2u(c[0]), f2u(c[1]), f2u(c[2]), f2u(c[3])}
	if cb.kind == kUint || cb.kind == kSint {
		for i := range v {
			v[i] = uint32(int32(c[i]))
		}
	}
	encodeElem(cb.format, cb.kind, v, px)
	g.fillSlice(&cb.lay, cb.slice, px)
	g.tex.invalidate(cb.lay.Base, cb.lay.Base+cb.lay.sliceBytes()*(cb.slice+1))
}

// clearDepth: flags 1 = depth, 2 = stencil
func (g *Latte) clearDepth(db dbDesc, z float32, stencil, flags uint32) {
	if !db.ok || flags&3 == 0 {
		return
	}
	g.tex.invalidate(surfRange(&db.lay, db.slice))
	if g.hw != nil {
		if g.hw.clearDepth(&db, z, stencil, flags) {
			return
		}
		g.hw.flush(surfRange(&db.lay, db.slice)) // a partial clear keeps the rest
		g.hw.invalidate(surfRange(&db.lay, db.slice))
	}
	wz, ws := flags&1 != 0, flags&2 != 0 && db.hasStencil()
	full := wz && (ws || !db.hasStencil())
	if full {
		px := make([]byte, db.lay.BPP/8)
		// build the element through writeDepth on a scratch page
		var v uint32
		switch db.format {
		case 1:
			x := uint16(math.Floor(float64(clamp01(z))*65535 + 0.5))
			px[0], px[1] = byte(x), byte(x>>8)
		case 6:
			v = f2u(z)
			px[0], px[1], px[2], px[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
		case 7:
			v = f2u(z)
			px[0], px[1], px[2], px[3], px[4] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24), byte(stencil)
		default:
			v = uint32(math.Floor(float64(clamp01(z))*0xffffff+0.5)) | stencil<<24
			px[0], px[1], px[2], px[3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
		}
		g.fillSlice(&db.lay, db.slice, px)
		return
	}
	addrs := make([]uint32, db.lay.Pitch)
	for y := uint32(0); y < db.lay.Height; y++ {
		db.lay.rowAddrs(0, y, db.slice, addrs)
		for _, a := range addrs {
			g.writeDepth(&db, a, z, uint8(stencil), wz, ws)
		}
	}
}

// readColor decodes one element of a colour buffer into floats (0..1 for
// the normalized formats).
func (g *Latte) readElem(lay *surfLayout, format uint32, k numKind, a uint32, buf []byte) [4]uint32 {
	g.mem.ReadBytes(a, buf)
	return decodeElem(format, k, buf)
}

// copyToScan copies a colour buffer (w x h of it) into a scan buffer's back
// buffer (target 1 = TV, 4 = GamePad), scaling it to fit.
func (g *Latte) copyToScan(cb cbDesc, w, h, target uint32) {
	m := g.m
	i := 0
	if target&4 != 0 {
		i = 1
	}
	sb := &m.gx2.Scan[i]
	if sb.Addr == 0 || !cb.ok || w == 0 || h == 0 {
		return
	}
	// the picture straight from the host GPU, when it drew it
	var host []uint32
	var hostW, hostH uint32
	if g.hw != nil {
		var ok bool
		if host, hostW, hostH, ok = g.hw.scanPixels(&cb); !ok {
			host = nil
			g.hw.flush(surfRange(&cb.lay, cb.slice))
		}
	}
	// a size past the buffer's own is a mistake: keep to the buffer
	w, h = min(w, cb.lay.Pitch), min(h, cb.lay.Height)
	if w == 0 || h == 0 {
		return
	}
	frame := sb.W * sb.H * 4
	dst := sb.Addr + sb.Back*frame
	row := make([]byte, sb.W*4)
	srcRow := make([]uint32, w)
	buf := make([]byte, 16)
	lastSY := ^uint32(0)
	xs := make([]uint32, sb.W) // the source column of each scan column
	for x := range xs {
		xs[x] = uint32(x) * w / sb.W
	}
	// only the source columns the scan buffer shows are read (a big buffer
	// shrunk to fit costs no more than a small one)
	cols := make([]uint32, 0, sb.W)
	for x, sx := range xs {
		if x == 0 || sx != xs[x-1] {
			cols = append(cols, sx)
		}
	}
	f := cb.format & 0x3f
	for y := uint32(0); y < sb.H; y++ {
		sy := y * h / sb.H
		if sy != lastSY && host != nil {
			lastSY = sy
			for _, x := range cols {
				if x < hostW && sy < hostH {
					srcRow[x] = host[sy*hostW+x]
				} else {
					srcRow[x] = 0
				}
			}
		}
		if sy != lastSY {
			lastSY = sy
			for _, x := range cols {
				a := cb.lay.addr(x, sy, cb.slice)
				if f == fmt8_8_8_8 && cb.kind != kSnorm && cb.kind != kUint && cb.kind != kSint {
					srcRow[x] = m.Mem.Read32(a) // bytes R, G, B, A
					continue
				}
				v := g.readElem(&cb.lay, cb.format, cb.kind, a, buf[:fmtBytes(cb.format)])
				if cb.kind == kSrgb {
					for c := 0; c < 3; c++ {
						v[c] = f2u(linearToSRGB(u2f(v[c])))
					}
				}
				q := func(c int) uint32 { return uint32(clamp01(u2f(v[c]))*255 + 0.5) }
				srcRow[x] = q(0)<<24 | q(1)<<16 | q(2)<<8 | q(3)
			}
		}
		out := m.Mem.Slice(dst+y*sb.W*4, len(row))
		if out == nil {
			out = row
		}
		for x, sx := range xs {
			binary.BigEndian.PutUint32(out[x*4:], srcRow[sx])
		}
		if &out[0] == &row[0] {
			m.Mem.WriteBytes(dst+y*sb.W*4, row)
		}
	}
}

// ---- surface copies ----

// packedSurf is one mip level and slice of a surface, as GX2 passes it to
// the copy packets.
type packedSurf struct {
	lay    surfLayout
	format uint32 // GX2 format (with the number-kind flags)
	width  uint32 // in elements
	height uint32
	slice  uint32
}

const packedSurfWords = 11

func (p *packedSurf) pack() []uint32 {
	l := &p.lay
	return []uint32{l.Base, l.Pitch, l.Height, l.Slices, l.Mode, l.Samples, b2u(l.Depth) | l.Swizzle, p.format, p.width, p.height, p.slice}
}

func unpackSurf(d []uint32) packedSurf {
	p := packedSurf{format: d[7], width: d[8], height: d[9], slice: d[10]}
	p.lay = surfLayout{Base: d[0], Pitch: d[1], Height: d[2], Slices: d[3], Mode: d[4], Samples: d[5],
		Depth: d[6]&1 != 0, Swizzle: d[6] &^ 1, BPP: formatBits(d[7])}
	return p
}

// gx2Kind: the number kind of a GX2 surface format
func gx2Kind(f uint32) numKind {
	switch f >> 8 & 0xf {
	case 1:
		return kSnorm
	case 2:
		return kUint
	case 3:
		return kSint
	case 4:
		return kSrgb
	case 8:
		return kFloat
	}
	return kUnorm
}

func (g *Latte) copySurface(src, dst packedSurf) {
	if src.lay.Base == 0 || dst.lay.Base == 0 {
		return
	}
	g.hwFlush(surfRange(&src.lay, src.slice))
	g.hwFlush(surfRange(&dst.lay, dst.slice)) // the parts not copied stay
	defer g.hwInvalidate(surfRange(&dst.lay, dst.slice))
	w := min(src.width, dst.width)
	h := min(src.height, dst.height)
	sb := src.lay.BPP / 8
	db := dst.lay.BPP / 8
	same := src.format&0x3f == dst.format&0x3f && sb == db
	buf := make([]byte, max(sb, db, 16))
	sa := make([]uint32, w)
	da := make([]uint32, w)
	for y := uint32(0); y < h; y++ {
		src.lay.rowAddrs(0, y, src.slice, sa)
		dst.lay.rowAddrs(0, y, dst.slice, da)
		for x := uint32(0); x < w; x++ {
			if same || src.lay.Depth {
				g.mem.ReadBytes(sa[x], buf[:sb])
				if !same {
					// depth to colour: a 24-bit depth becomes a 32-bit float
					v := decodeElem(src.format, kUnorm, buf[:sb])
					encodeElem(dst.format, gx2Kind(dst.format), v, buf[:db])
				}
				g.mem.WriteBytes(da[x], buf[:db])
				continue
			}
			g.mem.ReadBytes(sa[x], buf[:sb])
			v := decodeElem(src.format, gx2Kind(src.format), buf[:sb])
			encodeElem(dst.format, gx2Kind(dst.format), v, buf[:db])
			g.mem.WriteBytes(da[x], buf[:db])
		}
	}
	g.tex.invalidate(dst.lay.Base, dst.lay.Base+dst.lay.sliceBytes()*(dst.slice+1))
}
