package genesis

import "math"

// The VDP: planes A/B, window, sprites, shadow/highlight, DMA, interlace
// (class VDP in genesis.js).

// The VDP's colour DAC has 15 output steps: shadow uses steps 0-7, normal the
// even steps 0-14 and highlight steps 7-14. The steps aren't evenly spaced
// (closer together in the middle); RetroPixels models them with a cubic
// S-curve from black to full brightness: round(127.5 + 92.5x + 35x^3) for
// x = (k-7)/7 (the values JS computes).
var dacSteps = [15]uint32{0, 26, 49, 68, 85, 100, 114, 128, 141, 155, 170, 187, 206, 229, 255}

var lvN, lvS, lvH [8]uint32

func init() {
	for i := 0; i < 8; i++ {
		lvN[i] = dacSteps[i*2]
		lvS[i] = dacSteps[i]
		lvH[i] = dacSteps[7+i]
	}
}

type vdp struct {
	md    *Genesis
	vram  [0x10000]uint8
	cram  [64]uint16
	vsram [40]uint16
	reg   [24]uint8
	sat   [640]uint8 // the VDP's internal copy of sprite Y / size / link
	pal   [64 * 3]uint32

	bufA, bufB, bufS [336]uint8
	sprList          [20]int

	addr, code               int
	pending, fillPending     bool
	vintPending, hintPending bool
	hcount                   int
	sovr, scol               bool
	odd                      int
	prevOverflow             bool
	readBuf                  int
}

func (v *vdp) reset() {
	v.vram = [0x10000]uint8{}
	v.cram = [64]uint16{}
	v.vsram = [40]uint16{}
	v.reg = [24]uint8{}
	v.sat = [640]uint8{}
	v.addr, v.code, v.pending, v.fillPending = 0, 0, false, false
	v.vintPending, v.hintPending, v.hcount = false, false, 0
	v.sovr, v.scol, v.odd, v.prevOverflow = false, false, 0, false
	v.readBuf = 0
	for i := 0; i < 64; i++ {
		v.updatePal(i)
	}
}

func (v *vdp) h40() bool     { return v.reg[12]&0x81 != 0 }
func (v *vdp) v30() bool     { return v.reg[1]&8 != 0 }
func (v *vdp) im2() bool     { return v.reg[12]&6 == 6 }
func (v *vdp) display() bool { return v.reg[1]&0x40 != 0 }
func (v *vdp) width() int {
	if v.h40() {
		return 320
	}
	return 256
}
func (v *vdp) lines() int {
	if v.v30() {
		return 240
	}
	return 224
}

func (v *vdp) updatePal(i int) {
	w := v.cram[i]
	r, g, b := (w>>1)&7, (w>>5)&7, (w>>9)&7
	mk := func(L *[8]uint32) uint32 { return L[r]<<16 | L[g]<<8 | L[b] }
	v.pal[i] = mk(&lvS)
	v.pal[64+i] = mk(&lvN)
	v.pal[128+i] = mk(&lvH)
}

// ---- ports ----

func (v *vdp) status() int {
	md := v.md
	pos := md.now68k() - md.lineStart
	s := 0x3600
	if v.vintPending {
		s |= 0x80
	}
	if v.sovr {
		s |= 0x40
	}
	if v.scol {
		s |= 0x20
	}
	if v.odd != 0 && v.reg[12]&2 != 0 {
		s |= 0x10
	}
	if md.vblank || !v.display() {
		s |= 8
	}
	if pos >= active || pos < 40 {
		s |= 4
	}
	if md.pal {
		s |= 1
	}
	v.pending, v.sovr, v.scol = false, false, false
	return s
}

func (v *vdp) hv() int {
	md := v.md
	if v.reg[0]&2 != 0 && md.hvLatch >= 0 {
		return md.hvLatch
	}
	pos := int(md.now68k() - md.lineStart)
	if pos > lineClk-1 {
		pos = lineClk - 1
	}
	if pos < 0 {
		pos = 0
	}
	var hc int
	if v.h40() {
		i := pos * 210 / lineClk
		if i < 183 {
			hc = i
		} else {
			hc = i - 183 + 0xe4
		}
	} else {
		i := pos * 171 / lineClk
		if i < 148 {
			hc = i
		} else {
			hc = i - 148 + 0xe9
		}
	}
	vc := md.line
	if md.pal {
		lim := 0x102
		if v.v30() {
			lim = 0x10a
		}
		if vc > lim {
			vc -= 57
		}
	} else if !v.v30() && vc > 0xea {
		vc -= 6
	}
	vc &= 0xff
	if v.im2() {
		vc = (vc<<1 | vc>>7) & 0xff
	}
	return vc<<8 | hc&0xff
}

func (v *vdp) writeControl(val int) {
	if v.pending {
		v.pending = false
		v.addr = v.addr&0x3fff | (val&3)<<14
		v.code = v.code&3 | (val>>2)&0x3c
		if v.code&0x20 != 0 && v.reg[1]&0x10 != 0 {
			v.startDMA()
		}
		return
	}
	if val&0xc000 == 0x8000 {
		r := (val >> 8) & 0x1f
		if r < 24 {
			v.writeReg(r, uint8(val))
		}
		v.code &= 0x3c // a register write also clears the access code
		return
	}
	v.pending = true
	v.addr = v.addr&0xc000 | val&0x3fff
	v.code = v.code&0x3c | val>>14
}

func (v *vdp) writeReg(r int, val uint8) {
	if v.reg[1]&4 == 0 && r > 10 { // mode 4 (Master System) only has 11 registers
		return
	}
	old := v.reg[r]
	v.reg[r] = val
	if r == 0 || r == 1 {
		v.md.updateIRQ()
	}
	if r == 0 && val&2 != 0 && old&2 == 0 {
		v.md.hvLatch = v.hv()
	}
}

func (v *vdp) satBase() int {
	if v.h40() {
		return int(v.reg[5]&0x7e) << 9
	}
	return int(v.reg[5]&0x7f) << 9
}

func (v *vdp) vramWriteByte(a int, b uint8) {
	a &= 0xffff
	v.vram[a] = b
	off := a - v.satBase()
	if off >= 0 && off < 640 && off&7 < 4 {
		v.sat[off] = b
	}
}

func (v *vdp) writeWord(val int) {
	a := v.addr
	switch v.code & 0xf {
	case 1:
		if a&1 != 0 {
			val = (val&0xff)<<8 | val>>8
		}
		v.vramWriteByte(a&0xfffe, uint8(val>>8))
		v.vramWriteByte(a|1, uint8(val))
	case 3:
		i := (a >> 1) & 0x3f
		v.cram[i] = uint16(val & 0xeee)
		v.updatePal(i)
	case 5:
		i := (a >> 1) & 0x3f
		if i < 40 {
			v.vsram[i] = uint16(val & 0x7ff)
		}
	}
	v.addr = (v.addr + int(v.reg[15])) & 0xffff
}

func (v *vdp) writeData(val int) {
	v.pending = false
	v.writeWord(val)
	if v.fillPending {
		v.fillPending = false
		v.dmaFill(val)
	}
}

func (v *vdp) readData() int {
	v.pending = false
	a := v.addr
	var r int
	switch v.code & 0xf {
	case 0:
		r = int(v.vram[a&0xfffe])<<8 | int(v.vram[a|1])
	case 4:
		i := (a >> 1) & 0x3f
		var w uint16
		if i < 40 {
			w = v.vsram[i]
		} else {
			w = v.vsram[0]
		}
		r = int(w&0x7ff) | v.readBuf&0xf800
	case 8:
		r = int(v.cram[(a>>1)&0x3f]&0xeee) | v.readBuf&0xf111
	case 0xc:
		r = v.readBuf&0xff00 | int(v.vram[a^1])
	default:
		r = v.readBuf
	}
	v.readBuf = r
	v.addr = (v.addr + int(v.reg[15])) & 0xffff
	return r
}

// ---- DMA ----

func (v *vdp) dmaLength() int {
	n := int(v.reg[19]) | int(v.reg[20])<<8
	if n == 0 {
		return 0x10000
	}
	return n
}

func (v *vdp) startDMA() {
	mode := v.reg[23] >> 6
	if mode == 2 {
		v.fillPending = true
		return
	}
	n := v.dmaLength()
	if mode == 3 {
		src := int(v.reg[21]) | int(v.reg[22])<<8
		for i := 0; i < n; i++ {
			v.vramWriteByte(v.addr, v.vram[src])
			src = (src + 1) & 0xffff
			v.addr = (v.addr + int(v.reg[15])) & 0xffff
		}
		v.reg[21], v.reg[22] = uint8(src), uint8(src>>8)
		v.md.dmaStall(n, true, false)
	} else {
		src := (int(v.reg[21]) | int(v.reg[22])<<8 | int(v.reg[23]&0x7f)<<16) << 1
		md := v.md
		for i := 0; i < n; i++ {
			v.writeWord(md.dmaRead(src))
			src = src&0xfe0000 | (src+2)&0x1ffff
		}
		s := src >> 1
		v.reg[21], v.reg[22], v.reg[23] = uint8(s), uint8(s>>8), v.reg[23]&0x80|uint8(s>>16)&0x7f
		md.dmaStall(n, false, v.code&0xf == 1)
	}
	v.reg[19], v.reg[20] = 0, 0
	v.code &^= 0x20
}

func (v *vdp) dmaFill(val int) {
	n, t := v.dmaLength(), v.code&0xf
	for i := 0; i < n; i++ {
		switch t {
		case 1:
			v.vramWriteByte(v.addr^1, uint8(val>>8))
		case 3:
			k := (v.addr >> 1) & 0x3f
			v.cram[k] = uint16(val & 0xeee)
			v.updatePal(k)
		case 5:
			k := (v.addr >> 1) & 0x3f
			if k < 40 {
				v.vsram[k] = uint16(val & 0x7ff)
			}
		}
		v.addr = (v.addr + int(v.reg[15])) & 0xffff
	}
	v.reg[19], v.reg[20] = 0, 0
	v.code &^= 0x20
	v.md.dmaStall(n, true, false)
}

// ---- rendering ----

// drawPlane draws one background plane line into buf (value = prio<<7 | pal<<4 | index).
func (v *vdp) drawPlane(buf *[336]uint8, nt, hs, vsOff, y, W int) {
	vram := &v.vram
	im2 := v.im2()
	ws, hsz := v.reg[16]&3, (v.reg[16]>>4)&3
	pw := 32
	if ws == 3 {
		pw = 128
	} else if ws == 1 {
		pw = 64
	}
	ph := 32
	if hsz == 3 {
		ph = 128
	} else if hsz == 1 {
		ph = 64
	}
	if pw == 128 {
		ph = 32
	} else if pw == 64 && ph == 128 {
		ph = 64
	}
	cellH, vmask := 8, uint16(0x3ff)
	if im2 {
		cellH, vmask = 16, 0x7ff
	}
	pxMask, pyMask := pw*8-1, ph*cellH-1
	colMode := v.reg[11]&4 != 0
	fine := hs & 15
	lastCell, lastCol, py := -1, -2, 0
	var rowBytes uint32
	var flip int
	var pal uint8
	for x := 0; x < W; x++ {
		if colMode {
			col := ((x - fine + 16) >> 4) - 1
			if col < 0 {
				col = 0
			}
			if col != lastCol {
				lastCol = col
				vs := int(v.vsram[(col*2+vsOff)%40] & vmask)
				py = (y + vs) & pyMask
				lastCell = -1
			}
		} else if lastCol == -2 {
			lastCol = 0
			py = (y + int(v.vsram[vsOff]&vmask)) & pyMask
		}
		px := (x - hs) & pxMask
		cell := px >> 3
		if cell != lastCell {
			lastCell = cell
			ea := (nt + ((py/cellH)*pw+cell)*2) & 0xffff
			attr := int(vram[ea])<<8 | int(vram[ea+1])
			row := py & (cellH - 1)
			if attr&0x1000 != 0 {
				row = cellH - 1 - row
			}
			var ta int
			if im2 {
				ta = (attr&0x3ff)<<6 + row*4
			} else {
				ta = (attr&0x7ff)<<5 + row*4
			}
			rowBytes = uint32(vram[ta&0xffff])<<24 | uint32(vram[(ta+1)&0xffff])<<16 | uint32(vram[(ta+2)&0xffff])<<8 | uint32(vram[(ta+3)&0xffff])
			flip = 0
			if attr&0x800 != 0 {
				flip = 7
			}
			pal = uint8(((attr>>13)&3)<<4 | (attr>>8)&0x80)
		}
		n := (px & 7) ^ flip
		buf[x] = pal | uint8((rowBytes>>(28-n*4))&15)
	}
}

func (v *vdp) drawWindow(buf *[336]uint8, y, x0, x1 int) {
	vram := &v.vram
	im2 := v.im2()
	cellH := 8
	if im2 {
		cellH = 16
	}
	ww := 32
	nt := int(v.reg[3]&0x3e) << 10
	if v.h40() {
		ww = 64
		nt = int(v.reg[3]&0x3c) << 10
	}
	ry := y / cellH
	for x := x0; x < x1; x++ {
		ea := (nt + (ry*ww+(x>>3))*2) & 0xffff
		attr := int(vram[ea])<<8 | int(vram[ea+1])
		row := y & (cellH - 1)
		if attr&0x1000 != 0 {
			row = cellH - 1 - row
		}
		col := x & 7
		if attr&0x800 != 0 {
			col ^= 7
		}
		var ta int
		if im2 {
			ta = (attr & 0x3ff) << 6
		} else {
			ta = (attr & 0x7ff) << 5
		}
		ta += row*4 + col>>1
		b := vram[ta&0xffff]
		px := b >> 4
		if col&1 != 0 {
			px = b & 15
		}
		buf[x] = uint8(((attr>>13)&3)<<4|(attr>>8)&0x80) | px
	}
}

func (v *vdp) drawSprites(buf *[336]uint8, y, W int) {
	for i := 0; i < W; i++ {
		buf[i] = 0
	}
	vram, sat := &v.vram, &v.sat
	im2 := v.im2()
	max, perLine := 64, 16
	if v.h40() {
		max, perLine = 80, 20
	}
	dotMax := W
	base := v.satBase()
	cellH, yoff, ymask := 8, 128, 0x1ff
	if im2 {
		cellH, yoff, ymask = 16, 256, 0x3ff
	}
	// find sprites on this line
	n, idx, overflow := 0, 0, false
	for k := 0; k < max; k++ {
		o := idx * 8
		sy := (int(sat[o])<<8|int(sat[o+1]))&ymask - yoff
		size := int(sat[o+2])
		h := ((size & 3) + 1) * cellH
		if y >= sy && y < sy+h {
			if n == perLine {
				overflow = true
				break
			}
			v.sprList[n] = idx
			n++
		}
		idx = int(sat[o+3] & 0x7f)
		if idx == 0 || idx >= max {
			break
		}
	}
	if overflow {
		v.sovr = true
	}
	dots, masked, any := 0, false, false
	for k := 0; k < n && !masked; k++ {
		i := v.sprList[k]
		o := i * 8
		va := (base + o) & 0xffff
		sy := (int(sat[o])<<8|int(sat[o+1]))&ymask - yoff
		size := int(sat[o+2])
		hc, wc := (size&3)+1, ((size>>2)&3)+1
		attr := int(vram[(va+4)&0xffff])<<8 | int(vram[(va+5)&0xffff])
		sx := (int(vram[(va+6)&0xffff])<<8|int(vram[(va+7)&0xffff]))&0x1ff - 128
		if sx == -128 && (any || v.prevOverflow) {
			masked = true
			break
		}
		any = true
		row := y - sy
		if attr&0x1000 != 0 {
			row = hc*cellH - 1 - row
		}
		pal := uint8(((attr>>13)&3)<<4 | (attr>>8)&0x80)
		hflip := attr&0x800 != 0
		for c := 0; c < wc; c++ {
			if dots >= dotMax {
				masked = true
				break
			}
			dots += 8
			cc := c
			if hflip {
				cc = wc - 1 - c
			}
			tile := attr&0x7ff + cc*hc + row/cellH
			var ta int
			if im2 {
				ta = (tile & 0x3ff) << 6
			} else {
				ta = (tile & 0x7ff) << 5
			}
			ta += (row & (cellH - 1)) * 4
			for p := 0; p < 8; p++ {
				x := sx + c*8 + p
				if x < 0 || x >= W {
					continue
				}
				col := p
				if hflip {
					col = 7 - p
				}
				b := vram[(ta+col>>1)&0xffff]
				px := b >> 4
				if col&1 != 0 {
					px = b & 15
				}
				if px == 0 {
					continue
				}
				if buf[x]&15 != 0 {
					v.scol = true
					continue
				}
				buf[x] = pal | px
			}
		}
	}
	v.prevOverflow = overflow || dots >= dotMax
}

func (v *vdp) renderLine(line int, out []uint32) {
	W, im2 := v.width(), v.im2()
	oy := line
	if im2 {
		oy = line*2 + v.odd
	}
	o := oy * W
	bg := int(v.reg[7] & 0x3f)
	if !v.display() {
		c := v.pal[64+bg]
		for i := o; i < o+W; i++ {
			out[i] = c
		}
		v.prevOverflow = false
		return
	}
	y := oy
	vram := &v.vram
	// horizontal scroll
	hb := int(v.reg[13]&0x3f) << 10
	var ho int
	switch v.reg[11] & 3 {
	case 0:
		ho = 0
	case 1:
		ho = (line & 7) * 4
	case 2:
		ho = (line &^ 7) * 4
	default:
		ho = line * 4
	}
	ha := (hb + ho) & 0xffff
	hsA := (int(vram[ha])<<8 | int(vram[ha+1])) & 0x3ff
	hsB := (int(vram[ha+2])<<8 | int(vram[ha+3])) & 0x3ff
	A, B, S := &v.bufA, &v.bufB, &v.bufS
	v.drawPlane(B, int(v.reg[4]&7)<<13, hsB, 1, y, W)
	// window region
	wv, wdown := int(v.reg[18]&0x1f)*8, v.reg[18]&0x80 != 0
	wh, wright := int(v.reg[17]&0x1f)*16, v.reg[17]&0x80 != 0
	wx0, wx1 := 0, 0
	inV := line < wv
	if wdown {
		inV = line >= wv
	}
	switch {
	case inV:
		wx0, wx1 = 0, W
	case wright:
		wx0, wx1 = min(wh, W), W
	default:
		wx0, wx1 = 0, min(wh, W)
	}
	if wx0 > 0 || wx1 < W || wx1 == wx0 {
		v.drawPlane(A, int(v.reg[2]&0x38)<<10, hsA, 0, y, W)
	}
	if wx1 > wx0 {
		v.drawWindow(A, y, wx0, wx1)
	}
	v.drawSprites(S, y, W)
	// compose
	pal, sh := &v.pal, v.reg[12]&8 != 0
	for x := 0; x < W; x++ {
		a, b, s := A[x], B[x], S[x]
		var c int
		lv := 64
		// layer order: S hi, A hi, B hi, S lo, A lo, B lo, backdrop
		so, ao, bo := s&15, a&15, b&15
		var top int
		switch {
		case so != 0 && s&0x80 != 0:
			top = 1
		case ao != 0 && a&0x80 != 0:
			top = 2
		case bo != 0 && b&0x80 != 0:
			top = 3
		case so != 0:
			top = 1
		case ao != 0:
			top = 2
		case bo != 0:
			top = 3
		}
		pick := func() int {
			switch top {
			case 1:
				return int(s & 0x3f)
			case 2:
				return int(a & 0x3f)
			case 3:
				return int(b & 0x3f)
			}
			return bg
		}
		if !sh {
			c = pick()
		} else {
			lv = 0
			if a&0x80 != 0 || b&0x80 != 0 {
				lv = 64
			}
			if top == 1 {
				sc := int(s & 0x3f)
				if sc == 0x3e || sc == 0x3f {
					// shadow / highlight operator: show what's below the sprite
					under := -1
					switch {
					case ao != 0 && a&0x80 != 0:
						under = int(a)
					case bo != 0 && b&0x80 != 0:
						under = int(b)
					case ao != 0:
						under = int(a)
					case bo != 0:
						under = int(b)
					}
					if under < 0 {
						c = bg
					} else {
						c = under & 0x3f
					}
					if sc == 0x3e {
						if lv == 0 {
							lv = 64
						} else {
							lv = 128
						}
					} else {
						lv = 0
					}
				} else {
					c = sc
					if s&0x80 != 0 || sc&15 == 14 {
						lv = 64
					}
				}
			} else {
				c = pick()
			}
		}
		out[o+x] = pal[lv+c]
	}
}

// dmaCycles: the 68000 cycles a 68000-to-VDP DMA of n words holds the bus
// (Math.ceil(n * LINE / words / 7) in genesis.js).
func dmaCycles(n int, words float64) int64 {
	return int64(math.Ceil(float64(n*lineClk) / words / 7))
}
