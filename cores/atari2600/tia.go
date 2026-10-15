package atari2600

// ------------------------------------------------------------------
// TIA video
// ------------------------------------------------------------------

var copies = [8][]int{{0}, {0, 16}, {0, 32}, {0, 16, 32}, {0, 64}, {0}, {0, 32, 64}, {0}}

// collision bit for every combination of objects (see read())
// object bits: 1 P0, 2 P1, 4 M0, 8 M1, 16 BL, 32 PF
var cxTable = func() (t [64]uint16) {
	pairs := [][3]int{{4, 2, 0}, {4, 1, 1}, {8, 1, 2}, {8, 2, 3}, {1, 32, 4}, {1, 16, 5}, {2, 32, 6}, {2, 16, 7},
		{4, 32, 8}, {4, 16, 9}, {8, 32, 10}, {8, 16, 11}, {16, 32, 12}, {1, 2, 14}, {4, 8, 15}}
	for m := 0; m < 64; m++ {
		c := 0
		for _, p := range pairs {
			if m&p[0] != 0 && m&p[1] != 0 {
				c |= 1 << p[2]
			}
		}
		t[m] = uint16(c)
	}
	return
}()

type tia struct {
	m      *Atari
	lines  []uint32
	screen []uint32

	hclock, line, rendered    int
	frameDone                 bool
	frameLines                int
	ystart, fitWant, fitCount int
	firstVis, lastVis         int
	vsync, vblank             uint8

	colup0, colup1, colupf, colubk uint8
	ctrlpf, refp0, refp1           uint8
	pf0, pf1, pf2                  uint8
	pfBits                         int
	grp0n, grp0o, grp1n, grp1o     uint8
	enam0, enam1, enabln, enablo   uint8
	vdelp0, vdelp1, vdelbl         uint8
	resmp0, resmp1                 uint8
	nusiz0, nusiz1                 uint8
	hmp0, hmp1, hmm0, hmm1, hmbl   uint8
	p0, p1, m0, m1, bl             int
	noMain0, noMain1               bool
	hmoveBlank, lateHmove          bool
	cx                             uint16
	latch                          int
	fireLatched                    [2]bool
	wsync                          bool
	dumpStart                      int64
}

func newTIA(m *Atari) *tia {
	return &tia{
		m:          m,
		lines:      make([]uint32, maxLines*160),
		screen:     make([]uint32, 160*outH),
		frameLines: 262,
		ystart:     34, fitWant: -1,
		firstVis: -1, lastVis: -1,
	}
}

func (t *tia) clone(m *Atari) *tia {
	n := *t
	n.m = m
	n.lines = append([]uint32(nil), t.lines...)
	n.screen = append([]uint32(nil), t.screen...)
	return &n
}

// ---- timing ----

func (t *tia) clock3() {
	h := t.hclock + 3
	if h >= 114 && t.hclock < 114 {
		t.m.sound.tick()
	}
	if h >= lineClocks {
		t.renderTo(lineClocks)
		t.endLine()
		t.hclock = h - lineClocks
		t.m.sound.tick()
	} else {
		t.hclock = h
	}
}

func (t *tia) endLine() {
	if t.vblank&2 == 0 && t.line < maxLines {
		if t.firstVis < 0 {
			t.firstVis = t.line
		}
		t.lastVis = t.line
	}
	t.line++
	t.rendered = 0
	t.hmoveBlank = false
	t.noMain0, t.noMain1 = false, false
	if t.lateHmove {
		t.lateHmove = false
		t.applyHmove(8)
	}
	if t.line >= 330 && t.line >= t.frameLines+40 { // game isn't sending VSYNC (PAL games use 312 lines)
		t.endFrame()
	} else if t.line >= maxLines+20 {
		t.endFrame()
	}
}

func (t *tia) endFrame() {
	n := min(t.line, maxLines)
	t.frameLines = max(200, t.line)
	// keep the area the game draws on screen (like a TV), adjusting only when it stays put
	want := t.ystart
	if t.firstVis >= 0 {
		if t.firstVis < t.ystart {
			want = t.firstVis
		} else if t.lastVis >= t.ystart+outH {
			want = min(t.firstVis, t.lastVis-outH+1)
		}
	}
	want = max(0, min(want, maxLines-outH))
	if want != t.ystart {
		if want == t.fitWant {
			t.fitCount++
			if t.fitCount >= 3 {
				t.ystart = want
				t.fitCount = 0
			}
		} else {
			t.fitWant = want
			t.fitCount = 1
		}
	} else {
		t.fitCount = 0
	}
	for y := n; y < maxLines; y++ {
		clear(t.lines[y*160 : y*160+160])
	}
	copy(t.screen, t.lines[t.ystart*160:(t.ystart+outH)*160])
	t.frameDone = true
	t.line = 0
	t.firstVis = -1
	t.lastVis = -1
}

// ---- drawing ----

func (t *tia) renderTo(end int) {
	start := t.rendered
	if end <= start {
		return
	}
	t.rendered = end
	if end <= hblank || t.line >= maxLines {
		return
	}
	x0, x1 := max(start, hblank)-hblank, end-hblank
	row := t.line * 160
	buf := t.lines[row : row+160]
	if t.vblank&2 != 0 {
		clear(buf[x0:x1])
		return
	}
	c0, c1 := palette[t.colup0>>1], palette[t.colup1>>1]
	cpf, cbk := palette[t.colupf>>1], palette[t.colubk>>1]
	g0, g1 := t.grp0n, t.grp1n
	if t.vdelp0 != 0 {
		g0 = t.grp0o
	}
	if t.vdelp1 != 0 {
		g1 = t.grp1o
	}
	ebl := t.enabln
	if t.vdelbl != 0 {
		ebl = t.enablo
	}
	em0 := t.enam0 != 0 && t.resmp0 == 0
	em1 := t.enam1 != 0 && t.resmp1 == 0
	pri := t.ctrlpf&4 != 0
	score := t.ctrlpf&2 != 0 && !pri
	reflect := t.ctrlpf&1 != 0
	pfb := t.pfBits
	ns0, ns1 := int(t.nusiz0&7), int(t.nusiz1&7)
	ms0, ms1 := 1<<((t.nusiz0>>4)&3), 1<<((t.nusiz1>>4)&3)
	bs := 1 << ((t.ctrlpf >> 4) & 3)
	cp0, cp1 := copies[ns0], copies[ns1]
	cx := t.cx
	for x := x0; x < x1; x++ {
		if t.hmoveBlank && x < 8 {
			buf[x] = 0
			continue
		}
		m := 0
		// playfield
		blk := x >> 2
		idx := blk
		if blk >= 20 {
			if reflect {
				idx = 39 - blk
			} else {
				idx = blk - 20
			}
		}
		if (pfb>>idx)&1 != 0 {
			m |= 32
		}
		// players
		if g0 != 0 && t.playerBit(x, t.p0, ns0, cp0, t.refp0, g0, t.noMain0) {
			m |= 1
		}
		if g1 != 0 && t.playerBit(x, t.p1, ns1, cp1, t.refp1, g1, t.noMain1) {
			m |= 2
		}
		if em0 && missileBit(x, t.m0, ns0, cp0, ms0) {
			m |= 4
		}
		if em1 && missileBit(x, t.m1, ns1, cp1, ms1) {
			m |= 8
		}
		if ebl != 0 {
			d := x - t.bl
			if d < 0 {
				d += 160
			}
			if d < bs {
				m |= 16
			}
		}
		cx |= cxTable[m]
		var col uint32
		if pri {
			switch {
			case m&48 != 0:
				col = cpf
			case m&5 != 0:
				col = c0
			case m&10 != 0:
				col = c1
			default:
				col = cbk
			}
		} else {
			switch {
			case m&5 != 0:
				col = c0
			case m&10 != 0:
				col = c1
			case m&32 != 0:
				if score {
					if x < 80 {
						col = c0
					} else {
						col = c1
					}
				} else {
					col = cpf
				}
			case m&16 != 0:
				col = cpf
			default:
				col = cbk
			}
		}
		buf[x] = col
	}
	t.cx = cx
}

func (t *tia) playerBit(x, pos, ns int, cps []int, refl uint8, g uint8, noMain bool) bool {
	d := x - pos
	if d < 0 {
		d += 160
	}
	bit := -1
	if ns == 5 {
		d--
		if d < 0 {
			d += 160
		}
		if d < 16 {
			bit = d >> 1
		}
	} else if ns == 7 {
		d--
		if d < 0 {
			d += 160
		}
		if d < 32 {
			bit = d >> 2
		}
	} else {
		for _, o := range cps {
			if d >= o && d < o+8 {
				if !(o == 0 && noMain) {
					bit = d - o
				}
				break
			}
		}
	}
	if bit < 0 {
		return false
	}
	if ns >= 5 && noMain {
		return false
	}
	if refl != 0 {
		return (g>>bit)&1 != 0
	}
	return (g>>(7-bit))&1 != 0
}

func missileBit(x, pos, ns int, cps []int, size int) bool {
	d := x - pos
	if d < 0 {
		d += 160
	}
	if ns == 5 || ns == 7 {
		return d < size
	}
	for _, o := range cps {
		if d >= o && d < o+size {
			return true
		}
	}
	return false
}

func (t *tia) updatePF() {
	b := 0
	for i := 0; i < 4; i++ {
		if t.pf0&(0x10<<i) != 0 {
			b |= 1 << i
		}
	}
	for i := 0; i < 8; i++ {
		if t.pf1&(0x80>>i) != 0 {
			b |= 1 << (4 + i)
		}
	}
	for i := 0; i < 8; i++ {
		if t.pf2&(1<<i) != 0 {
			b |= 1 << (12 + i)
		}
	}
	t.pfBits = b
}

// ---- movement ----

// hm is a motion register's value: +7 (left) .. -8 (right)
func hm(v uint8) int {
	n := int(v>>4) & 15
	if n >= 8 {
		return n - 16
	}
	return n
}

func (t *tia) applyHmove(extra int) {
	mv := func(pos int, h uint8) int {
		p := pos - (hm(h) + extra)
		p %= 160
		if p < 0 {
			p += 160
		}
		return p
	}
	t.p0 = mv(t.p0, t.hmp0)
	t.p1 = mv(t.p1, t.hmp1)
	t.m0 = mv(t.m0, t.hmm0)
	t.m1 = mv(t.m1, t.hmm1)
	t.bl = mv(t.bl, t.hmbl)
	if t.resmp0 != 0 {
		t.m0 = lockedMissile(t.p0, t.nusiz0)
	}
	if t.resmp1 != 0 {
		t.m1 = lockedMissile(t.p1, t.nusiz1)
	}
}

func lockedMissile(p int, nus uint8) int {
	ns := nus & 7
	off := 3
	if ns == 5 {
		off = 6
	} else if ns == 7 {
		off = 10
	}
	return (p + off) % 160
}

func (t *tia) resetPos(isPlayer bool) int {
	h := t.hclock
	edge := hblank
	if t.hmoveBlank {
		edge += 8
	}
	if h < edge {
		if isPlayer {
			return 3
		}
		return 2
	}
	if isPlayer {
		return (h - hblank + 5) % 160
	}
	return (h - hblank + 4) % 160
}

// ---- registers ----

func (t *tia) write(r int, v uint8) {
	// Changes that affect the picture: draw everything up to now (plus the
	// chip's short internal delay) with the old values first.
	h := t.hclock
	switch r {
	case 0x00: // VSYNC
		t.renderTo(h)
		if v&2 != 0 && t.vsync&2 == 0 {
			t.vsync = v
			t.endFrame()
			t.rendered = h
			return
		}
		t.vsync = v
	case 0x01: // VBLANK
		t.renderTo(min(lineClocks, h+1))
		if t.vblank&0x80 != 0 && v&0x80 == 0 {
			t.dumpStart = t.m.cycles
		}
		if v&0x40 == 0 {
			t.fireLatched = [2]bool{}
		}
		t.vblank = v
	case 0x02:
		t.wsync = true
	case 0x03: // RSYNC
	case 0x04:
		t.renderTo(h)
		t.nusiz0 = v
	case 0x05:
		t.renderTo(h)
		t.nusiz1 = v
	case 0x06:
		t.renderTo(h)
		t.colup0 = v
	case 0x07:
		t.renderTo(h)
		t.colup1 = v
	case 0x08:
		t.renderTo(h)
		t.colupf = v
	case 0x09:
		t.renderTo(h)
		t.colubk = v
	case 0x0a:
		t.renderTo(min(lineClocks, h+1))
		t.ctrlpf = v
	case 0x0b:
		t.renderTo(min(lineClocks, h+1))
		t.refp0 = v & 8
	case 0x0c:
		t.renderTo(min(lineClocks, h+1))
		t.refp1 = v & 8
	case 0x0d:
		t.renderTo(min(lineClocks, h+2))
		t.pf0 = v
		t.updatePF()
	case 0x0e:
		t.renderTo(min(lineClocks, h+2))
		t.pf1 = v
		t.updatePF()
	case 0x0f:
		t.renderTo(min(lineClocks, h+2))
		t.pf2 = v
		t.updatePF()
	case 0x10:
		t.renderTo(h)
		t.p0 = t.resetPos(true)
		if h >= hblank {
			t.noMain0 = true
		}
		if t.resmp0 != 0 {
			t.m0 = lockedMissile(t.p0, t.nusiz0)
		}
	case 0x11:
		t.renderTo(h)
		t.p1 = t.resetPos(true)
		if h >= hblank {
			t.noMain1 = true
		}
		if t.resmp1 != 0 {
			t.m1 = lockedMissile(t.p1, t.nusiz1)
		}
	case 0x12:
		t.renderTo(h)
		t.m0 = t.resetPos(false)
	case 0x13:
		t.renderTo(h)
		t.m1 = t.resetPos(false)
	case 0x14:
		t.renderTo(h)
		t.bl = t.resetPos(false)
	case 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a:
		t.m.sound.write(r-0x15, v)
	case 0x1b:
		t.renderTo(min(lineClocks, h+1))
		t.grp0n = v
		t.grp1o = t.grp1n
	case 0x1c:
		t.renderTo(min(lineClocks, h+1))
		t.grp1n = v
		t.grp0o = t.grp0n
		t.enablo = t.enabln
	case 0x1d:
		t.renderTo(min(lineClocks, h+1))
		t.enam0 = v & 2
	case 0x1e:
		t.renderTo(min(lineClocks, h+1))
		t.enam1 = v & 2
	case 0x1f:
		t.renderTo(min(lineClocks, h+1))
		t.enabln = v & 2
	case 0x20:
		t.hmp0 = v
	case 0x21:
		t.hmp1 = v
	case 0x22:
		t.hmm0 = v
	case 0x23:
		t.hmm1 = v
	case 0x24:
		t.hmbl = v
	case 0x25:
		t.renderTo(h)
		t.vdelp0 = v & 1
	case 0x26:
		t.renderTo(h)
		t.vdelp1 = v & 1
	case 0x27:
		t.renderTo(h)
		t.vdelbl = v & 1
	case 0x28:
		t.renderTo(h)
		if t.resmp0 != 0 && v&2 == 0 {
			t.m0 = lockedMissile(t.p0, t.nusiz0)
		}
		t.resmp0 = v & 2
	case 0x29:
		t.renderTo(h)
		if t.resmp1 != 0 && v&2 == 0 {
			t.m1 = lockedMissile(t.p1, t.nusiz1)
		}
		t.resmp1 = v & 2
	case 0x2a: // HMOVE
		t.renderTo(h)
		if h < hblank {
			t.hmoveBlank = true
			t.applyHmove(0)
		} else if h >= 219 {
			t.lateHmove = true // at the very end of a line: moves on the next, without the black bar
		} else {
			t.applyHmove(0)
		}
	case 0x2b:
		t.hmp0, t.hmp1, t.hmm0, t.hmm1, t.hmbl = 0, 0, 0, 0, 0
	case 0x2c:
		t.renderTo(h)
		t.cx = 0
	}
}

func (t *tia) read(r int) uint8 {
	r &= 15
	if r < 8 {
		t.renderTo(t.hclock)
		c := int(t.cx)
		return uint8(((c>>(2*r))&1)<<7 | ((c>>(2*r+1))&1)<<6)
	}
	if r < 12 {
		return t.m.paddleBit(r - 8)
	}
	if r < 14 {
		p := r - 12
		down := t.m.fireDown(p)
		if t.vblank&0x40 != 0 {
			if down {
				t.fireLatched[p] = true
			}
			if t.fireLatched[p] {
				return 0
			}
			return 0x80
		}
		if down {
			return 0
		}
		return 0x80
	}
	return 0
}

// The state includes the picture being drawn, so a loaded game continues
// exactly as it would have.
func (t *tia) save(w *stateW) {
	w.u32s(t.lines)
	w.u32s(t.screen)
	for _, v := range []int{t.hclock, t.line, t.rendered, t.frameLines, t.ystart, t.fitWant, t.fitCount, t.firstVis, t.lastVis,
		t.pfBits, t.p0, t.p1, t.m0, t.m1, t.bl, int(t.cx), t.latch} {
		w.int(v)
	}
	w.bytes([]byte{t.vsync, t.vblank, t.colup0, t.colup1, t.colupf, t.colubk, t.ctrlpf, t.refp0, t.refp1, t.pf0, t.pf1, t.pf2,
		t.grp0n, t.grp0o, t.grp1n, t.grp1o, t.enam0, t.enam1, t.enabln, t.enablo, t.vdelp0, t.vdelp1, t.vdelbl, t.resmp0, t.resmp1,
		t.nusiz0, t.nusiz1, t.hmp0, t.hmp1, t.hmm0, t.hmm1, t.hmbl})
	w.bools([]bool{t.frameDone, t.noMain0, t.noMain1, t.hmoveBlank, t.lateHmove, t.fireLatched[0], t.fireLatched[1], t.wsync})
	w.i64(t.dumpStart)
}

func (t *tia) load(r *stateR) {
	r.u32sInto(t.lines)
	r.u32sInto(t.screen)
	ints := make([]int, 17)
	for i := range ints {
		ints[i] = r.int()
	}
	t.hclock, t.line, t.rendered, t.frameLines, t.ystart, t.fitWant, t.fitCount, t.firstVis, t.lastVis = ints[0], ints[1], ints[2], ints[3], ints[4], ints[5], ints[6], ints[7], ints[8]
	t.pfBits, t.p0, t.p1, t.m0, t.m1, t.bl, t.cx, t.latch = ints[9], ints[10], ints[11], ints[12], ints[13], ints[14], uint16(ints[15]), ints[16]
	b := make([]byte, 32)
	r.bytesInto(b)
	t.vsync, t.vblank, t.colup0, t.colup1, t.colupf, t.colubk, t.ctrlpf, t.refp0, t.refp1, t.pf0, t.pf1, t.pf2 = b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]
	t.grp0n, t.grp0o, t.grp1n, t.grp1o, t.enam0, t.enam1, t.enabln, t.enablo, t.vdelp0, t.vdelp1, t.vdelbl, t.resmp0, t.resmp1 = b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19], b[20], b[21], b[22], b[23], b[24]
	t.nusiz0, t.nusiz1, t.hmp0, t.hmp1, t.hmm0, t.hmm1, t.hmbl = b[25], b[26], b[27], b[28], b[29], b[30], b[31]
	f := make([]bool, 8)
	r.boolsInto(f)
	t.frameDone, t.noMain0, t.noMain1, t.hmoveBlank, t.lateHmove, t.fireLatched[0], t.fireLatched[1], t.wsync = f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]
	t.dumpStart = r.i64()
}
