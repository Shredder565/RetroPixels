package snes

// The picture unit (PPU in web/emu/snes.js): modes 0-7, sprites, windows,
// colour math, mosaic, offset-per-tile, hires, Mode 7.

// sprite sizes per OBSEL setting: small w, small h, large w, large h
var objSizes = [8][4]int{{8, 8, 16, 16}, {8, 8, 32, 32}, {8, 8, 64, 64}, {16, 16, 32, 32}, {16, 16, 64, 64}, {32, 32, 64, 64}, {16, 32, 32, 64}, {16, 32, 32, 32}}

// BG bits per pixel per mode (0 = no layer)
var bgBPP = [8][4]int{{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}}

type zTab struct {
	zb [8]int // [layer*2+prio]
	zo [4]int // [prio]
}

// front-to-back layer order per mode key
var zOrders = map[string][]string{
	"0":  {"S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "B2h", "B3h", "S0", "B2l", "B3l"},
	"1":  {"S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "B2h", "S0", "B2l"},
	"1x": {"B2h", "S3", "B0h", "B1h", "S2", "B0l", "B1l", "S1", "S0", "B2l"},
	"2":  {"S3", "B0h", "S2", "B1h", "S1", "B0l", "S0", "B1l"},
	"7":  {"S3", "S2", "S1", "B0h", "B0l", "S0"},
	"7x": {"S3", "S2", "B1h", "S1", "B0h", "B0l", "S0", "B1l"},
}

var zTables = func() map[string]*zTab {
	m := map[string]*zTab{}
	for k, list := range zOrders {
		t := &zTab{}
		n := len(list)
		for i, e := range list {
			z := n - i
			if e[0] == 'S' {
				t.zo[e[1]-'0'] = z
			} else {
				p := 0
				if e[2] == 'h' {
					p = 1
				}
				t.zb[int(e[1]-'0')*2+p] = z
			}
		}
		m[k] = t
	}
	return m
}()

var (
	z0, z1, z1x, z2, z7, z7x = zTables["0"], zTables["1"], zTables["1x"], zTables["2"], zTables["7"], zTables["7x"]
)

// 5-bit -> 8-bit with brightness
var bright = func() (t [16][32]uint32) {
	for b := 0; b < 16; b++ {
		for c := 0; c < 32; c++ {
			t[b][c] = uint32(jsRound(float64((c<<3)|(c>>2)) * float64(b+1) / 16))
		}
	}
	return
}()

type ppu struct {
	s        *SNES
	vram     [0x8000]uint16
	cgram    [256]uint16
	oam      [544]uint8
	frame512 [512 * 240]uint32
	frame256 [256 * 240]uint32
	layer    [4][512]uint32 // colour | 0x10000 opaque | prio << 17
	objBuf   [256]uint32    // colour | 0x10000 | prio << 17 | palHigh << 19
	wins     [6][256]uint8
	objList  []int

	inidisp, obsel, oamAddr, oamReload, oamLatch int
	oamPri                                       bool
	bgmode, mosaic                               int
	bgsc, bgnba, hofs, vofs                      [4]int
	ofsPrev, m7Prev                              int
	vmain, vaddr, vlatch                         int
	m7sel                                        int
	m7                                           [6]int
	m7hofs, m7vofs                               int
	cgaddr, cgLatch                              int
	cgReadHigh                                   bool
	wsel                                         [3]int
	wh                                           [4]int
	wbglog, wobjlog                              int
	tm, ts, tmw, tsw, cgwsel, cgadsub, fixed     int
	setini, mpy, hLatch, vLatch                  int
	hFlip, vFlip, latched, timeOver, rangeOver   bool
	field                                        int
	hiresUsed                                    bool
}

func newPPU(s *SNES) *ppu {
	p := &ppu{s: s, objList: make([]int, 0, 32)}
	p.reset()
	return p
}

func (r *ppu) reset() {
	r.vram = [0x8000]uint16{}
	r.cgram = [256]uint16{}
	r.oam = [544]uint8{}
	r.inidisp, r.obsel, r.oamAddr, r.oamReload, r.oamLatch, r.oamPri = 0x80, 0, 0, 0, 0, false
	r.bgmode, r.mosaic = 0, 0
	r.bgsc, r.bgnba, r.hofs, r.vofs = [4]int{}, [4]int{}, [4]int{}, [4]int{}
	r.ofsPrev, r.m7Prev = 0, 0
	r.vmain, r.vaddr, r.vlatch = 0x80, 0, 0
	r.m7sel, r.m7, r.m7hofs, r.m7vofs = 0, [6]int{}, 0, 0
	r.cgaddr, r.cgLatch, r.cgReadHigh = 0, -1, false
	r.wsel, r.wh, r.wbglog, r.wobjlog = [3]int{}, [4]int{}, 0, 0
	r.tm, r.ts, r.tmw, r.tsw, r.cgwsel, r.cgadsub, r.fixed, r.setini = 0, 0, 0, 0, 0, 0, 0, 0
	r.mpy, r.hLatch, r.vLatch, r.hFlip, r.vFlip, r.latched = 0, 0, 0, false, false, false
	r.timeOver, r.rangeOver, r.field = false, false, 0
	r.hiresUsed = false
}

// ---------- registers ----------

func (r *ppu) vramAddrMapped() int {
	a := r.vaddr
	switch (r.vmain >> 2) & 3 {
	case 0:
		return a & 0x7fff
	case 1:
		return (a&0xff00 | (a&0x1f)<<3 | (a>>5)&7) & 0x7fff
	case 2:
		return (a&0xfe00 | (a&0x3f)<<3 | (a>>6)&7) & 0x7fff
	}
	return (a&0xfc00 | (a&0x7f)<<3 | (a>>7)&7) & 0x7fff
}

func (r *ppu) vinc() {
	n := [4]int{1, 32, 128, 128}[r.vmain&3]
	r.vaddr = (r.vaddr + n) & 0xffff
}

func (r *ppu) write(reg int, val uint8) {
	v := int(val)
	switch reg {
	case 0x00:
		r.inidisp = v
	case 0x01:
		r.obsel = v
	case 0x02:
		r.oamReload = r.oamReload&0x100 | v
		r.oamAddr = (r.oamReload << 1) & 0x3ff
		r.oamPri = r.oamReload&0x8000 != 0
	case 0x03:
		r.oamReload = r.oamReload&0xff | (v&1)<<8 | (v&0x80)<<8
		r.oamAddr = (r.oamReload & 0x1ff) << 1
		r.oamPri = v&0x80 != 0
	case 0x04:
		a := r.oamAddr
		if a < 0x200 {
			if a&1 == 0 {
				r.oamLatch = v
			} else {
				r.oam[a-1] = uint8(r.oamLatch)
				r.oam[a] = val
			}
		} else {
			r.oam[0x200|a&0x1f] = val
		}
		r.oamAddr = (a + 1) & 0x3ff
	case 0x05:
		r.bgmode = v
	case 0x06:
		r.mosaic = v
	case 0x07, 0x08, 0x09, 0x0a:
		r.bgsc[reg-7] = v
	case 0x0b:
		r.bgnba[0], r.bgnba[1] = v&15, v>>4
	case 0x0c:
		r.bgnba[2], r.bgnba[3] = v&15, v>>4
	case 0x0d, 0x0f, 0x11, 0x13:
		k := (reg - 0x0d) >> 1
		r.hofs[k] = (v<<8 | r.ofsPrev&^7 | (r.hofs[k]>>8)&7) & 0x3ff
		r.ofsPrev = v
		if k == 0 {
			r.m7hofs = (v<<8 | r.m7Prev) & 0x1fff
			r.m7Prev = v
		}
	case 0x0e, 0x10, 0x12, 0x14:
		k := (reg - 0x0e) >> 1
		r.vofs[k] = (v<<8 | r.ofsPrev) & 0x3ff
		r.ofsPrev = v
		if k == 0 {
			r.m7vofs = (v<<8 | r.m7Prev) & 0x1fff
			r.m7Prev = v
		}
	case 0x15:
		r.vmain = v
	case 0x16:
		r.vaddr = r.vaddr&0xff00 | v
		r.vlatch = int(r.vram[r.vramAddrMapped()])
	case 0x17:
		r.vaddr = r.vaddr&0xff | v<<8
		r.vlatch = int(r.vram[r.vramAddrMapped()])
	case 0x18:
		a := r.vramAddrMapped()
		r.vram[a] = r.vram[a]&0xff00 | uint16(v)
		if r.vmain&0x80 == 0 {
			r.vinc()
		}
	case 0x19:
		a := r.vramAddrMapped()
		r.vram[a] = r.vram[a]&0xff | uint16(v)<<8
		if r.vmain&0x80 != 0 {
			r.vinc()
		}
	case 0x1a:
		r.m7sel = v
	case 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20:
		k := reg - 0x1b
		r.m7[k] = (v<<8 | r.m7Prev) & 0xffff
		r.m7Prev = v
		if k <= 1 {
			a16, b8 := int(int16(r.m7[0])), int(int8(r.m7[1]>>8))
			r.mpy = (a16 * b8) & 0xffffff
		}
	case 0x21:
		r.cgaddr, r.cgLatch, r.cgReadHigh = v, -1, false
	case 0x22:
		if r.cgLatch < 0 {
			r.cgLatch = v
		} else {
			r.cgram[r.cgaddr] = uint16((r.cgLatch | v<<8) & 0x7fff)
			r.cgaddr = (r.cgaddr + 1) & 0xff
			r.cgLatch = -1
		}
	case 0x23:
		r.wsel[0] = v
	case 0x24:
		r.wsel[1] = v
	case 0x25:
		r.wsel[2] = v
	case 0x26, 0x27, 0x28, 0x29:
		r.wh[reg-0x26] = v
	case 0x2a:
		r.wbglog = v
	case 0x2b:
		r.wobjlog = v
	case 0x2c:
		r.tm = v
	case 0x2d:
		r.ts = v
	case 0x2e:
		r.tmw = v
	case 0x2f:
		r.tsw = v
	case 0x30:
		r.cgwsel = v
	case 0x31:
		r.cgadsub = v
	case 0x32:
		c := v & 31
		if v&0x20 != 0 {
			r.fixed = r.fixed&^0x1f | c
		}
		if v&0x40 != 0 {
			r.fixed = r.fixed&^0x3e0 | c<<5
		}
		if v&0x80 != 0 {
			r.fixed = r.fixed&^0x7c00 | c<<10
		}
	case 0x33:
		r.setini = v
	}
}

func (r *ppu) read(reg int) uint8 {
	switch reg {
	case 0x34:
		return uint8(r.mpy)
	case 0x35:
		return uint8(r.mpy >> 8)
	case 0x36:
		return uint8(r.mpy >> 16)
	case 0x37:
		r.latchCounters()
		return r.s.mdr
	case 0x38:
		a := r.oamAddr
		var v uint8
		if a < 0x200 {
			v = r.oam[a]
		} else {
			v = r.oam[0x200|a&0x1f]
		}
		r.oamAddr = (a + 1) & 0x3ff
		return v
	case 0x39:
		v := uint8(r.vlatch)
		if r.vmain&0x80 == 0 {
			r.vlatch = int(r.vram[r.vramAddrMapped()])
			r.vinc()
		}
		return v
	case 0x3a:
		v := uint8(r.vlatch >> 8)
		if r.vmain&0x80 != 0 {
			r.vlatch = int(r.vram[r.vramAddrMapped()])
			r.vinc()
		}
		return v
	case 0x3b:
		c := r.cgram[r.cgaddr]
		if !r.cgReadHigh {
			r.cgReadHigh = true
			return uint8(c)
		}
		r.cgReadHigh = false
		r.cgaddr = (r.cgaddr + 1) & 0xff
		return uint8(c>>8) & 0x7f
	case 0x3c:
		var v int
		if r.hFlip {
			v = (r.hLatch >> 8) & 1
		} else {
			v = r.hLatch & 0xff
		}
		r.hFlip = !r.hFlip
		return uint8(v)
	case 0x3d:
		var v int
		if r.vFlip {
			v = (r.vLatch >> 8) & 1
		} else {
			v = r.vLatch & 0xff
		}
		r.vFlip = !r.vFlip
		return uint8(v)
	case 0x3e:
		v := uint8(1)
		if r.timeOver {
			v |= 0x80
		}
		if r.rangeOver {
			v |= 0x40
		}
		return v
	case 0x3f:
		v := uint8(r.field<<7) | 3
		if r.latched {
			v |= 0x40
		}
		if r.s.pal {
			v |= 0x10
		}
		r.hFlip, r.vFlip, r.latched = false, false, false
		return v
	}
	return r.s.mdr
}

func (r *ppu) latchCounters() {
	s := r.s
	r.hLatch = min(339, floorDiv(int(s.clock-s.lineStart), 4))
	r.vLatch = s.line
	r.latched = true
}

func (r *ppu) vblankStart() {
	if r.inidisp&0x80 == 0 {
		r.oamAddr = (r.oamReload << 1) & 0x3ff
	}
}

func floorDiv(a, b int) int {
	q := a / b
	if (a%b != 0) && ((a < 0) != (b < 0)) {
		q--
	}
	return q
}

// ---------- windows ----------

func (r *ppu) winMask(sel, logic int, out *[256]uint8) {
	e1, e2 := sel&2 != 0, sel&8 != 0
	if !e1 && !e2 {
		*out = [256]uint8{}
		return
	}
	l1, r1, l2, r2 := r.wh[0], r.wh[1], r.wh[2], r.wh[3]
	i1, i2 := 0, 0
	if sel&1 != 0 {
		i1 = 1
	}
	if sel&4 != 0 {
		i2 = 1
	}
	for x := 0; x < 256; x++ {
		a, b := -1, -1
		if e1 {
			a = b2i(x >= l1 && x <= r1) ^ i1
		}
		if e2 {
			b = b2i(x >= l2 && x <= r2) ^ i2
		}
		var w int
		if a < 0 {
			w = b
		} else if b < 0 {
			w = a
		} else {
			switch logic {
			case 0:
				w = a | b
			case 1:
				w = a & b
			case 2:
				w = a ^ b
			default:
				w = a ^ b ^ 1
			}
		}
		out[x] = uint8(w)
	}
}

// ---------- background layers ----------

// optLookup: the offset-per-tile values for layer k at x (modes 2, 4, 6).
func (r *ppu) optLookup(k, x int) (h, v int, hasH, hasV bool) {
	mode := r.bgmode & 7
	sc := r.bgsc[2]
	mapBase := (sc & 0xfc) << 8
	h3, v3 := r.hofs[2], r.vofs[2]
	col := ((x + r.hofs[k]&7) >> 3) - 1
	if col < 0 {
		return
	}
	tx, ty := ((h3>>3)+col)&63, (v3>>3)&63
	addr := func(row int) int {
		a := mapBase + ((ty+row)&31)<<5 + tx&31
		if tx&32 != 0 && sc&1 != 0 {
			a += 0x400
		}
		return a & 0x7fff
	}
	bit := 0x4000
	if k == 0 {
		bit = 0x2000
	}
	if mode == 4 {
		e := int(r.vram[addr(0)])
		if e&bit != 0 {
			if e&0x8000 != 0 {
				v, hasV = e&0x3ff, true
			} else {
				h, hasH = e&0x3f8|r.hofs[k]&7, true
			}
		}
	} else {
		eh, ev := int(r.vram[addr(0)]), int(r.vram[addr(1)])
		if eh&bit != 0 {
			h, hasH = eh&0x3f8|r.hofs[k]&7, true
		}
		if ev&bit != 0 {
			v, hasV = ev&0x3ff, true
		}
	}
	return
}

func (r *ppu) renderBG(k, bpp, y int, hires bool, buf *[512]uint32, opt bool) {
	vram, cg := &r.vram, &r.cgram
	big := (r.bgmode >> (4 + k)) & 1
	tw, th := 8, 8
	if hires || big != 0 {
		tw = 16
	}
	if big != 0 {
		th = 16
	}
	sc := r.bgsc[k]
	mapBase, wide, tall := (sc&0xfc)<<8, sc&1, sc&2
	chrBase := r.bgnba[k] << 12
	words := bpp * 4
	W := 256
	if hires {
		W = 512
	}
	mos := 1
	if (r.mosaic>>k)&1 != 0 {
		mos = (r.mosaic >> 4) + 1
	}
	ly := y
	if mos > 1 {
		ly = y - (y-1)%mos
	}
	palBase := 0
	if r.bgmode == 0 {
		palBase = k * 32
	}
	direct := bpp == 8 && r.cgwsel&1 != 0
	hofsBase, vofsBase := r.hofs[k], r.vofs[k]
	mapWpx, mapHpx := 32*tw, 32*th
	if wide != 0 {
		mapWpx = 64 * tw
	}
	if tall != 0 {
		mapHpx = 64 * th
	}
	thS, twS := 3, 3 // (th and tw as shifts)
	if th == 16 {
		thS = 4
	}
	if tw == 16 {
		twS = 4
	}
	lastCell, attr := -1, 0
	var pix [16]uint8
	tileW := 8
	if hires || big != 0 {
		tileW = 16
	}
	palMul := 0
	switch bpp {
	case 2:
		palMul = 4
	case 4:
		palMul = 16
	}
	// mosaic blocks: x is the block's first pixel (sx - sx % step)
	step := mos
	if hires {
		step = mos * 2
	}
	blockX, inBlock := 0, 0
	for sx := 0; sx < W; sx++ {
		if inBlock == step {
			inBlock, blockX = 0, sx
		}
		inBlock++
		x := sx
		if mos > 1 {
			x = blockX
		}
		hofs, vofs := hofsBase, vofsBase
		if opt {
			ox := x
			if hires {
				ox = x >> 1
			}
			if h, v, hasH, hasV := r.optLookup(k, ox); hasH || hasV {
				if hasH {
					hofs = h
				}
				if hasV {
					vofs = v
				}
			}
		}
		hx := hofs
		if hires {
			hx = hofs * 2
		}
		px := (hx + x) & (mapWpx - 1)
		py := (vofs + ly) & (mapHpx - 1)
		cy := py >> thS
		tx := px >> twS // (16-pixel cells in hires)
		cellKey := tx<<8 | cy | vofs<<20
		if cellKey != lastCell {
			lastCell = cellKey
			mx, my := tx, cy
			ma := mapBase + (my&31)<<5 + mx&31
			if mx&32 != 0 && wide != 0 {
				ma += 0x400
			}
			if my&32 != 0 && tall != 0 {
				if wide != 0 {
					ma += 0x800
				} else {
					ma += 0x400
				}
			}
			attr = int(vram[ma&0x7fff])
			vf, hf := attr&0x8000 != 0, attr&0x4000 != 0
			row := py & (th - 1)
			if vf {
				row = th - 1 - row
			}
			tile := attr & 0x3ff
			if th == 16 && row >= 8 {
				tile += 16
			}
			for half := 0; half < tileW/8; half++ {
				t := tile
				if tileW == 16 {
					if hf {
						t += 1 - half
					} else {
						t += half
					}
				}
				base := (chrBase + (t&0x3ff)*words + row&7) & 0x7fff
				p01 := int(vram[base])
				p23, p45, p67 := 0, 0, 0
				if bpp >= 4 {
					p23 = int(vram[(base+8)&0x7fff])
				}
				if bpp == 8 {
					p45 = int(vram[(base+16)&0x7fff])
					p67 = int(vram[(base+24)&0x7fff])
				}
				for c := 0; c < 8; c++ {
					bit := 7 - c
					if hf {
						bit = c
					}
					v := (p01>>bit)&1 | ((p01>>(bit+8))&1)<<1
					if bpp >= 4 {
						v |= ((p23>>bit)&1)<<2 | ((p23>>(bit+8))&1)<<3
					}
					if bpp == 8 {
						v |= ((p45>>bit)&1)<<4 | ((p45>>(bit+8))&1)<<5 | ((p67>>bit)&1)<<6 | ((p67>>(bit+8))&1)<<7
					}
					pix[half*8+c] = uint8(v)
				}
			}
		}
		var v int
		if hires || tw == 16 {
			v = int(pix[px&15])
		} else {
			v = int(pix[px&7])
		}
		if v == 0 {
			buf[sx] = 0
			continue
		}
		pal, prio := (attr>>10)&7, (attr>>13)&1
		var color int
		if direct {
			color = (v&7)<<2 | (pal&1)<<1 | (((v>>3)&7)<<2|pal&2)<<5 | (((v>>6)&3)<<3|pal&4)<<10
		} else {
			color = int(cg[(palBase+pal*palMul+v)&0xff])
		}
		buf[sx] = uint32(color | 0x10000 | prio<<17)
	}
}

func s13(v int) int { return ((v & 0x1fff) ^ 0x1000) - 0x1000 }

func m7clip(v int) int {
	if v&0x2000 != 0 {
		return v | ^0x3ff
	}
	return v & 0x3ff
}

func (r *ppu) renderMode7(y int, buf *[512]uint32, ext bool) {
	vram, cg := &r.vram, &r.cgram
	sel := r.m7sel
	A, B, C, D := int(int16(r.m7[0])), int(int16(r.m7[1])), int(int16(r.m7[2])), int(int16(r.m7[3]))
	X0, Y0 := s13(r.m7[4]), s13(r.m7[5])
	H, V := s13(r.m7hofs), s13(r.m7vofs)
	mos := 1
	if r.mosaic&1 != 0 {
		mos = (r.mosaic >> 4) + 1
	}
	ly := y
	if mos > 1 {
		ly = y - (y-1)%mos
	}
	sy := ly
	if sel&2 != 0 {
		sy = 255 - ly
	}
	hx, vy := m7clip(H-X0), m7clip(V-Y0)
	bx := int(int32(A*hx)&^63) + int(int32(B*vy)&^63) + int(int32(B*sy)&^63) + X0<<8
	by := int(int32(C*hx)&^63) + int(int32(D*vy)&^63) + int(int32(D*sy)&^63) + Y0<<8
	direct := r.cgwsel&1 != 0
	for sx := 0; sx < 256; sx++ {
		x0 := sx
		if mos > 1 {
			x0 = sx - sx%mos
		}
		x := x0
		if sel&1 != 0 {
			x = 255 - x0
		}
		px, py := int(int32(bx+A*x))>>8, int(int32(by+C*x))>>8
		var tile int
		out := px < 0 || py < 0 || px > 1023 || py > 1023
		if out && sel&0x80 != 0 {
			if sel&0x40 != 0 {
				tile = 0
			} else {
				buf[sx] = 0
				continue
			}
			px &= 7
			py &= 7
		} else {
			px &= 1023
			py &= 1023
			tile = int(vram[(py>>3)<<7+px>>3] & 0xff)
		}
		v := int(vram[tile<<6+(py&7)<<3+px&7] >> 8)
		if ext {
			if v&0x7f == 0 {
				buf[sx] = 0
				continue
			}
			buf[sx] = uint32(int(cg[v&0x7f]) | 0x10000 | (v>>7)<<17)
		} else {
			if v == 0 {
				buf[sx] = 0
				continue
			}
			var color int
			if direct {
				color = (v&7)<<2 | (((v>>3)&7)<<2)<<5 | (((v>>6)&3)<<3)<<10
			} else {
				color = int(cg[v])
			}
			buf[sx] = uint32(color | 0x10000)
		}
	}
}

// ---------- sprites ----------

func (r *ppu) renderOBJ(y int) {
	oam, vram, cg, buf := &r.oam, &r.vram, &r.cgram, &r.objBuf
	*buf = [256]uint32{}
	sz := objSizes[r.obsel>>5]
	base, gap := (r.obsel&7)<<13, (((r.obsel>>3)&3)+1)<<12
	first := 0
	if r.oamPri {
		first = (r.oamAddr >> 2) & 0x7f
	}
	list := r.objList[:0]
	for n := 0; n < 128; n++ {
		i := (first + n) & 127
		hi := int(oam[0x200+i>>2]) >> ((i & 3) * 2)
		large := hi&2 != 0
		w, h := sz[0], sz[1]
		if large {
			w, h = sz[2], sz[3]
		}
		x := int(oam[i*4]) | (hi&1)<<8
		if x >= 256 {
			x -= 512
		}
		sy := int(oam[i*4+1])
		row := (y - sy) & 0xff
		if row >= h {
			continue
		}
		if x <= -w || x >= 256 {
			continue
		}
		if len(list) == 32 {
			r.rangeOver = true
			break
		}
		list = append(list, i)
	}
	r.objList = list
	tiles := 0
	for li := len(list) - 1; li >= 0; li-- {
		i := list[li]
		hi := int(oam[0x200+i>>2]) >> ((i & 3) * 2)
		large := hi&2 != 0
		w, h := sz[0], sz[1]
		if large {
			w, h = sz[2], sz[3]
		}
		x := int(oam[i*4]) | (hi&1)<<8
		if x >= 256 {
			x -= 512
		}
		attr := int(oam[i*4+3])
		t := int(oam[i*4+2]) | (attr&1)<<8
		row := (y - int(oam[i*4+1])) & 0xff
		if attr&0x80 != 0 {
			row = h - 1 - row
		}
		pal, prio, hf := (attr>>1)&7, (attr>>4)&3, attr&0x40 != 0
		cy := row >> 3
		palHi := 0
		if pal >= 4 {
			palHi = 1
		}
		for c := 0; c < w/8; c++ {
			sx := x + c*8
			if sx <= -8 || sx >= 256 {
				continue
			}
			tiles++
			if tiles > 34 {
				r.timeOver = true
				return
			}
			cx := c
			if hf {
				cx = w/8 - 1 - c
			}
			tn := (((t>>4)+cy)&15)<<4 | (t+cx)&15
			g := 0
			if t&0x100 != 0 {
				g = gap
			}
			addr := (base + g + tn*16 + row&7) & 0x7fff
			p01, p23 := int(vram[addr]), int(vram[(addr+8)&0x7fff])
			for p := 0; p < 8; p++ {
				xx := sx + p
				if xx < 0 || xx >= 256 {
					continue
				}
				bit := 7 - p
				if hf {
					bit = p
				}
				v := (p01>>bit)&1 | ((p01>>(bit+8))&1)<<1 | ((p23>>bit)&1)<<2 | ((p23>>(bit+8))&1)<<3
				if v == 0 {
					continue
				}
				buf[xx] = uint32(int(cg[128+pal*16+v]) | 0x10000 | prio<<17 | palHi<<19)
			}
		}
	}
}

// ---------- compose one line ----------

// pickScreen returns the front layer (0-3 BG, 4 OBJ, 5 backdrop), its colour
// and the sprite palette-high bit.
func (r *ppu) pickScreen(enable, winEn, x, xs, valid int, zt *zTab) (int, int, int) {
	bestZ, bestL, bestC, bestPH := 0, 5, int(r.cgram[0]), 0
	for k := 0; k < 4; k++ {
		if enable&(1<<k) == 0 || valid&(1<<k) == 0 {
			continue
		}
		if winEn&(1<<k) != 0 && r.wins[k][xs] != 0 {
			continue
		}
		v := r.layer[k][x]
		if v&0x10000 == 0 {
			continue
		}
		z := zt.zb[k*2+int((v>>17)&1)]
		if z > bestZ {
			bestZ, bestL, bestC = z, k, int(v&0x7fff)
		}
	}
	if enable&16 != 0 {
		v := r.objBuf[xs]
		if v&0x10000 != 0 && !(winEn&16 != 0 && r.wins[4][xs] != 0) {
			z := zt.zo[(v>>17)&3]
			if z > bestZ {
				bestL, bestC, bestPH = 4, int(v&0x7fff), int((v>>19)&1)
			}
		}
	}
	return bestL, bestC, bestPH
}

// rgb turns a BGR555 colour at a brightness into 0x00RRGGBB.
func rgb(c int, br *[32]uint32) uint32 {
	return br[c&31]<<16 | br[(c>>5)&31]<<8 | br[(c>>10)&31]
}

func (r *ppu) renderLine(y int) {
	outY := y - 1
	if outY < 0 || outY >= 240 {
		return
	}
	f512, f256 := &r.frame512, &r.frame256
	if r.inidisp&0x80 != 0 {
		clear(f512[outY*512 : outY*512+512])
		clear(f256[outY*256 : outY*256+256])
		return
	}
	mode := r.bgmode & 7
	hires := mode == 5 || mode == 6 || r.setini&8 != 0
	W := 256
	if mode == 5 || mode == 6 {
		W = 512
	}
	if hires {
		r.hiresUsed = true
	}
	bpp := bgBPP[mode]
	layers := r.tm | r.ts
	opt := mode == 2 || mode == 4 || mode == 6
	for k := 0; k < 4; k++ {
		if layers&(1<<k) == 0 {
			continue
		}
		if mode == 7 {
			if k == 0 {
				r.renderMode7(y, &r.layer[0], false)
			} else if k == 1 && r.setini&0x40 != 0 {
				r.renderMode7(y, &r.layer[1], true)
			}
			continue
		}
		if bpp[k] == 0 {
			continue
		}
		r.renderBG(k, bpp[k], y, W == 512, &r.layer[k], k < 2 && opt)
	}
	if layers&16 != 0 {
		r.renderOBJ(y)
	} else {
		r.objBuf = [256]uint32{}
	}
	var zt *zTab
	switch mode {
	case 1:
		zt = z1
		if r.bgmode&8 != 0 {
			zt = z1x
		}
	case 7:
		zt = z7
		if r.setini&0x40 != 0 {
			zt = z7x
		}
	case 0:
		zt = z0
	default:
		zt = z2
	}
	valid := 0
	if mode == 7 {
		valid = 1
		if r.setini&0x40 != 0 {
			valid = 3
		}
	} else {
		for i, b := range bpp {
			if b != 0 {
				valid |= 1 << i
			}
		}
	}
	sel := [6]int{r.wsel[0] & 15, r.wsel[0] >> 4, r.wsel[1] & 15, r.wsel[1] >> 4, r.wsel[2] & 15, r.wsel[2] >> 4}
	logic := [6]int{r.wbglog & 3, (r.wbglog >> 2) & 3, (r.wbglog >> 4) & 3, (r.wbglog >> 6) & 3, r.wobjlog & 3, (r.wobjlog >> 2) & 3}
	for k := 0; k < 6; k++ {
		r.winMask(sel[k], logic[k], &r.wins[k])
	}
	cw := &r.wins[5]
	cgw, cga := r.cgwsel, r.cgadsub
	clipMode, mathMode, useSub := (cgw>>6)&3, (cgw>>4)&3, cgw&2 != 0
	sub, half := cga&0x80 != 0, cga&0x40 != 0
	br := &bright[r.inidisp&15]
	o512, o256 := outY*512, outY*256
	for sx := 0; sx < W; sx++ {
		xs := sx
		if W == 512 {
			xs = sx >> 1
		}
		ml, mc, mph := r.pickScreen(r.tm, r.tmw, sx, xs, valid, zt)
		inCw := cw[xs] != 0
		black := clipMode == 3 || (clipMode == 1 && !inCw) || (clipMode == 2 && inCw)
		if black {
			mc = 0
		}
		mathOk := mathMode == 0 || (mathMode == 1 && inCw) || (mathMode == 2 && !inCw)
		var layerMath bool
		if ml == 4 {
			layerMath = cga&16 != 0 && mph != 0
		} else {
			layerMath = cga&(1<<ml) != 0
		}
		c := mc
		if mathOk && layerMath {
			var oc int
			halfOk := half
			if useSub {
				sl, scol, _ := r.pickScreen(r.ts, r.tsw, sx, xs, valid, zt)
				if sl == 5 {
					oc = r.fixed
					halfOk = false
				} else {
					oc = scol
				}
			} else {
				oc = r.fixed
			}
			rr, g, b := mc&31, (mc>>5)&31, (mc>>10)&31
			r2, g2, b2 := oc&31, (oc>>5)&31, (oc>>10)&31
			if sub {
				rr, g, b = max(rr-r2, 0), max(g-g2, 0), max(b-b2, 0)
			} else {
				rr, g, b = rr+r2, g+g2, b+b2
			}
			if halfOk && !black {
				rr >>= 1
				g >>= 1
				b >>= 1
			}
			c = min(rr, 31) | min(g, 31)<<5 | min(b, 31)<<10
		}
		px := rgb(c, br)
		if W == 512 {
			f512[o512+sx] = px
			if sx&1 == 0 {
				f256[o256+sx>>1] = px
			}
		} else {
			f512[o512+sx*2] = px
			f512[o512+sx*2+1] = px
			f256[o256+sx] = px
		}
	}
}

func (r *ppu) state(s *sio) {
	s.U16s(r.vram[:])
	s.U16s(r.cgram[:])
	s.Bytes(r.oam[:])
	s.Ints(&r.inidisp, &r.obsel, &r.oamAddr, &r.oamReload, &r.oamLatch)
	s.Bool(&r.oamPri)
	s.Ints(&r.bgmode, &r.mosaic, &r.ofsPrev, &r.m7Prev, &r.vmain, &r.vaddr, &r.vlatch, &r.m7sel, &r.m7hofs, &r.m7vofs, &r.cgaddr, &r.cgLatch)
	s.Bool(&r.cgReadHigh)
	s.Ints(&r.wbglog, &r.wobjlog, &r.tm, &r.ts, &r.tmw, &r.tsw, &r.cgwsel, &r.cgadsub, &r.fixed, &r.setini, &r.mpy, &r.hLatch, &r.vLatch)
	s.Bools(&r.hFlip, &r.vFlip, &r.latched, &r.timeOver, &r.rangeOver)
	s.Int(&r.field)
	s.IntSlice(r.bgsc[:])
	s.IntSlice(r.bgnba[:])
	s.IntSlice(r.hofs[:])
	s.IntSlice(r.vofs[:])
	s.IntSlice(r.m7[:])
	s.IntSlice(r.wsel[:])
	s.IntSlice(r.wh[:])
}
