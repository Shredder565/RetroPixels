package gb

import (
	"math"
	"sort"
)

// The PPU (the video part of the GB class in gb.js). Colours are 0x00RRGGBB.

func rgb(r, g, b int) uint32 { return uint32(r)<<16 | uint32(g)<<8 | uint32(b) }

// dmgGreen: the original Game Boy's shades (the web player's default palette).
var dmgGreen = [4]uint32{rgb(0xe0, 0xf0, 0xc8), rgb(0x98, 0xc0, 0x78), rgb(0x48, 0x70, 0x50), rgb(0x10, 0x24, 0x20)}

// cgbRGB: Game Boy Color colours. Raw 15-bit values look harsher on a modern
// screen than on the console's LCD, whose colours bleed into each other a
// little. RetroPixels' correction: decode each 5-bit channel to linear light,
// let a little of the other two channels into it, and encode it again.
var cgbRGB = func() []uint32 {
	mix := [3][3]float64{{0.84, 0.12, 0.04}, {0.06, 0.80, 0.14}, {0.10, 0.12, 0.78}}
	lin := func(x int) float64 { return math.Pow(float64(x)/31, 2.2) }
	enc := func(y float64) int { return int(jsRound(255 * math.Pow(math.Min(1, y), 1/2.2))) }
	out := make([]uint32, 32768)
	for v := range out {
		c := [3]float64{lin(v & 31), lin((v >> 5) & 31), lin((v >> 10) & 31)}
		var o [3]int
		for i, m := range mix {
			o[i] = enc(m[0]*c[0] + m[1]*c[1] + m[2]*c[2])
		}
		out[v] = rgb(o[0], o[1], o[2])
	}
	return out
}()

// jsRound is JavaScript's Math.round (halves round up).
func jsRound(x float64) float64 {
	r := math.Floor(x)
	if x-r >= 0.5 {
		r++
	}
	return r
}

// checkStat: the STAT interrupt line is the OR of the enabled sources; an
// interrupt is requested on its rising edge.
func (g *GB) checkStat() {
	// With the screen off nothing changes: the LY=LYC flag and the interrupt line keep their state.
	if !g.lcdOn() {
		return
	}
	st, d, ly := int(g.io[0x41]), g.lineDot, g.ly
	g.lycFlag = g.lyCmp == int(g.io[0x45])
	vis := ly < 144
	line := (st&0x40 != 0 && g.lycFlag) ||
		(st&0x08 != 0 && vis && (g.mode == 0 || (g.mode == 3 && d >= g.m3end-4))) ||
		(st&0x10 != 0 && !vis) ||
		(st&0x20 != 0 && g.mode2Source(ly, d))
	if line && !g.statLine {
		g.ifl |= 2
	}
	g.statLine = line
}

// mode2Source: the mode 2 interrupt source, from LY's change to the end of
// the OAM scan, plus a short pulse at the start of line 144 (together with
// the vertical blank interrupt).
func (g *GB) mode2Source(ly, d int) bool {
	if g.firstLine {
		return false
	}
	if ly < 144 {
		return d < 84
	}
	return ly == 144 && d < 4
}

// ppuTick: timing of a visible line, in dots from the moment LY changes:
// 0-3 LY changes (STAT still shows mode 0, OAM already locked), 4-83 mode 2
// (VRAM locked from 80), 84 mode 3, which ends at 256 + SCX&7 + objects +
// window, then mode 0 until 456.
func (g *GB) ppuTick(t int) {
	if !g.lcdOn() {
		g.lcdOffDots += t
		if g.lcdOffDots >= 70224 {
			g.lcdOffDots -= 70224
			g.frameDone = true
		}
		return
	}
	g.lineDot += t
	if g.lineDot >= 456 {
		g.lineDot -= 456
		g.firstLine = false
		g.ly++
		if g.ly > 153 {
			g.ly = 0
			g.winLine = 0
		}
	}
	d, ly := g.lineDot, g.ly
	if ly < 144 {
		var mode int
		switch {
		case g.firstLine:
			if d >= 84 && d < g.m3end {
				mode = 3
			}
		case d < 4:
		case d < 84:
			mode = 2
		case d < g.m3end:
			mode = 3
		}
		if mode == 3 && g.mode != 3 {
			g.m3end = g.mode3Length()
		}
		if mode == 0 && g.mode == 3 {
			g.renderLine()
			if g.hdmaActive {
				g.hdmaPending = true
			}
		}
		g.mode = mode
		if g.firstLine {
			g.oamLock = d >= 84 && d < g.m3end
		} else {
			g.oamLock = d < g.m3end
		}
		lockFrom := 80
		if g.firstLine {
			lockFrom = 84
		}
		g.vramLock = d >= lockFrom && d < g.m3end
		// writes are refused a little later than reads
		if g.firstLine {
			g.oamWLock = g.oamLock
		} else {
			g.oamWLock = (d >= 4 && d < 80) || (d >= 84 && d < g.m3end)
		}
		g.vramWLock = d >= 84 && d < g.m3end
		g.lyReg = ly
		if d < 4 && !g.firstLine {
			g.lyCmp = -1
		} else {
			g.lyCmp = ly
		}
		if mode == 3 && d >= g.m3end {
			g.m3end = 456
		}
	} else {
		if ly == 144 && d < 4 {
			if g.lyReg != 144 { // vertical blank begins
				g.ifl |= 1
				g.frameDone = true
			}
			g.lyReg, g.lyCmp, g.mode = 144, -1, 0
		} else {
			g.mode = 1
			// line 153 reports LY = 0 after its first M-cycle
			if ly == 153 {
				switch {
				case d < 4:
					g.lyReg, g.lyCmp = 153, -1
				case d < 8:
					g.lyReg, g.lyCmp = 0, 153
				default:
					g.lyReg, g.lyCmp = 0, 0
				}
			} else {
				g.lyReg = ly
				if d < 4 {
					g.lyCmp = -1
				} else {
					g.lyCmp = ly
				}
			}
		}
		g.oamLock, g.vramLock, g.oamWLock, g.vramWLock = false, false, false, false
	}
	g.checkStat()
}

// mode3Length: mode 3's length in dots (from dot 0 of the line to the start of mode 0).
func (g *GB) mode3Length() int {
	lcdc, scx := int(g.io[0x40]), int(g.io[0x43]&7)
	n := 256 + scx
	// window: the fetcher restarts when it reaches the window
	wx := int(g.io[0x4b])
	if lcdc&0x20 != 0 && g.ly >= int(g.io[0x4a]) && wx < 167 {
		n += 6
	}
	if lcdc&2 != 0 {
		h := 8
		if lcdc&4 != 0 {
			h = 16
		}
		var xsb [10]int
		xs := xsb[:0]
		for i := 0; i < 40 && len(xs) < 10; i++ {
			sy := int(g.oam[i*4]) - 16
			if g.ly >= sy && g.ly < sy+h {
				xs = append(xs, int(g.oam[i*4+1]))
			}
		}
		sort.Ints(xs)
		// each object costs 6 dots, plus a wait for the background fetch of the first object in a tile
		var seen [64]bool
		pen := 0
		for _, x := range xs {
			if x >= 168 {
				continue
			}
			px := x + scx
			tile := px >> 3
			pen += 6
			if !seen[tile] {
				seen[tile] = true
				pen += max(0, 5-(px&7))
			}
		}
		if pen != 0 {
			n += pen - 3 // the last fetch overlaps the end of the line
		}
	}
	return n
}

// color: a CGB 15-bit colour (with the gentle correction).
func (g *GB) color(palIdx, c int, obj bool) uint32 {
	p := &g.bgPal
	if obj {
		p = &g.obPal
	}
	i := palIdx*8 + c*2
	return cgbRGB[(int(p[i])|int(p[i+1])<<8)&0x7fff]
}

func (g *GB) renderLine() {
	y, lcdc, cgb := g.ly, int(g.io[0x40]), g.cgb
	row := y * 160
	scr := g.screen
	bgIdx, bgPri := &g.lineBg, &g.lineBgPri // colour index 0-3 for priority
	scx, scy, wx, wy := int(g.io[0x43]), int(g.io[0x42]), int(g.io[0x4b])-7, int(g.io[0x4a])
	bgp := int(g.io[0x47])
	bgOn := cgb || lcdc&1 != 0
	winOn := lcdc&0x20 != 0 && (cgb || lcdc&1 != 0) && y >= wy && wx < 160
	signed := lcdc&0x10 == 0
	usedWin := false
	for x := 0; x < 160; x++ {
		var mapBase, px, py int
		inWin := winOn && x >= wx
		switch {
		case inWin:
			mapBase = 0x1800
			if lcdc&0x40 != 0 {
				mapBase = 0x1c00
			}
			px, py = x-wx, g.winLine
			usedWin = true
		case bgOn:
			mapBase = 0x1800
			if lcdc&0x08 != 0 {
				mapBase = 0x1c00
			}
			px, py = (x+scx)&255, (y+scy)&255
		default:
			bgIdx[x], bgPri[x] = 0, 0
			if cgb {
				scr[row+x] = 0xffffff
			} else {
				scr[row+x] = g.dmgPal[bgp&3]
			}
			continue
		}
		mapAddr := mapBase + ((py >> 3) << 5) + (px >> 3)
		tile := int(g.vram[mapAddr])
		attr := 0
		if cgb {
			attr = int(g.vram[0x2000+mapAddr])
		}
		ty := py & 7
		if attr&0x40 != 0 {
			ty = 7 - ty
		}
		tx := px & 7
		if attr&0x20 != 0 {
			tx = 7 - tx
		}
		t := tile * 16
		if signed {
			t = 0x1000 + int(int8(tile))*16
		}
		bank := 0
		if attr&8 != 0 {
			bank = 0x2000
		}
		lo, hi := int(g.vram[bank+t+ty*2]), int(g.vram[bank+t+ty*2+1])
		c := ((lo >> (7 - tx)) & 1) | (((hi >> (7 - tx)) & 1) << 1)
		bgIdx[x] = byte(c)
		bgPri[x] = 0
		if attr&0x80 != 0 {
			bgPri[x] = 1
		}
		if cgb {
			scr[row+x] = g.color(attr&7, c, false)
		} else {
			scr[row+x] = g.dmgPal[(bgp>>(c*2))&3]
		}
	}
	if usedWin {
		g.winLine++
	}
	if lcdc&2 != 0 {
		g.renderSprites(y, row)
	}
}

func (g *GB) renderSprites(y, row int) {
	lcdc, cgb, scr := int(g.io[0x40]), g.cgb, g.screen
	bgIdx, bgPri := &g.lineBg, &g.lineBgPri
	h := 8
	if lcdc&4 != 0 {
		h = 16
	}
	var lb [10]int
	list := lb[:0]
	for i := 0; i < 40 && len(list) < 10; i++ {
		sy := int(g.oam[i*4]) - 16
		if y >= sy && y < sy+h {
			list = append(list, i)
		}
	}
	// Priority: CGB by OAM order; DMG by X position, then OAM order.
	if !cgb {
		sort.SliceStable(list, func(a, b int) bool {
			xa, xb := g.oam[list[a]*4+1], g.oam[list[b]*4+1]
			if xa != xb {
				return xa < xb
			}
			return list[a] < list[b]
		})
	}
	drawn := &g.sprD
	*drawn = [160]byte{}
	for _, i := range list {
		sy, sx, attr := int(g.oam[i*4])-16, int(g.oam[i*4+1])-8, int(g.oam[i*4+3])
		tile := int(g.oam[i*4+2])
		if h == 16 {
			tile &= 0xfe
		}
		ty := y - sy
		if attr&0x40 != 0 {
			ty = h - 1 - ty
		}
		bank := 0
		if cgb && attr&8 != 0 {
			bank = 0x2000
		}
		addr := bank + tile*16 + ty*2
		lo, hi := int(g.vram[addr]), int(g.vram[addr+1])
		for px := 0; px < 8; px++ {
			x := sx + px
			if x < 0 || x >= 160 || drawn[x] != 0 {
				continue
			}
			bit := 7 - px
			if attr&0x20 != 0 {
				bit = px
			}
			c := ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
			if c == 0 {
				continue
			}
			drawn[x] = 1
			// behind-background flags (and the CGB master priority bit)
			if bgIdx[x] != 0 {
				if cgb {
					if lcdc&1 != 0 && (attr&0x80 != 0 || bgPri[x] != 0) {
						continue
					}
				} else if attr&0x80 != 0 {
					continue
				}
			}
			if cgb {
				scr[row+x] = g.color(attr&7, c, true)
			} else {
				reg := 0x48
				if attr&0x10 != 0 {
					reg = 0x49
				}
				scr[row+x] = g.dmgPal[(int(g.io[reg])>>(c*2))&3]
			}
		}
	}
}
