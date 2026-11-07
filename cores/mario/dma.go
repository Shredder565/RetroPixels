package mario

// dma is the Z80 DMA chip as the game uses it: programmed through port 0 with
// a memory-to-memory block copy, which runs when the board's ready line
// (written at 7e85) goes high.
type dma struct {
	wr      [7]int // the base byte last written to each write register
	addrA   int
	addrB   int
	length  int
	follow  []int // the follow-up bytes still expected: register*16+what
	enabled bool
	srcA    bool // port A is the source
	stepA   int  // address steps: +1, -1 or 0
	stepB   int
	curA    int
	curB    int
}

// what a follow-up byte sets
const (
	fAddrALo = iota
	fAddrAHi
	fLenLo
	fLenHi
	fAddrBLo
	fAddrBHi
	fIgnore
)

func (d *dma) write(v int) {
	if len(d.follow) > 0 {
		f := d.follow[0]
		d.follow = d.follow[1:]
		switch f {
		case fAddrALo:
			d.addrA = d.addrA&0xff00 | v
		case fAddrAHi:
			d.addrA = d.addrA&0xff | v<<8
		case fLenLo:
			d.length = d.length&0xff00 | v
		case fLenHi:
			d.length = d.length&0xff | v<<8
		case fAddrBLo:
			d.addrB = d.addrB&0xff00 | v
		case fAddrBHi:
			d.addrB = d.addrB&0xff | v<<8
		}
		return
	}
	step := func(v int) int {
		switch (v >> 4) & 3 {
		case 0:
			return -1
		case 1:
			return 1
		}
		return 0
	}
	more := func(bits ...int) {
		for i := 0; i < len(bits); i += 2 {
			if v&bits[i] != 0 {
				d.follow = append(d.follow, bits[i+1])
			}
		}
	}
	switch {
	case v&0x80 == 0 && v&3 != 0: // WR0: direction, port A address, length
		d.wr[0] = v
		d.srcA = v&4 != 0
		more(0x08, fAddrALo, 0x10, fAddrAHi, 0x20, fLenLo, 0x40, fLenHi)
	case v&0x87 == 0x04: // WR1: port A
		d.wr[1] = v
		d.stepA = step(v)
		more(0x40, fIgnore)
	case v&0x87 == 0x00: // WR2: port B
		d.wr[2] = v
		d.stepB = step(v)
		more(0x40, fIgnore)
	case v&0x83 == 0x80: // WR3: mask and match
		d.wr[3] = v
		more(0x08, fIgnore, 0x10, fIgnore)
		if v&0x40 != 0 {
			d.enabled = true
		}
	case v&0x83 == 0x81: // WR4: mode, port B address
		d.wr[4] = v
		more(0x04, fAddrBLo, 0x08, fAddrBHi, 0x10, fIgnore)
	case v&0xc7 == 0x82: // WR5: ready and control lines
		d.wr[5] = v
	case v&0x83 == 0x83: // WR6: commands
		d.wr[6] = v
		switch v {
		case 0xcf: // load
			d.curA, d.curB = d.addrA, d.addrB
		case 0x87: // enable
			d.enabled = true
		case 0x83: // disable
			d.enabled = false
		case 0xc3: // reset
			d.enabled = false
			d.follow = nil
		case 0xbb: // read mask follows
			d.follow = append(d.follow, fIgnore)
		}
	}
}

// runDMA: the block copy, done at once; the Z80 is held off the bus for the
// time it takes.
func (b *board) runDMA() {
	d := &b.dma
	if !d.enabled || d.wr[0]&3 == 0 {
		return
	}
	n := d.length + 1
	src, dst, ss, ds := d.curA, d.curB, d.stepA, d.stepB
	if !d.srcA {
		src, dst, ss, ds = d.curB, d.curA, d.stepB, d.stepA
	}
	for i := 0; i < n; i++ {
		b.write(uint16(dst), b.read(uint16(src)))
		src += ss
		dst += ds
	}
	if d.srcA {
		d.curA, d.curB = src, dst
	} else {
		d.curB, d.curA = src, dst
	}
	d.enabled = false
	b.stall += n * dmaByteCycles
}

// dmaByteCycles: Z80 cycles the DMA takes per byte copied.
const dmaByteCycles = 6
