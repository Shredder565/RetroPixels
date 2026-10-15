package atari7800

import (
	"fmt"
	"math/rand/v2"
)

// Test cartridges made by the tests themselves (no game files are kept in the
// repository): a program that builds display lists in RAM and then, frame
// after frame, pokes random values into MARIA, the TIA's sound, the POKEYs,
// the bank switches, the RAM at $4000 and its own display lists, reads the
// controllers, timers and POKEY's random numbers into the colours, and sets
// display-list interrupts, so every part of the machine shows in the picture
// or the sound.

// asm is a tiny 6502 assembler.
type asm struct {
	b      []byte
	org    int
	labels map[string]int
	fix    []asmFix
}

type asmFix struct {
	at    int
	label string
	rel   bool
	part  int // 0 whole address, 1 low byte, 2 high byte
}

func newAsm(org int) *asm { return &asm{org: org, labels: map[string]int{}} }

func (p *asm) pc() int             { return p.org + len(p.b) }
func (p *asm) op(b ...byte) *asm   { p.b = append(p.b, b...); return p }
func (p *asm) label(l string) *asm { p.labels[l] = p.pc(); return p }
func (p *asm) lda(v int) *asm      { return p.op(0xa9, byte(v)) }
func (p *asm) sta(a int) *asm      { return p.op(0x8d, byte(a), byte(a>>8)) }
func (p *asm) ldaAbs(a int) *asm   { return p.op(0xad, byte(a), byte(a>>8)) }
func (p *asm) poke(a, v int) *asm  { return p.lda(v).sta(a) }
func (p *asm) br(op byte, l string) *asm {
	p.op(op, 0)
	p.fix = append(p.fix, asmFix{len(p.b) - 1, l, true, 0})
	return p
}
func (p *asm) abs(op byte, l string) *asm {
	p.op(op, 0, 0)
	p.fix = append(p.fix, asmFix{len(p.b) - 2, l, false, 0})
	return p
}

// ldaPart: lda #<label (part 1) or lda #>label (part 2)
func (p *asm) ldaPart(l string, part int) *asm {
	p.op(0xa9, 0)
	p.fix = append(p.fix, asmFix{len(p.b) - 1, l, false, part})
	return p
}

func (p *asm) link() []byte {
	for _, f := range p.fix {
		a, ok := p.labels[f.label]
		if !ok {
			panic("no label " + f.label)
		}
		switch {
		case f.part == 1:
			p.b[f.at] = byte(a)
		case f.part == 2:
			p.b[f.at] = byte(a >> 8)
		case f.rel:
			d := a - (p.org + f.at + 1)
			if d < -128 || d > 127 {
				panic("branch too far: " + f.label)
			}
			p.b[f.at] = byte(d)
		default:
			p.b[f.at], p.b[f.at+1] = byte(a), byte(a>>8)
		}
	}
	return p.b
}

// exCart describes a test cartridge board.
type exCart struct {
	name     string
	header   int // 0 none, 3 or 4
	mapper   int
	size     int    // ROM bytes (one image; bankset boards hold two)
	ram      string // "", "ram", "mram", "hram", "bankram"
	rom4000  string // "", "rom", "bank6"
	pokeys   []int
	bankset  bool
	v3bits   int // extra version 3 type bits
	v4zero   bool
	ffffType bool // version 4 header with $FFFF in the old type bytes
}

// exCarts: every kind of board the core knows, with each kind of header.
func exCarts() []exCart {
	return []exCart{
		{name: "flat32-v3", header: 3, mapper: mapFlat, size: 32 << 10},
		{name: "flat48-v4-pokey450", header: 4, mapper: mapFlat, size: 48 << 10, pokeys: []int{0x450}},
		{name: "flat16-noheader", mapper: mapFlat, size: 16 << 10},
		{name: "flat32-noheader", mapper: mapFlat, size: 32 << 10},
		{name: "flat32-v4-ffff", header: 4, mapper: mapFlat, size: 32 << 10, ffffType: true},
		{name: "flat32-v4zero-pokey440", header: 4, v4zero: true, mapper: mapFlat, size: 32 << 10, pokeys: []int{0x440}},
		{name: "flat32-v3-pokey4000", header: 3, mapper: mapFlat, size: 32 << 10, pokeys: []int{0x4000}},
		{name: "flat32-v4-ram", header: 4, mapper: mapFlat, size: 32 << 10, ram: "ram"},
		{name: "sg128-v3", header: 3, mapper: mapSuperGame, size: 128 << 10},
		{name: "sg128-v3-ram", header: 3, mapper: mapSuperGame, size: 128 << 10, ram: "ram"},
		{name: "sg128-v3-bankram", header: 3, mapper: mapSuperGame, size: 128 << 10, ram: "bankram"},
		{name: "sg128-v4-bankram-pokey450", header: 4, mapper: mapSuperGame, size: 128 << 10, ram: "bankram", pokeys: []int{0x450}},
		{name: "sg128-v4-mram", header: 4, mapper: mapSuperGame, size: 128 << 10, ram: "mram"},
		{name: "sg128-v3-mram", header: 3, mapper: mapSuperGame, size: 128 << 10, ram: "mram"},
		{name: "sg256-v4-hram", header: 4, mapper: mapSuperGame, size: 256 << 10, ram: "hram"},
		{name: "sg128-v3-hram", header: 3, mapper: mapSuperGame, size: 128 << 10, ram: "hram"},
		{name: "sg144-v3-rom4000", header: 3, mapper: mapSuperGame, size: 144 << 10, rom4000: "rom"},
		{name: "sg144-v4-rom4000-pokey800", header: 4, mapper: mapSuperGame, size: 144 << 10, rom4000: "rom", pokeys: []int{0x800}},
		{name: "sg128-v4-bank6", header: 4, mapper: mapSuperGame, size: 128 << 10, rom4000: "bank6"},
		{name: "sg128-v3-bank6", header: 3, mapper: mapSuperGame, size: 128 << 10, rom4000: "bank6"},
		{name: "sg128-noheader", mapper: mapSuperGame, size: 128 << 10},
		{name: "sg144-noheader", mapper: mapSuperGame, size: 144 << 10},
		{name: "sg64-noheader", mapper: mapSuperGame, size: 64 << 10},
		{name: "sg128-v4-pokey4000", header: 4, mapper: mapSuperGame, size: 128 << 10, pokeys: []int{0x4000}},
		{name: "sg128-v3-pokey4000-ram", header: 3, mapper: mapSuperGame, size: 128 << 10, pokeys: []int{0x4000}, ram: "ram"},
		{name: "sg128-v4-2pokeys", header: 4, mapper: mapSuperGame, size: 128 << 10, pokeys: []int{0x440, 0x450}},
		{name: "sg128-v3-2pokeys", header: 3, mapper: mapSuperGame, size: 128 << 10, pokeys: []int{0x450, 0x440}},
		{name: "sg128-v3-pokey800-ym", header: 3, mapper: mapSuperGame, size: 128 << 10, pokeys: []int{0x800}, v3bits: 0x0800},
		{name: "activision-v3", header: 3, mapper: mapActivision, size: 128 << 10},
		{name: "activision-v4", header: 4, mapper: mapActivision, size: 128 << 10},
		{name: "absolute-v3", header: 3, mapper: mapAbsolute, size: 64 << 10},
		{name: "absolute-v4-pokey450", header: 4, mapper: mapAbsolute, size: 64 << 10, pokeys: []int{0x450}},
		{name: "bankset-flat32-v4", header: 4, mapper: mapFlat, size: 32 << 10, bankset: true},
		{name: "bankset-sg128-v3-pokey450", header: 3, mapper: mapSuperGame, size: 128 << 10, bankset: true, pokeys: []int{0x450}},
		{name: "bankset-sg128-v4-hram", header: 4, mapper: mapSuperGame, size: 128 << 10, bankset: true, ram: "hram"},
		{name: "souper-v4", header: 4, mapper: mapSouper, size: 128 << 10},
		{name: "souper-v3", header: 3, mapper: mapSouper, size: 128 << 10},
	}
}

// a78Header makes the 128-byte .a78 header for a board.
func a78Header(c exCart, romLen int) []byte {
	h := make([]byte, 128)
	h[0] = byte(c.header)
	copy(h[1:], "ATARI7800")
	copy(h[17:], "RetroPixels test "+c.name)
	h[49], h[50], h[51], h[52] = byte(romLen>>24), byte(romLen>>16), byte(romLen>>8), byte(romLen)
	copy(h[100:], "ACTUAL CART DATA STARTS HERE")
	t := c.v3bits
	switch c.mapper {
	case mapSuperGame:
		t |= 0x0002
	case mapActivision:
		t |= 0x0100
	case mapAbsolute:
		t |= 0x0200
	case mapSouper:
		t |= 0x1000
	}
	switch c.ram {
	case "ram":
		t |= 0x0004
	case "bankram":
		t |= 0x0020
	case "mram":
		t |= 0x0080
	case "hram":
		t |= 0x4000
	}
	switch c.rom4000 {
	case "rom":
		t |= 0x0008
	case "bank6":
		t |= 0x0010
	}
	if c.bankset {
		t |= 0x2000
	}
	for _, at := range c.pokeys {
		t |= map[int]int{0x4000: 0x0001, 0x450: 0x0040, 0x440: 0x0400, 0x800: 0x8000}[at]
	}
	h[53], h[54] = byte(t>>8), byte(t)
	if c.header >= 4 && !c.v4zero {
		if c.ffffType {
			h[53], h[54] = 0xff, 0xff
		}
		h[64] = byte(c.mapper)
		var o byte
		switch c.ram {
		case "ram":
			o = 1
		case "mram":
			o = 2
		case "hram":
			o = 3
		case "bankram":
			o = 6
		}
		switch c.rom4000 {
		case "rom":
			o = 4
		case "bank6":
			o = 5
		}
		if c.bankset {
			o |= 0x80
		}
		h[65] = o
		au := 0
		has := func(a int) bool {
			for _, x := range c.pokeys {
				if x == a {
					return true
				}
			}
			return false
		}
		switch {
		case has(0x440) && has(0x450):
			au = 3
		case has(0x440):
			au = 1
		case has(0x450):
			au = 2
		case has(0x800):
			au = 4
		case has(0x4000):
			au = 5
		}
		if c.v3bits&0x0800 != 0 {
			au |= 8
		}
		h[67] = byte(au)
	}
	return h
}

// exerciserROM builds a test cartridge (with its header, when it has one).
func exerciserROM(c exCart, seed uint64) []byte {
	rng := rand.New(rand.NewPCG(seed, uint64(len(c.name))*7919+uint64(c.mapper)))
	rn := func(n int) int { return rng.IntN(n) }
	rom := make([]byte, c.size)
	for i := range rom {
		// graphics: a mix of solid runs and noise
		if (i>>6)&3 == 0 {
			rom[i] = byte(rn(256))
		} else {
			rom[i] = byte(0x55 * ((i >> 8) & 3))
			if rn(5) == 0 {
				rom[i] = byte(rn(256))
			}
		}
	}

	// Where the program goes ($E000-$FFF9 on every board) and where that is in the ROM.
	const codeOrg = 0xe000
	var codeOff int
	switch c.mapper {
	case mapFlat:
		codeOff = len(rom) - 0x2000
	case mapSuperGame:
		codeOff = (len(rom)>>14-1)*0x4000 + 0x2000
	case mapActivision:
		codeOff = 0x1c000
	case mapAbsolute:
		codeOff = 0xe000
	default:
		codeOff = len(rom) - 0x2000
	}

	// Display lists, built in ROM and copied to RAM at $1800 at reset.
	const dllRAM = 0x1800
	tbl := make([]byte, 0, 0x800)
	var hposAt, pwAt []int // RAM addresses of hpos / palette-width bytes
	nz := 0
	lines := 0
	zoneHeights := []int{}
	for lines < 256 {
		h := 1 + rn(16)
		if rn(3) == 0 {
			h = 1 + rn(4)
		}
		zoneHeights = append(zoneHeights, h)
		lines += h
		nz++
	}
	dlStart := dllRAM + nz*3
	dls := []byte{}
	dlAddr := make([]int, nz)
	hiChoices := func() int {
		switch rn(10) {
		case 0:
			return 0x18 + rn(16) // RAM
		case 1:
			return 0x40 + rn(64) // $4000 area: RAM, ROM or a POKEY
		case 2:
			return rn(0x18) // TIA / MARIA / RIOT area
		case 3, 4, 5:
			return 0x80 + rn(64) // banked ROM
		default:
			return 0xa0 + rn(96)
		}
	}
	for z := 0; z < nz; z++ {
		dlAddr[z] = dlStart + len(dls)
		n := rn(7)
		for e := 0; e < n; e++ {
			lo, hi := rn(256), hiChoices()
			pal := rn(8)
			wcode := rn(32)
			hpos := rn(256)
			if rn(3) == 0 { // 5-byte header
				mode := 0x40
				if rn(2) == 0 {
					mode |= 0x80 // write mode
				}
				if rn(3) == 0 {
					mode |= 0x20 // indirect (character) mode
					wcode = 32 - (1 + rn(12))
				}
				dls = append(dls, byte(lo), byte(mode), byte(hi))
				pwAt = append(pwAt, dlStart+len(dls))
				dls = append(dls, byte(pal<<5|wcode))
				hposAt = append(hposAt, dlStart+len(dls))
				dls = append(dls, byte(hpos))
			} else {
				if wcode == 0 {
					wcode = 1 + rn(31)
				}
				dls = append(dls, byte(lo))
				pwAt = append(pwAt, dlStart+len(dls))
				dls = append(dls, byte(pal<<5|wcode), byte(hi))
				hposAt = append(hposAt, dlStart+len(dls))
				dls = append(dls, byte(hpos))
			}
		}
		dls = append(dls, 0, 0)
	}
	for z := 0; z < nz; z++ {
		b0 := zoneHeights[z] - 1
		if rn(8) == 0 {
			b0 |= 0x80 // DLI
		}
		if rn(5) == 0 {
			b0 |= 0x40 // holey DMA, 16 lines
		}
		if rn(5) == 0 {
			b0 |= 0x20 // holey DMA, 8 lines
		}
		tbl = append(tbl, byte(b0), byte(dlAddr[z]>>8), byte(dlAddr[z]))
	}
	tbl = append(tbl, dls...)
	if len(tbl) > 0x800 {
		tbl = tbl[:0x800]
	}
	pages := (len(tbl) + 255) >> 8
	for len(tbl) < pages*256 {
		tbl = append(tbl, 0)
	}

	p := newAsm(codeOrg)
	// reset
	p.label("reset").op(0x78, 0xd8, 0xa2, 0xff, 0x9a) // sei cld ldx #$ff txs
	p.poke(0x3c, 0x60)                                // MARIA DMA off while setting up
	// copy the display lists to RAM
	p.ldaPart("table", 1).op(0x85, 0xf0).ldaPart("table", 2).op(0x85, 0xf1)
	p.op(0xa9, 0x00, 0x85, 0xf2, 0xa9, byte(dllRAM>>8), 0x85, 0xf3)
	p.op(0xa2, byte(pages), 0xa0, 0x00)
	p.label("copy").op(0xb1, 0xf0, 0x91, 0xf2, 0xc8).br(0xd0, "copy")
	p.op(0xe6, 0xf1, 0xe6, 0xf3, 0xca).br(0xd0, "copy")
	p.poke(0x2c, dllRAM>>8).poke(0x30, dllRAM&0xff)
	for r := 0x20; r < 0x40; r++ {
		if r&3 != 0 || r == 0x20 {
			p.poke(r, rn(256))
		}
	}
	p.poke(0x34, 0x80+rn(0x60))
	p.poke(0x3c, 0x40)
	p.lda(0).op(0x85, 0x80, 0x85, 0x81)

	// the frames
	maria := []int{0x20}
	for r := 0x21; r < 0x40; r++ {
		if r&3 != 0 {
			maria = append(maria, r)
		}
	}
	reads := []int{0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x28, 0x280, 0x282, 0x284, 0x285, 0x480 + rn(128), 0x4000 + rn(0x4000), 0x8000 + rn(0x4000), 0x1800 + rn(0x1000)}
	for _, a := range c.pokeys {
		if a == 0x4000 {
			reads = append(reads, 0x400a+16*rn(0x3ff))
		} else {
			reads = append(reads, a+10)
		}
	}
	colours := maria[1:]
	segs := 30
	for s := 0; s < segs; s++ {
		ls := fmt.Sprintf("seg%d", s)
		p.label(ls)
		// wait for the start of vertical blank
		p.label(ls+"a").op(0x2c, 0x28, 0x00).br(0x30, ls+"a")
		p.label(ls+"b").op(0x2c, 0x28, 0x00).br(0x10, ls+"b")
		p.op(0xe6, 0x80) // frame counter
		ops := 10 + rn(30)
		for o := 0; o < ops; o++ {
			switch k := rn(100); {
			case k < 15:
				p.poke(colours[rn(len(colours))], rn(256))
			case k < 18:
				p.poke(0x20, rn(256))
			case k < 22:
				v := rn(0x20) | 0x40 | (rn(2) << 7)
				if rn(8) == 0 {
					v = 0x60 | rn(0x20) // DMA off
				}
				p.poke(0x3c, v)
			case k < 24:
				p.poke(0x34, rn(256))
			case k < 32:
				p.poke(0x15+rn(6), rn(256))
			case k < 42:
				if len(c.pokeys) > 0 {
					at := c.pokeys[rn(len(c.pokeys))]
					r := rn(10)
					v := rn(256)
					if r == 8 {
						v &= 0xf9 | rn(256)
					}
					p.poke(at+r, v)
				} else {
					p.poke(0x24, 0)
				}
			case k < 47:
				p.poke(0x24, 0) // WSYNC
			case k < 57:
				p.ldaAbs(reads[rn(len(reads))])
				if rn(2) == 0 || len(hposAt) == 0 {
					p.sta(colours[rn(len(colours))])
				} else {
					p.sta(hposAt[rn(len(hposAt))])
				}
			case k < 64:
				if len(hposAt) > 0 {
					p.op(0xa5, 0x80, 0x65, 0x81).sta(hposAt[rn(len(hposAt))]) // lda frame; adc $81
				}
			case k < 68:
				if len(pwAt) > 0 {
					w := rn(8)<<5 | (1 + rn(31))
					p.poke(pwAt[rn(len(pwAt))], w)
				}
			case k < 72:
				p.poke(dllRAM+nz*3+rn(len(dls)), rn(256))
			case k < 74:
				p.poke(dllRAM+rn(nz)*3, rn(256)&0xef)
			case k < 82:
				switch c.mapper {
				case mapSuperGame:
					p.poke(0x8000+rn(0x4000), rn(256))
				case mapActivision:
					p.poke(0xff80+rn(16), rn(256))
				case mapAbsolute:
					p.poke(0x8000, rn(4))
				default:
					p.poke(0x8000+rn(0x8000), rn(256)) // ROM: nothing
				}
			case k < 87:
				p.poke(0x4000+rn(0x4000), rn(256))
			case k < 89:
				p.poke(0x01, rn(256)) // VBLANK: button latches
			case k < 91:
				p.poke(0x294+rn(4), rn(256)) // RIOT timer
			case k < 93:
				p.poke(0x480+rn(128), rn(256))
			case k < 95:
				// joystick moves a display-list entry
				if len(hposAt) > 0 {
					p.op(0xad, 0x80, 0x02, 0x49, 0xff, 0x18, 0x65, 0x81, 0x85, 0x81).sta(hposAt[rn(len(hposAt))])
				}
			case k < 97:
				// buttons skip some writes
				sk := fmt.Sprintf("s%d_%d", s, o)
				p.ldaAbs([]int{0x0c, 0x0d, 0x08, 0x09, 0x282}[rn(5)]).br([]byte{0x30, 0x10, 0xd0, 0xf0}[rn(4)], sk)
				p.poke(colours[rn(len(colours))], rn(256)).poke(0x15+rn(6), rn(256))
				p.label(sk)
			case k < 98:
				// the display list list: RAM, or the copy in ROM
				if rn(2) == 0 {
					p.ldaPart("table", 2).sta(0x2c).ldaPart("table", 1).sta(0x30)
				} else {
					p.poke(0x2c, dllRAM>>8).poke(0x30, dllRAM&0xff)
				}
			default:
				p.poke(0x3c, 0x40|rn(0x20)) // all four graphics modes, kangaroo, char width
			}
		}
	}
	p.abs(0x4c, "seg0")
	// display-list interrupt: a few colour changes
	p.label("nmi").op(0x48).op(0xe6, 0x81, 0xa5, 0x81).sta(0x20)
	for i := 0; i < 1+rn(3); i++ {
		p.poke(colours[rn(len(colours))], rn(256))
	}
	if rn(2) == 0 {
		p.poke(0x24, 0)
	}
	p.op(0x68, 0x40)
	p.label("irq").op(0x40)
	p.label("table")
	p.op(tbl...)
	if p.pc() > 0xff80 {
		panic(fmt.Sprintf("test program too long: %d bytes", len(p.b)))
	}
	code := p.link()
	copy(rom[codeOff:], code)
	vec := codeOff + 0x1ffa
	nmi, reset, irq := p.labels["nmi"], p.labels["reset"], p.labels["irq"]
	rom[vec], rom[vec+1] = byte(nmi), byte(nmi>>8)
	rom[vec+2], rom[vec+3] = byte(reset), byte(reset>>8)
	rom[vec+4], rom[vec+5] = byte(irq), byte(irq>>8)

	if c.bankset {
		// MARIA's image: the same display lists, other graphics
		m := make([]byte, len(rom))
		for i := range m {
			m[i] = byte(rn(256)) &^ byte(rn(256))
		}
		copy(m[codeOff:], code)
		rom = append(rom, m...)
	}
	if c.header == 0 {
		return rom
	}
	return append(a78Header(c, len(rom)), rom...)
}
