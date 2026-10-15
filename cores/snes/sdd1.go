package snes

// S-DD1 (web/emu/snes_sdd1.js): ROM banking for banks $C0-$FF and graphics
// decompression while the SNES copies them with DMA.

// sdd1Evolution: per probability state, the run-length generator it uses and
// the states that follow a likely (MPS) and an unlikely (LPS) bit.
var sdd1Evolution = func() [33][3]uint8 {
	var t [33][3]uint8
	t[0] = [3]uint8{0, 25, 25}
	st := 1
	for g, n := range []int{4, 4, 4, 4, 2, 2, 2, 2} {
		for i := 0; i < n; i++ {
			t[st][0] = uint8(g)
			st++
		}
	}
	for st := 1; st <= 24; st++ {
		t[st][1], t[st][2] = uint8(min(24, st+1)), uint8(max(1, st-1))
	}
	warmDrop := []uint8{1, 2, 4, 8, 12, 16, 18, 22}
	for g := 0; g < 8; g++ {
		next := uint8(26 + g)
		if g == 7 {
			next = 24
		}
		t[25+g] = [3]uint8{uint8(g), next, warmDrop[g]}
	}
	return t
}()

var sdd1Neighbours = [4][2]int{{0x0e, 1}, {0x0c, 1}, {0x06, 1}, {0x0c, 3}}

type sdd1Dec struct {
	readByte func(addr int) int
	mps      [32]uint8
	state    [32]uint8
	runMPS   [8]int32
	runLPS   [8]uint8
	prev     [8]uint16

	addr, cur, bitPos, planes, ctxBits, bitNum, plane, held int
	endOfRun                                                bool
}

func (d *sdd1Dec) start(addr int) {
	d.addr = addr
	head := d.readByte(addr)
	d.planes, d.ctxBits = head&0xc0, head&0x30
	d.cur, d.bitPos = head, 4
	d.mps, d.state, d.runMPS, d.runLPS, d.prev = [32]uint8{}, [32]uint8{}, [8]int32{}, [8]uint8{}, [8]uint16{}
	d.bitNum, d.plane = 0, 0
	d.held = -1
}

func (d *sdd1Dec) inBit() int {
	b := (d.cur >> (7 - d.bitPos)) & 1
	d.bitPos++
	if d.bitPos == 8 {
		d.bitPos = 0
		d.addr = (d.addr + 1) & 0xffffff
		d.cur = d.readByte(d.addr)
	}
	return b
}

func (d *sdd1Dec) genBit(n int) int {
	M, L := &d.runMPS, &d.runLPS
	if M[n] == 0 && L[n] == 0 {
		if d.inBit() == 0 {
			M[n] = 1 << n
		} else {
			l := 0
			for i := 0; i < n; i++ {
				l |= d.inBit() << i
			}
			M[n] = int32(l ^ (1<<n - 1))
			L[n] = 1
		}
	}
	bit := 0
	if M[n] == 0 {
		bit = 1
	}
	if bit != 0 {
		L[n] = 0
	} else {
		M[n]--
	}
	d.endOfRun = M[n] == 0 && L[n] == 0
	return bit
}

func (d *sdd1Dec) probBit(ctx int) int {
	st, mps := d.state[ctx], int(d.mps[ctx])
	e := &sdd1Evolution[st]
	lps := d.genBit(int(e[0]))
	if d.endOfRun {
		if lps != 0 {
			d.state[ctx] = e[2]
			if st < 2 {
				d.mps[ctx] = uint8(mps ^ 1)
			}
		} else {
			d.state[ctx] = e[1]
		}
	}
	return lps ^ mps
}

func (d *sdd1Dec) planeOf(n int) int {
	switch d.planes {
	case 0x00:
		return 6 | n&1
	case 0x40:
		return (n>>6)&6 | n&1
	case 0x80:
		return 4 | (n>>6)&2 | n&1
	}
	return n & 7
}

func (d *sdd1Dec) modelBit() int {
	plane := d.planeOf(d.bitNum)
	d.plane = plane
	h := int(d.prev[plane])
	nb := sdd1Neighbours[d.ctxBits>>4]
	ctx := (plane&1)<<4 | (h>>5)&nb[0] | h&nb[1]
	bit := d.probBit(ctx)
	d.prev[plane] = uint16(h<<1 | bit)
	d.bitNum++
	return bit
}

func (d *sdd1Dec) next() uint8 {
	if d.planes == 0xc0 {
		v := 0
		for k := 0; k < 8; k++ {
			v |= d.modelBit() << k
		}
		return uint8(v)
	}
	if d.held >= 0 {
		v := d.held
		d.held = -1
		return uint8(v)
	}
	a, b := 0, 0
	for k := 7; k >= 0; k-- {
		a |= d.modelBit() << k
		b |= d.modelBit() << k
	}
	d.held = b
	return uint8(a)
}

func (d *sdd1Dec) save(s *sio) {
	s.Ints(&d.addr, &d.cur, &d.bitPos, &d.planes, &d.ctxBits, &d.bitNum, &d.plane, &d.held)
	s.Bool(&d.endOfRun)
	s.Bytes(d.mps[:])
	s.Bytes(d.state[:])
	s.I32s(d.runMPS[:])
	s.Bytes(d.runLPS[:])
	s.U16s(d.prev[:])
}

type sdd1 struct {
	s    *SNES
	c    *cart
	regs [8]uint8
	dec  sdd1Dec
}

func newSDD1(s *SNES) coproc {
	c := &sdd1{s: s, c: s.cart}
	c.dec.readByte = c.romByte
	s.claim(0x00, 0x3f, 0x4000, 0x4fff, true)
	s.claim(0x00, 0x3f, 0x8000, 0xffff, true)
	s.claim(0xc0, 0xff, 0x0000, 0xffff, false)
	c.reset()
	return c
}

func (c *sdd1) reset() { c.regs = [8]uint8{0, 0, 0, 0, 0, 1, 2, 3} }

func (c *sdd1) romByte(addr int) int {
	bank, off := (addr>>16)&0xff, addr&0xffff
	if bank >= 0xc0 {
		return c.c.romAt(int(c.regs[4+(bank>>4)&3]&7)<<20 | (bank&0x0f)<<16 | off)
	}
	if off >= 0x8000 {
		return c.c.romAt((bank&0x3f)<<15 | off&0x7fff)
	}
	v := c.c.read(bank, off)
	if v < 0 {
		return 0
	}
	return v
}

func (c *sdd1) read(bank, off int) int {
	if bank >= 0xc0 || (bank&0x40 == 0 && off >= 0x8000) {
		return c.romByte(bank<<16 | off)
	}
	if off >= 0x4800 && off < 0x4808 {
		return int(c.regs[off&7])
	}
	return -1
}

func (c *sdd1) write(bank, off int, v uint8) bool {
	if bank >= 0xc0 || (bank&0x40 == 0 && off >= 0x8000) {
		return true
	}
	if off >= 0x4800 && off < 0x4808 {
		c.regs[off&7] = v
		return true
	}
	return false
}

func (c *sdd1) dmaFeed(ch, addr, count int) func() uint8 {
	bit := uint8(1) << ch
	if c.regs[0]&c.regs[1]&bit == 0 {
		return nil
	}
	c.regs[1] &^= bit
	c.dec.start(addr)
	return c.dec.next
}

func (c *sdd1) state(s *sio) {
	s.Bytes(c.regs[:])
	c.dec.save(s)
}
