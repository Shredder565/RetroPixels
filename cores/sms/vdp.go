package sms

// The VDP: the Master System's video chip in its "mode 4" (tiles, 64 sprites,
// line interrupts, 192- and 224-line screens) plus the older TMS9918 modes.
// A port of the VDP class in web/emu/sms.js.

// tmsColours: the TMS9918's 16 colours, as RetroPixels shows them with this
// VDP's own 6-bit colours (--bbggrr, 2 bits per channel).
var tmsColours = [16]uint8{
	0x00, 0x00, // transparent, black
	0x18, 0x1d, // medium green, light green
	0x35, 0x35, // dark blue, light blue
	0x16, 0x3d, // dark red, cyan
	0x17, 0x17, // medium red, light red
	0x1a, 0x2b, // dark yellow, light yellow
	0x18, 0x26, // dark green, magenta
	0x2a, 0x3f, // grey, white
}

var tmsPal [16]uint32

func init() {
	for i, c := range tmsColours {
		tmsPal[i] = rgb6(c)
	}
}

// rgb6 turns a --bbggrr colour into 0x00RRGGBB.
func rgb6(c uint8) uint32 {
	r, g, b := uint32(c&3)*85, uint32(c>>2&3)*85, uint32(c>>4&3)*85
	return r<<16 | g<<8 | b
}

type vdp struct {
	sms  *SMS
	vram [0x4000]uint8
	cram [32]uint8
	reg  [16]uint8
	pal  [32]uint32

	bgCol, bgPri, spr [256]uint8

	addr, code int
	latch      bool
	buf        uint8
	status     uint8
	lineIrq    bool
	lineCnt    int
	vscroll    int
}

func (v *vdp) reset() {
	v.vram = [0x4000]uint8{}
	v.cram = [32]uint8{}
	v.reg = [16]uint8{}
	// the BIOS leaves these set up
	v.reg[0], v.reg[1], v.reg[2], v.reg[3], v.reg[4], v.reg[5] = 0x36, 0x80, 0xff, 0xff, 0xff, 0xff
	v.reg[6], v.reg[10] = 0xfb, 0xff
	v.addr, v.code, v.latch, v.buf = 0, 0, false, 0
	v.status, v.lineIrq, v.lineCnt, v.vscroll = 0, false, 0xff, 0
	for i := range v.pal {
		v.updatePal(i)
	}
}

func (v *vdp) updatePal(i int) { v.pal[i] = rgb6(v.cram[i]) }

func (v *vdp) lines() int {
	if v.reg[0]&6 == 6 && v.reg[1]&0x18 == 0x10 {
		return 224
	}
	return 192
}

func (v *vdp) displayOn() bool { return v.reg[1]&0x40 != 0 }

func (v *vdp) irqLine() bool {
	return (v.status&0x80 != 0 && v.reg[1]&0x20 != 0) || (v.lineIrq && v.reg[0]&0x10 != 0)
}

// ---- ports ----

func (v *vdp) writeControl(b uint8) {
	if !v.latch {
		v.latch = true
		v.addr = (v.addr & 0x3f00) | int(b)
		return
	}
	v.latch = false
	v.addr = int(b&0x3f)<<8 | (v.addr & 0xff)
	v.code = int(b >> 6)
	if v.code == 0 {
		v.buf = v.vram[v.addr]
		v.addr = (v.addr + 1) & 0x3fff
	} else if v.code == 2 {
		r := b & 15
		if r < 11 {
			v.reg[r] = uint8(v.addr)
			v.sms.updateIrq()
		}
	}
}

func (v *vdp) writeData(b uint8) {
	v.latch = false
	if v.code == 3 {
		i := v.addr & 31
		v.cram[i] = b & 0x3f
		v.updatePal(i)
	} else {
		v.vram[v.addr] = b
	}
	v.buf = b
	v.addr = (v.addr + 1) & 0x3fff
}

func (v *vdp) readData() uint8 {
	v.latch = false
	r := v.buf
	v.buf = v.vram[v.addr]
	v.addr = (v.addr + 1) & 0x3fff
	return r
}

func (v *vdp) readStatus() uint8 {
	r := v.status | 0x1f
	v.status, v.lineIrq, v.latch = 0, false, false
	v.sms.updateIrq()
	return r
}

// lineDone is called at the end of each line: the line counter runs on lines
// 0..active (inclusive).
func (v *vdp) lineDone(line int) {
	if line <= v.lines() {
		if v.lineCnt == 0 {
			v.lineCnt = int(v.reg[10])
			v.lineIrq = true
		} else {
			v.lineCnt = (v.lineCnt - 1) & 0xff
		}
	} else {
		v.lineCnt = int(v.reg[10])
	}
}

// ---- drawing ----

func fill(out []uint32, c uint32) {
	for i := range out {
		out[i] = c
	}
}

func (v *vdp) renderLine(y int, out []uint32, w int) {
	o := y * w
	row := out[o : o+w]
	backdrop := v.pal[16+int(v.reg[7]&15)]
	if !v.displayOn() {
		fill(row, backdrop)
		return
	}
	if v.reg[0]&4 == 0 {
		v.renderTMS(y, out, w)
		return
	}
	vram := &v.vram
	r0 := v.reg[0]
	tall := v.lines() == 224
	var nameBase int
	if tall {
		nameBase = int(v.reg[2]&0x0c)<<10 | 0x0700
	} else {
		nameBase = int(v.reg[2]&0x0e) << 10
	}
	sx := int(v.reg[8])
	if r0&0x40 != 0 && y < 16 {
		sx = 0
	}
	col, pri := &v.bgCol, &v.bgPri
	// background
	lastCell := -1
	var b0, b1, b2, b3 int
	var palOff, prio, hf int
	for x := 0; x < 256; x++ {
		vs := v.vscroll
		if r0&0x80 != 0 && x >= 192 {
			vs = 0
		}
		r := y + vs
		if tall {
			r &= 255
		} else {
			r %= 224
		}
		bx := (x - sx) & 255
		cell := (r>>3)<<5 | bx>>3
		if cell != lastCell {
			lastCell = cell
			ea := (nameBase + cell*2) & 0x3fff
			e := int(vram[ea]) | int(vram[(ea+1)&0x3fff])<<8
			ty := r & 7
			if e&0x400 != 0 {
				ty = 7 - ty
			}
			pa := (e&0x1ff)<<5 + ty*4
			b0, b1, b2, b3 = int(vram[pa&0x3fff]), int(vram[(pa+1)&0x3fff]), int(vram[(pa+2)&0x3fff]), int(vram[(pa+3)&0x3fff])
			hf = e & 0x200
			palOff = 0
			if e&0x800 != 0 {
				palOff = 16
			}
			prio = (e >> 12) & 1
		}
		bit := 7 - (bx & 7)
		if hf != 0 {
			bit = bx & 7
		}
		c := (b0>>bit)&1 | ((b1>>bit)&1)<<1 | ((b2>>bit)&1)<<2 | ((b3>>bit)&1)<<3
		col[x] = uint8(c | palOff)
		if prio != 0 && c != 0 {
			pri[x] = 1
		} else {
			pri[x] = 0
		}
	}
	// sprites
	spr := &v.spr
	*spr = [256]uint8{}
	satBase := int(v.reg[5]&0x7e) << 7
	tallSpr, zoom := v.reg[1]&2 != 0, v.reg[1]&1 != 0
	h := 8
	if tallSpr {
		h = 16
	}
	zs := 0
	if zoom {
		zs = 1
	}
	h <<= zs
	patHi := 0
	if v.reg[6]&4 != 0 {
		patHi = 256
	}
	shift := 0
	if r0&8 != 0 {
		shift = 8
	}
	n := 0
	for i := 0; i < 64; i++ {
		sy0 := int(vram[satBase+i])
		if sy0 == 0xd0 && !tall {
			break
		}
		sy := sy0 + 1
		if sy > 240 {
			sy -= 256
		}
		if y < sy || y >= sy+h {
			continue
		}
		if n == 8 {
			v.status |= 0x40
			break
		}
		n++
		sxp := int(vram[satBase+128+i*2]) - shift
		pat := int(vram[satBase+129+i*2]) | patHi
		if tallSpr {
			pat &^= 1
		}
		r := (y - sy) >> zs
		pa := pat<<5 + r*4
		p0, p1, p2, p3 := int(vram[pa&0x3fff]), int(vram[(pa+1)&0x3fff]), int(vram[(pa+2)&0x3fff]), int(vram[(pa+3)&0x3fff])
		for px := 0; px < 8; px++ {
			bit := 7 - px
			c := (p0>>bit)&1 | ((p1>>bit)&1)<<1 | ((p2>>bit)&1)<<2 | ((p3>>bit)&1)<<3
			if c == 0 {
				continue
			}
			for z := 0; z <= zs; z++ {
				x := sxp + px
				if zoom {
					x = sxp + px*2 + z
				}
				if x < 0 || x >= 256 {
					continue
				}
				if spr[x] != 0 {
					v.status |= 0x20
					continue
				}
				spr[x] = uint8(16 | c)
			}
		}
	}
	// compose
	pal := &v.pal
	for x := 0; x < w; x++ {
		if spr[x] != 0 && pri[x] == 0 {
			row[x] = pal[spr[x]]
		} else {
			row[x] = pal[col[x]]
		}
	}
	if r0&0x20 != 0 { // left column hidden
		fill(row[:8], backdrop)
	}
}

// renderTMS draws the older TMS9918 modes (Graphics I / II, Text,
// Multicolor), used by SG-1000 era games and some homebrew: a fixed
// 16-colour palette rounded to the colours this VDP can make.
func (v *vdp) renderTMS(y int, out []uint32, w int) {
	vram := &v.vram
	reg := &v.reg
	o := y * w
	row := out[o : o+w]
	tc := func(i uint8) uint32 {
		if i == 0 {
			i = reg[7] & 15
		}
		return tmsPal[i]
	}
	if !v.displayOn() {
		fill(row, tc(reg[7]&15))
		return
	}
	m1, m2, m3 := reg[1]&0x10 != 0, reg[1]&8 != 0, reg[0]&2 != 0
	nameBase := int(reg[2]&0x0f) << 10
	trow, line := y>>3, y&7
	col := &v.bgCol // TMS colour index per pixel, 0 = transparent
	if m1 {         // Text: 40 columns of 6 pixels, no sprites
		fg, bg := reg[7]>>4, reg[7]&15
		patBase := int(reg[4]&7) << 11
		for i := range col {
			col[i] = bg
		}
		for cx := 0; cx < 40; cx++ {
			p := vram[(patBase+int(vram[(nameBase+trow*40+cx)&0x3fff])<<3+line)&0x3fff]
			for b := 0; b < 6; b++ {
				if p&(0x80>>b) != 0 {
					col[8+cx*6+b] = fg
				} else {
					col[8+cx*6+b] = bg
				}
			}
		}
		for x := 0; x < w; x++ {
			row[x] = tc(col[x])
		}
		return
	}
	for cx := 0; cx < 32; cx++ {
		name := int(vram[(nameBase+trow*32+cx)&0x3fff])
		var p, c uint8
		if m2 { // Multicolor: 4x4 blocks
			a := int(reg[4]&7)<<11 + name<<3 + (trow&3)<<1 + line>>2
			vv := vram[a&0x3fff]
			for b := 0; b < 8; b++ {
				if b < 4 {
					col[cx*8+b] = vv >> 4
				} else {
					col[cx*8+b] = vv & 15
				}
			}
			continue
		}
		if m3 { // Graphics II: the screen in three thirds, a colour byte per pattern line
			idx := (trow>>3)<<8 | name
			p = vram[(int(reg[4]&4)<<11+(idx&(int(reg[4]&3)<<8|0xff))<<3+line)&0x3fff]
			c = vram[(int(reg[3]&0x80)<<6+(idx&(int(reg[3]&0x7f)<<3|7))<<3+line)&0x3fff]
		} else { // Graphics I: a colour byte per 8 patterns
			p = vram[(int(reg[4]&7)<<11+name<<3+line)&0x3fff]
			c = vram[(int(reg[3])<<6+name>>3)&0x3fff]
		}
		for b := 0; b < 8; b++ {
			if p&(0x80>>b) != 0 {
				col[cx*8+b] = c >> 4
			} else {
				col[cx*8+b] = c & 15
			}
		}
	}
	// sprites: 32 of them, 4 per line, one colour each
	spr := &v.spr
	*spr = [256]uint8{}
	sat, pg := int(reg[5]&0x7f)<<7, int(reg[6]&7)<<11
	big, mag := reg[1]&2 != 0, reg[1]&1 != 0
	ms := 0
	if mag {
		ms = 1
	}
	size := 8
	if big {
		size = 16
	}
	size <<= ms
	halves := 1
	if big {
		halves = 2
	}
	n := 0
	for i := 0; i < 32; i++ {
		a := sat + i*4
		sy0 := int(vram[a&0x3fff])
		if sy0 == 0xd0 {
			break
		}
		sy := sy0 + 1
		if sy > 0xe0 {
			sy -= 256
		}
		if y < sy || y >= sy+size {
			continue
		}
		if n == 4 {
			if v.status&0x40 == 0 {
				v.status = (v.status & 0xe0) | 0x40 | uint8(i)
			}
			break
		}
		n++
		attr := vram[(a+3)&0x3fff]
		c := attr & 15
		sx := int(vram[(a+1)&0x3fff])
		if attr&0x80 != 0 {
			sx -= 32
		}
		r := (y - sy) >> ms
		pat := int(vram[(a+2)&0x3fff])
		if big {
			pat &= 0xfc
		}
		for half := 0; half < halves; half++ {
			bits := vram[(pg+pat<<3+r+half*16)&0x3fff]
			for b := 0; b < 8; b++ {
				if bits&(0x80>>b) == 0 {
					continue
				}
				for z := 0; z <= ms; z++ {
					x := sx + (half*8+b)<<ms + z
					if x < 0 || x >= 256 {
						continue
					}
					if spr[x] != 0 {
						v.status |= 0x20
						continue
					}
					spr[x] = 16 | c
				}
			}
		}
	}
	for x := 0; x < w; x++ {
		s := spr[x] & 15
		if s != 0 {
			row[x] = tc(s)
		} else {
			row[x] = tc(col[x])
		}
	}
}
