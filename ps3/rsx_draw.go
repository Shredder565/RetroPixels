package ps3

// Drawing: a software renderer for the RSX's 3D engine (NV4097) and its 2D
// blitters. Draws run the vertex program per vertex, clip, rasterise with
// perspective-correct interpolation, run the fragment program per pixel and
// write the colour/depth surfaces in video memory, as the RSX would.

import (
	"math"
	"os"
	"runtime"
	"strconv"
	"sync"
)

type drawRange struct{ first, count uint32 }

type rsxDraw struct {
	vp      [544 * 4]uint32
	vpDec   [544 * 4]uint32 // vp as vpProg was decoded from
	vpLoad  uint32
	vpDirty bool
	vpProg  []vpInst
	vpBase  int
	vc      [512]vec4
	vcLoad  uint32

	prim    uint32
	indexed bool
	ranges  []drawRange
	inline  []uint32
	imm     [16]vec4

	branchBits uint32
	// the vertex program's trace for the GPU (see vpTrace), for vpGen,
	// vpBase and branchBits
	vpGen          int
	trGen, trStart int
	trBits         uint32
	trPCs          []int
	trHash         uint64
	trOK, trDone   bool

	fpCache map[uint32]*fpProg

	blit blitState
}

type blitState struct {
	// NV3062 (2D surface)
	dstFormat, pitchIn, pitchOut, srcOff, dstOff uint32
	dstDMA                                       uint32
	// NV3089 (scaled image)
	conv, format, op  uint32
	clipX, clipY      int
	clipW, clipH      int
	outX, outY        int
	outW, outH        int
	dsdx, dtdy        int32
	inW, inH          uint32
	inPitch, inOrigin uint32
	inOff             uint32
	inX, inY          uint32
	srcDMA            uint32
	// NV0039 (memory to memory)
	mmSrcDMA, mmDstDMA                             uint32
	mmIn, mmOut, mmPitchIn, mmPitchOut, mmLen, mmN uint32
	mmFormat                                       uint32
}

var rsxStat map[uint32]int

var rsxTrace = os.Getenv("PS3_RSXTRACE") != ""

func (r *rsxState) d() *rsxDraw {
	if r.draw == nil {
		r.draw = &rsxDraw{fpCache: map[uint32]*fpProg{}}
		for i := range r.draw.imm {
			r.draw.imm[i] = vec4{0, 0, 0, 1}
		}
	}
	return r.draw
}

func (r *rsxState) reg(a uint32) uint32 { return r.regs[a/4] }
func (r *rsxState) regF(a uint32) float32 {
	return math.Float32frombits(r.regs[a/4])
}

func (m *Machine) rsxDraw(r *rsxState, reg, v uint32) {
	if rsxStat != nil {
		rsxStat[reg]++
	}
	d := r.d()
	sub := reg >> 13
	if sub != 0 {
		m.rsxBlitMethod(r, d, sub, reg&0x1fff, v)
		return
	}
	switch {
	case reg >= 0xB80 && reg < 0xC00: // transform program words
		if d.vpLoad < uint32(len(d.vp)) {
			d.vp[d.vpLoad] = v
			d.vpLoad++
			d.vpDirty = true
		}
		return
	case reg >= 0x1F00 && reg < 0x1F80: // transform constants
		if d.vcLoad < 512*4 {
			d.vc[d.vcLoad/4][d.vcLoad%4] = math.Float32frombits(v)
			d.vcLoad++
		}
		return
	case reg >= 0x1C00 && reg < 0x1D00: // SET_VERTEX_DATA4F_M
		i := (reg - 0x1C00) / 16
		d.imm[i][(reg/4)&3] = math.Float32frombits(v)
		return
	case reg >= 0x1880 && reg < 0x1900: // 2F
		i := (reg - 0x1880) / 8
		d.imm[i][(reg/4)&1] = math.Float32frombits(v)
		if (reg/4)&1 == 1 {
			d.imm[i][2], d.imm[i][3] = 0, 1
		}
		return
	case reg >= 0x1E40 && reg < 0x1E80: // 1F
		i := (reg - 0x1E40) / 4
		d.imm[i] = vec4{math.Float32frombits(v), 0, 0, 1}
		return
	case reg >= 0x1500 && reg < 0x1600: // 3F
		i := (reg - 0x1500) / 16
		c := (reg / 4) & 3
		if c < 3 {
			d.imm[i][c] = math.Float32frombits(v)
			d.imm[i][3] = 1
		}
		return
	case reg >= 0x1940 && reg < 0x1980: // 4UB
		i := (reg - 0x1940) / 4
		d.imm[i] = vec4{float32(v>>24) / 255, float32(v>>16&0xff) / 255, float32(v>>8&0xff) / 255, float32(v&0xff) / 255}
		return
	case reg >= 0x1980 && reg < 0x1A00: // 4S
		i := (reg - 0x1980) / 8
		if (reg/4)&1 == 0 {
			d.imm[i][0], d.imm[i][1] = float32(int16(v)), float32(int16(v>>16))
		} else {
			d.imm[i][2], d.imm[i][3] = float32(int16(v)), float32(int16(v>>16))
		}
		return
	case reg >= 0x1900 && reg < 0x1940: // 2S
		i := (reg - 0x1900) / 4
		d.imm[i] = vec4{float32(int16(v)), float32(int16(v >> 16)), 0, 1}
		return
	}
	switch reg {
	case 0x1E9C: // TRANSFORM_PROGRAM_LOAD
		d.vpLoad = v * 4
	case 0x1EA0: // TRANSFORM_PROGRAM_START
		d.vpDirty = true
	case 0x1EFC: // TRANSFORM_CONSTANT_LOAD
		d.vcLoad = v * 4
	case 0x1FF8:
		d.branchBits = v
	case 0x1808: // BEGIN_END
		if v != 0 {
			d.prim = v
			d.ranges = d.ranges[:0]
			d.inline = d.inline[:0]
			d.indexed = false
		} else {
			m.rsxFlushDraw(r, d)
		}
	case 0x1814: // DRAW_ARRAYS
		d.ranges = append(d.ranges, drawRange{v & 0xffffff, v>>24 + 1})
		d.indexed = false
	case 0x1824: // DRAW_INDEX_ARRAY
		d.ranges = append(d.ranges, drawRange{v & 0xffffff, v>>24 + 1})
		d.indexed = true
	case 0x1818: // INLINE_ARRAY
		d.inline = append(d.inline, v)
	case 0x1D94: // CLEAR_SURFACE
		m.rsxClear(r, v)
	}
}

// ---- surfaces

type rsxSurface struct {
	addr          uint32
	pitch         uint32
	fmt           uint32
	zAddr, zPitch uint32
	zFmt          uint32
	x, y, w, h    int // clip
	ok            bool
	// more colour targets drawn at once (B, C, D after A): the fragment
	// program's R2/H4, R3/H6, R4/H8 go to them
	mrt  [3]struct{ addr, pitch uint32 }
	nmrt int
}

func (m *Machine) dmaAddr(ctx, off uint32) uint32 {
	switch ctx {
	case 0xFEED0001, 0x66616661:
		return m.gcm.ioToAddr(off)
	}
	return RSXBase + off
}

func (m *Machine) surface(r *rsxState) rsxSurface {
	s := rsxSurface{}
	f := r.reg(0x208)
	s.fmt = f & 0x1f
	s.zFmt = f >> 5 & 7
	target := r.reg(0x220)
	s.pitch = r.reg(0x20C)
	off := r.reg(0x210)
	ctx := r.reg(0x194)
	if target == 2 { // B only
		s.pitch, off, ctx = r.reg(0x21C), r.reg(0x218), r.reg(0x18C)
	}
	if target != 0 {
		s.addr = m.dmaAddr(ctx, off)
	}
	if target&0x10 != 0 && s.addr != 0 { // A and more
		n := 1
		switch target & 0xf {
		case 7:
			n = 2
		case 0xf:
			n = 3
		}
		regs := [3][3]uint32{{0x18C, 0x218, 0x21C}, {0x1B4, 0x288, 0x280}, {0x1B8, 0x28C, 0x284}}
		for k := 0; k < n; k++ {
			a := m.dmaAddr(r.reg(regs[k][0]), r.reg(regs[k][1]))
			p := r.reg(regs[k][2])
			if p < 16 {
				break
			}
			s.mrt[k].addr, s.mrt[k].pitch = a, p
			s.nmrt = k + 1
		}
	}
	s.zAddr = m.dmaAddr(r.reg(0x198), r.reg(0x214))
	s.zPitch = r.reg(0x22C)
	h, v := r.reg(0x200), r.reg(0x204)
	s.x, s.w = int(h&0xffff), int(h>>16)
	s.y, s.h = int(v&0xffff), int(v>>16)
	s.ok = s.addr != 0 && s.w > 0 && s.h > 0 && s.pitch >= 16 && s.w <= 4096 && s.h <= 4096
	return s
}

func surfBpp(f uint32) uint32 {
	switch f {
	case 1, 2, 3:
		return 2
	case 9:
		return 1
	case 10:
		return 2
	case 11:
		return 8
	case 12:
		return 16
	}
	return 4
}

// readPixel / writePixel convert a surface pixel to and from RGBA.
func (m *Machine) readPixel(s *rsxSurface, a uint32) vec4 {
	switch s.fmt {
	case 3:
		c := m.Mem.Read16(a)
		return vec4{float32(c>>11) / 31, float32(c>>5&63) / 63, float32(c&31) / 31, 1}
	case 1, 2:
		c := m.Mem.Read16(a)
		return vec4{float32(c>>10&31) / 31, float32(c>>5&31) / 31, float32(c&31) / 31, 1}
	case 9:
		v := float32(m.Mem.Read8(a)) / 255
		return vec4{v, v, v, v}
	case 10:
		c := m.Mem.Read16(a)
		return vec4{float32(c>>8) / 255, float32(c&0xff) / 255, 0, 1}
	case 13:
		v := math.Float32frombits(m.Mem.Read32(a))
		return vec4{v, v, v, v}
	case 11:
		return vec4{halfToF32(m.Mem.Read16(a)), halfToF32(m.Mem.Read16(a + 2)), halfToF32(m.Mem.Read16(a + 4)), halfToF32(m.Mem.Read16(a + 6))}
	case 12:
		return vec4{m.Mem.ReadF32(a), m.Mem.ReadF32(a + 4), m.Mem.ReadF32(a + 8), m.Mem.ReadF32(a + 12)}
	case 14, 15, 16: // B8G8R8 orders (A8B8G8R8)
		c := m.Mem.Read32(a)
		al := float32(c>>24) / 255
		if s.fmt != 16 {
			al = 1
		}
		return vec4{float32(c&0xff) / 255, float32(c>>8&0xff) / 255, float32(c>>16&0xff) / 255, al}
	}
	c := m.Mem.Read32(a)
	al := float32(c>>24) / 255
	if s.fmt != 8 {
		al = 1
	}
	return vec4{float32(c>>16&0xff) / 255, float32(c>>8&0xff) / 255, float32(c&0xff) / 255, al}
}

// rgbaMask is each of R, G, B, A's bit in a colour mask (A 8, R 4, G 2, B 1).
var rgbaMask = [4]uint32{4, 2, 1, 8}

func u8c(x float32) uint32 {
	if !(x > 0) {
		return 0
	}
	if x >= 1 {
		return 255
	}
	return uint32(x*255 + 0.5)
}

func (m *Machine) writePixel(s *rsxSurface, a uint32, c vec4, mask uint32) {
	switch s.fmt {
	case 3:
		v := uint16(u8c(c[0])>>3)<<11 | uint16(u8c(c[1])>>2)<<5 | uint16(u8c(c[2])>>3)
		m.Mem.Write16(a, v)
		return
	case 1, 2:
		v := uint16(u8c(c[0])>>3)<<10 | uint16(u8c(c[1])>>3)<<5 | uint16(u8c(c[2])>>3)
		m.Mem.Write16(a, v)
		return
	case 9:
		m.Mem.Write8(a, uint8(u8c(c[0])))
		return
	case 10:
		m.Mem.Write16(a, uint16(u8c(c[0])<<8|u8c(c[1])))
		return
	case 13:
		m.Mem.Write32(a, math.Float32bits(c[0]))
		return
	case 11:
		for i := 0; i < 4; i++ {
			if mask&rgbaMask[i] != 0 {
				m.Mem.Write16(a+uint32(2*i), f32ToHalf(c[i]))
			}
		}
		return
	case 12:
		for i := 0; i < 4; i++ {
			if mask&rgbaMask[i] != 0 {
				m.Mem.Write32(a+uint32(4*i), math.Float32bits(c[i]))
			}
		}
		return
	}
	var v uint32
	if s.fmt == 14 || s.fmt == 15 || s.fmt == 16 {
		v = u8c(c[3])<<24 | u8c(c[2])<<16 | u8c(c[1])<<8 | u8c(c[0])
	} else {
		v = u8c(c[3])<<24 | u8c(c[0])<<16 | u8c(c[1])<<8 | u8c(c[2])
	}
	if mask != 0xf {
		old := m.Mem.Read32(a)
		var keep uint32
		// mask bits: A 8, R 4, G 2, B 1 (in ARGB byte order)
		if mask&8 == 0 {
			keep |= 0xff000000
		}
		if mask&4 == 0 {
			keep |= 0x00ff0000
		}
		if mask&2 == 0 {
			keep |= 0x0000ff00
		}
		if mask&1 == 0 {
			keep |= 0x000000ff
		}
		if s.fmt == 14 || s.fmt == 15 || s.fmt == 16 {
			keep = keep&0xff00ff00 | (keep&0xff)<<16 | keep>>16&0xff
		}
		v = v&^keep | old&keep
	}
	m.Mem.Write32(a, v)
}

// colorMask is SET_COLOR_MASK as A R G B bits (8 4 2 1).
func colorMask(v uint32) uint32 {
	var r uint32
	if v>>24&0xff != 0 {
		r |= 8
	}
	if v>>16&0xff != 0 {
		r |= 4
	}
	if v>>8&0xff != 0 {
		r |= 2
	}
	if v&0xff != 0 {
		r |= 1
	}
	return r
}

func (m *Machine) rsxClear(r *rsxState, v uint32) {
	if m.gpu != nil {
		m.gpu.clear(r, v)
		return
	}
	if m.rsxSkip() {
		return
	}
	s := m.surface(r)
	x0, y0, x1, y1 := s.x, s.y, s.x+s.w, s.y+s.h
	sh, sv := r.reg(0x8C0), r.reg(0x8C4)
	if sw, shh := int(sh>>16), int(sv>>16); sw > 0 && shh > 0 {
		x0, y0 = max(x0, int(sh&0xffff)), max(y0, int(sv&0xffff))
		x1, y1 = min(x1, int(sh&0xffff)+sw), min(y1, int(sv&0xffff)+shh)
	}
	if rsxTrace {
		m.Logf("RSX clear %x surface %08X fmt %d pitch %d %dx%d z %08X", v, s.addr, s.fmt, s.pitch, s.w, s.h, s.zAddr)
	}
	if v&0xf0 != 0 && s.ok {
		c := r.reg(0x1D90)
		col := vec4{float32(c>>16&0xff) / 255, float32(c>>8&0xff) / 255, float32(c&0xff) / 255, float32(c>>24) / 255}
		mask := uint32(0)
		if v&0x80 != 0 {
			mask |= 8
		}
		if v&0x10 != 0 {
			mask |= 4
		}
		if v&0x20 != 0 {
			mask |= 2
		}
		if v&0x40 != 0 {
			mask |= 1
		}
		bpp := surfBpp(s.fmt)
		// every colour target being drawn is cleared
		addrs := []uint32{s.addr}
		pitches := []uint32{s.pitch}
		for k := 0; k < s.nmrt; k++ {
			addrs = append(addrs, s.mrt[k].addr)
			pitches = append(pitches, s.mrt[k].pitch)
		}
		for t, base := range addrs {
			pitch := pitches[t]
			if bpp == 4 && mask == 0xf && (s.fmt == 8 || s.fmt == 4 || s.fmt == 5) {
				row := make([]byte, (x1-x0)*4)
				for i := 0; i < len(row); i += 4 {
					row[i], row[i+1], row[i+2], row[i+3] = byte(c>>24), byte(c>>16), byte(c>>8), byte(c)
				}
				for y := y0; y < y1; y++ {
					m.Mem.WriteBytes(base+uint32(y)*pitch+uint32(x0)*4, row)
				}
			} else {
				for y := y0; y < y1; y++ {
					for x := x0; x < x1; x++ {
						m.writePixel(&s, base+uint32(y)*pitch+uint32(x)*bpp, col, mask)
					}
				}
			}
		}
	}
	if v&3 != 0 && s.zAddr != 0 && s.zPitch >= 16 {
		zv := r.reg(0x1D8C)
		if s.zFmt == 1 { // Z16
			for y := y0; y < y1; y++ {
				for x := x0; x < x1; x++ {
					m.Mem.Write16(s.zAddr+uint32(y)*s.zPitch+uint32(x)*2, uint16(zv))
				}
			}
		} else {
			for y := y0; y < y1; y++ {
				for x := x0; x < x1; x++ {
					a := s.zAddr + uint32(y)*s.zPitch + uint32(x)*4
					old := m.Mem.Read32(a)
					nv := old
					if v&1 != 0 {
						nv = nv&0xff | zv&0xffffff00
					}
					if v&2 != 0 {
						nv = nv&0xffffff00 | zv&0xff
					}
					m.Mem.Write32(a, nv)
				}
			}
		}
	}
}

// ---- vertices

type rsxVertex struct {
	out [16]vec4
}

// fetchAttr reads vertex attribute i for vertex index idx.
func (m *Machine) fetchAttr(r *rsxState, d *rsxDraw, i int, idx uint32) vec4 {
	f := m.attrSource(r, d, i)
	return m.fetch(&f, idx)
}

// attrSrc is where one vertex attribute comes from in a draw.
type attrSrc struct {
	size, typ, stride, off, loc uint32
	imm                         vec4
}

func (m *Machine) attrSource(r *rsxState, d *rsxDraw, i int) attrSrc {
	f := r.reg(0x1740 + 4*uint32(i))
	offReg := r.reg(0x1680 + 4*uint32(i))
	// (a per-instance divider, f>>16, isn't used by draws here: every
	// attribute is per vertex)
	return attrSrc{
		size:   f >> 4 & 0xf,
		typ:    f & 0xf,
		stride: f >> 8 & 0xff,
		off:    offReg&0x7fffffff + r.reg(0x1738),
		loc:    offReg >> 31,
		imm:    d.imm[i],
	}
}

// attrBytes is how many bytes an attribute of this type and size takes.
func attrBytes(typ, size uint32) uint32 {
	n := min(size, 4)
	switch typ {
	case 2:
		return 4 * n
	case 1, 3, 5:
		return 2 * n
	case 4, 7:
		return n
	case 6:
		return 4
	}
	return 0
}

func (m *Machine) fetch(f *attrSrc, idx uint32) vec4 {
	if f.size == 0 {
		return f.imm
	}
	a := m.gcm.offsetToAddr(f.off+f.stride*idx, f.loc)
	n := attrBytes(f.typ, f.size)
	p := m.Mem.Ptr(a)
	if uint32(len(p)) < n {
		// across a page (or not mapped): byte by byte
		var b [16]byte
		for k := uint32(0); k < n; k++ {
			b[k] = m.Mem.Read8(a + k)
		}
		p = b[:]
	}
	return decodeAttr(p, f.typ, f.size)
}

func decodeAttr(p []byte, typ, size uint32) vec4 {
	v := vec4{0, 0, 0, 1}
	n := int(min(size, 4))
	switch typ {
	case 2: // float
		for c := 0; c < n; c++ {
			v[c] = math.Float32frombits(be32(p[4*c:]))
		}
	case 3: // half
		for c := 0; c < n; c++ {
			v[c] = halfToF32(be16(p[2*c:]))
		}
	case 1: // signed normalized 16
		for c := 0; c < n; c++ {
			x := float32(int16(be16(p[2*c:]))) / 32767
			v[c] = max(-1, x)
		}
	case 5: // signed 16
		for c := 0; c < n; c++ {
			v[c] = float32(int16(be16(p[2*c:])))
		}
	case 4: // unsigned byte normalized
		for c := 0; c < n; c++ {
			v[c] = float32(p[c]) / 255
		}
	case 7: // unsigned byte
		for c := 0; c < n; c++ {
			v[c] = float32(p[c])
		}
	case 6: // CMP: 11/11/10 signed normalized
		w := be32(p)
		v[0] = float32(int32(w<<21)>>21) / 1023
		v[1] = float32(int32(w<<10)>>21) / 1023
		v[2] = float32(int32(w)>>22) / 511
	}
	return v
}

func (m *Machine) rsxVP(d *rsxDraw, r *rsxState) {
	start := int(r.reg(0x1EA0))
	if !d.vpDirty && d.vpProg != nil && d.vpBase == start {
		return
	}
	// decode only the instructions that changed since last time
	full := len(d.vpProg) != 544
	if full {
		d.vpProg = make([]vpInst, 544)
	}
	changed := full
	for i := 0; i < 544; i++ {
		var w [4]uint32
		copy(w[:], d.vp[i*4:i*4+4])
		if !full && w == [4]uint32(d.vpDec[i*4:i*4+4]) {
			continue
		}
		copy(d.vpDec[i*4:i*4+4], w[:])
		d.vpProg[i] = decodeVP(w)
		changed = true
	}
	d.vpBase = start
	d.vpDirty = false
	if changed {
		d.vpGen++
	}
}

// rsxRenderEvery draws only every n-th frame (for quick test runs).
var rsxRenderEvery, _ = strconv.Atoi(os.Getenv("PS3_RENDEREVERY"))

// rsxRenderFrom (PS3_RENDERFROM=n) draws nothing before frame n (tests).
var rsxRenderFrom, _ = strconv.Atoi(os.Getenv("PS3_RENDERFROM"))

func (m *Machine) rsxSkip() bool {
	if m.frame < uint64(rsxRenderFrom) {
		return true
	}
	return rsxRenderEvery > 1 && (m.frame+1)%uint64(rsxRenderEvery) != 0 && (m.frame+2)%uint64(rsxRenderEvery) != 0
}

func (m *Machine) rsxFlushDraw(r *rsxState, d *rsxDraw) {
	if len(d.ranges) == 0 && len(d.inline) == 0 {
		return
	}
	if m.rsxSkip() {
		return
	}
	if m.gpu != nil {
		if m.gpu.draw(r, d) {
			return
		}
		// the software renderer draws it: give it memory as the GPU has it
		m.gpu.Fallbacks++
		m.gpu.syncRange(0, 0xffffffff, nil)
		defer func() {
			s := m.surface(r)
			m.gpu.wroteRange(s.addr, s.addr+s.pitch*uint32(s.y+s.h))
			for k := 0; k < s.nmrt; k++ {
				m.gpu.wroteRange(s.mrt[k].addr, s.mrt[k].addr+s.mrt[k].pitch*uint32(s.y+s.h))
			}
			if s.zAddr != 0 {
				m.gpu.wroteRange(s.zAddr, s.zAddr+s.zPitch*uint32(s.y+s.h))
			}
		}()
	}
	s := m.surface(r)
	if !s.ok {
		return
	}
	verts, vlist := m.rsxVertices(r, d)
	if rsxTrace {
		m.Logf("RSX draw prim %d, %d vertices, surface %08X %dx%d fmt %d, fp %08X", d.prim, len(vlist), s.addr, s.w, s.h, s.fmt, r.reg(0x8E4))
		for i, v := range verts {
			if i >= 4 {
				break
			}
			m.Logf("  v%d pos %v col %v tex0 %v", i, v.out[0], v.out[1], v.out[7])
		}
	}
	m.rasterPrims(r, d, &s, verts, vlist)
	if drawHook != nil {
		drawHook(m, r, &s)
	}
}

// rsxVertices runs the vertex program on the draw's vertices: the
// vertices, and the list of them in draw order (-1 = primitive restart).
func (m *Machine) rsxVertices(r *rsxState, d *rsxDraw) ([]rsxVertex, []int) {
	m.rsxVP(d, r)
	inMask := r.reg(0x1FF0)
	idx := m.rsxIndices(r, d)
	// run the vertex program
	var verts []rsxVertex
	var x vpRun
	cache := map[uint32]int{}
	var vlist []int
	if len(d.inline) > 0 {
		verts, vlist = m.inlineVertices(r, d, inMask, &x)
	} else {
		var uniq []uint32
		uniq, vlist = dedupe(idx, cache)
		verts = make([]rsxVertex, len(uniq))
		if vpStat {
			vpStatVerts += len(uniq)
			vpStatInsts += len(uniq) * len(d.vpProg[min(d.vpStart(), len(d.vpProg)):])
			vpStatDraws++
		}
		run := func(from, to int, x *vpRun) {
			for n := from; n < to; n++ {
				var attrs [16]vec4
				for a := 0; a < 16; a++ {
					if inMask>>a&1 != 0 {
						attrs[a] = m.fetchAttr(r, d, a, uniq[n])
					}
				}
				d.runVP(d.vpProg, d.vpStart(), &attrs, x)
				verts[n].out = x.out
			}
		}
		// the vertices are independent: big draws use every core
		workers := min(vpWorkers, len(uniq)/vpChunk)
		if workers <= 1 {
			run(0, len(uniq), &x)
		} else {
			// memory pages are made on first touch: make them here, not
			// in several workers at once
			lo, hi := uniq[0], uniq[0]
			for _, i := range uniq {
				lo, hi = min(lo, i), max(hi, i)
			}
			for a := 0; a < 16; a++ {
				f := r.reg(0x1740 + 4*uint32(a))
				if inMask>>a&1 == 0 || f>>4&0xf == 0 {
					continue
				}
				stride := f >> 8 & 0xff
				offReg := r.reg(0x1680 + 4*uint32(a))
				off := offReg&0x7fffffff + r.reg(0x1738)
				first := m.gcm.offsetToAddr(off+stride*lo, offReg>>31)
				last := m.gcm.offsetToAddr(off+stride*hi+16, offReg>>31)
				for p := first &^ pageMask; p <= last && p >= first&^pageMask; p += pageSize {
					m.Mem.page(p)
				}
			}
			var wg sync.WaitGroup
			per := (len(uniq) + workers - 1) / workers
			for w := 0; w < workers; w++ {
				from, to := w*per, min(len(uniq), (w+1)*per)
				wg.Add(1)
				go func() {
					defer wg.Done()
					var x vpRun
					run(from, to, &x)
				}()
			}
			wg.Wait()
		}
	}
	return verts, vlist
}

// vpWorkers run a big draw's vertex program at once, vpChunk vertices or
// more each.
var vpWorkers = max(1, min(runtime.NumCPU(), 8))

const vpChunk = 128

var vpStat = os.Getenv("PS3_VPSTAT") != ""
var vpStatVerts, vpStatInsts, vpStatDraws int

// rsxIndices lists the draw's vertex indices (0xffffffff: restart).
func (m *Machine) rsxIndices(r *rsxState, d *rsxDraw) []uint32 {
	var idx []uint32
	if d.indexed {
		ia := r.reg(0x181C)
		dma := r.reg(0x1820)
		loc := dma & 0xf
		is16 := dma>>4&0xf == 1
		base := m.gcm.offsetToAddr(ia, loc)
		bi := r.reg(0x173C)
		restart := r.reg(0x1DAC) != 0
		ri := r.reg(0x1DB0)
		for _, rg := range d.ranges {
			for k := uint32(0); k < rg.count; k++ {
				var x uint32
				if is16 {
					x = uint32(m.Mem.Read16(base + 2*(rg.first+k)))
				} else {
					x = m.Mem.Read32(base + 4*(rg.first+k))
				}
				if restart && x == ri {
					idx = append(idx, 0xffffffff)
					continue
				}
				idx = append(idx, x+bi)
			}
		}
	} else if len(d.inline) == 0 {
		for _, rg := range d.ranges {
			for k := uint32(0); k < rg.count; k++ {
				idx = append(idx, rg.first+k)
			}
		}
	}
	return idx
}

// dedupe gives the distinct indices and, for each index, its place among
// them (-1 for a restart).
func dedupe(idx []uint32, cache map[uint32]int) (uniq []uint32, vlist []int) {
	for _, i := range idx {
		if i == 0xffffffff {
			vlist = append(vlist, -1)
			continue
		}
		k, ok := cache[i]
		if !ok {
			k = len(uniq)
			cache[i] = k
			uniq = append(uniq, i)
		}
		vlist = append(vlist, k)
	}
	return
}

// drawHook, when set (debugging), is called after each draw.
var drawHook func(m *Machine, r *rsxState, s *rsxSurface)

func (d *rsxDraw) vpStart() int { return d.vpBase }

// vpTraced traces the current vertex program (once for each program,
// start and branch bits).
func (d *rsxDraw) vpTraced() ([]int, uint64, bool) {
	if !d.trDone || d.trGen != d.vpGen || d.trStart != d.vpBase || d.trBits != d.branchBits {
		d.trDone, d.trGen, d.trStart, d.trBits = true, d.vpGen, d.vpBase, d.branchBits
		d.trPCs, d.trOK = vpTrace(d.vpProg, d.vpBase, d.branchBits)
		if d.trOK {
			d.trHash = vpTraceHash(d.vp[:], d.trPCs)
		}
	}
	return d.trPCs, d.trHash, d.trOK
}

// inlineVertices decodes INLINE_ARRAY data with the array formats.
func (m *Machine) inlineVertices(r *rsxState, d *rsxDraw, inMask uint32, x *vpRun) ([]rsxVertex, []int) {
	// stride: sum of the enabled attributes' sizes (in words)
	var verts []rsxVertex
	var vlist []int
	data := d.inline
	pos := 0
	for pos < len(data) {
		var attrs [16]vec4
		for a := 0; a < 16; a++ {
			attrs[a] = d.imm[a]
			f := r.reg(0x1740 + 4*uint32(a))
			size := int(f >> 4 & 0xf)
			if size == 0 {
				continue
			}
			v := vec4{0, 0, 0, 1}
			switch f & 0xf {
			case 2:
				for c := 0; c < size && pos < len(data); c++ {
					v[c] = math.Float32frombits(data[pos])
					pos++
				}
			case 4, 7:
				if pos < len(data) {
					w := data[pos]
					pos++
					for c := 0; c < size; c++ {
						b := float32(w >> (8 * c) & 0xff)
						if f&0xf == 4 {
							b /= 255
						}
						v[c] = b
					}
				}
			default:
				pos += (size + 1) / 2
			}
			attrs[a] = v
		}
		d.runVP(d.vpProg, d.vpStart(), &attrs, x)
		verts = append(verts, rsxVertex{out: x.out})
		vlist = append(vlist, len(verts)-1)
	}
	return verts, vlist
}

// ---- display

// Screen: the last flipped picture as 0x00RRGGBB pixels.
type Screen struct {
	W, H int
	Pix  []uint32
}

func (m *Machine) present(id uint32) {
	g := m.gcm
	var off, pitch, w, h uint32
	if id&0x100 != 0 {
		s := g.rescSrcs[id&7]
		off, pitch, w, h = s.offset, s.pitch, s.width, s.height
	} else {
		d := g.display[id&7]
		off, pitch, w, h = d.offset, d.pitch, d.width, d.height
	}
	if w == 0 || h == 0 || w > 4096 || h > 4096 {
		return
	}
	sc := m.screen
	if sc == nil || sc.W != int(w) || sc.H != int(h) {
		sc = &Screen{W: int(w), H: int(h), Pix: make([]uint32, w*h)}
		m.screen = sc
	}
	if m.gpu != nil {
		m.gpu.endFrame()
		if m.gpu.screen(RSXBase+off, pitch, int(w), int(h), sc) {
			return
		}
	}
	row := make([]byte, w*4)
	for y := uint32(0); y < h; y++ {
		m.Mem.ReadBytes(RSXBase+off+y*pitch, row)
		for x := uint32(0); x < w; x++ {
			// A8R8G8B8 in memory order
			sc.Pix[y*w+x] = uint32(row[4*x+1])<<16 | uint32(row[4*x+2])<<8 | uint32(row[4*x+3])
		}
	}
}

// Picture returns the last frame shown (nil before the first flip).
func (m *Machine) Picture() *Screen { return m.screen }

// dedupeBuf is dedupe's working memory, kept between draws.
type dedupeBuf struct {
	uniq  []uint32
	vlist []int
	slot  []int32  // index-lo -> place in uniq, valid where stamp = gen
	stamp []uint32 //
	gen   uint32
	cache map[uint32]int
}

// dedupe is dedupe with a table instead of a map when the indices span
// a small range (as they nearly always do).
func (b *dedupeBuf) dedupe(idx []uint32) ([]uint32, []int) {
	lo, hi := uint32(0xffffffff), uint32(0)
	for _, i := range idx {
		if i != 0xffffffff {
			lo, hi = min(lo, i), max(hi, i)
		}
	}
	uniq, vlist := b.uniq[:0], b.vlist[:0]
	if lo > hi || hi-lo >= 1<<20 {
		if b.cache == nil {
			b.cache = map[uint32]int{}
		}
		clear(b.cache)
		u, v := dedupe(idx, b.cache)
		return u, v
	}
	span := int(hi-lo) + 1
	if len(b.slot) < span {
		b.slot = make([]int32, span)
		b.stamp = make([]uint32, span)
		b.gen = 0
	}
	b.gen++
	if b.gen == 0 {
		clear(b.stamp)
		b.gen = 1
	}
	for _, i := range idx {
		if i == 0xffffffff {
			vlist = append(vlist, -1)
			continue
		}
		j := i - lo
		if b.stamp[j] != b.gen {
			b.stamp[j] = b.gen
			b.slot[j] = int32(len(uniq))
			uniq = append(uniq, i)
		}
		vlist = append(vlist, int(b.slot[j]))
	}
	b.uniq, b.vlist = uniq, vlist
	return uniq, vlist
}
