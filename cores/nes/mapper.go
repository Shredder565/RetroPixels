package nes

// Cartridge boards (mappers): the common ones from web/emu/nes.js.

type mapper interface {
	b() *base
	cpuRead(a int) int // -1 = open bus
	cpuWrite(a, v int)
	ppuRead(a int) int
	ppuWrite(a, v int)
	cpuClock()
	scanline(line int)
	irq() bool
	state(s *sio)
}

// Optional parts of a board.
type (
	ntMapper interface { // owns the nametables
		ntRead(a int) int
		ntWrite(a, v int)
	}
	a12Mapper interface{ ppuAddr(a int) } // watches the PPU address bus
	vblanker  interface{ vblank() }
	resetter  interface{ reset() }
	batteryer interface { // battery save that isn't plain PRG RAM
		batteryData() []byte
		loadBattery(d []byte)
	}
)

// expansion sound on the cartridge
type extAudio interface {
	clock()
	output() float64
	state(s *sio)
}

type base struct {
	n      *NES
	cart   *cart
	prg    []byte
	chr    []byte
	chrRam bool
	prgRam []byte
	mirror int
	irqF   bool
	prgMap [4]int // 4 × 8 KB windows at $8000
	chrMap [8]int // 8 × 1 KB windows
	ntPgs  [4]int
	audio  extAudio

	// overrides of setPrg8 / setChr1 by derived boards (nil = plain)
	prg8Hook func(slot, bank int)
	chr1Hook func(slot, bank int)
}

func newBase(n *NES, c *cart) *base {
	m := &base{n: n, cart: c, prg: c.prg, chr: c.chr, chrRam: c.chrRam, prgRam: make([]byte, 0x2000), mirror: c.mirror}
	m.setPrg32(0)
	m.setChr8(0)
	return m
}

func (m *base) b() *base { return m }

func (m *base) cpuCycles() int64 { return m.n.cpu.Cycles }
func (m *base) openBus() int     { return m.n.openBus }

func (m *base) prgBanks8() int { return len(m.prg) >> 13 }
func (m *base) chrBanks1() int { return len(m.chr) >> 10 }
func wrap(bank, n int) int {
	if n <= 0 {
		return 0
	}
	return ((bank % n) + n) % n
}
func (m *base) setPrg8(slot, bank int) {
	if m.prg8Hook != nil {
		m.prg8Hook(slot, bank)
		return
	}
	m.rawPrg8(slot, bank)
}
func (m *base) rawPrg8(slot, bank int) { m.prgMap[slot] = wrap(bank, m.prgBanks8()) * 0x2000 }
func (m *base) setPrg16(slot, bank int) {
	m.setPrg8(slot*2, bank*2)
	m.setPrg8(slot*2+1, bank*2+1)
}
func (m *base) setPrg32(bank int) {
	for i := 0; i < 4; i++ {
		m.setPrg8(i, bank*4+i)
	}
}
func (m *base) setChr1(slot, bank int) {
	if m.chr1Hook != nil {
		m.chr1Hook(slot, bank)
		return
	}
	m.rawChr1(slot, bank)
}
func (m *base) rawChr1(slot, bank int) { m.chrMap[slot] = wrap(bank, m.chrBanks1()) * 0x400 }
func (m *base) setChr2(slot, bank int) {
	m.setChr1(slot*2, bank*2)
	m.setChr1(slot*2+1, bank*2+1)
}
func (m *base) setChr4(slot, bank int) {
	for i := 0; i < 4; i++ {
		m.setChr1(slot*4+i, bank*4+i)
	}
}
func (m *base) setChr8(bank int) {
	for i := 0; i < 8; i++ {
		m.setChr1(i, bank*8+i)
	}
}

// setNt maps each of the four nametables to a 1 KB page of nametable RAM.
func (m *base) setNt(a, b, c, d int) {
	m.mirror = mirCustom
	m.ntPgs = [4]int{a, b, c, d}
}

func (m *base) cpuRead(a int) int {
	if a >= 0x8000 {
		return int(m.prg[m.prgMap[(a>>13)&3]+a&0x1fff])
	}
	if a >= 0x6000 {
		return int(m.prgRam[a&0x1fff])
	}
	return -1
}
func (m *base) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		m.prgRam[a&0x1fff] = byte(v)
	}
}
func (m *base) ppuRead(a int) int { return int(m.chr[m.chrMap[a>>10]+a&0x3ff]) }
func (m *base) ppuWrite(a, v int) {
	if m.chrRam {
		m.chr[m.chrMap[a>>10]+a&0x3ff] = byte(v)
	}
}

// ntIndex: nametable address ($2000-$2FFF) -> index into the 4 KB VRAM
func (m *base) ntIndex(a int) int {
	t, off := (a>>10)&3, a&0x3ff
	switch m.mirror {
	case mirH:
		return (t>>1)<<10 | off
	case mirV:
		return (t&1)<<10 | off
	case mirOneLo:
		return off
	case mirOneHi:
		return 0x400 | off
	case mirCustom:
		return m.ntPgs[t]<<10 | off
	}
	return t<<10 | off
}
func (m *base) cpuClock()    {}
func (m *base) scanline(int) {}
func (m *base) irq() bool    { return m.irqF }
func (m *base) state(s *sio) { m.baseState(s) }
func (m *base) baseState(s *sio) {
	s.Bytes(m.prgRam)
	if m.chrRam {
		s.Bytes(m.chr)
	}
	s.Ints(&m.mirror)
	s.Bool(&m.irqF)
	s.IntSlice(m.prgMap[:])
	s.IntSlice(m.chrMap[:])
	s.IntSlice(m.ntPgs[:])
	if m.audio != nil {
		m.audio.state(s)
	}
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

// 0 — NROM
type m0 struct{ *base }

// 1 — MMC1 (Zelda, Metroid, Mega Man 2…)
type m1 struct {
	*base
	shift, ctrl, chr0, chr1, prgReg int
	lastWrite                       int64
	ramBank                         int
	canDisableRam, noRam, ramOn     bool
	upd                             func()
}

func newM1(n *NES, c *cart) *m1 {
	m := &m1{base: newBase(n, c), shift: 0x10, ctrl: 0x0c, lastWrite: -10}
	// SOROM / SXROM boards have 16-32 KB of RAM, banked by CHR register bits.
	ram := c.prgRamSize + c.prgNvramSize
	if ram > 0x2000 {
		m.prgRam = make([]byte, ram)
	}
	m.canDisableRam = c.mapper == 1 // the MMC1A (mapper 155) can't switch its RAM off
	m.noRam = c.nes2 && ram == 0
	m.upd = m.update
	m.update()
	return m
}
func (m *m1) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 {
		if !m.noRam && (m.ramOn || !m.canDisableRam) {
			return int(m.prgRam[m.ramBank+a&0x1fff])
		}
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m1) cpuWrite(a, v int) {
	if a < 0x8000 {
		if a >= 0x6000 && (m.ramOn || !m.canDisableRam) {
			m.prgRam[m.ramBank+a&0x1fff] = byte(v)
		}
		return
	}
	// Writes on back-to-back cycles (from RMW instructions) are ignored by the chip.
	now := m.cpuCycles()
	consecutive := now-m.lastWrite < 2
	m.lastWrite = now
	if v&0x80 != 0 {
		m.shift = 0x10
		m.ctrl |= 0x0c
		m.upd()
		return
	}
	if consecutive {
		return
	}
	done := m.shift & 1
	m.shift = m.shift>>1 | (v&1)<<4
	if done != 0 {
		r := m.shift
		m.shift = 0x10
		switch (a >> 13) & 3 {
		case 0:
			m.ctrl = r
		case 1:
			m.chr0 = r
		case 2:
			m.chr1 = r
		case 3:
			m.prgReg = r
		}
		m.upd()
	}
}

var mmc1Mirror = [4]int{mirOneLo, mirOneHi, mirV, mirH}

func (m *m1) update() {
	m.mirror = mmc1Mirror[m.ctrl&3]
	// Large boards (SUROM, 512 KB) use a CHR bit to pick the 256 KB PRG half.
	outer := 0
	if len(m.prg) > 0x40000 {
		outer = m.chr0 & 0x10
	}
	p := m.prgReg&0x0f | outer
	switch (m.ctrl >> 2) & 3 {
	case 0, 1:
		m.setPrg32(p >> 1)
	case 2:
		m.setPrg16(0, outer)
		m.setPrg16(1, p)
	case 3:
		m.setPrg16(0, p)
		m.setPrg16(1, outer|0x0f)
	}
	if m.ctrl&0x10 != 0 {
		m.setChr4(0, m.chr0)
		m.setChr4(1, m.chr1)
	} else {
		m.setChr8(m.chr0 >> 1)
	}
	m.ramOn = m.prgReg&0x10 == 0
	banks := len(m.prgRam) >> 13
	m.ramBank = 0
	if banks > 1 {
		m.ramBank = (((m.chr0 >> 2) & 3) % banks) << 13
	}
}
func (m *m1) state(s *sio) {
	m.baseState(s)
	s.Ints(&m.shift, &m.ctrl, &m.chr0, &m.chr1, &m.prgReg, &m.ramBank)
	s.I64(&m.lastWrite)
	s.Bool(&m.ramOn)
}

// 2 — UxROM (Mega Man, Castlevania, Contra…)
type m2 struct{ *base }

func newM2(n *NES, c *cart) *m2 { m := &m2{newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m2) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg16(0, v&0xff) // (bus conflicts are off in the web version)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 3 — CNROM
type m3 struct{ *base }

func (m *m3) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setChr8(v & m.cpuRead(a))
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 4 — MMC3 (SMB3, Kirby, TMNT 2 & 3…)
type m4 struct {
	*base
	bankSel                     int
	r                           [8]int
	irqLatch, irqCounter        int
	irqReload, irqEnable        bool
	lastA12                     int
	a12LowSince                 int64
	fourScreen, useRamCtl, revA bool
	ramCtl                      int
	upd, clk                    func()
}

func newM4(n *NES, c *cart) *m4 {
	m := &m4{base: newBase(n, c), r: [8]int{0, 2, 4, 5, 6, 7, 0, 1}}
	m.fourScreen = c.mirror == mirFour
	// $A001 RAM enable/write-protect. Old (iNES) headers can't tell MMC3 from MMC6 games,
	// whose RAM control works differently, so it's only honoured for NES 2.0 MMC3 headers.
	m.ramCtl = 0x80
	m.useRamCtl = (c.mapper == 4 || c.mapper == 118 || c.mapper == 119) && c.nes2 && c.submapper != 1
	m.revA = c.mapper == 4 && c.submapper == 4 // MMC3A: no IRQ on a natural reload to 0
	m.upd, m.clk = m.update, m.clockIrq
	m.update()
	return m
}
func (m *m4) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 && m.useRamCtl && m.ramCtl&0x80 == 0 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m4) cpuWrite(a, v int) {
	if a < 0x8000 {
		if !m.useRamCtl || m.ramCtl&0xc0 == 0x80 {
			m.base.cpuWrite(a, v)
		}
		return
	}
	even := a&1 == 0
	switch a & 0xe000 {
	case 0x8000:
		if even {
			m.bankSel = v
		} else {
			m.r[m.bankSel&7] = v
		}
		m.upd()
	case 0xa000:
		if even {
			if !m.fourScreen {
				if v&1 != 0 {
					m.mirror = mirH
				} else {
					m.mirror = mirV
				}
			}
		} else {
			m.ramCtl = v
		}
	case 0xc000:
		if even {
			m.irqLatch = v
		} else {
			m.irqCounter = 0
			m.irqReload = true
		}
	case 0xe000:
		if even {
			m.irqEnable = false
			m.irqF = false
		} else {
			m.irqEnable = true
		}
	}
}
func (m *m4) update() {
	r := &m.r
	if m.bankSel&0x40 != 0 {
		m.setPrg8(0, -2)
		m.setPrg8(2, r[6])
	} else {
		m.setPrg8(0, r[6])
		m.setPrg8(2, -2)
	}
	m.setPrg8(1, r[7])
	m.setPrg8(3, -1)
	inv := 0
	if m.bankSel&0x80 != 0 {
		inv = 4
	}
	m.setChr1(0^inv, r[0]&0xfe)
	m.setChr1(1^inv, r[0]|1)
	m.setChr1(2^inv, r[1]&0xfe)
	m.setChr1(3^inv, r[1]|1)
	m.setChr1(4^inv, r[2])
	m.setChr1(5^inv, r[3])
	m.setChr1(6^inv, r[4])
	m.setChr1(7^inv, r[5])
}

// The scanline counter is clocked by rising edges of PPU address line A12
// (after it has been low for a little while).
func (m *m4) ppuAddr(a int) {
	a12 := (a >> 12) & 1
	if a12 != 0 && m.lastA12 == 0 {
		if m.cpuCycles()-m.a12LowSince >= 3 {
			m.clk()
		}
	} else if a12 == 0 && m.lastA12 != 0 {
		m.a12LowSince = m.cpuCycles()
	}
	m.lastA12 = a12
}
func (m *m4) ppuRead(a int) int { m.ppuAddr(a); return m.base.ppuRead(a) }
func (m *m4) clockIrq() {
	was, forced := m.irqCounter, m.irqReload
	if m.irqCounter == 0 || m.irqReload {
		m.irqCounter = m.irqLatch
		m.irqReload = false
	} else {
		m.irqCounter--
	}
	if m.irqCounter == 0 && m.irqEnable && (!m.revA || was != 0 || forced) {
		m.irqF = true
	}
}
func (m *m4) m4State(s *sio) {
	m.baseState(s)
	s.Ints(&m.bankSel, &m.irqLatch, &m.irqCounter, &m.lastA12, &m.ramCtl)
	s.IntSlice(m.r[:])
	s.Bools(&m.irqReload, &m.irqEnable)
	s.I64(&m.a12LowSince)
}
func (m *m4) state(s *sio) { m.m4State(s) }

// 7 — AxROM (Battletoads, Wizards & Warriors…)
type m7 struct{ *base }

func newM7(n *NES, c *cart) *m7 { m := &m7{newBase(n, c)}; m.mirror = mirOneLo; return m }
func (m *m7) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	m.setPrg32(v & 0x0f)
	if v&0x10 != 0 {
		m.mirror = mirOneHi
	} else {
		m.mirror = mirOneLo
	}
}

// 9 / 10 — MMC2 / MMC4 (Punch-Out!!, Fire Emblem): CHR latches flip on special tiles.
type m9 struct {
	*base
	mmc4  bool
	cr    [4]int
	latch [2]int
}

func newM9(n *NES, c *cart, mmc4 bool) *m9 {
	m := &m9{base: newBase(n, c), mmc4: mmc4, latch: [2]int{1, 1}}
	if mmc4 {
		m.setPrg16(1, -1)
	} else {
		m.setPrg8(1, -3)
		m.setPrg8(2, -2)
		m.setPrg8(3, -1)
	}
	m.updateChr()
	return m
}
func (m *m9) cpuWrite(a, v int) {
	if a < 0xa000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf000 {
	case 0xa000:
		if m.mmc4 {
			m.setPrg16(0, v&0x0f)
		} else {
			m.setPrg8(0, v&0x0f)
		}
	case 0xb000:
		m.cr[0] = v & 0x1f
	case 0xc000:
		m.cr[1] = v & 0x1f
	case 0xd000:
		m.cr[2] = v & 0x1f
	case 0xe000:
		m.cr[3] = v & 0x1f
	case 0xf000:
		if v&1 != 0 {
			m.mirror = mirH
		} else {
			m.mirror = mirV
		}
	}
	m.updateChr()
}
func (m *m9) updateChr() {
	i0, i1 := 0, 2
	if m.latch[0] != 0 {
		i0 = 1
	}
	if m.latch[1] != 0 {
		i1 = 3
	}
	m.setChr4(0, m.cr[i0])
	m.setChr4(1, m.cr[i1])
}
func (m *m9) ppuRead(a int) int {
	v := m.base.ppuRead(a)
	if a == 0x0fd8 || (m.mmc4 && a&0x1ff0 == 0x0fd0) {
		m.latch[0] = 0
		m.updateChr()
	} else if a == 0x0fe8 || (m.mmc4 && a&0x1ff0 == 0x0fe0) {
		m.latch[0] = 1
		m.updateChr()
	} else if a&0x1ff8 == 0x1fd8 {
		m.latch[1] = 0
		m.updateChr()
	} else if a&0x1ff8 == 0x1fe8 {
		m.latch[1] = 1
		m.updateChr()
	}
	return v
}
func (m *m9) state(s *sio) { m.baseState(s); s.IntSlice(m.cr[:]); s.IntSlice(m.latch[:]) }

// 11 — Color Dreams; 66 — GxROM; 140 — Jaleco
type m11 struct{ *base }

func (m *m11) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32(v & 3)
		m.setChr8(v >> 4)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m66 struct{ *base }

func (m *m66) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32((v >> 4) & 3)
		m.setChr8(v & 3)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m140 struct{ *base }

func (m *m140) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		m.setPrg32((v >> 4) & 3)
		m.setChr8(v & 15)
	}
}

// 13 — CPROM (Videomation): switchable upper 4 KB of CHR RAM
type m13 struct{ *base }

func newM13(n *NES, c *cart) *m13 {
	m := &m13{newBase(n, c)}
	m.setChr4(0, 0)
	m.setChr4(1, 0)
	return m
}
func (m *m13) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setChr4(1, v&3)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 34 — BNROM / NINA-001 (NINA-001 boards have CHR ROM; BNROM has CHR RAM)
type m34 struct {
	*base
	nina bool
}

func newM34(n *NES, c *cart) *m34 {
	return &m34{newBase(n, c), c.submapper == 1 || (c.submapper != 2 && !c.chrRam)}
}
func (m *m34) cpuWrite(a, v int) {
	if a >= 0x8000 {
		if !m.nina {
			m.setPrg32(v & m.cpuRead(a))
		}
		return
	}
	if m.nina {
		switch a {
		case 0x7ffd:
			m.setPrg32(v & 1)
		case 0x7ffe:
			m.setChr4(0, v&15)
		case 0x7fff:
			m.setChr4(1, v&15)
		}
	}
	m.base.cpuWrite(a, v)
}

// 69 — Sunsoft FME-7 (Batman: Return of the Joker, Gimmick!) — banking and IRQ
type m69 struct {
	*base
	cmd, ramMode, ramBank, irqEnable, irqCount int
	s5b                                        *s5b
}

func newM69(n *NES, c *cart) *m69 {
	m := &m69{base: newBase(n, c)}
	m.setPrg8(3, -1)
	m.s5b = newS5B() // Sunsoft 5B sound (Gimmick!)
	m.audio = m.s5b
	return m
}
func (m *m69) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 {
		if m.ramMode&0x40 != 0 {
			if m.ramMode&0x80 != 0 {
				return int(m.prgRam[a&0x1fff])
			}
			return -1
		}
		return int(m.prg[(m.ramBank%m.prgBanks8())*0x2000+a&0x1fff])
	}
	return m.base.cpuRead(a)
}

var fme7Mirror = [4]int{mirV, mirH, mirOneLo, mirOneHi}

func (m *m69) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		if m.ramMode&0xc0 == 0xc0 {
			m.prgRam[a&0x1fff] = byte(v)
		}
		return
	}
	if a >= 0x8000 && a < 0xa000 {
		m.cmd = v & 15
		return
	}
	if a >= 0xc000 {
		if a < 0xe000 {
			m.s5b.writeAddr(v)
		} else {
			m.s5b.writeData(v)
		}
		return
	}
	if a >= 0xa000 && a < 0xc000 {
		c := m.cmd
		switch {
		case c < 8:
			m.setChr1(c, v)
		case c == 8:
			m.ramMode = v
			m.ramBank = v & 0x3f
		case c < 12:
			m.setPrg8(c-9, v&0x3f)
		case c == 12:
			m.mirror = fme7Mirror[v&3]
		case c == 13:
			m.irqEnable = v
			m.irqF = false
		case c == 14:
			m.irqCount = m.irqCount&0xff00 | v
		default:
			m.irqCount = m.irqCount&0xff | v<<8
		}
	}
}
func (m *m69) cpuClock() {
	m.s5b.clock()
	if m.irqEnable&0x80 != 0 {
		m.irqCount = (m.irqCount - 1) & 0xffff
		if m.irqCount == 0xffff && m.irqEnable&1 != 0 {
			m.irqF = true
		}
	}
}
func (m *m69) state(s *sio) {
	m.baseState(s)
	s.Ints(&m.cmd, &m.ramMode, &m.ramBank, &m.irqEnable, &m.irqCount)
}

// 71 — Camerica (Micro Machines, Fire Hawk); 180 — UNROM with fixed first bank (Crazy Climber)
type m71 struct{ *base }

func newM71(n *NES, c *cart) *m71 { m := &m71{newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m71) cpuWrite(a, v int) {
	if a >= 0xc000 {
		m.setPrg16(0, v&0x0f)
	} else if a >= 0x9000 && a < 0xa000 {
		if v&0x10 != 0 {
			m.mirror = mirOneHi
		} else {
			m.mirror = mirOneLo
		}
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m180 struct{ *base }

func newM180(n *NES, c *cart) *m180 { m := &m180{newBase(n, c)}; m.setPrg16(0, 0); return m }
func (m *m180) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg16(1, v&7)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 206 — Namco 108 (MMC3 without IRQs or mirroring control)
type m206 struct{ *m4 }

func (m *m206) cpuWrite(a, v int) {
	if a >= 0x8000 && a < 0xa000 {
		if a&1 == 0 {
			m.bankSel = v & 7
		} else {
			m.r[m.bankSel&7] = v & 0x3f
		}
		m.upd()
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m206) ppuRead(a int) int { return m.base.ppuRead(a) }

// 5 — MMC5 (Castlevania III, Just Breed, many romhacks)
type mmc5Win struct {
	rom bool
	off int
}

type m5 struct {
	*base
	exram                                                   []byte
	prgMode, chrMode, ramProt1, ramProt2, exMode, ntMap     int
	fillTile, fillAttr                                      int
	prgRegs                                                 [5]int
	chrA                                                    [8]int
	chrB                                                    [4]int
	chrHi, lastSet, irqCompare                              int
	irqEnable, irqPending, inFrame                          bool
	lineCount, mulA, mulB, exAttr                           int
	splitCtl, splitScroll, splitBank, splitFineY, splitAttr int
	inSplit                                                 bool
	ciram                                                   []byte
	win                                                     [4]mmc5Win
	snd                                                     *mmc5Audio
}

func newM5(n *NES, c *cart) *m5 {
	m := &m5{base: newBase(n, c)}
	m.prgRam = make([]byte, 0x10000)
	m.exram = make([]byte, 0x400)
	m.prgMode, m.chrMode = 3, 3
	m.prgRegs = [5]int{0, 0, 0, 0, 0xff} // $5113-$5117
	m.mulA, m.mulB = 0xff, 0xff
	m.ciram = c.vram // the console's 2 KB of nametable RAM
	m.snd = newMMC5Audio()
	m.audio = m.snd
	m.updatePrg()
	return m
}
func (m *m5) irq() bool { return m.irqPending && m.irqEnable }
func (m *m5) updatePrg() {
	// Each of the four 8 KB windows at $8000 resolves to ROM or RAM.
	set := func(slot, reg, bankSize int) {
		v := m.prgRegs[reg]
		rom := reg == 4 || v&0x80 != 0
		bank8 := v & 0x7f
		if bankSize == 32 {
			bank8 = bank8&^3 | slot
		} else if bankSize == 16 {
			bank8 = bank8&^1 | slot&1
		}
		if rom {
			m.win[slot] = mmc5Win{true, (bank8 % m.prgBanks8()) * 0x2000}
		} else {
			m.win[slot] = mmc5Win{false, (bank8 & 7) * 0x2000}
		}
	}
	switch m.prgMode {
	case 0:
		for i := 0; i < 4; i++ {
			set(i, 4, 32)
		}
	case 1:
		set(0, 2, 16)
		set(1, 2, 16)
		set(2, 4, 16)
		set(3, 4, 16)
	case 2:
		set(0, 2, 16)
		set(1, 2, 16)
		set(2, 3, 8)
		set(3, 4, 8)
	default:
		set(0, 1, 8)
		set(1, 2, 8)
		set(2, 3, 8)
		set(3, 4, 8)
	}
}
func (m *m5) cpuRead(a int) int {
	if a >= 0x8000 {
		w := m.win[(a>>13)&3]
		if w.rom {
			return int(m.prg[w.off+a&0x1fff])
		}
		return int(m.prgRam[w.off+a&0x1fff])
	}
	if a >= 0x6000 {
		return int(m.prgRam[(m.prgRegs[0]&7)*0x2000+a&0x1fff])
	}
	if a >= 0x5c00 {
		if m.exMode >= 2 {
			return int(m.exram[a&0x3ff])
		}
		return -1
	}
	switch a {
	case 0x5204:
		v := 0
		if m.irqPending {
			v |= 0x80
		}
		if m.inFrame {
			v |= 0x40
		}
		m.irqPending = false
		return v
	case 0x5015:
		return m.snd.status()
	case 0x5205:
		return (m.mulA * m.mulB) & 0xff
	case 0x5206:
		return (m.mulA * m.mulB) >> 8
	}
	return -1
}
func (m *m5) ramWritable() bool { return m.ramProt1 == 2 && m.ramProt2 == 1 }
func (m *m5) cpuWrite(a, v int) {
	if a >= 0x8000 {
		w := m.win[(a>>13)&3]
		if !w.rom && m.ramWritable() {
			m.prgRam[w.off+a&0x1fff] = byte(v)
		}
		return
	}
	if a >= 0x6000 {
		if m.ramWritable() {
			m.prgRam[(m.prgRegs[0]&7)*0x2000+a&0x1fff] = byte(v)
		}
		return
	}
	if a >= 0x5c00 {
		if m.exMode != 3 {
			m.exram[a&0x3ff] = byte(v)
		}
		return
	}
	switch a {
	case 0x5100:
		m.prgMode = v & 3
		m.updatePrg()
		return
	case 0x5101:
		m.chrMode = v & 3
		return
	case 0x5102:
		m.ramProt1 = v & 3
		return
	case 0x5103:
		m.ramProt2 = v & 3
		return
	case 0x5104:
		m.exMode = v & 3
		return
	case 0x5105:
		m.ntMap = v
		return
	case 0x5106:
		m.fillTile = v
		return
	case 0x5107:
		m.fillAttr = v & 3
		return
	case 0x5130:
		m.chrHi = v & 3
		return
	case 0x5200:
		m.splitCtl = v
		return
	case 0x5201:
		m.splitScroll = v
		return
	case 0x5202:
		m.splitBank = v
		return
	case 0x5203:
		m.irqCompare = v
		return
	case 0x5204:
		m.irqEnable = v&0x80 != 0
		return
	case 0x5205:
		m.mulA = v
		return
	case 0x5206:
		m.mulB = v
		return
	}
	if a >= 0x5000 && a <= 0x5015 {
		m.snd.write(a, v)
		return
	}
	if a >= 0x5113 && a <= 0x5117 {
		m.prgRegs[a-0x5113] = v
		m.updatePrg()
		return
	}
	if a >= 0x5120 && a <= 0x5127 {
		m.chrA[a-0x5120] = (v | m.chrHi<<8) & 0xffff
		m.lastSet = 0
		return
	}
	if a >= 0x5128 && a <= 0x512b {
		m.chrB[a-0x5128] = (v | m.chrHi<<8) & 0xffff
		m.lastSet = 1
		return
	}
}

// Which CHR register set applies: with 8x16 sprites, sprites use set A and the
// background set B; with 8x8 sprites, whichever set was written last.
func (m *m5) chrAddr(a int) int {
	ppu := m.n.ppu
	tall := ppu.ctrl&0x20 != 0
	useB := m.lastSet == 1
	if tall {
		useB = ppu.fetch == 1
	}
	var bank, size int
	if useB {
		switch m.chrMode {
		case 0:
			bank, size = m.chrB[3], 8
		case 1:
			bank, size = m.chrB[3], 4
		case 2:
			if (a>>11)&1 != 0 {
				bank = m.chrB[3]
			} else {
				bank = m.chrB[1]
			}
			size = 2
		default:
			bank, size = m.chrB[(a>>10)&3], 1
		}
	} else {
		switch m.chrMode {
		case 0:
			bank, size = m.chrA[7], 8
		case 1:
			if a>>12 != 0 {
				bank = m.chrA[7]
			} else {
				bank = m.chrA[3]
			}
			size = 4
		case 2:
			bank, size = m.chrA[(a>>11)<<1|1], 2
		default:
			bank, size = m.chrA[a>>10], 1
		}
	}
	span := size * 0x400
	return (bank*span + a&(span-1)) % len(m.chr)
}
func (m *m5) ppuRead(a int) int {
	fetch := m.n.ppu.fetch
	// Split-screen tiles come from their own 4 KB CHR bank, at the split's own fine scroll.
	if fetch == 1 && m.inSplit {
		return int(m.chr[(m.splitBank*0x1000+(a&0xff8|m.splitFineY))%len(m.chr)])
	}
	// Extended attribute mode: each background tile picks its own 4 KB CHR bank.
	if m.exMode == 1 && fetch == 1 {
		bank := m.exAttr&0x3f | m.chrHi<<6
		return int(m.chr[(bank*0x1000+a&0xfff)%len(m.chr)])
	}
	return int(m.chr[m.chrAddr(a)])
}
func (m *m5) ppuWrite(a, v int) {
	if m.chrRam {
		m.chr[m.chrAddr(a)] = byte(v)
	}
}
func (m *m5) ntRead(a int) int {
	ppu := m.n.ppu
	off, src := a&0x3ff, (m.ntMap>>(((a>>10)&3)*2))&3
	if ppu.fetch == 1 && m.splitCtl&0x80 != 0 && m.exMode <= 1 {
		// Vertical split: tiles on one side of a column come from ExRAM with their own vertical scroll.
		if off < 0x3c0 {
			dot := ppu.dot
			col := ((dot - 1) >> 3) + 2
			if dot >= 321 {
				col = (dot - 321) >> 3
			}
			t := m.splitCtl & 0x1f
			if m.splitCtl&0x40 != 0 {
				m.inSplit = col >= t
			} else {
				m.inSplit = col < t
			}
			if m.inSplit {
				line := ppu.line
				if dot >= 321 {
					if ppu.line == 261 {
						line = 0
					} else {
						line = ppu.line + 1
					}
				}
				y := m.splitScroll + line
				if y >= 240 {
					y -= 240
				}
				y %= 240
				row, c := y>>3, col&31
				m.splitFineY = y & 7
				at := int(m.exram[0x3c0+(row>>2)<<3+(c>>2)])
				m.splitAttr = (at >> ((row&2)<<1 | c&2)) & 3
				return int(m.exram[row<<5+c])
			}
		} else if m.inSplit {
			return m.splitAttr * 0x55
		}
	}
	if m.exMode == 1 && ppu.fetch == 1 {
		if off < 0x3c0 {
			m.exAttr = int(m.exram[off])
		} else {
			p := (m.exAttr >> 6) & 3
			return p | p<<2 | p<<4 | p<<6
		}
	}
	switch src {
	case 0:
		return int(m.ciram[off])
	case 1:
		return int(m.ciram[0x400|off])
	case 2:
		if m.exMode <= 1 {
			return int(m.exram[off])
		}
		return 0
	}
	if off < 0x3c0 {
		return m.fillTile
	}
	return m.fillAttr * 0x55
}
func (m *m5) ntWrite(a, v int) {
	off, src := a&0x3ff, (m.ntMap>>(((a>>10)&3)*2))&3
	switch {
	case src == 0:
		m.ciram[off] = byte(v)
	case src == 1:
		m.ciram[0x400|off] = byte(v)
	case src == 2 && m.exMode <= 1:
		m.exram[off] = byte(v)
	}
}
func (m *m5) scanline(line int) {
	if line >= 240 {
		return
	}
	if !m.inFrame {
		m.inFrame = true
		m.lineCount = 0
		m.irqPending = false
	} else {
		m.lineCount++
		if m.lineCount == m.irqCompare {
			m.irqPending = true
		}
	}
}
func (m *m5) vblank()   { m.inFrame = false; m.inSplit = false }
func (m *m5) cpuClock() { m.snd.clock() }
func (m *m5) state(s *sio) {
	m.baseState(s)
	s.Bytes(m.exram)
	s.Ints(&m.prgMode, &m.chrMode, &m.ramProt1, &m.ramProt2, &m.exMode, &m.ntMap, &m.fillTile, &m.fillAttr,
		&m.chrHi, &m.lastSet, &m.irqCompare, &m.lineCount, &m.mulA, &m.mulB, &m.exAttr,
		&m.splitCtl, &m.splitScroll, &m.splitBank, &m.splitFineY, &m.splitAttr)
	s.IntSlice(m.prgRegs[:])
	s.IntSlice(m.chrA[:])
	s.IntSlice(m.chrB[:])
	s.Bools(&m.irqEnable, &m.irqPending, &m.inFrame, &m.inSplit)
	if !s.save {
		m.updatePrg()
	}
}
