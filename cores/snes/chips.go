package snes

import (
	"fmt"
	"strings"
)

// Cartridge coprocessors (web/emu/snes_chips.js and the snes_*.js files that
// add to its list). A chip answers bus accesses on the 4 KB pages it claims.

type coproc interface {
	read(bank, off int) int            // a byte, or -1 to let the cartridge mapping answer
	write(bank, off int, v uint8) bool // true when the chip took the write
	reset()
	state(s *sio)
}

// sramChip is a coprocessor that keeps the battery save itself (its getSRAM,
// which may return nil).
type sramChip interface{ getSRAM() []byte }

// dmaFeeder is a coprocessor that can supply DMA data itself (S-DD1).
type dmaFeeder interface {
	dmaFeed(ch, addr, count int) func() uint8
}

type chipEntry struct {
	name   string
	detect func(c *cart) bool
	create func(s *SNES, opts *chipOpts) (coproc, error) // nil: not supported yet
}

// chipOpts: what the coprocessors get from core.Options.
type chipOpts struct {
	sram     []byte
	firmware map[string][]byte
}

// FirmwareError is returned when a cartridge chip needs its program, which
// the player supplies (the web page asks for it once and keeps it; the native
// app does the same). Pass the file in core.Options.Firmware[Chip].
type FirmwareError struct {
	Chip string // "DSP-3", "ST018", ...
	File string // the usual file name, "dsp3.rom"
	Size int    // its size in bytes
	Msg  string
}

func (e *FirmwareError) Error() string { return e.Msg }

// claim marks banks b0..b1 (and their $80 mirrors when mirror) at offsets o0..o1 as the chip's.
func (s *SNES) claim(b0, b1, o0, o1 int, mirror bool) {
	for b := b0; b <= b1; b++ {
		for p := o0 >> 12; p <= o1>>12; p++ {
			s.chipPages[b<<4|p] = true
			if mirror {
				s.chipPages[(b|0x80)<<4|p] = true
			}
		}
	}
}

func s16(x int) int { return int(int16(x)) }
func s8(x int) int  { return int(int8(x)) }

// raw header title bytes
func (c *cart) titleBytes() []byte {
	e := min(c.headerAt+21, len(c.rom))
	if c.headerAt >= e {
		return nil
	}
	return c.rom[c.headerAt:e]
}

func (c *cart) titleHas(text string) bool { return strings.Contains(strings.ToUpper(c.title), text) }
func (c *cart) mapMode() int              { return c.hdr(0x15) }

func isDSP(c *cart) bool { return c.chip >= 0x03 && c.chip <= 0x05 }

func dspKind(c *cart) int {
	if !isDSP(c) {
		return 0
	}
	if c.titleHas("DUNGEON MASTER") {
		return 2
	}
	t := c.titleBytes()
	if len(t) >= 2 && t[0] == 0x53 && t[1] == 0x44 && c.titleHas("GX") {
		return 3
	}
	if c.titleHas("TOP GEAR 3000") || c.titleHas("PLANETS CHAMP") {
		return 4
	}
	return 1
}

// chipList is RPSNESChips as the web page builds it (scripts loaded in the
// order web/index.html gives): snes_necdsp.js's entries first, then
// snes_chips.js's "not yet" names, then the others in load order.
var chipList []chipEntry

func init() {
	notYet := func(name string, d func(c *cart) bool) chipEntry { return chipEntry{name: name, detect: d} }
	chipList = []chipEntry{
		// snes_necdsp.js (unshift)
		{"DSP-3", func(c *cart) bool { return dspKind(c) == 3 }, func(s *SNES, o *chipOpts) (coproc, error) { return newDSPn(s, o, "DSP-3", 0x20, 0x3f) }},
		{"DSP-4", func(c *cart) bool { return dspKind(c) == 4 }, func(s *SNES, o *chipOpts) (coproc, error) { return newDSPn(s, o, "DSP-4", 0x30, 0x3f) }},
		{"ST010", func(c *cart) bool { return c.chip == 0xf6 && c.titleHas("F1 ROC") }, func(s *SNES, o *chipOpts) (coproc, error) { return newST01x(s, o, "ST010") }},
		{"ST011", func(c *cart) bool { return c.chip == 0xf6 }, func(s *SNES, o *chipOpts) (coproc, error) { return newST01x(s, o, "ST011") }},
		// snes_chips.js
		notYet("DSP-3", func(c *cart) bool { return dspKind(c) == 3 }),
		notYet("DSP-4", func(c *cart) bool { return dspKind(c) == 4 }),
		notYet("ST010", func(c *cart) bool { return c.chip == 0xf6 && c.titleHas("F1 ROC") }),
		notYet("ST011", func(c *cart) bool { return c.chip == 0xf6 }),
		// snes_dsp.js
		{"DSP-2", func(c *cart) bool { return dspKind(c) == 2 }, func(s *SNES, o *chipOpts) (coproc, error) { return newDSPChip(s, 2, o) }},
		{"DSP-1", func(c *cart) bool { return dspKind(c) == 1 }, func(s *SNES, o *chipOpts) (coproc, error) { return newDSPChip(s, 1, o) }},
		// snes_cx4.js
		{"Cx4", func(c *cart) bool { return c.chip == 0xf3 }, func(s *SNES, o *chipOpts) (coproc, error) { return newCx4(s), nil }},
		// snes_sdd1.js
		{"S-DD1", func(c *cart) bool { return c.chip == 0x43 || c.chip == 0x45 }, func(s *SNES, o *chipOpts) (coproc, error) { return newSDD1(s), nil }},
		// snes_spc7110.js
		{"SPC7110", func(c *cart) bool { return c.chip == 0xf9 || (c.chip == 0xf5 && c.mapMode() == 0x3a) }, func(s *SNES, o *chipOpts) (coproc, error) { return newSPC7110(s, o), nil }},
		// snes_misc.js
		{"OBC1", func(c *cart) bool { return c.chip == 0x25 }, func(s *SNES, o *chipOpts) (coproc, error) { return newOBC1(s), nil }},
		{"S-RTC", func(c *cart) bool { return c.chip == 0x55 }, func(s *SNES, o *chipOpts) (coproc, error) { return newSRTC(s, o), nil }},
		// snes_st018.js
		{"ST018", func(c *cart) bool {
			m := c.mapMode()
			lorom := m&0x0f == 0 || m&0x0f == 2 || c.mapping == mapLo
			return c.chip == 0xf5 && lorom && m != 0x3a
		}, func(s *SNES, o *chipOpts) (coproc, error) { return newST018(s, o) }},
	}
}

func findChip(c *cart) *chipEntry {
	for i := range chipList {
		if chipList[i].detect(c) {
			return &chipList[i]
		}
	}
	return nil
}

func errUnsupported(chip string) error {
	return fmt.Errorf("This game uses the %s chip inside the cartridge, which RetroPixels' SNES emulator doesn't support yet.", chip)
}
