package snes

import "strings"

// The cartridge: ROM, battery RAM and the LoROM / HiROM / ExHiROM layouts
// (Cart in web/emu/snes.js).

const (
	mapLo = iota
	mapHi
	mapExHi
)

type cart struct {
	rom      []byte
	size     int
	headerAt int
	mapping  int
	title    string
	chip     int
	sramSize int
	pal      bool
	sram     []byte
	sramMask int
	sa1      *sa1 // the SA-1 chip takes over the cartridge bus
}

func newCart(romIn []byte, saved []byte) *cart {
	rom := romIn
	if len(rom)&0x3ff == 0x200 {
		rom = rom[0x200:] // copier header
	}
	c := &cart{rom: append([]byte(nil), rom...)}
	c.size = len(c.rom)
	score := func(base int) int {
		if base+0x40 > c.size {
			return -100
		}
		r := c.rom
		s := 0
		chk, cmp := int(r[base+0x1e])|int(r[base+0x1f])<<8, int(r[base+0x1c])|int(r[base+0x1d])<<8
		if chk^cmp == 0xffff {
			s += 4
		}
		mode := int(r[base+0x15])
		reset := int(r[base+0x3c]) | int(r[base+0x3d])<<8
		if reset >= 0x8000 {
			s += 3
		} else {
			s -= 4
		}
		if base == 0x7fc0 && mode&0xef == 0x20 {
			s += 2
		}
		if base == 0xffc0 && mode&0xef == 0x21 {
			s += 2
		}
		if base == 0x40ffc0 && mode&0xef == 0x25 {
			s += 3
		}
		if base == 0x7fc0 && mode == 0x23 {
			s += 2
		}
		printable := 0
		for i := 0; i < 21; i++ {
			if ch := r[base+i]; ch >= 0x20 && ch < 0x7f {
				printable++
			}
		}
		if printable > 16 {
			s++
		}
		at := base&^0x7fff + reset&0x7fff
		op := -1
		if at < len(r) {
			op = int(r[at])
		}
		switch op {
		case 0x78, 0x18, 0x38, 0x9c, 0x4c, 0x5c:
			s += 2
		}
		return s
	}
	type cand struct{ s, base, m int }
	cands := []cand{{score(0x7fc0), 0x7fc0, mapLo}, {score(0xffc0), 0xffc0, mapHi}, {score(0x40ffc0), 0x40ffc0, mapExHi}}
	// a stable sort by score, highest first (as Array.prototype.sort)
	for i := 1; i < len(cands); i++ {
		for j := i; j > 0 && cands[j].s > cands[j-1].s; j-- {
			cands[j], cands[j-1] = cands[j-1], cands[j]
		}
	}
	c.headerAt, c.mapping = cands[0].base, cands[0].m
	h := c.headerAt
	var t []byte
	for i := h; i < h+21 && i < c.size; i++ {
		b := c.rom[i]
		if b < 0x20 || b > 0x7e {
			b = ' '
		}
		t = append(t, b)
	}
	c.title = strings.TrimSpace(string(t))
	// (bytes past the end of a tiny ROM read as 0, like JS's undefined here)
	c.chip = c.hdr(0x16)
	if k := c.hdr(0x18); k != 0 {
		// JS shifts in 32 bits: a nonsense size byte gives no (or no usable) RAM
		if v := int(int32(uint32(0x400) << (k & 31))); v > 0 {
			c.sramSize = min(0x20000, v)
		}
	}
	p := c.hdr(0x19)
	c.pal = p >= 2 && p <= 12
	c.sram = make([]byte, max(c.sramSize, 1))
	for i := range c.sram {
		c.sram[i] = 0xff
	}
	if len(saved) > 0 && c.sramSize > 0 {
		copy(c.sram, saved[:min(len(saved), c.sramSize)])
	}
	if c.sramSize > 0 {
		c.sramMask = c.sramSize - 1
	}
	return c
}

// header byte at headerAt+i (0 past the end of the ROM, as JS's undefined | 0)
func (c *cart) hdr(i int) int {
	if a := c.headerAt + i; a >= 0 && a < len(c.rom) {
		return int(c.rom[a])
	}
	return 0
}

// special names a cartridge chip the core can't run, or "".
func (c *cart) special() string {
	if e := findChip(c); e != nil {
		if e.create != nil {
			return ""
		}
		return e.name
	}
	ch := c.chip
	switch {
	case ch == 0x03 || ch == 0x05:
		return "DSP-1"
	case ch >= 0x13 && ch <= 0x1a:
		return "Super FX"
	case ch == 0x34 || ch == 0x35:
		return "SA-1"
	case ch == 0x43 || ch == 0x45:
		return "S-DD1"
	case ch == 0xf5 || ch == 0xf9:
		return "SPC7110"
	case ch == 0xf3:
		return "Cx4"
	}
	return ""
}

func (c *cart) romAt(off int) int {
	n := c.size
	if off < n {
		return int(c.rom[off])
	}
	if n == 0 {
		return 0
	}
	// mirror: drop the highest bits until it fits
	base, size, a, mask := 0, n, off&0xffffff, 0x800000
	for a >= size {
		for a&mask == 0 {
			mask >>= 1
		}
		a -= mask
		if size > mask {
			size -= mask
			base += mask
		}
		mask >>= 1
	}
	if i := base + a; i < len(c.rom) {
		return int(c.rom[i])
	}
	return 0
}

// read returns -1 when the address isn't cartridge ROM / RAM.
func (c *cart) read(bank, off int) int {
	if c.sa1 != nil {
		return c.sa1.snesRead(bank, off)
	}
	return c.readPlain(bank, off)
}

func (c *cart) readPlain(bank, off int) int {
	if c.mapping == mapLo {
		b := bank & 0x7f
		if b >= 0x70 && b < 0x7e && off < 0x8000 && c.sramSize != 0 {
			return int(c.sram[((bank&0xf)<<15|off)&c.sramMask])
		}
		if off >= 0x8000 || b >= 0x40 {
			return c.romAt(b<<15 | off&0x7fff)
		}
		return -1
	}
	b := bank & 0x7f
	if b < 0x40 && off >= 0x6000 && off < 0x8000 {
		if c.sramSize != 0 {
			return int(c.sram[((bank&0x1f)<<13|(off-0x6000))&c.sramMask])
		}
		return -1
	}
	if c.mapping == mapExHi {
		if bank >= 0xc0 {
			return c.romAt((bank&0x3f)<<16 | off)
		}
		if bank >= 0x40 && bank < 0x7e {
			return c.romAt(0x400000 | (bank&0x3f)<<16 | off)
		}
		if off >= 0x8000 {
			hi := 0
			if bank < 0x80 {
				hi = 0x400000
			}
			return c.romAt(hi | (bank&0x3f)<<16 | off)
		}
		return -1
	}
	if b >= 0x40 || off >= 0x8000 {
		return c.romAt((bank&0x3f)<<16 | off)
	}
	return -1
}

func (c *cart) write(bank, off int, v uint8) {
	if c.sa1 != nil {
		c.sa1.snesWrite(bank, off, v)
		return
	}
	if c.sramSize == 0 {
		return
	}
	i := -1
	if c.mapping == mapLo {
		if b := bank & 0x7f; b >= 0x70 && b < 0x7e && off < 0x8000 {
			i = ((bank&0xf)<<15 | off) & c.sramMask
		}
	} else if bank&0x7f < 0x40 && off >= 0x6000 && off < 0x8000 {
		i = ((bank&0x1f)<<13 | (off - 0x6000)) & c.sramMask
	}
	if i >= 0 {
		c.sram[i] = v
	}
}
