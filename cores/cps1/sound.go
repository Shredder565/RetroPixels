package cps1

// The sound board: a Z80 with
//
//	0000-7fff ROM         8000-bfff banked ROM (16 KB banks, f004 bit 0)
//	d000-d7ff RAM         f000-f001 YM2151      f002 MSM6295
//	f004 bank select      f006 MSM6295 rate pin (not wired on these boards)
//	f008 sound command    f00a fade command
//
// The YM2151's timer interrupt is the Z80's interrupt. The YM2151 makes a
// sample every 64 Z80 cycles; the MSM6295 runs from 1 MHz / 132.

// Mixing levels for the two chips (the output is mono), measured against
// MAME's recording of the game.
var (
	fmLevel  = 0.35 / 32768
	okiLevel = 0.285 / 2048
)

func (s soundBus) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	switch {
	case a < 0x8000:
		return b.zrom[a]
	case a < 0xc000:
		o := 0x8000 + b.zbank*0x4000 + (a - 0x8000)
		if o < len(b.zrom) {
			return b.zrom[o]
		}
		return 0xff
	case a >= 0xd000 && a < 0xd800:
		return b.zram[a-0xd000]
	}
	switch a {
	case 0xf000, 0xf001:
		return b.ym.Read()
	case 0xf002:
		return b.oki.Read()
	case 0xf008:
		return b.latch
	case 0xf00a:
		return b.fade
	}
	return 0xff
}

func (s soundBus) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	if a >= 0xd000 && a < 0xd800 {
		b.zram[a-0xd000] = v
		return
	}
	switch a {
	case 0xf000, 0xf001:
		b.ym.Write(a&1, v)
	case 0xf002:
		b.oki.Write(v)
	case 0xf004:
		b.zbank = int(v & 1)
	}
}

func (s soundBus) In(uint16) uint8   { return 0xff }
func (s soundBus) Out(uint16, uint8) {}

// syncZ80 runs the sound CPU up to where the 68000 is.
func (b *board) syncZ80() {
	c := min(b.cpu.Cycles, int64(lines*lineCyc))
	b.runZ80To(float64(c) / lineCyc * z80Line)
}

func (b *board) runZ80To(target float64) {
	for b.zcyc < target {
		c := b.z80.Step()
		b.zcyc += float64(c)
		b.soundClocks(c)
	}
}

// soundClocks runs the sound chips for c Z80 cycles.
func (b *board) soundClocks(c int) {
	b.ymClk += c
	for b.ymClk >= 64 {
		b.ymClk -= 64
		b.ym.Tick()
		// a YM2151 sample lasts 64/z80Hz s and an MSM6295 sample
		// okiDiv/okiHz s: count both in units of 1/(okiHz*z80Hz) s
		b.okiAcc += 64 * okiHz
		for b.okiAcc >= okiDiv*z80Hz {
			b.okiAcc -= okiDiv * z80Hz
			b.okiOut = float64(b.oki.Sample())
		}
		v := float64(b.ym.Out[0]+b.ym.Out[1])*fmLevel + b.okiOut*okiLevel
		b.out.Add(v)
	}
}
