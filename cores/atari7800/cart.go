package atari7800

import (
	"errors"
	"strings"
)

// Board kinds.
const (
	mapFlat = iota
	mapSuperGame
	mapActivision
	mapAbsolute
	mapSouper
)

var mapperNames = []string{"flat", "supergame", "activision", "absolute", "souper"}

// cartInfo is what the .a78 header says about the board.
type cartInfo struct {
	mapper  int
	ram     string // "", "ram", "mram", "hram", "bankram"
	rom4000 string // "", "rom", "bank6"
	pokeys  []int  // where the POKEYs are
	ym      bool
	bankset bool
	tv      int
	save    int
	header  bool
}

// parseCart reads the .a78 header. Version 3 headers use the "cart type" bits
// at bytes 53-54; version 4 headers have separate mapper, option and audio
// bytes at 64-67 (and may fill the old bits with $FFFF).
func parseCart(data []byte) (rom []byte, c cartInfo, name string) {
	rom = data
	if len(data) > 128 && string(data[1:10]) == "ATARI7800" {
		c.header = true
		var nb strings.Builder
		for i := 17; i < 49 && data[i] != 0; i++ {
			nb.WriteRune(rune(data[i]))
		}
		name = strings.TrimSpace(nb.String())
		rom = data[128:]
		ver, t := int(data[0]), int(data[53])<<8|int(data[54])
		c.tv = int(data[57] & 1)
		c.save = int(data[58])
		v4 := ver >= 4 && (t == 0xffff || data[64] != 0 || data[65] != 0 || data[66] != 0 || data[67] != 0)
		if v4 {
			if int(data[64]) < len(mapperNames) {
				c.mapper = int(data[64])
			}
			c.ram = [8]string{"", "ram", "mram", "hram", "", "", "bankram", ""}[data[65]&7]
			c.rom4000 = [8]string{"", "", "", "", "rom", "bank6", "", ""}[data[65]&7]
			c.bankset = data[65]&0x80 != 0
			au := data[67] & 7
			if au == 1 || au == 3 {
				c.pokeys = append(c.pokeys, 0x440)
			}
			if au == 2 || au == 3 {
				c.pokeys = append(c.pokeys, 0x450)
			}
			if au == 4 {
				c.pokeys = append(c.pokeys, 0x800)
			}
			if au == 5 {
				c.pokeys = append(c.pokeys, 0x4000)
			}
			c.ym = data[67]&8 != 0
		} else {
			switch {
			case t&0x0100 != 0:
				c.mapper = mapActivision
			case t&0x0200 != 0:
				c.mapper = mapAbsolute
			case t&0x1000 != 0:
				c.mapper = mapSouper
			case t&0x0002 != 0:
				c.mapper = mapSuperGame
			}
			if t&0x0004 != 0 {
				c.ram = "ram"
			}
			if t&0x0020 != 0 {
				c.ram = "bankram"
			}
			if t&0x0080 != 0 {
				c.ram = "mram"
			}
			if t&0x4000 != 0 {
				c.ram = "hram"
			}
			if t&0x0008 != 0 {
				c.rom4000 = "rom"
			}
			if t&0x0010 != 0 {
				c.rom4000 = "bank6"
			}
			if t&0x2000 != 0 {
				c.bankset = true
			}
			if t&0x0001 != 0 {
				c.pokeys = append(c.pokeys, 0x4000)
			}
			if t&0x0040 != 0 {
				c.pokeys = append(c.pokeys, 0x450)
			}
			if t&0x0400 != 0 {
				c.pokeys = append(c.pokeys, 0x440)
			}
			if t&0x8000 != 0 {
				c.pokeys = append(c.pokeys, 0x800)
			}
			c.ym = t&0x0800 != 0
		}
	}
	// Headerless dumps: guess from the size.
	if !c.header && len(rom) > 49152 {
		c.mapper = mapSuperGame
		if len(rom) == 147456 {
			c.rom4000 = "rom"
		}
	}
	return rom, c, name
}

type cartPokey struct {
	at   int
	chip *pokey
}

type cart struct {
	c          cartInfo
	rom, mrom  []byte // the processor's ROM and MARIA's (different on bankset boards)
	kind       int
	pokeys     []cartPokey
	pokey4000  *pokey
	ram4000    []byte // RAM at $4000, or nil
	ramMask    int
	ramBank    int
	bank       int
	banks      int
	extra      int // 144K boards: an extra first bank sits at $4000
	base       int // flat boards: where the ROM starts
	guessRam   bool
	now        func() int64 // processor clocks so far (set by the console)
	romMissing bool         // a read went past the end of the ROM
}

var errSouper = errors.New("This Atari 7800 game uses the SOUPER cartridge board, which RetroPixels doesn't support yet.")

func newCart(data []byte) (*cart, error) {
	rom, c, _ := parseCart(data)
	if c.mapper == mapSouper {
		return nil, errSouper
	}
	k := &cart{c: c, kind: c.mapper, rom: rom, mrom: rom}
	// Bankset boards hold two ROM images: the processor reads the first, MARIA the second.
	if c.bankset {
		k.rom = rom[:len(rom)>>1]
		k.mrom = rom[len(rom)>>1:]
	}
	for _, at := range c.pokeys {
		p := cartPokey{at, newPOKEY()}
		k.pokeys = append(k.pokeys, p)
		if at == 0x4000 && k.pokey4000 == nil {
			k.pokey4000 = p.chip
		}
	}
	// RAM at $4000: 16K, two switchable 16K banks, or 2K repeated through the 16K
	switch {
	case c.ram == "bankram":
		k.ram4000 = make([]byte, 0x8000)
	case c.ram == "mram":
		k.ram4000 = make([]byte, 0x800)
	case c.ram != "":
		k.ram4000 = make([]byte, 0x4000)
	}
	k.ramMask = 0x3fff
	if c.ram == "mram" {
		k.ramMask = 0x7ff
	}
	k.banks = max(1, len(k.rom)>>14)
	if c.rom4000 == "rom" {
		k.extra = 1
	}
	if k.kind == mapFlat {
		k.base = 0x10000 - len(k.rom)
	}
	k.now = func() int64 { return 0 }
	k.guessRam = k.kind == mapSuperGame && k.ram4000 == nil && k.extra == 0 && k.pokey4000 == nil && c.rom4000 != "bank6"
	return k, nil
}

// romAt reads the ROM; past its end (a board too small for its header) the
// web core reads "undefined", which this treats as 0.
func (k *cart) romAt(rom []byte, i int) uint8 {
	if i < 0 || i >= len(rom) {
		k.romMissing = true
		return 0
	}
	return rom[i]
}

// read: a is $4000-$FFFF; maria: read by MARIA's DMA (bankset boards show it the second ROM)
func (k *cart) read(a int, maria bool) uint8 {
	if k.pokey4000 != nil && a < 0x8000 && k.ram4000 == nil {
		return k.pokey4000.read(a, k.now())
	}
	if k.ram4000 != nil && a < 0x8000 {
		return k.ram4000[(k.ramBank<<14)|(a&k.ramMask)]
	}
	rom := k.rom
	if maria {
		rom = k.mrom
	}
	switch k.kind {
	case mapFlat:
		if a >= k.base {
			return k.romAt(rom, a-k.base)
		}
		return 0xff
	case mapSuperGame:
		if a >= 0xc000 {
			return k.romAt(rom, (k.banks-1)*0x4000+(a-0xc000))
		}
		if a >= 0x8000 {
			return k.romAt(rom, ((k.bank+k.extra)%k.banks)*0x4000+(a-0x8000))
		}
		if k.extra != 0 {
			return k.romAt(rom, a-0x4000)
		}
		return k.romAt(rom, max(0, k.banks-2)*0x4000+(a-0x4000))
	case mapActivision:
		// fixed 8K pieces, with a switchable 16K window at $A000-$DFFF
		if a >= 0xa000 && a < 0xe000 {
			return k.romAt(rom, k.bank*0x4000+(a-0xa000))
		}
		off := a & 0x1fff
		switch a & 0xe000 {
		case 0x4000:
			return k.romAt(rom, 0x1a000+off)
		case 0x6000:
			return k.romAt(rom, 0x18000+off)
		case 0x8000:
			return k.romAt(rom, 0x1e000+off)
		default:
			return k.romAt(rom, 0x1c000+off)
		}
	case mapAbsolute:
		if a < 0x8000 {
			return k.romAt(rom, k.bank*0x4000+(a-0x4000))
		}
		return k.romAt(rom, 0x8000+(a-0x8000))
	}
	return 0xff
}

func (k *cart) write(a int, v uint8) {
	if k.pokey4000 != nil && a < 0x8000 && k.ram4000 == nil {
		k.pokey4000.write(a, v)
		return
	}
	// Some SuperGame dumps don't say they have RAM at $4000; a write there
	// means it's present. (The web core fills it by reading through the new,
	// empty RAM, so it starts cleared.)
	if k.ram4000 == nil && k.guessRam && a < 0x8000 {
		k.ram4000 = make([]byte, 0x4000)
		k.ramMask = 0x3fff
	}
	if k.ram4000 != nil && a < 0x8000 {
		k.ram4000[(k.ramBank<<14)|(a&k.ramMask)] = v
		return
	}
	switch k.kind {
	case mapSuperGame:
		if a >= 0x8000 && a < 0xc000 {
			if k.c.ram == "bankram" {
				k.ramBank = int(v>>5) & 1
				k.bank = int(v & 0x1f)
			} else {
				k.bank = int(v)
			}
		}
	case mapActivision:
		if a >= 0xff80 && a <= 0xff8f {
			k.bank = a & 7
		}
	case mapAbsolute:
		if a == 0x8000 {
			if v&1 != 0 {
				k.bank = 0
			} else if v&2 != 0 {
				k.bank = 1
			}
		}
	}
}

// ioChip: the POKEY in the $0400-$0FFF area at a, or nil.
func (k *cart) ioChip(a int) *pokey {
	for _, p := range k.pokeys {
		if p.at != 0x4000 && a >= p.at && a < p.at+16 {
			return p.chip
		}
	}
	return nil
}

func (k *cart) save(w *stateW) {
	w.int(k.bank)
	w.int(k.ramBank)
	w.bool(k.ram4000 != nil)
	if k.ram4000 != nil {
		w.bytes(k.ram4000)
	}
	w.u32(uint32(len(k.pokeys)))
	for _, p := range k.pokeys {
		p.chip.save(w)
	}
}

// load restores a state. As in the web core, RAM the board found it had after
// the state was made (a SuperGame dump that didn't say) stays.
func (k *cart) load(r *stateR) {
	k.bank = r.int()
	k.ramBank = r.int()
	if r.bool() {
		ram := r.bytes()
		if r.err == nil && len(ram) != 0x800 && len(ram) != 0x4000 && len(ram) != 0x8000 {
			r.err = errBadState
		}
		if r.err == nil {
			if len(k.ram4000) != len(ram) {
				k.ram4000 = make([]byte, len(ram))
			}
			copy(k.ram4000, ram)
			k.ramMask = 0x3fff
			if len(ram) == 0x800 {
				k.ramMask = 0x7ff
			}
		}
	}
	if r.count(len(k.pokeys)) {
		for _, p := range k.pokeys {
			p.chip.load(r)
		}
	}
}

// clone copies the board's changing parts (for loading a state safely).
func (k *cart) clone() *cart {
	n := *k
	if k.ram4000 != nil {
		n.ram4000 = append([]byte(nil), k.ram4000...)
	}
	n.pokeys = make([]cartPokey, len(k.pokeys))
	n.pokey4000 = nil
	for i, p := range k.pokeys {
		c := *p.chip
		n.pokeys[i] = cartPokey{p.at, &c}
		if p.chip == k.pokey4000 {
			n.pokey4000 = n.pokeys[i].chip
		}
	}
	return &n
}
