package ddragon3

// The sound board: a Z80 with
//
//	0000-bfff ROM        c000-c7ff RAM       c800-c801 YM2151
//	d800 MSM6295         e000 sound command  e800 MSM6295 bank (256 KB)
//
// A sound command from the 68000 is a non-maskable interrupt; the program
// times its music by polling the YM2151's timer flags (its interrupt line
// is not used). The YM2151 makes a sample every 64 Z80 cycles; the MSM6295
// runs from 1.056 MHz / 132.

// Mixing levels for the two chips (the output is mono), measured against
// MAME's recording of the game.
var (
	fmLevel  = 0.233 / 32768
	okiLevel = 1.536 / 2048
)

func (s soundBus) Read(addr uint16) uint8 {
	b, a := s.b, int(addr)
	switch {
	case a < 0xc000:
		return b.zrom[a]
	case a < 0xc800:
		return b.zram[a-0xc000]
	case a == 0xc800 || a == 0xc801:
		return b.ym.Read()
	case a == 0xd800:
		return b.oki.Read()
	case a == 0xe000:
		return b.latch
	}
	return 0xff
}

func (s soundBus) Write(addr uint16, v uint8) {
	b, a := s.b, int(addr)
	switch {
	case a >= 0xc000 && a < 0xc800:
		b.zram[a-0xc000] = v
	case a == 0xc800 || a == 0xc801:
		b.ym.Write(a&1, v)
	case a == 0xd800:
		b.oki.Write(v)
	case a == 0xe800:
		b.setOkiBank(int(v))
	}
}

func (s soundBus) In(uint16) uint8   { return 0xff }
func (s soundBus) Out(uint16, uint8) {}

// syncZ80 runs the sound CPU up to where the 68000 is.
func (b *board) syncZ80() {
	b.runZ80To(float64(b.cpu.Cycles) / lineCyc * z80Line)
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
