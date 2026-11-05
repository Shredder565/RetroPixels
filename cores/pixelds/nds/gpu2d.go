package nds

import "encoding/binary"

// engine2D is one of the DS's two 2D graphics engines (A, which can also
// show the 3D engine's picture as BG0, and B). Each draws one line at a time
// into out.
type engine2D struct {
	m   *Machine
	isA bool

	dispcnt          uint32
	bgcnt            [4]uint16
	hofs, vofs       [4]uint16
	pa, pb, pc, pd   [2]int16
	refX, refY       [2]int32 // registers
	curX, curY       [2]int32 // internal reference points
	win0h, win1h     uint16
	win0v, win1v     uint16
	winin, winout    uint16
	mosaic           uint16
	bldcnt, bldalpha uint16
	bldy             uint16
	masterBright     uint16

	out [256]uint32

	// line buffers
	bg      [4][256]uint32 // bit 31: opaque; low 15 bits colour
	alpha3d [256]uint8
	obj     [256]uint32 // bit 31 opaque, bit 30 semi-transparent, bits 16-19 bitmap alpha+1
	objPrio [256]uint8
	objWin  [256]bool
}

const opaque = 1 << 31

func newEngine2D(m *Machine, isA bool) *engine2D { return &engine2D{m: m, isA: isA} }

func (e *engine2D) bgVRAM() region {
	if e.isA {
		return e.m.vram.abg
	}
	return e.m.vram.bbg
}

func (e *engine2D) objVRAM() region {
	if e.isA {
		return e.m.vram.aobj
	}
	return e.m.vram.bobj
}

func (e *engine2D) palBase() int {
	if e.isA {
		return 0
	}
	return 0x400
}

func (e *engine2D) palColor(off int) uint32 {
	return uint32(binary.LittleEndian.Uint16(e.m.pal[e.palBase()+off:])) & 0x7fff
}

func (e *engine2D) read16(off uint32) uint16 {
	switch off {
	case 0x00:
		return uint16(e.dispcnt)
	case 0x02:
		return uint16(e.dispcnt >> 16)
	case 0x08, 0x0a, 0x0c, 0x0e:
		return e.bgcnt[(off-8)/2]
	case 0x48:
		return e.winin
	case 0x4a:
		return e.winout
	case 0x50:
		return e.bldcnt
	case 0x52:
		return e.bldalpha
	case 0x60, 0x62:
		if e.isA {
			return uint16(e.m.gpu3d.read32(0x04000060) >> (off & 2 * 8))
		}
	case 0x6c:
		return e.masterBright
	}
	return 0
}

func (e *engine2D) write16(off uint32, v uint16) {
	switch {
	case off == 0x00:
		e.dispcnt = e.dispcnt&0xffff0000 | uint32(v)
	case off == 0x02:
		e.dispcnt = e.dispcnt&0xffff | uint32(v)<<16
	case off >= 0x08 && off < 0x10:
		e.bgcnt[(off-8)/2] = v
	case off >= 0x10 && off < 0x20:
		i := (off - 0x10) / 4
		if off&2 == 0 {
			e.hofs[i] = v & 0x1ff
		} else {
			e.vofs[i] = v & 0x1ff
		}
	case off >= 0x20 && off < 0x40:
		i := (off - 0x20) / 0x10
		switch (off - 0x20) & 0xf {
		case 0:
			e.pa[i] = int16(v)
		case 2:
			e.pb[i] = int16(v)
		case 4:
			e.pc[i] = int16(v)
		case 6:
			e.pd[i] = int16(v)
		case 8:
			e.refX[i] = signExt28(uint32(e.refX[i])&0xffff0000 | uint32(v))
			e.curX[i] = e.refX[i]
		case 0xa:
			e.refX[i] = signExt28(uint32(e.refX[i])&0xffff | uint32(v)<<16)
			e.curX[i] = e.refX[i]
		case 0xc:
			e.refY[i] = signExt28(uint32(e.refY[i])&0xffff0000 | uint32(v))
			e.curY[i] = e.refY[i]
		case 0xe:
			e.refY[i] = signExt28(uint32(e.refY[i])&0xffff | uint32(v)<<16)
			e.curY[i] = e.refY[i]
		}
	case off == 0x40:
		e.win0h = v
	case off == 0x42:
		e.win1h = v
	case off == 0x44:
		e.win0v = v
	case off == 0x46:
		e.win1v = v
	case off == 0x48:
		e.winin = v & 0x3f3f
	case off == 0x4a:
		e.winout = v & 0x3f3f
	case off == 0x4c:
		e.mosaic = v
	case off == 0x50:
		e.bldcnt = v & 0x3fff
	case off == 0x52:
		e.bldalpha = v & 0x1f1f
	case off == 0x54:
		e.bldy = v & 0x1f
	case off >= 0x60 && off < 0x64:
		if e.isA {
			old := e.m.gpu3d.read32(0x04000060)
			sh := off & 2 * 8
			e.m.gpu3d.write32(0x04000060, old&^(0xffff<<sh)|uint32(v)<<sh)
		}
	case off == 0x6c:
		e.masterBright = v & 0xc01f
	}
}

func signExt28(v uint32) int32 { return int32(v<<4) >> 4 }

func (e *engine2D) frameStart() {
	e.curX, e.curY = e.refX, e.refY
}

func rgb(c uint32) uint32 {
	r, g, b := c&31, c>>5&31, c>>10&31
	return (r<<3|r>>2)<<16 | (g<<3|g>>2)<<8 | (b<<3 | b>>2)
}

// renderLine draws line y into out (0x00RRGGBB).
func (e *engine2D) renderLine(y int) {
	mode := e.dispcnt >> 16 & 3
	if !e.isA {
		mode &= 1
	}
	if e.dispcnt&0x80 != 0 { // forced blank
		mode = 0
	}
	line := e.out[:]
	switch mode {
	case 0:
		for i := range line {
			line[i] = 0xffffff
		}
		e.advanceAffine()
		return
	case 2: // a VRAM bank shown directly
		bank := e.m.vram.banks[e.dispcnt>>18&3]
		for x := 0; x < 256; x++ {
			line[x] = rgb(uint32(binary.LittleEndian.Uint16(bank[(y*256+x)*2:])))
		}
		e.composeCapture(y)
		e.brightness()
		e.advanceAffine()
		return
	case 3: // main memory display FIFO: not used by games
		for i := range line {
			line[i] = 0
		}
		return
	}
	e.renderNormal(y)
	e.brightness()
}

func (e *engine2D) advanceAffine() {
	for i := 0; i < 2; i++ {
		e.curX[i] += int32(e.pb[i])
		e.curY[i] += int32(e.pd[i])
	}
}

// composeCapture: display capture isn't done yet.
func (e *engine2D) composeCapture(y int) {}

func (e *engine2D) brightness() {
	mb := e.masterBright
	f := uint32(mb & 0x1f)
	if f > 16 {
		f = 16
	}
	switch mb >> 14 {
	case 1:
		for i, c := range e.out {
			r, g, b := c>>16&0xff, c>>8&0xff, c&0xff
			r += (255 - r) * f / 16
			g += (255 - g) * f / 16
			b += (255 - b) * f / 16
			e.out[i] = r<<16 | g<<8 | b
		}
	case 2:
		for i, c := range e.out {
			r, g, b := c>>16&0xff, c>>8&0xff, c&0xff
			r -= r * f / 16
			g -= g * f / 16
			b -= b * f / 16
			e.out[i] = r<<16 | g<<8 | b
		}
	}
}

// ---------------- backgrounds ----------------

func (e *engine2D) charBase(i int) uint32 {
	b := uint32(e.bgcnt[i]>>2&15) * 0x4000
	if e.isA {
		b += (e.dispcnt >> 24 & 7) * 0x10000
	}
	return b
}

func (e *engine2D) screenBase(i int) uint32 {
	b := uint32(e.bgcnt[i]>>8&31) * 0x800
	if e.isA {
		b += (e.dispcnt >> 27 & 7) * 0x10000
	}
	return b
}

func (e *engine2D) extPal(slot int, pal, idx uint32) uint32 {
	var r region
	if e.isA {
		r = e.m.vram.abgExt
	} else {
		r = e.m.vram.bbgExt
	}
	return uint32(r.read16(uint32(slot)*0x2000+pal*512+idx*2)) & 0x7fff
}

func (e *engine2D) renderText(i, y int) {
	cnt := e.bgcnt[i]
	vr := e.bgVRAM()
	w, h := uint32(256), uint32(256)
	switch cnt >> 14 {
	case 1:
		w = 512
	case 2:
		h = 512
	case 3:
		w, h = 512, 512
	}
	cb, sb := e.charBase(i), e.screenBase(i)
	c256 := cnt&0x80 != 0
	extOn := c256 && e.dispcnt&(1<<30) != 0
	slot := i
	if i < 2 && cnt&(1<<13) != 0 {
		slot += 2
	}
	py := (uint32(y) + uint32(e.vofs[i])) & (h - 1)
	out := &e.bg[i]
	hofs := uint32(e.hofs[i])
	for x := 0; x < 256; {
		px := (uint32(x) + hofs) & (w - 1)
		blk := px>>8 + (py>>8)*(w>>8)
		ma := sb + blk*0x800 + (py&255>>3)*64 + (px&255>>3)*2
		ent := uint32(vr.read16(ma))
		tile := ent & 0x3ff
		ty := py & 7
		if ent&0x800 != 0 {
			ty = 7 - ty
		}
		pal := ent >> 12
		// draw the rest of this tile's row
		for tx := px & 7; tx < 8 && x < 256; tx++ {
			cx := tx
			if ent&0x400 != 0 {
				cx = 7 - tx
			}
			var idx uint32
			if c256 {
				idx = uint32(vr.read8(cb + tile*64 + ty*8 + cx))
			} else {
				b := uint32(vr.read8(cb + tile*32 + ty*4 + cx/2))
				idx = b >> (cx & 1 * 4) & 15
			}
			if idx == 0 {
				out[x] = 0
			} else if c256 {
				if extOn {
					out[x] = opaque | e.extPal(slot, pal, idx)
				} else {
					out[x] = opaque | e.palColor(int(idx)*2)
				}
			} else {
				out[x] = opaque | e.palColor(int(pal*16+idx)*2)
			}
			x++
		}
	}
}

// renderAffine draws a rotating/scaling BG (kind: 0 affine, 1 extended with
// 16-bit map entries, 2 256-colour bitmap, 3 direct colour bitmap, 4 large
// bitmap).
func (e *engine2D) renderAffine(i, kind int) {
	cnt := e.bgcnt[i]
	vr := e.bgVRAM()
	j := i - 2
	var w, h int32
	sz := cnt >> 14
	switch kind {
	case 0, 1:
		w = 128 << sz
		h = w
	case 2, 3:
		w, h = [4]int32{128, 256, 512, 512}[sz], [4]int32{128, 256, 256, 512}[sz]
	case 4:
		w, h = [4]int32{512, 1024, 512, 1024}[sz], [4]int32{1024, 512, 1024, 512}[sz]
	}
	wrap := cnt&(1<<13) != 0
	cx, cy := e.curX[j], e.curY[j]
	pa, pc := int32(e.pa[j]), int32(e.pc[j])
	cb, sb := e.charBase(i), e.screenBase(i)
	extOn := e.dispcnt&(1<<30) != 0
	out := &e.bg[i]
	bmpBase := uint32(cnt>>8&31) * 0x4000
	for x := 0; x < 256; x++ {
		px, py := cx>>8, cy>>8
		cx += pa
		cy += pc
		if wrap {
			px &= w - 1
			py &= h - 1
		} else if px < 0 || py < 0 || px >= w || py >= h {
			out[x] = 0
			continue
		}
		switch kind {
		case 0:
			tile := uint32(vr.read8(sb + uint32(py>>3*(w>>3)+px>>3)))
			idx := uint32(vr.read8(cb + tile*64 + uint32(py&7)*8 + uint32(px&7)))
			if idx == 0 {
				out[x] = 0
			} else {
				out[x] = opaque | e.palColor(int(idx)*2)
			}
		case 1:
			ent := uint32(vr.read16(sb + uint32(py>>3*(w>>3)+px>>3)*2))
			tx, ty := uint32(px&7), uint32(py&7)
			if ent&0x400 != 0 {
				tx = 7 - tx
			}
			if ent&0x800 != 0 {
				ty = 7 - ty
			}
			idx := uint32(vr.read8(cb + (ent&0x3ff)*64 + ty*8 + tx))
			if idx == 0 {
				out[x] = 0
			} else if extOn {
				out[x] = opaque | e.extPal(i, ent>>12, idx)
			} else {
				out[x] = opaque | e.palColor(int(idx)*2)
			}
		case 2:
			idx := uint32(vr.read8(bmpBase + uint32(py*w+px)))
			if idx == 0 {
				out[x] = 0
			} else {
				out[x] = opaque | e.palColor(int(idx)*2)
			}
		case 3:
			c := uint32(vr.read16(bmpBase + uint32(py*w+px)*2))
			if c&0x8000 == 0 {
				out[x] = 0
			} else {
				out[x] = opaque | c&0x7fff
			}
		case 4:
			idx := uint32(vr.read8(uint32(py*w + px)))
			if idx == 0 {
				out[x] = 0
			} else {
				out[x] = opaque | e.palColor(int(idx)*2)
			}
		}
	}
}

// ---------------- sprites ----------------

var objW = [3][4]int{{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}}
var objH = [3][4]int{{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}}

func (e *engine2D) renderObjs(y int) {
	for i := range e.obj {
		e.obj[i] = 0
		e.objPrio[i] = 4
		e.objWin[i] = false
	}
	oam := e.m.oam[:1024]
	if !e.isA {
		oam = e.m.oam[1024:]
	}
	vr := e.objVRAM()
	palBase := e.palBase() + 0x200
	extOn := e.dispcnt&(1<<31) != 0
	var extR region
	if e.isA {
		extR = e.m.vram.aobjExt
	} else {
		extR = e.m.vram.bobjExt
	}
	for n := 0; n < 128; n++ {
		a0 := uint32(binary.LittleEndian.Uint16(oam[n*8:]))
		a1 := uint32(binary.LittleEndian.Uint16(oam[n*8+2:]))
		a2 := uint32(binary.LittleEndian.Uint16(oam[n*8+4:]))
		affine := a0&0x100 != 0
		if !affine && a0&0x200 != 0 {
			continue // hidden
		}
		shape := a0 >> 14
		if shape == 3 {
			continue
		}
		size := a1 >> 14
		w, h := objW[shape][size], objH[shape][size]
		bw, bh := w, h // bounding box
		if affine && a0&0x200 != 0 {
			bw, bh = w*2, h*2
		}
		oy := int(a0 & 0xff)
		if oy >= 192 {
			oy -= 256
		}
		dy := y - oy
		if dy < 0 || dy >= bh {
			continue
		}
		ox := int(a1 & 0x1ff)
		if ox >= 256 {
			ox -= 512
		}
		mode := a0 >> 10 & 3
		prio := uint8(a2 >> 10 & 3)
		c256 := a0&0x2000 != 0
		tile := a2 & 0x3ff
		palN := a2 >> 12
		var pa, pb, pc, pd int32 = 256, 0, 0, 256
		if affine {
			g := int(a1>>9&31) * 32
			pa = int32(int16(binary.LittleEndian.Uint16(oam[g+6:])))
			pb = int32(int16(binary.LittleEndian.Uint16(oam[g+14:])))
			pc = int32(int16(binary.LittleEndian.Uint16(oam[g+22:])))
			pd = int32(int16(binary.LittleEndian.Uint16(oam[g+30:])))
		}
		for bx := 0; bx < bw; bx++ {
			sx := ox + bx
			if sx < 0 || sx >= 256 {
				continue
			}
			var tx, ty int
			if affine {
				rx, ry := int32(bx-bw/2), int32(dy-bh/2)
				tx = int((pa*rx+pb*ry)>>8) + w/2
				ty = int((pc*rx+pd*ry)>>8) + h/2
				if tx < 0 || ty < 0 || tx >= w || ty >= h {
					continue
				}
			} else {
				tx, ty = bx, dy
				if a1&0x1000 != 0 {
					tx = w - 1 - tx
				}
				if a1&0x2000 != 0 {
					ty = h - 1 - ty
				}
			}
			var col uint32
			var ok bool
			if mode == 3 { // bitmap
				var addr uint32
				if e.dispcnt&0x40 != 0 {
					addr = tile*(128<<(e.dispcnt>>22&1)) + uint32(ty*w+tx)*2
				} else if e.dispcnt&0x20 != 0 {
					addr = (tile&0x1f)*0x10 + (tile&0x3e0)*0x80 + uint32(ty*512+tx*2)
				} else {
					addr = (tile&0x0f)*0x10 + (tile&0x3f0)*0x80 + uint32(ty*256+tx*2)
				}
				c := uint32(vr.read16(addr))
				if c&0x8000 == 0 {
					continue
				}
				if palN == 0 {
					continue // alpha 0: invisible
				}
				col, ok = c&0x7fff|(palN+1)<<16, true
			} else {
				var idx uint32
				if c256 {
					var addr uint32
					if e.dispcnt&0x10 != 0 {
						addr = tile*(32<<(e.dispcnt>>20&3)) + uint32((ty>>3)*(w>>3)+(tx>>3))*64
					} else {
						addr = (tile&^1)*32 + uint32((ty>>3)*1024+(tx>>3)*64)
					}
					idx = uint32(vr.read8(addr + uint32(ty&7)*8 + uint32(tx&7)))
				} else {
					var addr uint32
					if e.dispcnt&0x10 != 0 {
						addr = tile*(32<<(e.dispcnt>>20&3)) + uint32((ty>>3)*(w>>3)+(tx>>3))*32
					} else {
						addr = tile*32 + uint32((ty>>3)*1024+(tx>>3)*32)
					}
					b := uint32(vr.read8(addr + uint32(ty&7)*4 + uint32(tx&7)/2))
					idx = b >> (uint32(tx) & 1 * 4) & 15
				}
				if idx == 0 {
					continue
				}
				if mode == 2 {
					e.objWin[sx] = true
					continue
				}
				switch {
				case c256 && extOn:
					col = uint32(extR.read16(palN*512+idx*2)) & 0x7fff
				case c256:
					col = uint32(binary.LittleEndian.Uint16(e.m.pal[palBase+int(idx)*2:])) & 0x7fff
				default:
					col = uint32(binary.LittleEndian.Uint16(e.m.pal[palBase+int(palN*16+idx)*2:])) & 0x7fff
				}
				ok = true
			}
			if !ok {
				continue
			}
			if e.obj[sx]&opaque == 0 || prio < e.objPrio[sx] {
				v := opaque | col
				if mode == 1 {
					v |= 1 << 30
				}
				e.obj[sx] = v
				e.objPrio[sx] = prio
			}
		}
	}
}

// ---------------- putting it together ----------------

func inRange(v, lo, hi int) bool {
	if lo <= hi {
		return v >= lo && v < hi
	}
	return v >= lo || v < hi
}

func (e *engine2D) renderNormal(y int) {
	dc := e.dispcnt
	mode := int(dc & 7)
	enabled := dc >> 8 & 31
	is3D := e.isA && dc&8 != 0
	for i := 0; i < 4; i++ {
		if enabled&(1<<i) == 0 {
			continue
		}
		switch {
		case i == 0 && is3D:
			e.m.gpu3d.line(y, &e.bg[0], &e.alpha3d)
		case i < 2:
			e.renderText(i, y)
		case i == 2:
			switch mode {
			case 0, 1, 3:
				e.renderText(2, y)
			case 2, 4:
				e.renderAffine(2, 0)
			case 5:
				e.renderAffine(2, e.extKind(2))
			case 6:
				e.renderAffine(2, 4)
			default:
				enabled &^= 4
			}
		case i == 3:
			switch mode {
			case 0:
				e.renderText(3, y)
			case 1, 2:
				e.renderAffine(3, 0)
			case 3, 4, 5:
				e.renderAffine(3, e.extKind(3))
			default:
				enabled &^= 8
			}
		}
	}
	e.advanceAffine()
	if enabled&16 != 0 {
		e.renderObjs(y)
	} else {
		for i := range e.obj {
			e.obj[i] = 0
			e.objWin[i] = false
		}
	}

	// BGs in drawing order: priority, then number
	var order [4]int
	n := 0
	for p := uint16(0); p < 4; p++ {
		for i := 0; i < 4; i++ {
			if enabled&(1<<i) != 0 && e.bgcnt[i]&3 == p {
				order[n] = i
				n++
			}
		}
	}

	winOn := dc>>13&7 != 0
	w0x1, w0x2 := int(e.win0h>>8), int(e.win0h&0xff)
	w1x1, w1x2 := int(e.win1h>>8), int(e.win1h&0xff)
	in0 := dc&(1<<13) != 0 && inRange(y, int(e.win0v>>8), int(e.win0v&0xff))
	in1 := dc&(1<<14) != 0 && inRange(y, int(e.win1v>>8), int(e.win1v&0xff))
	objWinOn := dc&(1<<15) != 0

	backdrop := e.palColor(0)
	eff := e.bldcnt >> 6 & 3
	t1, t2 := uint32(e.bldcnt&63), uint32(e.bldcnt>>8&63)
	eva, evb := uint32(e.bldalpha&31), uint32(e.bldalpha>>8&31)
	if eva > 16 {
		eva = 16
	}
	if evb > 16 {
		evb = 16
	}
	ey := uint32(e.bldy)
	if ey > 16 {
		ey = 16
	}

	for x := 0; x < 256; x++ {
		mask := uint32(0x3f)
		if winOn {
			switch {
			case in0 && inRange(x, w0x1, w0x2):
				mask = uint32(e.winin & 0x3f)
			case in1 && inRange(x, w1x1, w1x2):
				mask = uint32(e.winin >> 8 & 0x3f)
			case objWinOn && e.objWin[x]:
				mask = uint32(e.winout >> 8 & 0x3f)
			default:
				mask = uint32(e.winout & 0x3f)
			}
		}
		// the top two layers
		top, second := uint32(5), uint32(5)
		topC, secC := backdrop, backdrop
		found := 0
		objV := e.obj[x]
		objP := e.objPrio[x]
		objOK := objV&opaque != 0 && mask&16 != 0
		k := 0
		for p := uint8(0); p < 4 && found < 2; p++ {
			if objOK && objP == p {
				if found == 0 {
					top, topC = 4, objV&0x7fff
				} else {
					second, secC = 4, objV&0x7fff
				}
				found++
				if found == 2 {
					break
				}
			}
			for ; k < n && e.bgcnt[order[k]]&3 == uint16(p); k++ {
				i := order[k]
				v := e.bg[i][x]
				if v&opaque == 0 || mask&(1<<i) == 0 {
					continue
				}
				if found == 0 {
					top, topC = uint32(i), v&0x7fff
				} else if found == 1 {
					second, secC = uint32(i), v&0x7fff
				}
				found++
				if found == 2 {
					break
				}
			}
		}
		c := topC
		switch {
		case top == 4 && objV&(1<<30) != 0 && t2&(1<<second) != 0:
			c = blend(topC, secC, eva, evb)
		case top == 4 && objV>>16&31 != 0 && t2&(1<<second) != 0:
			a := objV>>16&31 - 1
			c = blend(topC, secC, a+1, 16-a)
		case top == 0 && is3D && e.alpha3d[x] < 31 && t2&(1<<second) != 0:
			a := uint32(e.alpha3d[x])
			c = blend3D(topC, secC, a)
		case mask&32 != 0 && t1&(1<<top) != 0:
			switch eff {
			case 1:
				if t2&(1<<second) != 0 {
					c = blend(topC, secC, eva, evb)
				}
			case 2:
				c = brighten(topC, ey)
			case 3:
				c = darken(topC, ey)
			}
		}
		e.out[x] = rgb(c)
	}
}

func (e *engine2D) extKind(i int) int {
	cnt := e.bgcnt[i]
	if cnt&0x80 == 0 {
		return 1
	}
	if cnt&4 == 0 {
		return 2
	}
	return 3
}

func blend(a, b, eva, evb uint32) uint32 {
	r := min32((a&31*eva+b&31*evb)>>4, 31)
	g := min32((a>>5&31*eva+b>>5&31*evb)>>4, 31)
	bl := min32((a>>10&31*eva+b>>10&31*evb)>>4, 31)
	return r | g<<5 | bl<<10
}

func blend3D(a, b, alpha uint32) uint32 {
	r := (a&31*(alpha+1) + b&31*(31-alpha)) >> 5
	g := (a>>5&31*(alpha+1) + b>>5&31*(31-alpha)) >> 5
	bl := (a>>10&31*(alpha+1) + b>>10&31*(31-alpha)) >> 5
	return r | g<<5 | bl<<10
}

func brighten(a, ey uint32) uint32 {
	r, g, b := a&31, a>>5&31, a>>10&31
	r += (31 - r) * ey >> 4
	g += (31 - g) * ey >> 4
	b += (31 - b) * ey >> 4
	return r | g<<5 | b<<10
}

func darken(a, ey uint32) uint32 {
	r, g, b := a&31, a>>5&31, a>>10&31
	r -= r * ey >> 4
	g -= g * ey >> 4
	b -= b * ey >> 4
	return r | g<<5 | b<<10
}

func min32(a, b uint32) uint32 {
	if a < b {
		return a
	}
	return b
}
