package gba

// Video: modes 0-5, sprites, windows, blending, mosaic (PPU in web/emu/gba.js).

// color: 15-bit BGR to 0x00RRGGBB.
var color = func() (t [0x8000]uint32) {
	for i := range t {
		r, g, b := uint32(i&31), uint32(i>>5&31), uint32(i>>10&31)
		t[i] = (r<<3|r>>2)<<16 | (g<<3|g>>2)<<8 | (b<<3 | b>>2)
	}
	return
}()

var objSize = [4][4][2]int{ // [shape][size] -> w, h
	{{8, 8}, {16, 16}, {32, 32}, {64, 64}},
	{{16, 8}, {32, 8}, {32, 16}, {64, 32}},
	{{8, 16}, {8, 32}, {16, 32}, {32, 64}},
	{{8, 8}, {8, 8}, {8, 8}, {8, 8}},
}

// layer ids: 0-3 BGs, 4 OBJ, 5 backdrop
type ppu struct {
	g          *GBA
	bg         [4][240]uint32 // 0 = transparent, else 0x10000 | color15
	obj        [240]uint32    // 0 = none, else 0x10000 | color | prio<<17 | semi<<19
	objWin     [240]uint8
	winMask    [240]uint8
	affX, affY [2]int32 // internal reference points (BG2, BG3)
}

func sx28(v uint32) int32 { return int32(v<<4) >> 4 }

func (p *ppu) latchAffine() {
	g := p.g
	for k := 0; k < 2; k++ {
		b := 0x28 + k*0x10
		p.affX[k] = sx28(g.io32(b))
		p.affY[k] = sx28(g.io32(b + 4))
	}
}

// writeRef: a write to BGxX/BGxY resets the internal point.
func (p *ppu) writeRef(k int, isY bool) {
	g := p.g
	b := 0x28 + k*0x10
	if isY {
		p.affY[k] = sx28(g.io32(b + 4))
	} else {
		p.affX[k] = sx28(g.io32(b))
	}
}

func (p *ppu) stepAffine() {
	g := p.g
	for k := 0; k < 2; k++ {
		b := 0x20 + k*0x10
		p.affX[k] += int32(int16(g.io16(b + 2))) // PB
		p.affY[k] += int32(int16(g.io16(b + 6))) // PD
	}
}

func (p *ppu) renderLine(y int, out []uint32) {
	g := p.g
	dispcnt := int(g.io16(0))
	o := y * 240
	if dispcnt&0x80 != 0 {
		row := out[o : o+240]
		for i := range row {
			row[i] = 0xffffff
		}
		p.stepAffine()
		return
	}
	mode := dispcnt & 7
	mosaic := int(g.io16(0x4c))
	bgMosH, bgMosV := (mosaic&15)+1, (mosaic>>4&15)+1
	// which BGs exist in this mode
	enabled := dispcnt >> 8 & 0x1f
	switch {
	case mode == 1:
		enabled &= 0x17
	case mode == 2:
		enabled &= 0x1c
	case mode >= 3:
		enabled &= 0x14
	}
	for k := 0; k < 4; k++ {
		if enabled&(1<<k) == 0 {
			continue
		}
		cnt := int(g.io16(0x08 + k*2))
		my := y
		if cnt&0x40 != 0 {
			my = y - y%bgMosV
		}
		buf := &p.bg[k]
		switch {
		case mode == 0 || (mode == 1 && k < 2):
			p.textBG(k, cnt, my, buf)
		case mode <= 2:
			p.affineBG(k, cnt, buf)
		default:
			p.bitmapBG(mode, dispcnt, buf)
		}
		if cnt&0x40 != 0 && bgMosH > 1 {
			for x := 0; x < 240; x++ {
				buf[x] = buf[x-x%bgMosH]
			}
		}
	}
	p.stepAffine()
	if enabled&0x10 != 0 {
		p.sprites(y, dispcnt, mode)
	} else {
		p.obj = [240]uint32{}
		p.objWin = [240]uint8{}
	}
	// windows
	wm := &p.winMask
	if dispcnt&0xe000 != 0 {
		winin, winout := int(g.io16(0x48)), int(g.io16(0x4a))
		for x := range wm {
			wm[x] = uint8(winout & 0x3f)
		}
		if dispcnt&0x8000 != 0 {
			m := uint8(winout >> 8 & 0x3f)
			for x := 0; x < 240; x++ {
				if p.objWin[x] != 0 {
					wm[x] = m
				}
			}
		}
		for w := 1; w >= 0; w-- {
			if dispcnt&(0x2000<<w) == 0 {
				continue
			}
			h, v := int(g.io16(0x40+w*2)), int(g.io16(0x44+w*2))
			x1, x2, y1, y2 := h>>8, h&0xff, v>>8, v&0xff
			var inY bool
			if y1 <= y2 {
				inY = y >= y1 && y < y2
			} else {
				inY = y >= y1 || y < y2
			}
			if !inY {
				continue
			}
			m := uint8(winin >> (w * 8) & 0x3f)
			if x1 <= x2 {
				for x := x1; x < x2 && x < 240; x++ {
					wm[x] = m
				}
			} else {
				for x := 0; x < 240; x++ {
					if x >= x1 || x < x2 {
						wm[x] = m
					}
				}
			}
		}
	} else {
		for x := range wm {
			wm[x] = 0x3f
		}
	}
	// compose
	var prio [4]int
	for k := 0; k < 4; k++ {
		prio[k] = int(g.io16(0x08+k*2)) & 3
	}
	bldcnt := int(g.io16(0x50))
	bmode, alpha := bldcnt>>6&3, int(g.io16(0x52))
	bldy := min(16, int(g.io16(0x54))&31)
	eva, evb := min(16, alpha&31), min(16, alpha>>8&31)
	backdrop := uint32(g.pal16(0)) & 0x7fff
	// BG order by priority (stable by index)
	var order [4]int
	no := 0
	for pr := 0; pr < 4; pr++ {
		for k := 0; k < 4; k++ {
			if enabled&(1<<k) != 0 && prio[k] == pr {
				order[no] = k
				no++
			}
		}
	}
	row := out[o : o+240]
	for x := 0; x < 240; x++ {
		m := int(wm[x])
		top, topC, second, secC := 5, backdrop, 5, backdrop
		// find the top two layers
		ov := p.obj[x]
		objOk := ov != 0 && m&0x10 != 0
		objP := 9
		if objOk {
			objP = int(ov >> 17 & 3)
		}
		placedObj := false
		for i := 0; i < no; i++ {
			k := order[i]
			if m&(1<<k) == 0 {
				continue
			}
			v := p.bg[k][x]
			if v == 0 {
				continue
			}
			if !placedObj && objP <= prio[k] {
				placedObj = true
				if top == 5 {
					top, topC = 4, ov&0x7fff
				} else if second == 5 {
					second, secC = 4, ov&0x7fff
					break
				}
			}
			if top == 5 {
				top, topC = k, v&0x7fff
			} else {
				second, secC = k, v&0x7fff
				break
			}
		}
		if !placedObj && objOk {
			if top == 5 {
				top, topC = 4, ov&0x7fff
			} else if second == 5 {
				second, secC = 4, ov&0x7fff
			}
		}
		c := topC
		semi := top == 4 && ov&0x80000 != 0
		if semi && bldcnt&(0x100<<second) != 0 {
			c = blend(topC, secC, eva, evb)
		} else if m&0x20 != 0 && bmode != 0 && bldcnt&(1<<top) != 0 {
			switch bmode {
			case 1:
				if bldcnt&(0x100<<second) != 0 {
					c = blend(topC, secC, eva, evb)
				}
			case 2:
				c = brighten(topC, bldy)
			default:
				c = darken(topC, bldy)
			}
		}
		row[x] = color[c]
	}
}

func (p *ppu) textBG(k, cnt, y int, buf *[240]uint32) {
	g := p.g
	vram := &g.vram
	charBase, screenBase := (cnt>>2&3)*0x4000, (cnt>>8&31)*0x800
	c256, size := cnt&0x80 != 0, cnt>>14&3
	hofs, vofs := int(g.io16(0x10+k*4))&0x1ff, int(g.io16(0x12+k*4))&0x1ff
	wMask, hMask := 255, 255
	if size&1 != 0 {
		wMask = 511
	}
	if size&2 != 0 {
		hMask = 511
	}
	py := (y + vofs) & hMask
	ty, fy := py>>3, py&7
	lastTx, rowAddr, palBase, hflip := -1, 0, 0, 0
	for x := 0; x < 240; x++ {
		px := (x + hofs) & wMask
		tx := px >> 3
		if tx != lastTx {
			lastTx = tx
			block := 0
			switch size {
			case 1:
				block = tx >> 5
			case 2:
				block = ty >> 5
			case 3:
				block = tx>>5 + (ty>>5)*2
			}
			ea := screenBase + block*0x800 + ((ty&31)*32+(tx&31))*2
			entry := int(vram[ea&0xffff]) | int(vram[(ea+1)&0xffff])<<8
			row := fy
			if entry&0x800 != 0 {
				row = 7 - fy
			}
			hflip = 0
			if entry&0x400 != 0 {
				hflip = 7
			}
			tile := entry & 0x3ff
			if c256 {
				rowAddr = charBase + tile*64 + row*8
				palBase = 0
			} else {
				rowAddr = charBase + tile*32 + row*4
				palBase = (entry >> 12) * 16
			}
		}
		fx := (px & 7) ^ hflip
		if rowAddr >= 0x10000 {
			buf[x] = 0
			continue
		}
		var idx int
		if c256 {
			idx = int(vram[rowAddr+fx])
		} else {
			b := int(vram[rowAddr+fx>>1])
			if fx&1 != 0 {
				idx = b >> 4
			} else {
				idx = b & 15
			}
		}
		if idx != 0 {
			buf[x] = 0x10000 | uint32(g.pal16(palBase+idx))&0x7fff
		} else {
			buf[x] = 0
		}
	}
}

func (p *ppu) affineBG(k, cnt int, buf *[240]uint32) {
	g := p.g
	vram := &g.vram
	a := k - 2
	b := 0x20 + a*0x10
	pa, pc := int32(int16(g.io16(b))), int32(int16(g.io16(b+4)))
	charBase, screenBase := (cnt>>2&3)*0x4000, (cnt>>8&31)*0x800
	size := int32(128) << (cnt >> 14 & 3)
	wrap := cnt&0x2000 != 0
	tiles := int(size >> 3)
	tx, ty := p.affX[a], p.affY[a]
	for x := 0; x < 240; x, tx, ty = x+1, tx+pa, ty+pc {
		px, py := tx>>8, ty>>8
		if wrap {
			px &= size - 1
			py &= size - 1
		} else if px < 0 || py < 0 || px >= size || py >= size {
			buf[x] = 0
			continue
		}
		tile := int(vram[(screenBase+int(py>>3)*tiles+int(px>>3))&0xffff])
		idx := int(vram[(charBase+tile*64+int(py&7)*8+int(px&7))&0xffff])
		if idx != 0 {
			buf[x] = 0x10000 | uint32(g.pal16(idx))&0x7fff
		} else {
			buf[x] = 0
		}
	}
}

func (p *ppu) bitmapBG(mode, dispcnt int, buf *[240]uint32) {
	g := p.g
	vram := &g.vram
	pa, pc := int32(int16(g.io16(0x20))), int32(int16(g.io16(0x24)))
	tx, ty := p.affX[0], p.affY[0]
	page := 0
	if dispcnt&0x10 != 0 && mode != 3 {
		page = 0xa000
	}
	w, h := int32(240), int32(160)
	if mode == 5 {
		w, h = 160, 128
	}
	for x := 0; x < 240; x, tx, ty = x+1, tx+pa, ty+pc {
		px, py := tx>>8, ty>>8
		if px < 0 || py < 0 || px >= w || py >= h {
			buf[x] = 0
			continue
		}
		if mode == 4 {
			idx := int(vram[page+int(py)*240+int(px)])
			if idx != 0 {
				buf[x] = 0x10000 | uint32(g.pal16(idx))&0x7fff
			} else {
				buf[x] = 0
			}
		} else {
			a := page + int(py*w+px)*2
			buf[x] = 0x10000 | (uint32(vram[a])|uint32(vram[a+1])<<8)&0x7fff
		}
	}
}

func (p *ppu) sprites(y, dispcnt, mode int) {
	g := p.g
	vram := &g.vram
	obj, win := &p.obj, &p.objWin
	*obj = [240]uint32{}
	*win = [240]uint8{}
	map1d := dispcnt&0x40 != 0
	mosaic := int(g.io16(0x4c))
	mosH, mosV := (mosaic>>8&15)+1, (mosaic>>12&15)+1
	minTile := 0
	if mode >= 3 {
		minTile = 512
	}
	for i := 0; i < 128; i++ {
		a0, a1, a2 := g.oam16(i*4), g.oam16(i*4+1), g.oam16(i*4+2)
		affine := a0&0x100 != 0
		if !affine && a0&0x200 != 0 {
			continue // hidden
		}
		shape, sz := a0>>14, a1>>14
		w, h := objSize[shape][sz][0], objSize[shape][sz][1]
		dbl := affine && a0&0x200 != 0
		bw, bh := w, h
		if dbl {
			bw, bh = w*2, h*2
		}
		sy := a0 & 0xff
		if sy >= 160 {
			sy -= 256
		}
		line := y - sy
		if line < 0 || line >= bh {
			continue
		}
		sx := a1 & 0x1ff
		if sx >= 240 {
			sx -= 512
		}
		omode := a0 >> 10 & 3
		if omode == 3 {
			continue
		}
		c256 := a0&0x2000 != 0
		tile, prio, palBank := a2&0x3ff, a2>>10&3, (a2>>12)*16
		mos := a0&0x1000 != 0
		if mos {
			line -= line % mosV
		}
		rowTiles := 32
		if map1d {
			if c256 {
				rowTiles = w / 4
			} else {
				rowTiles = w / 8
			}
		}
		var flags uint32 = uint32(prio) << 17
		if omode == 1 {
			flags |= 0x80000
		}
		pix := func(tx, ty int) uint32 {
			t := tile
			if c256 {
				t = (t &^ 1) + (ty>>3)*rowTiles + (tx>>3)*2
			} else {
				t = t + (ty>>3)*rowTiles + tx>>3
			}
			t &= 0x3ff
			if t < minTile {
				return 0
			}
			if c256 {
				idx := int(vram[0x10000+t*32+(ty&7)*8+(tx&7)])
				if idx != 0 {
					return 0x10000 | uint32(g.pal16(256+idx))&0x7fff
				}
				return 0
			}
			b := int(vram[0x10000+t*32+(ty&7)*4+(tx&7)>>1])
			idx := b & 15
			if tx&1 != 0 {
				idx = b >> 4
			}
			if idx != 0 {
				return 0x10000 | uint32(g.pal16(256+palBank+idx))&0x7fff
			}
			return 0
		}
		put := func(x int, v uint32) {
			if omode == 2 {
				win[x] = 1
				return
			}
			if cur := obj[x]; cur != 0 && int(cur>>17&3) <= prio {
				return
			}
			obj[x] = v | flags
		}
		if affine {
			pp := (a1 >> 9 & 31) * 16
			pa, pb := int(int16(g.oam16(pp+3))), int(int16(g.oam16(pp+7)))
			pc, pd := int(int16(g.oam16(pp+11))), int(int16(g.oam16(pp+15)))
			cx, cy := bw>>1, bh>>1
			dy := line - cy
			for ix := 0; ix < bw; ix++ {
				x := sx + ix
				if x < 0 || x >= 240 {
					continue
				}
				dx := ix - cx
				tx, ty := (pa*dx+pb*dy)>>8+w>>1, (pc*dx+pd*dy)>>8+h>>1
				if tx < 0 || ty < 0 || tx >= w || ty >= h {
					continue
				}
				if v := pix(tx, ty); v != 0 {
					put(x, v)
				}
			}
		} else {
			hf, vf := a1&0x1000 != 0, a1&0x2000 != 0
			ty := line
			if vf {
				ty = h - 1 - line
			}
			for ix := 0; ix < w; ix++ {
				x := sx + ix
				if x < 0 || x >= 240 {
					continue
				}
				mx := ix
				if mos {
					mx -= x % mosH
				}
				tx := mx
				if hf {
					tx = w - 1 - mx
				}
				if v := pix(tx, ty); v != 0 {
					put(x, v)
				}
			}
		}
	}
}

func blend(a, b uint32, eva, evb int) uint32 {
	r := (int(a&31)*eva + int(b&31)*evb) >> 4
	gg := (int(a>>5&31)*eva + int(b>>5&31)*evb) >> 4
	bb := (int(a>>10&31)*eva + int(b>>10&31)*evb) >> 4
	r, gg, bb = min(r, 31), min(gg, 31), min(bb, 31)
	return uint32(r | gg<<5 | bb<<10)
}

func brighten(a uint32, e int) uint32 {
	r, g, b := int(a&31), int(a>>5&31), int(a>>10&31)
	return uint32((r + ((31-r)*e)>>4) | (g+((31-g)*e)>>4)<<5 | (b+((31-b)*e)>>4)<<10)
}

func darken(a uint32, e int) uint32 {
	r, g, b := int(a&31), int(a>>5&31), int(a>>10&31)
	return uint32((r - (r*e)>>4) | (g-(g*e)>>4)<<5 | (b-(b*e)>>4)<<10)
}
