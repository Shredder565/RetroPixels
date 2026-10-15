package nes

// Extra cartridge boards (web/emu/nes_mappers.js): Konami VRC, Namco, Bandai,
// Jaleco, Irem, Taito, Sunsoft, and the popular unlicensed boards and multicarts.

// Common 2-bit mirroring encodings.
var (
	vh1 = [4]int{mirV, mirH, mirOneLo, mirOneHi}
	hv1 = [4]int{mirH, mirV, mirOneLo, mirOneHi}
)

func hOrV(cond bool) int {
	if cond {
		return mirH
	}
	return mirV
}

func vOrH(cond bool) int {
	if cond {
		return mirV
	}
	return mirH
}

func hiOrLo(cond bool) int {
	if cond {
		return mirOneHi
	}
	return mirOneLo
}

// ===================================================================
// Konami
// ===================================================================

// vrcIrq is the IRQ counter shared by the VRC4, VRC6 and VRC7: an 8-bit
// up-counter that fires on overflow, clocked either every CPU cycle or once
// per scanline (a prescaler that counts 341 PPU dots, i.e. 113.67 CPU cycles).
type vrcIrq struct {
	latch, counter, prescaler                   int
	enabled, enableAfterAck, cycleMode, pending bool
}

func newVrcIrq() vrcIrq { return vrcIrq{prescaler: 341} }

func (q *vrcIrq) control(v int) {
	q.enableAfterAck = v&1 != 0
	q.enabled = v&2 != 0
	q.cycleMode = v&4 != 0
	if q.enabled {
		q.counter = q.latch
		q.prescaler = 341
	}
	q.pending = false
}
func (q *vrcIrq) ack() { q.pending = false; q.enabled = q.enableAfterAck }
func (q *vrcIrq) clock() {
	if !q.enabled {
		return
	}
	if !q.cycleMode {
		q.prescaler -= 3
		if q.prescaler > 0 {
			return
		}
		q.prescaler += 341
	}
	if q.counter == 0xff {
		q.counter = q.latch
		q.pending = true
	} else {
		q.counter++
	}
}
func (q *vrcIrq) state(s *sio) {
	s.Ints(&q.latch, &q.counter, &q.prescaler)
	s.Bools(&q.enabled, &q.enableAfterAck, &q.cycleMode, &q.pending)
}

// 21 / 22 / 23 / 25 — VRC2 and VRC4 (Gradius II, Contra Japan, Ganbare Goemon, Wai Wai World 2...)
// The boards differ in which CPU address lines reach the chip's two register-select pins.
type vrc24 struct {
	*base
	lines        [][2]int
	vrc4         bool
	chrShift     int
	prg0, prg1   int
	swap         int
	chrLo, chrHi [8]int
	vrc          vrcIrq
}

func newVRC24(n *NES, c *cart, no int) *vrc24 {
	m := &vrc24{base: newBase(n, c), vrc4: true, vrc: newVrcIrq()}
	sub := c.submapper
	switch no {
	case 21:
		switch sub {
		case 1:
			m.lines = [][2]int{{2, 4}}
		case 2:
			m.lines = [][2]int{{0x40, 0x80}}
		default:
			m.lines = [][2]int{{2, 4}, {0x40, 0x80}}
		}
	case 22:
		m.lines = [][2]int{{2, 1}}
		m.vrc4 = false
		m.chrShift = 1
	case 23:
		switch sub {
		case 1, 3:
			m.lines = [][2]int{{1, 2}}
		case 2:
			m.lines = [][2]int{{4, 8}}
		default:
			m.lines = [][2]int{{1, 2}, {4, 8}}
		}
		m.vrc4 = sub != 3
	default:
		switch sub {
		case 1, 3:
			m.lines = [][2]int{{2, 1}}
		case 2:
			m.lines = [][2]int{{8, 4}}
		default:
			m.lines = [][2]int{{2, 1}, {8, 4}}
		}
		m.vrc4 = sub != 3
	}
	m.update()
	return m
}
func (m *vrc24) reg(a int) int {
	r := 0
	for _, l := range m.lines {
		if a&l[0] != 0 {
			r |= 1
		}
		if a&l[1] != 0 {
			r |= 2
		}
	}
	return r
}
func (m *vrc24) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	r := m.reg(a)
	switch a & 0xf000 {
	case 0x8000:
		m.prg0 = v & 0x1f
	case 0x9000:
		if !m.vrc4 {
			m.mirror = hOrV(v&1 != 0)
		} else if r < 2 {
			m.mirror = vh1[v&3]
		} else {
			m.swap = (v >> 1) & 1
		}
	case 0xa000:
		m.prg1 = v & 0x1f
	case 0xf000:
		if !m.vrc4 {
			break
		}
		switch r {
		case 0:
			m.vrc.latch = m.vrc.latch&0xf0 | v&0x0f
		case 1:
			m.vrc.latch = m.vrc.latch&0x0f | (v&0x0f)<<4
		case 2:
			m.vrc.control(v)
		default:
			m.vrc.ack()
		}
		return
	default:
		i := ((a>>12)-0xb)<<1 | r>>1
		if r&1 != 0 {
			m.chrHi[i] = v & 0x1f
		} else {
			m.chrLo[i] = v & 0x0f
		}
	}
	m.update()
}
func (m *vrc24) update() {
	if m.swap != 0 {
		m.setPrg8(0, -2)
		m.setPrg8(2, m.prg0)
	} else {
		m.setPrg8(0, m.prg0)
		m.setPrg8(2, -2)
	}
	m.setPrg8(1, m.prg1)
	m.setPrg8(3, -1)
	for i := 0; i < 8; i++ {
		m.setChr1(i, (m.chrLo[i]|m.chrHi[i]<<4)>>m.chrShift)
	}
}
func (m *vrc24) cpuClock() {
	if m.vrc4 {
		m.vrc.clock()
		m.irqF = m.vrc.pending
	}
}
func (m *vrc24) state(s *sio) {
	m.baseState(s)
	s.Ints(&m.prg0, &m.prg1, &m.swap)
	s.IntSlice(m.chrLo[:])
	s.IntSlice(m.chrHi[:])
	m.vrc.state(s)
}

// 73 — VRC3 (Salamander): 16-bit IRQ counter
type m73 struct {
	*base
	latch, counter, ctl int
}

func newM73(n *NES, c *cart) *m73 { m := &m73{base: newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m73) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf000 {
	case 0x8000, 0x9000, 0xa000, 0xb000:
		sh := ((a >> 12) - 8) * 4
		m.latch = m.latch&^(0xf<<sh) | (v&0xf)<<sh
	case 0xc000:
		m.ctl = v & 7
		if v&2 != 0 {
			m.counter = m.latch
		}
		m.irqF = false
	case 0xd000:
		m.irqF = false
		m.ctl = m.ctl&^2 | (m.ctl&1)<<1
	case 0xf000:
		m.setPrg16(0, v&7)
	}
}
func (m *m73) cpuClock() {
	if m.ctl&2 == 0 {
		return
	}
	if m.ctl&4 != 0 { // 8-bit mode: only the low byte counts
		lo := (m.counter + 1) & 0xff
		if lo == 0 {
			m.counter = m.counter&0xff00 | m.latch&0xff
			m.irqF = true
		} else {
			m.counter = m.counter&0xff00 | lo
		}
	} else if m.counter == 0xffff {
		m.counter = m.latch
		m.irqF = true
	} else {
		m.counter++
	}
}
func (m *m73) state(s *sio) { m.baseState(s); s.Ints(&m.latch, &m.counter, &m.ctl) }

// 75 — VRC1 (Ganbare Goemon!, Exciting Boxing)
type m75 struct {
	*base
	c  [2]int
	ch int
}

func newM75(n *NES, c *cart) *m75 { m := &m75{base: newBase(n, c)}; m.setPrg8(3, -1); return m }
func (m *m75) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf000 {
	case 0x8000:
		m.setPrg8(0, v&0x0f)
	case 0x9000:
		if m.mirror != mirFour {
			m.mirror = hOrV(v&1 != 0)
		}
		m.ch = v
	case 0xa000:
		m.setPrg8(1, v&0x0f)
	case 0xc000:
		m.setPrg8(2, v&0x0f)
	case 0xe000:
		m.c[0] = v & 0x0f
	case 0xf000:
		m.c[1] = v & 0x0f
	}
	m.setChr4(0, m.c[0]|(m.ch&2)<<3)
	m.setChr4(1, m.c[1]|(m.ch&4)<<2)
}
func (m *m75) state(s *sio) { m.baseState(s); s.IntSlice(m.c[:]); s.Int(&m.ch) }

// 24 / 26 — VRC6 (Akumajou Densetsu, Madara, Esper Dream 2) with its two pulse channels and sawtooth
type m24 struct {
	*base
	swapLines   bool
	prg16, prg8 int
	r           [8]int
	ctl         int
	vrc         vrcIrq
	snd         *vrc6
}

func newM24(n *NES, c *cart, swapLines bool) *m24 {
	m := &m24{base: newBase(n, c), swapLines: swapLines, ctl: 0x20, vrc: newVrcIrq(), snd: newVRC6()}
	m.audio = m.snd
	m.update()
	return m
}
func (m *m24) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 && m.ctl&0x80 == 0 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m24) cpuWrite(a, v int) {
	if a < 0x8000 {
		if m.ctl&0x80 != 0 {
			m.base.cpuWrite(a, v)
		}
		return
	}
	r := a & 3
	if m.swapLines {
		r = (r&1)<<1 | r>>1
	}
	switch a & 0xf000 {
	case 0x8000:
		m.prg16 = v & 0x0f
	case 0x9000, 0xa000, 0xb000:
		if a&0xf000 == 0xb000 && r == 3 {
			m.ctl = v
			break
		}
		m.snd.write((a>>12)-9, r, v)
		return
	case 0xc000:
		m.prg8 = v & 0x1f
	case 0xd000:
		m.r[r] = v
	case 0xe000:
		m.r[4+r] = v
	case 0xf000:
		switch r {
		case 0:
			m.vrc.latch = v
		case 1:
			m.vrc.control(v)
		case 2:
			m.vrc.ack()
		}
		return
	}
	m.update()
}
func (m *m24) update() {
	m.setPrg16(0, m.prg16)
	m.setPrg8(2, m.prg8)
	m.setPrg8(3, -1)
	r := &m.r
	switch m.ctl & 3 {
	case 0:
		for i := 0; i < 8; i++ {
			m.setChr1(i, r[i])
		}
	case 1:
		for i := 0; i < 8; i++ {
			m.setChr1(i, r[i>>1]&0xfe|i&1)
		}
	default:
		for i := 0; i < 4; i++ {
			m.setChr1(i, r[i])
		}
		for i := 4; i < 8; i++ {
			m.setChr1(i, r[4+(i-4)>>1]&0xfe|i&1)
		}
	}
	if m.mirror != mirFour {
		m.mirror = vh1[(m.ctl>>2)&3]
	}
}
func (m *m24) cpuClock() { m.vrc.clock(); m.irqF = m.vrc.pending; m.snd.clock() }
func (m *m24) state(s *sio) {
	m.baseState(s)
	s.Ints(&m.prg16, &m.prg8, &m.ctl)
	s.IntSlice(m.r[:])
	m.vrc.state(s)
}

// 85 — VRC7 (Lagrange Point, Tiny Toon Adventures 2) with its FM synthesizer
type m85 struct {
	*base
	hiLine int
	p      [3]int
	c      [8]int
	ctl    int
	vrc    vrcIrq
	snd    *vrc7
}

func newM85(n *NES, c *cart) *m85 {
	m := &m85{base: newBase(n, c), vrc: newVrcIrq(), snd: newVRC7()}
	switch c.submapper { // which address line picks the second register
	case 1:
		m.hiLine = 0x08
	case 2:
		m.hiLine = 0x10
	default:
		m.hiLine = 0x18
	}
	m.audio = m.snd
	m.update()
	return m
}
func (m *m85) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 && m.ctl&0x80 == 0 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m85) cpuWrite(a, v int) {
	if a < 0x8000 {
		if m.ctl&0x80 != 0 {
			m.base.cpuWrite(a, v)
		}
		return
	}
	hi := 0
	if a&m.hiLine != 0 {
		hi = 1
	}
	switch a & 0xf000 {
	case 0x8000:
		m.p[hi] = v & 0x3f
	case 0x9000:
		if a&0x10 != 0 {
			if a&0x20 != 0 {
				m.snd.writeData(v)
			} else {
				m.snd.writeAddr(v)
			}
			return
		}
		if hi == 0 {
			m.p[2] = v & 0x3f
		}
	case 0xa000, 0xb000, 0xc000, 0xd000:
		m.c[((a>>12)-0xa)*2+hi] = v
	case 0xe000:
		if hi == 0 {
			m.ctl = v
			m.snd.mute = v&0x40 != 0
		} else {
			m.vrc.latch = v
		}
	case 0xf000:
		if hi == 0 {
			m.vrc.control(v)
		} else {
			m.vrc.ack()
		}
		return
	}
	m.update()
}
func (m *m85) update() {
	m.setPrg8(0, m.p[0])
	m.setPrg8(1, m.p[1])
	m.setPrg8(2, m.p[2])
	m.setPrg8(3, -1)
	for i := 0; i < 8; i++ {
		m.setChr1(i, m.c[i])
	}
	m.mirror = vh1[m.ctl&3]
}
func (m *m85) cpuClock() { m.vrc.clock(); m.irqF = m.vrc.pending; m.snd.clock() }
func (m *m85) state(s *sio) {
	m.baseState(s)
	s.IntSlice(m.p[:])
	s.IntSlice(m.c[:])
	s.Int(&m.ctl)
	m.vrc.state(s)
}

// ===================================================================
// Bandai
// ===================================================================

// eeprom is a serial EEPROM (I2C) used for saves on Bandai boards: the 24C02
// (256 bytes, device address + word address, MSB first) and the older X24C01
// (128 bytes, address sent directly, LSB first).
type eeprom struct {
	data                               []byte
	x01                                bool
	scl, sda, st, bits, byt, addr, out int
	next, ackIn, purpose               int
}

func newEeprom(size int, x24c01 bool) *eeprom {
	return &eeprom{data: make([]byte, size), x01: x24c01, scl: 1, sda: 1, out: 1, ackIn: 1}
}

// states: 0 idle, 1 receiving a byte, 2 sending our ACK, 3 sending a byte, 4 waiting for the master's ACK
func (e *eeprom) set(scl, sda int) {
	if e.scl != 0 && scl != 0 {
		if e.sda != 0 && sda == 0 {
			e.start()
		} else if e.sda == 0 && sda != 0 {
			e.st = 0
			e.out = 1
		}
	} else if e.scl == 0 && scl != 0 {
		e.rise(sda)
	} else if e.scl != 0 && scl == 0 {
		e.fall()
	}
	e.scl, e.sda = scl, sda
}
func (e *eeprom) start() { e.st, e.bits, e.byt, e.purpose, e.out = 1, 0, 0, 0, 1 }
func (e *eeprom) rise(sda int) {
	switch e.st {
	case 1:
		if e.x01 {
			e.byt |= sda << e.bits
		} else {
			e.byt = e.byt<<1 | sda
		}
		e.bits++
	case 3:
		e.bits++
	case 4:
		e.ackIn = sda
	}
}
func (e *eeprom) loadByte() {
	e.byt = int(e.data[e.addr])
	e.addr = (e.addr + 1) % len(e.data)
	e.bits = 0
	e.st = 3
	e.out = e.bitOut(0)
}
func (e *eeprom) bitOut(i int) int {
	if e.x01 {
		return (e.byt >> i) & 1
	}
	return (e.byt >> (7 - i)) & 1
}
func (e *eeprom) fall() {
	switch e.st {
	case 1:
		if e.bits < 8 {
			return
		}
		if e.x01 {
			if e.purpose == 0 {
				e.addr = e.byt & 0x7f
				e.next = 1
				if e.byt&0x80 != 0 {
					e.next = 3
				}
				e.purpose = 2
			} else {
				e.data[e.addr] = byte(e.byt)
				e.addr = e.addr&0x7c | (e.addr+1)&3
				e.next = 1
			}
		} else if e.purpose == 0 {
			if e.byt>>4 != 0xa {
				e.st = 0
				e.out = 1
				return
			}
			e.next = 1
			if e.byt&1 != 0 {
				e.next = 3
			}
			e.purpose = 1
		} else if e.purpose == 1 {
			e.addr = e.byt
			e.next = 1
			e.purpose = 2
		} else {
			e.data[e.addr] = byte(e.byt)
			e.addr = e.addr&0xf0 | (e.addr+1)&0x0f
			e.next = 1
		}
		e.st = 2
		e.out = 0
	case 2:
		if e.next == 3 {
			e.loadByte()
		} else {
			e.st, e.bits, e.byt, e.out = 1, 0, 0, 1
		}
	case 3:
		if e.bits < 8 {
			e.out = e.bitOut(e.bits)
		} else {
			e.st, e.out, e.ackIn = 4, 1, 1
		}
	case 4:
		if e.ackIn == 0 {
			e.loadByte()
		} else {
			e.st = 0
			e.out = 1
		}
	}
}
func (e *eeprom) state(s *sio) {
	s.Bytes(e.data)
	s.Ints(&e.scl, &e.sda, &e.st, &e.bits, &e.byt, &e.addr, &e.out, &e.next, &e.ackIn, &e.purpose)
}

// 16 / 153 / 159 — Bandai FCG-1/2, LZ93D50 (Dragon Ball, SD Gundam, Famicom Jump II)
type m16 struct {
	*base
	kind                         int
	lowRegs, highRegs, copyLatch bool
	c                            [8]int
	prgReg                       int
	irqOn                        bool
	counter, latch               int
	ramOn                        bool
	eepCtl                       int
	eep                          *eeprom
}

func newM16(n *NES, c *cart, kind int) *m16 {
	sub := c.submapper
	m := &m16{base: newBase(n, c), kind: kind}
	m.lowRegs = kind == 16 && sub != 5     // FCG-1/2 registers at $6000
	m.highRegs = !(kind == 16 && sub == 4) // LZ93D50 registers at $8000
	m.copyLatch = !(kind == 16 && sub == 4)
	if kind == 159 {
		m.eep = newEeprom(128, true)
	} else if kind == 16 && sub != 4 {
		m.eep = newEeprom(256, false)
	}
	m.update()
	return m
}
func (m *m16) batteryData() []byte {
	if m.eep == nil {
		return nil
	}
	return m.eep.data
}
func (m *m16) loadBattery(d []byte) {
	if m.eep != nil {
		copy(m.eep.data, d)
	}
}
func (m *m16) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 {
		if m.kind == 153 {
			if m.ramOn {
				return int(m.prgRam[a&0x1fff])
			}
			return -1
		}
		bit := 0
		if m.eep != nil && m.eepCtl&0x80 != 0 {
			bit = m.eep.out
		}
		return m.openBus()&0xef | bit<<4
	}
	return m.base.cpuRead(a)
}
func (m *m16) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		if m.kind == 153 {
			if m.ramOn {
				m.prgRam[a&0x1fff] = byte(v)
			}
			return
		}
		if !m.lowRegs {
			return
		}
	} else if a < 0x6000 || !m.highRegs {
		return
	}
	switch a & 0x0f {
	case 0, 1, 2, 3, 4, 5, 6, 7:
		m.c[a&7] = v
	case 8:
		m.prgReg = v
	case 9:
		m.mirror = vh1[v&3]
	case 0xa:
		m.irqOn = v&1 != 0
		if m.copyLatch {
			m.counter = m.latch
		}
		m.irqF = false
	case 0xb:
		m.latch = m.latch&0xff00 | v
		if !m.copyLatch || m.lowRegs {
			m.counter = m.counter&0xff00 | v
		}
	case 0xc:
		m.latch = m.latch&0xff | v<<8
		if !m.copyLatch || m.lowRegs {
			m.counter = m.counter&0xff | v<<8
		}
	case 0xd:
		if m.kind == 153 {
			m.ramOn = v&0x20 != 0
		} else {
			m.eepCtl = v
			if m.eep != nil {
				m.eep.set((v>>5)&1, (v>>6)&1)
			}
		}
	}
	m.update()
}
func (m *m16) update() {
	if m.kind == 153 {
		outer := 0
		if (m.c[0]|m.c[1]|m.c[2]|m.c[3])&1 != 0 {
			outer = 0x10
		}
		m.setPrg16(0, outer|m.prgReg&0x0f)
		m.setPrg16(1, outer|0x0f)
	} else {
		m.setPrg16(0, m.prgReg&0x0f)
		m.setPrg16(1, -1)
		for i := 0; i < 8; i++ {
			m.setChr1(i, m.c[i])
		}
	}
}
func (m *m16) cpuClock() {
	if !m.irqOn {
		return
	}
	m.counter = (m.counter - 1) & 0xffff
	if m.counter == 0 {
		m.irqF = true
	}
}
func (m *m16) state(s *sio) {
	m.baseState(s)
	s.IntSlice(m.c[:])
	s.Ints(&m.prgReg, &m.counter, &m.latch, &m.eepCtl)
	s.Bools(&m.irqOn, &m.ramOn)
	if m.eep != nil {
		m.eep.state(s)
	}
}

// 70 — Bandai 74161 (Kamen Rider Club); 152 — the same with one-screen mirroring control
type m70 struct {
	*base
	oneScreen bool
}

func newM70(n *NES, c *cart, oneScreen bool) *m70 {
	m := &m70{newBase(n, c), oneScreen}
	m.setPrg16(1, -1)
	return m
}
func (m *m70) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	mask := 15
	if m.oneScreen {
		mask = 7
	}
	m.setPrg16(0, (v>>4)&mask)
	m.setChr8(v & 15)
	if m.oneScreen {
		m.mirror = hiOrLo(v&0x80 != 0)
	}
}

// ===================================================================
// Namco
// ===================================================================

// 19 — Namco 163 (Megami Tensei II, King of Kings, Final Lap) with its wavetable sound;
// 210 — Namco 175 / 340 (Famista, Wagyan Land 2): the same banking without the extras.
type m19 struct {
	*base
	kind    int // 163, 175 or 340 (0 = not known yet)
	ciram   []byte
	c       [12]int // 8 pattern windows + 4 nametables
	p       [3]int
	ramOff  [2]int
	counter int
	ram175  bool
	snd     *n163
}

func newM19(n *NES, c *cart, kind int) *m19 {
	m := &m19{base: newBase(n, c), kind: kind, ciram: c.vram}
	for i := range m.c {
		m.c[i] = 0xff
	}
	for i := 8; i < 12; i++ {
		if c.mirror == mirV {
			m.c[i] = 0xe0 | i&1
		} else {
			m.c[i] = 0xe0 | (i>>1)&1
		}
	}
	if kind == 163 {
		m.snd = newN163()
		m.audio = m.snd
	}
	m.update()
	return m
}
func (m *m19) update() {
	m.setPrg8(0, m.p[0])
	m.setPrg8(1, m.p[1])
	m.setPrg8(2, m.p[2])
	m.setPrg8(3, -1)
}
func (m *m19) cpuRead(a int) int {
	if m.kind == 163 {
		switch {
		case a >= 0x4800 && a < 0x5000:
			return m.snd.readData()
		case a >= 0x5000 && a < 0x5800:
			return m.counter & 0xff
		case a >= 0x5800 && a < 0x6000:
			return m.counter >> 8
		}
	}
	if a >= 0x6000 && a < 0x8000 && m.kind == 175 && !m.ram175 {
		return -1
	}
	if a >= 0x6000 && a < 0x8000 && m.kind == 340 {
		return -1
	}
	return m.base.cpuRead(a)
}

var n340Mirror = [4]int{mirOneLo, mirV, mirOneHi, mirH}

func (m *m19) cpuWrite(a, v int) {
	if a < 0x6000 {
		if m.kind != 163 {
			return
		}
		switch {
		case a >= 0x4800 && a < 0x5000:
			m.snd.writeData(v)
		case a >= 0x5000 && a < 0x5800:
			m.counter = m.counter&0xff00 | v
			m.irqF = false
		case a >= 0x5800:
			m.counter = m.counter&0xff | v<<8
			m.irqF = false
		}
		return
	}
	if a < 0x8000 {
		if m.kind != 175 || m.ram175 {
			m.base.cpuWrite(a, v)
		}
		return
	}
	if a < 0xc000 {
		m.c[(a-0x8000)>>11] = v
		return
	}
	if a < 0xe000 {
		if m.kind == 163 {
			m.c[8+(a-0xc000)>>11] = v
		} else if m.kind == 175 && a < 0xc800 {
			m.ram175 = v&1 != 0
		}
		return
	}
	switch {
	case a < 0xe800:
		m.p[0] = v & 0x3f
		if m.kind == 163 {
			m.snd.disabled = v&0x40 != 0
		}
		if m.kind == 0 && v&0xc0 != 0 {
			m.kind = 340 // a game that sets mirroring bits is on a 340
		}
		if m.kind == 340 {
			m.mirror = n340Mirror[v>>6]
		}
	case a < 0xf000:
		m.p[1] = v & 0x3f
		m.ramOff = [2]int{(v >> 6) & 1, (v >> 7) & 1}
	case a < 0xf800:
		m.p[2] = v & 0x3f
	default:
		if m.kind == 163 {
			m.snd.addr = v
		}
	}
	m.update()
}

// window returns an index into CHR, or into CIRAM with bit 20 set.
func (m *m19) window(slot, a int) int {
	r := m.c[slot]
	if m.kind == 163 && r >= 0xe0 && (slot >= 8 || m.ramOff[slot>>2] == 0) {
		return (r&1)<<10 | a&0x3ff | 0x100000
	}
	return (r%(len(m.chr)>>10))*0x400 + a&0x3ff
}
func (m *m19) ppuRead(a int) int {
	i := m.window(a>>10, a)
	if i&0x100000 != 0 {
		return int(m.ciram[i&0x7ff])
	}
	return int(m.chr[i])
}
func (m *m19) ppuWrite(a, v int) {
	i := m.window(a>>10, a)
	if i&0x100000 != 0 {
		m.ciram[i&0x7ff] = byte(v)
	} else if m.chrRam {
		m.chr[i] = byte(v)
	}
}
func (m *m19) ntRead(a int) int {
	if m.kind != 163 {
		return int(m.ciram[m.ntIndex(a)])
	}
	i := m.window(8+(a>>10)&3, a)
	if i&0x100000 != 0 {
		return int(m.ciram[i&0x7ff])
	}
	return int(m.chr[i])
}
func (m *m19) ntWrite(a, v int) {
	if m.kind != 163 {
		m.ciram[m.ntIndex(a)] = byte(v)
		return
	}
	i := m.window(8+(a>>10)&3, a)
	if i&0x100000 != 0 {
		m.ciram[i&0x7ff] = byte(v)
	}
}
func (m *m19) cpuClock() {
	if m.kind == 163 {
		if m.counter&0x8000 != 0 && m.counter&0x7fff != 0x7fff {
			m.counter++
			if m.counter&0x7fff == 0x7fff {
				m.irqF = true
			}
		}
		m.snd.clock()
	}
}
func (m *m19) state(s *sio) {
	m.baseState(s)
	s.Ints(&m.kind, &m.counter)
	s.IntSlice(m.c[:])
	s.IntSlice(m.p[:])
	s.IntSlice(m.ramOff[:])
	s.Bool(&m.ram175)
}

// 76 — Namco 3446 (Digital Devil Story): Namco 108 with 2 KB CHR banks
type m76 struct {
	*base
	sel int
	r   [8]int
}

func newM76(n *NES, c *cart) *m76 { m := &m76{base: newBase(n, c)}; m.update(); return m }
func (m *m76) cpuWrite(a, v int) {
	if a < 0x8000 || a >= 0xa000 {
		m.base.cpuWrite(a, v)
		return
	}
	if a&1 == 0 {
		m.sel = v & 7
	} else {
		m.r[m.sel] = v
	}
	m.update()
}
func (m *m76) update() {
	m.setPrg8(0, m.r[6])
	m.setPrg8(1, m.r[7])
	m.setPrg8(2, -2)
	m.setPrg8(3, -1)
	for i := 0; i < 4; i++ {
		m.setChr2(i, m.r[2+i])
	}
}
func (m *m76) state(s *sio) { m.baseState(s); s.Int(&m.sel); s.IntSlice(m.r[:]) }

// 88 / 154 — Namco 108 with the two halves of CHR split by A12; 154 adds one-screen mirroring.
// 95 — Namco 108 where CHR bank bit 5 also picks the nametable (Dragon Buster).
type n108 struct {
	*base
	variant, sel int
	r            [8]int
}

func newN108(n *NES, c *cart, variant int) *n108 {
	m := &n108{base: newBase(n, c), variant: variant, r: [8]int{0, 2, 4, 5, 6, 7, 0, 1}}
	m.update()
	return m
}
func (m *n108) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	if m.variant == 154 {
		m.mirror = hiOrLo(v&0x40 != 0)
	}
	if a >= 0xa000 {
		return
	}
	if a&1 == 0 {
		m.sel = v & 7
	} else {
		m.r[m.sel] = v & 0x3f
	}
	m.update()
}
func (m *n108) update() {
	r := &m.r
	m.setPrg8(0, r[6]&0x0f)
	m.setPrg8(1, r[7]&0x0f)
	m.setPrg8(2, -2)
	m.setPrg8(3, -1)
	split := m.variant == 88 || m.variant == 154
	lo := func(x int) int {
		if split {
			return x & 0x3f &^ 0x40
		}
		return x
	}
	hi := func(x int) int {
		if split {
			return x | 0x40
		}
		return x
	}
	m.setChr2(0, lo(r[0])>>1)
	m.setChr2(1, lo(r[1])>>1)
	for i := 0; i < 4; i++ {
		m.setChr1(4+i, hi(r[2+i]))
	}
	if m.variant == 95 {
		m.setNt((r[0]>>5)&1, (r[0]>>5)&1, (r[1]>>5)&1, (r[1]>>5)&1)
	}
}
func (m *n108) state(s *sio) { m.baseState(s); s.Int(&m.sel); s.IntSlice(m.r[:]) }

// ===================================================================
// Jaleco
// ===================================================================

// 18 — Jaleco SS88006 (Ninja Jajamaru, Pizza Pop!, Magic John)
type m18 struct {
	*base
	p                          [3]int
	c                          [8]int
	latch, counter, ctl, ramOn int
}

func newM18(n *NES, c *cart) *m18 { m := &m18{base: newBase(n, c)}; m.update(); return m }
func (m *m18) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 && m.ramOn&1 == 0 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m18) cpuWrite(a, v int) {
	if a < 0x8000 {
		if m.ramOn&3 == 3 {
			m.base.cpuWrite(a, v)
		}
		return
	}
	r, nib := ((a-0x8000)>>12)*4+a&3, v&0x0f
	switch {
	case r < 6:
		i := r >> 1
		if r&1 != 0 {
			m.p[i] = m.p[i]&0x0f | nib<<4
		} else {
			m.p[i] = m.p[i]&0xf0 | nib
		}
	case r == 6:
		m.ramOn = v & 3
	case r >= 8 && r < 24:
		i := (r - 8) >> 1
		if r&1 != 0 {
			m.c[i] = m.c[i]&0x0f | nib<<4
		} else {
			m.c[i] = m.c[i]&0xf0 | nib
		}
	case r >= 24 && r < 28:
		sh := (r - 24) * 4
		m.latch = m.latch&^(0xf<<sh) | nib<<sh
	case r == 28:
		m.counter = m.latch
		m.irqF = false
	case r == 29:
		m.ctl = v & 0x0f
		m.irqF = false
	case r == 30:
		m.mirror = hv1[v&3]
	}
	m.update()
}
func (m *m18) update() {
	for i := 0; i < 3; i++ {
		m.setPrg8(i, m.p[i])
	}
	m.setPrg8(3, -1)
	for i := 0; i < 8; i++ {
		m.setChr1(i, m.c[i])
	}
}
func (m *m18) cpuClock() {
	if m.ctl&1 == 0 {
		return
	}
	c := m.ctl
	mask := 0xffff
	switch {
	case c&8 != 0:
		mask = 0xf
	case c&4 != 0:
		mask = 0xff
	case c&2 != 0:
		mask = 0xfff
	}
	low := m.counter & mask
	if low == 0 {
		m.irqF = true
	}
	m.counter = m.counter&^mask | (low-1)&mask
}
func (m *m18) state(s *sio) {
	m.baseState(s)
	s.IntSlice(m.p[:])
	s.IntSlice(m.c[:])
	s.Ints(&m.latch, &m.counter, &m.ctl, &m.ramOn)
}

// 72 / 92 — Jaleco JF-17 / JF-19: bit 7 latches the PRG bank, bit 6 the CHR bank
type m72 struct {
	*base
	upper bool
	last  int
}

func newM72(n *NES, c *cart, upper bool) *m72 {
	m := &m72{base: newBase(n, c), upper: upper}
	if upper {
		m.setPrg16(0, 0)
	} else {
		m.setPrg16(1, -1)
	}
	return m
}
func (m *m72) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	if v&0x80 != 0 && m.last&0x80 == 0 {
		slot := 0
		if m.upper {
			slot = 1
		}
		m.setPrg16(slot, v&0x0f)
	}
	if v&0x40 != 0 && m.last&0x40 == 0 {
		m.setChr8(v & 0x0f)
	}
	m.last = v
}
func (m *m72) state(s *sio) { m.baseState(s); s.Int(&m.last) }

// 86 — Jaleco JF-13 (Moero!! Pro Yakyuu); 87 — Jaleco/Konami CNROM-like at $6000
type m86 struct{ *base }

func (m *m86) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x7000 {
		m.setPrg32((v >> 4) & 3)
		m.setChr8(v&3 | (v>>4)&4)
	}
}

type m87 struct{ *base }

func (m *m87) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		m.setChr8((v&1)<<1 | (v>>1)&1)
	}
}

// 78 — Irem 74HC161 (Holy Diver) / Jaleco JF-16 (Uchuusen Cosmo Carrier)
type m78 struct {
	*base
	hv bool
}

func newM78(n *NES, c *cart) *m78 {
	m := &m78{base: newBase(n, c)}
	m.setPrg16(1, -1)
	sub := c.submapper
	m.hv = sub == 3 || (sub != 1 && (c.mirror == mirV || c.mirror == mirFour))
	if m.hv {
		m.mirror = mirH
	} else {
		m.mirror = mirOneLo
	}
	return m
}
func (m *m78) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	m.setPrg16(0, v&7)
	m.setChr8(v >> 4)
	if m.hv {
		m.mirror = vOrH(v&8 != 0)
	} else {
		m.mirror = hiOrLo(v&8 != 0)
	}
}

// ===================================================================
// Irem
// ===================================================================

// 32 — Irem G-101 (Image Fight, Major League, Kaiketsu Yanchamaru 2)
type m32 struct {
	*base
	p0, p1, mode int
	major        bool
}

func newM32(n *NES, c *cart) *m32 {
	m := &m32{base: newBase(n, c), major: c.submapper == 1}
	if m.major {
		m.mirror = mirOneLo
	}
	m.update()
	return m
}
func (m *m32) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf000 {
	case 0x8000:
		m.p0 = v & 0x1f
	case 0x9000:
		if !m.major {
			m.mirror = hOrV(v&1 != 0)
			m.mode = (v >> 1) & 1
		}
	case 0xa000:
		m.p1 = v & 0x1f
	case 0xb000:
		m.setChr1(a&7, v)
		return
	}
	m.update()
}
func (m *m32) update() {
	if m.mode != 0 {
		m.setPrg8(0, -2)
		m.setPrg8(2, m.p0)
	} else {
		m.setPrg8(0, m.p0)
		m.setPrg8(2, -2)
	}
	m.setPrg8(1, m.p1)
	m.setPrg8(3, -1)
}
func (m *m32) state(s *sio) { m.baseState(s); s.Ints(&m.p0, &m.p1, &m.mode) }

// 65 — Irem H3001 (Daiku no Gen-san 2, Spartan X 2)
type m65 struct {
	*base
	p              [3]int
	mode           int
	irqOn          bool
	counter, latch int
}

func newM65(n *NES, c *cart) *m65 {
	m := &m65{base: newBase(n, c), p: [3]int{0, 1, 0xfe}}
	m.update()
	return m
}
func (m *m65) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf000 {
	case 0x8000:
		m.p[0] = v
	case 0x9000:
		switch a & 7 {
		case 0:
			m.mode = v >> 7
		case 1:
			m.mirror = hOrV(v&0x80 != 0)
		case 3:
			m.irqOn = v&0x80 != 0
			m.irqF = false
		case 4:
			m.counter = m.latch
			m.irqF = false
		case 5:
			m.latch = m.latch&0xff | v<<8
		case 6:
			m.latch = m.latch&0xff00 | v
		}
	case 0xa000:
		m.p[1] = v
	case 0xb000:
		m.setChr1(a&7, v)
		return
	case 0xc000:
		m.p[2] = v
	}
	m.update()
}
func (m *m65) update() {
	if m.mode != 0 {
		m.setPrg8(0, m.p[2])
		m.setPrg8(2, m.p[0])
	} else {
		m.setPrg8(0, m.p[0])
		m.setPrg8(2, m.p[2])
	}
	m.setPrg8(1, m.p[1])
	m.setPrg8(3, -1)
}
func (m *m65) cpuClock() {
	if m.irqOn && m.counter > 0 {
		m.counter--
		if m.counter == 0 {
			m.irqF = true
			m.irqOn = false
		}
	}
}
func (m *m65) state(s *sio) {
	m.baseState(s)
	s.IntSlice(m.p[:])
	s.Ints(&m.mode, &m.counter, &m.latch)
	s.Bool(&m.irqOn)
}

// 77 — Irem (Napoleon Senki): 2 KB of switchable CHR ROM, 6 KB CHR RAM and four-screen nametables
type m77 struct {
	*base
	ram []byte
	cb  int
}

func newM77(n *NES, c *cart) *m77 {
	m := &m77{base: newBase(n, c), ram: make([]byte, 0x2000)}
	m.mirror = mirFour
	m.setPrg32(0)
	return m
}
func (m *m77) cpuWrite(a, v int) {
	if a >= 0x8000 {
		v &= m.cpuRead(a)
		m.setPrg32(v & 0x0f)
		m.cb = v >> 4
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m77) ppuRead(a int) int {
	if a < 0x800 {
		return int(m.chr[(m.cb*0x800+a)%len(m.chr)])
	}
	return int(m.ram[a])
}
func (m *m77) ppuWrite(a, v int) {
	if a >= 0x800 {
		m.ram[a] = byte(v)
	}
}
func (m *m77) state(s *sio) { m.baseState(s); s.Bytes(m.ram); s.Int(&m.cb) }

// 97 — Irem TAM-S1 (Kaiketsu Yanchamaru): fixed $8000, switchable $C000
type m97 struct{ *base }

func newM97(n *NES, c *cart) *m97 {
	m := &m97{newBase(n, c)}
	m.setPrg16(0, -1)
	m.setPrg16(1, 0)
	return m
}

var m97Mirror = [4]int{mirOneLo, mirH, mirV, mirOneHi}

func (m *m97) cpuWrite(a, v int) {
	if a < 0x8000 || a >= 0xc000 {
		m.base.cpuWrite(a, v)
		return
	}
	m.setPrg16(1, v&0x1f)
	m.mirror = m97Mirror[v>>6]
}

// ===================================================================
// Taito
// ===================================================================

// 33 — Taito TC0190 (Akira, Don Doko Don); 48 — TC0690 (Bubble Bobble 2, The Jetsons) adds an MMC3-style IRQ
type m33 struct {
	*m4
	tc0690 bool
	p      [2]int
	c      [6]int
}

func newM33(n *NES, c *cart, tc0690 bool) *m33 {
	m := &m33{m4: newM4(n, c), tc0690: tc0690, p: [2]int{0, 1}}
	m.upd = m.update
	m.update()
	return m
}
func (m *m33) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xe003 {
	case 0x8000:
		m.p[0] = v & 0x3f
		if !m.tc0690 {
			m.mirror = hOrV(v&0x40 != 0)
		}
	case 0x8001:
		m.p[1] = v & 0x3f
	case 0x8002:
		m.c[0] = v
	case 0x8003:
		m.c[1] = v
	case 0xa000, 0xa001, 0xa002, 0xa003:
		m.c[2+a&3] = v
	case 0xc000:
		if m.tc0690 {
			m.irqLatch = v ^ 0xff
		}
	case 0xc001:
		if m.tc0690 {
			m.irqCounter = 0
			m.irqReload = true
		}
	case 0xc002:
		if m.tc0690 {
			m.irqEnable = true
		}
	case 0xc003:
		if m.tc0690 {
			m.irqEnable = false
			m.irqF = false
		}
	case 0xe000:
		if m.tc0690 {
			m.mirror = hOrV(v&0x40 != 0)
		}
	}
	m.update()
}
func (m *m33) update() {
	m.setPrg8(0, m.p[0])
	m.setPrg8(1, m.p[1])
	m.setPrg8(2, -2)
	m.setPrg8(3, -1)
	m.setChr2(0, m.c[0])
	m.setChr2(1, m.c[1])
	for i := 0; i < 4; i++ {
		m.setChr1(4+i, m.c[2+i])
	}
}
func (m *m33) ppuRead(a int) int {
	if m.tc0690 {
		m.ppuAddr(a)
	}
	return m.base.ppuRead(a)
}
func (m *m33) state(s *sio) { m.m4State(s); s.IntSlice(m.p[:]); s.IntSlice(m.c[:]) }

// 80 — Taito X1-005 (Minelvaton Saga, Kyonshiizu 2); 207 — the same board with
// nametables picked by CHR bank bit 7 (Fudou Myouou Den)
type m80 struct {
	*base
	alt   bool
	ram   []byte
	ramOn bool
	c     [6]int
	p     [3]int
}

func newM80(n *NES, c *cart, alt bool) *m80 {
	m := &m80{base: newBase(n, c), alt: alt, ram: make([]byte, 128), p: [3]int{0, 1, 0xfe}}
	m.update()
	return m
}
func (m *m80) batteryData() []byte  { return m.ram }
func (m *m80) loadBattery(d []byte) { copy(m.ram, d) }
func (m *m80) cpuRead(a int) int {
	if a >= 0x7f00 && a < 0x8000 {
		if m.ramOn {
			return int(m.ram[a&0x7f])
		}
		return -1
	}
	if a < 0x8000 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m80) cpuWrite(a, v int) {
	if a >= 0x7f00 && a < 0x8000 {
		if m.ramOn {
			m.ram[a&0x7f] = byte(v)
		}
		return
	}
	if a < 0x7ef0 || a > 0x7eff {
		return
	}
	r := a & 0x0f
	switch {
	case r < 6:
		m.c[r] = v
	case r < 8:
		if !m.alt {
			m.mirror = vOrH(v&1 != 0)
		}
	case r < 10:
		m.ramOn = v == 0xa3
	default:
		m.p[(r-10)>>1] = v
	}
	m.update()
}
func (m *m80) update() {
	c := &m.c
	m.setChr2(0, (c[0]&0x7f)>>1)
	m.setChr2(1, (c[1]&0x7f)>>1)
	for i := 0; i < 4; i++ {
		m.setChr1(4+i, c[2+i])
	}
	m.setPrg8(0, m.p[0])
	m.setPrg8(1, m.p[1])
	m.setPrg8(2, m.p[2])
	m.setPrg8(3, -1)
	if m.alt {
		m.setNt(c[0]>>7, c[0]>>7, c[1]>>7, c[1]>>7)
	}
}
func (m *m80) state(s *sio) {
	m.baseState(s)
	s.Bytes(m.ram)
	s.Bool(&m.ramOn)
	s.IntSlice(m.c[:])
	s.IntSlice(m.p[:])
}

// 82 — Taito X1-017 (SD Keiji Blader, Kyuukyoku Harikiri Stadium)
type m82 struct {
	*base
	c     [6]int
	p     [3]int
	inv   int
	ramOn [3]int
}

func newM82(n *NES, c *cart) *m82 {
	m := &m82{base: newBase(n, c), p: [3]int{0, 1, 2}}
	m.update()
	return m
}
func ramSlot82(a int) int {
	switch {
	case a < 0x6800:
		return 0
	case a < 0x7000:
		return 1
	case a < 0x7400:
		return 2
	}
	return -1
}
func (m *m82) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 {
		if s := ramSlot82(a); s >= 0 && m.ramOn[s] != 0 {
			return int(m.prgRam[a-0x6000])
		}
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m82) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x7ef0 {
		if s := ramSlot82(a); s >= 0 && m.ramOn[s] != 0 {
			m.prgRam[a-0x6000] = byte(v)
		}
		return
	}
	if a < 0x7ef0 || a > 0x7eff {
		return
	}
	r := a & 0x0f
	switch {
	case r < 6:
		m.c[r] = v
	case r == 6:
		m.mirror = vOrH(v&1 != 0)
		m.inv = (v >> 1) & 1
	case r == 7:
		m.ramOn[0] = b2i(v == 0xca)
	case r == 8:
		m.ramOn[1] = b2i(v == 0x69)
	case r == 9:
		m.ramOn[2] = b2i(v == 0x84)
	case r < 13:
		m.p[r-10] = v >> 2
	}
	m.update()
}
func (m *m82) update() {
	c := &m.c
	x := 0
	if m.inv != 0 {
		x = 4
	}
	m.setChr1(0^x, c[0]&0xfe)
	m.setChr1(1^x, c[0]|1)
	m.setChr1(2^x, c[1]&0xfe)
	m.setChr1(3^x, c[1]|1)
	for i := 0; i < 4; i++ {
		m.setChr1((4+i)^x, c[2+i])
	}
	m.setPrg8(0, m.p[0])
	m.setPrg8(1, m.p[1])
	m.setPrg8(2, m.p[2])
	m.setPrg8(3, -1)
}
func (m *m82) state(s *sio) {
	m.baseState(s)
	s.IntSlice(m.c[:])
	s.IntSlice(m.p[:])
	s.Int(&m.inv)
	s.IntSlice(m.ramOn[:])
}

// ===================================================================
// Sunsoft
// ===================================================================

// 67 — Sunsoft-3 (Fantasy Zone II, Mito Koumon)
type m67 struct {
	*base
	counter int
	irqOn   bool
	toggle  int
}

func newM67(n *NES, c *cart) *m67 { m := &m67{base: newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m67) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xf800 {
	case 0x8800:
		m.setChr2(0, v)
	case 0x9800:
		m.setChr2(1, v)
	case 0xa800:
		m.setChr2(2, v)
	case 0xb800:
		m.setChr2(3, v)
	case 0xc800:
		m.toggle ^= 1
		if m.toggle != 0 {
			m.counter = m.counter&0xff | v<<8
		} else {
			m.counter = m.counter&0xff00 | v
		}
	case 0xd800:
		m.irqOn = v&0x10 != 0
		m.toggle = 0
		m.irqF = false
	case 0xe800:
		m.mirror = vh1[v&3]
	case 0xf800:
		m.setPrg16(0, v)
	}
}
func (m *m67) cpuClock() {
	if !m.irqOn {
		return
	}
	if m.counter == 0 {
		m.counter = 0xffff
		m.irqF = true
		m.irqOn = false
	} else {
		m.counter--
	}
}
func (m *m67) state(s *sio) { m.baseState(s); s.Ints(&m.counter, &m.toggle); s.Bool(&m.irqOn) }

// 68 — Sunsoft-4 (After Burner, Maharaja): nametables can come from CHR ROM
type m68 struct {
	*base
	ciram        []byte
	nt           [2]int
	ntRom, ramOn bool
}

func newM68(n *NES, c *cart) *m68 {
	m := &m68{base: newBase(n, c), ciram: c.vram, nt: [2]int{0x80, 0x80}}
	m.setPrg16(1, -1)
	return m
}
func (m *m68) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x8000 && !m.ramOn {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m68) cpuWrite(a, v int) {
	if a < 0x8000 {
		if m.ramOn {
			m.base.cpuWrite(a, v)
		}
		return
	}
	switch a & 0xf000 {
	case 0x8000:
		m.setChr2(0, v)
	case 0x9000:
		m.setChr2(1, v)
	case 0xa000:
		m.setChr2(2, v)
	case 0xb000:
		m.setChr2(3, v)
	case 0xc000:
		m.nt[0] = v | 0x80
	case 0xd000:
		m.nt[1] = v | 0x80
	case 0xe000:
		m.mirror = vh1[v&3]
		m.ntRom = v&0x10 != 0
	case 0xf000:
		m.setPrg16(0, v&0x0f)
		m.ramOn = v&0x10 != 0
	}
}
func (m *m68) ntRead(a int) int {
	i := m.ntIndex(a)
	if !m.ntRom {
		return int(m.ciram[i])
	}
	if i>>10 > 1 {
		return 0 // (four-screen header: no such page on this board)
	}
	return int(m.chr[(m.nt[i>>10]*0x400+a&0x3ff)%len(m.chr)])
}
func (m *m68) ntWrite(a, v int) {
	if !m.ntRom {
		m.ciram[m.ntIndex(a)] = byte(v)
	}
}
func (m *m68) state(s *sio) { m.baseState(s); s.IntSlice(m.nt[:]); s.Bools(&m.ntRom, &m.ramOn) }

// 89 — Sunsoft-2 on Sunsoft-3 board (Tenka no Goikenban); 93 — Sunsoft-2 (Fantasy Zone, Shanghai)
type m89 struct{ *base }

func newM89(n *NES, c *cart) *m89 { m := &m89{newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m89) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	m.setPrg16(0, (v>>4)&7)
	m.setChr8(v&7 | (v&0x80)>>4)
	m.mirror = hiOrLo(v&8 != 0)
}

type m93 struct {
	*base
	chrOn bool
}

func newM93(n *NES, c *cart) *m93 { m := &m93{newBase(n, c), true}; m.setPrg16(1, -1); return m }
func (m *m93) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	m.setPrg16(0, (v>>4)&7)
	m.chrOn = v&1 != 0
}
func (m *m93) ppuRead(a int) int {
	if m.chrOn {
		return m.base.ppuRead(a)
	}
	return 0
}
func (m *m93) ppuWrite(a, v int) {
	if m.chrOn {
		m.base.ppuWrite(a, v)
	}
}
func (m *m93) state(s *sio) { m.baseState(s); s.Bool(&m.chrOn) }

// 184 — Sunsoft-1 (Atlantis no Nazo, Wing of Madoola)
type m184 struct{ *base }

func (m *m184) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		m.setChr4(0, v&7)
		m.setChr4(1, (v>>4)&7|4)
	}
}

// ===================================================================
// Nintendo boards built on the MMC1 / MMC3
// ===================================================================

// 105 — NES-EVENT (Nintendo World Championships 1990): MMC1 plus a countdown timer
type m105 struct {
	*m1
	started, timer, limit int
}

func newM105(n *NES, c *cart) *m105 {
	m := &m105{m1: newM1(n, c), limit: 0x20000000 | 4<<25}
	m.upd = m.update
	m.update()
	return m
}
func (m *m105) update() {
	// The CHR register's I bit holds the timer in reset; the game toggles it once to unlock banking.
	ibit := m.chr0 & 0x10
	if m.started == 0 && ibit != 0 {
		m.started = 1
	} else if m.started == 1 && ibit == 0 {
		m.started = 2
	}
	m.mirror = mmc1Mirror[m.ctrl&3]
	if m.started < 2 {
		m.setPrg32(0)
	} else if m.chr0&8 == 0 {
		m.setPrg32((m.chr0 >> 1) & 3)
	} else {
		p := m.prgReg & 7
		switch (m.ctrl >> 2) & 3 {
		case 0, 1:
			m.setPrg32(4 + p>>1)
		case 2:
			m.setPrg16(0, 8)
			m.setPrg16(1, 8|p)
		case 3:
			m.setPrg16(0, 8|p)
			m.setPrg16(1, 15)
		}
	}
	m.setChr8(0)
}
func (m *m105) cpuClock() {
	if m.chr0&0x10 != 0 {
		m.timer = 0
		m.irqF = false
	} else if m.started == 2 {
		m.timer++
		if m.timer == m.limit {
			m.irqF = true
		}
	}
}
func (m *m105) state(s *sio) { m.m1.state(s); s.Ints(&m.started, &m.timer) }

// 47 — NES-QJ (Super Spike V'Ball + Nintendo World Cup); 37 — PAL-ZZ (Super Mario Bros. + Tetris + Nintendo World Cup)
type m47 struct {
	*m4
	zz    bool
	outer int
}

func newM47(n *NES, c *cart, zz bool) *m47 {
	m := &m47{m4: newM4(n, c), zz: zz}
	m.prg8Hook = m.setPrg8
	m.chr1Hook = m.setChr1
	m.update()
	return m
}
func (m *m47) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		m.outer = v & 7
		m.upd()
		return
	}
	m.m4.cpuWrite(a, v)
}
func (m *m47) setPrg8(slot, bank int) {
	if !m.zz {
		m.rawPrg8(slot, bank&0x0f|(m.outer&1)<<4)
		return
	}
	q := m.outer
	b := 0
	if q&4 != 0 {
		b |= 0x10
	}
	if q&3 == 3 {
		b |= 0x08
	}
	m.rawPrg8(slot, b|bank&7)
}
func (m *m47) setChr1(slot, bank int) {
	o := m.outer & 1
	if m.zz {
		o = (m.outer >> 2) & 1
	}
	m.rawChr1(slot, bank&0x7f|o<<7)
}
func (m *m47) state(s *sio) { m.m4State(s); s.Int(&m.outer) }

// 118 — TxSROM (Armadillo, NES Play Action Football): CHR bank bit 7 picks each nametable
type m118 struct{ *m4 }

func newM118(n *NES, c *cart) *m118 {
	m := &m118{newM4(n, c)}
	m.upd = m.update
	m.update()
	return m
}
func (m *m118) update() {
	m.m4.update()
	r := &m.r
	// The bank register that covers each of the first four 1 KB CHR windows decides that nametable.
	reg := [4]int{r[0], r[0], r[1], r[1]}
	if m.bankSel&0x80 != 0 {
		reg = [4]int{r[2], r[3], r[4], r[5]}
	}
	m.setNt(reg[0]>>7, reg[1]>>7, reg[2]>>7, reg[3]>>7)
}
func (m *m118) cpuWrite(a, v int) {
	if a&0xe001 == 0xa000 {
		return
	}
	m.m4.cpuWrite(a, v)
}

// 119 — TQROM (Pinbot, High Speed): CHR ROM plus 8 KB CHR RAM, picked by bank bit 6.
// 74 / 191 / 192 / 194 / 195 — Chinese MMC3 boards where some bank numbers select CHR RAM instead of ROM.
type m119 struct {
	*m4
	isRam   func(b int) bool
	vram8   []byte
	ramSlot [8]int
}

func newM119(n *NES, c *cart, isRam func(int) bool) *m119 {
	m := &m119{m4: newM4(n, c), isRam: isRam, vram8: make([]byte, 0x2000)}
	if m.isRam == nil {
		m.isRam = func(b int) bool { return b&0x40 != 0 }
	}
	m.chr1Hook = m.setChr1
	m.update()
	return m
}
func (m *m119) setChr1(slot, bank int) {
	if m.isRam(bank & 0xff) {
		m.ramSlot[slot] = 1
		m.chrMap[slot] = (bank & 7) * 0x400
		return
	}
	m.ramSlot[slot] = 0
	m.rawChr1(slot, bank)
}
func (m *m119) ppuRead(a int) int {
	m.ppuAddr(a)
	s := a >> 10
	if m.ramSlot[s] != 0 {
		return int(m.vram8[m.chrMap[s]+a&0x3ff])
	}
	return int(m.chr[m.chrMap[s]+a&0x3ff])
}
func (m *m119) ppuWrite(a, v int) {
	s := a >> 10
	if m.ramSlot[s] != 0 {
		m.vram8[m.chrMap[s]+a&0x3ff] = byte(v)
	}
}
func (m *m119) state(s *sio) { m.m4State(s); s.Bytes(m.vram8); s.IntSlice(m.ramSlot[:]) }

// 64 — Tengen RAMBO-1 (Klax, Skull & Crossbones, Shinobi)
type m64 struct {
	*m4
	r16                  [16]int
	cycleMode, prescaler int
}

func newM64(n *NES, c *cart) *m64 {
	m := &m64{m4: newM4(n, c), r16: [16]int{0, 2, 4, 5, 6, 7, 0, 1, 1, 3, 0, 0, 0, 0, 0, 2}}
	m.upd, m.clk = m.update, m.clockIrq
	m.update()
	return m
}
func (m *m64) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	even := a&1 == 0
	switch a & 0xe000 {
	case 0x8000:
		if even {
			m.bankSel = v
		} else {
			m.r16[m.bankSel&15] = v
		}
		m.update()
	case 0xa000:
		if even && m.mirror != mirFour {
			m.mirror = hOrV(v&1 != 0)
		}
	case 0xc000:
		if even {
			m.irqLatch = v
		} else {
			m.cycleMode = v & 1
			m.irqReload = true
			m.irqCounter = 0
			m.prescaler = 0
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
func (m *m64) update() {
	r := &m.r16
	s := m.bankSel
	if s&0x40 != 0 {
		m.setPrg8(0, r[15])
		m.setPrg8(1, r[6])
		m.setPrg8(2, r[7])
	} else {
		m.setPrg8(0, r[6])
		m.setPrg8(1, r[7])
		m.setPrg8(2, r[15])
	}
	m.setPrg8(3, -1)
	x := 0
	if s&0x80 != 0 {
		x = 4
	}
	if s&0x20 != 0 {
		m.setChr1(0^x, r[0])
		m.setChr1(1^x, r[8])
		m.setChr1(2^x, r[1])
		m.setChr1(3^x, r[9])
	} else {
		m.setChr1(0^x, r[0]&0xfe)
		m.setChr1(1^x, r[0]|1)
		m.setChr1(2^x, r[1]&0xfe)
		m.setChr1(3^x, r[1]|1)
	}
	for i := 0; i < 4; i++ {
		m.setChr1((4+i)^x, r[2+i])
	}
}

// clockIrq: a forced reload ($C001) loads one more than the latch, so the IRQ
// comes a line later than on the MMC3.
func (m *m64) clockIrq() {
	if m.irqReload {
		m.irqCounter = m.irqLatch
		if m.irqLatch != 0 {
			m.irqCounter++
		}
		m.irqReload = false
	} else if m.irqCounter == 0 {
		m.irqCounter = m.irqLatch
	} else {
		m.irqCounter--
	}
	if m.irqCounter == 0 && m.irqEnable {
		m.irqF = true
	}
}
func (m *m64) ppuRead(a int) int {
	if m.cycleMode == 0 {
		m.ppuAddr(a)
	}
	return m.base.ppuRead(a)
}
func (m *m64) cpuClock() {
	if m.cycleMode != 0 {
		m.prescaler++
		if m.prescaler >= 4 {
			m.prescaler = 0
			m.clockIrq()
		}
	}
}
func (m *m64) state(s *sio) { m.m4State(s); s.IntSlice(m.r16[:]); s.Ints(&m.cycleMode, &m.prescaler) }

// 245 — Waixing MMC3 with an outer PRG bit in CHR register 0 and CHR RAM
type m245 struct{ *m4 }

func newM245(n *NES, c *cart) *m245 {
	m := &m245{newM4(n, c)}
	m.upd = m.update
	m.update()
	return m
}
func (m *m245) update() {
	if m.chrRam {
		m.setChr8(0)
	} else {
		m.m4.update()
	}
	outer := (m.r[0] & 2) << 5
	r := &m.r
	if m.bankSel&0x40 != 0 {
		m.setPrg8(0, outer|0x3e)
		m.setPrg8(2, outer|r[6]&0x3f)
	} else {
		m.setPrg8(0, outer|r[6]&0x3f)
		m.setPrg8(2, outer|0x3e)
	}
	m.setPrg8(1, outer|r[7]&0x3f)
	m.setPrg8(3, outer|0x3f)
}

// ===================================================================
// Small discrete-logic boards (licensed and unlicensed)
// ===================================================================

// 15 — K-1029 / K-1030P 100-in-1 Contra Function 16
type m15 struct {
	*base
	chrProtect bool
}

func newM15(n *NES, c *cart) *m15 { m := &m15{base: newBase(n, c)}; m.write(0x8000, 0); return m }
func (m *m15) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.write(a, v)
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m15) write(a, v int) {
	b, s := v&0x3f, v>>7
	m.mirror = hOrV(v&0x40 != 0)
	switch a & 3 {
	case 0:
		m.setPrg16(0, b&^1)
		m.setPrg16(1, b|1)
	case 1:
		m.setPrg16(0, b)
		m.setPrg16(1, b|7)
	case 2:
		for i := 0; i < 4; i++ {
			m.setPrg8(i, b*2+s)
		}
	case 3:
		m.setPrg16(0, b)
		m.setPrg16(1, b)
	}
	m.chrProtect = a&3 == 0 || a&3 == 3
}
func (m *m15) ppuWrite(a, v int) {
	if !m.chrProtect {
		m.base.ppuWrite(a, v)
	}
}
func (m *m15) state(s *sio) { m.baseState(s); s.Bool(&m.chrProtect) }

// 28 — Action 53 (homebrew multicart)
type m28 struct {
	*base
	sel, chrReg, inner, mode, outer int
}

func newM28(n *NES, c *cart) *m28 { m := &m28{base: newBase(n, c), outer: 0xff}; m.update(); return m }
func (m *m28) cpuWrite(a, v int) {
	if a >= 0x5000 && a < 0x6000 {
		m.sel = (v>>6)&2 | v&1
		return
	}
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch m.sel {
	case 0:
		m.chrReg = v & 3
		if m.mode&2 == 0 {
			m.mode = m.mode&^1 | (v>>4)&1
		}
	case 1:
		m.inner = v & 0x0f
		if m.mode&2 == 0 {
			m.mode = m.mode&^1 | (v>>4)&1
		}
	case 2:
		m.mode = v & 0x3f
	case 3:
		m.outer = v
	}
	m.update()
}
func (m *m28) bankFor(half int) int {
	pmode, size := (m.mode>>2)&3, (m.mode>>4)&3
	mask := (2 << size) - 1
	outer := m.outer << 1
	if pmode < 2 {
		return outer&^mask | (m.inner<<1|half)&mask
	}
	fixedHalf := 1
	if pmode == 2 {
		fixedHalf = 0
	}
	if half == fixedHalf {
		return outer | half
	}
	return outer&^mask | m.inner&mask
}
func (m *m28) update() {
	m.setPrg16(0, m.bankFor(0))
	m.setPrg16(1, m.bankFor(1))
	m.setChr8(m.chrReg)
	m.mirror = mmc1Mirror[m.mode&3]
}
func (m *m28) state(s *sio) { m.baseState(s); s.Ints(&m.sel, &m.chrReg, &m.inner, &m.mode, &m.outer) }

// 30 — UNROM 512 (homebrew: Black Box Challenge, Mega Man fan games)
type m30 struct {
	*base
	oneScreen bool
}

func newM30(n *NES, c *cart) *m30 {
	m := &m30{base: newBase(n, c)}
	f := c.flags6 & 9
	m.oneScreen = f == 8
	if f == 9 {
		m.mirror = mirFour
	} else if f == 8 {
		m.mirror = mirOneLo
	}
	m.setPrg16(1, -1)
	return m
}
func (m *m30) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	if !m.cart.battery {
		v &= m.cpuRead(a)
	}
	m.setPrg16(0, v&0x1f)
	m.setChr8((v >> 5) & 3)
	if m.oneScreen {
		m.mirror = hiOrLo(v&0x80 != 0)
	}
}

// 38 — Bit Corp (Crime Busters)
type m38 struct{ *base }

func (m *m38) cpuWrite(a, v int) {
	if a >= 0x7000 && a < 0x8000 {
		m.setPrg32(v & 3)
		m.setChr8((v >> 2) & 3)
	}
}

// 41 — Caltron 6-in-1
type m41 struct {
	*base
	outer, inner int
}

func (m *m41) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x6800 {
		m.outer = a & 0x3f
		m.setPrg32(a & 7)
		m.mirror = hOrV(a&0x20 != 0)
	} else if a >= 0x8000 && m.outer&4 != 0 {
		m.inner = v & 3 & m.cpuRead(a)
	} else {
		return
	}
	m.setChr8((m.outer>>1)&0x0c | m.inner)
}
func (m *m41) state(s *sio) { m.baseState(s); s.Ints(&m.outer, &m.inner) }

// 79 / 146 — AVE NINA-03/06 and Sachen 3015; 113 — the multicart version
type m79 struct {
	*base
	multi bool
}

func (m *m79) cpuWrite(a, v int) {
	if a&0xe100 != 0x4100 {
		m.base.cpuWrite(a, v)
		return
	}
	if m.multi {
		m.setPrg32((v >> 3) & 7)
		m.setChr8(v&7 | (v>>3)&8)
		m.mirror = vOrH(v&0x80 != 0)
	} else {
		m.setPrg32((v >> 3) & 1)
		m.setChr8(v & 7)
	}
}

// 94 — UN1ROM (Senjou no Ookami)
type m94 struct{ *base }

func newM94(n *NES, c *cart) *m94 { m := &m94{newBase(n, c)}; m.setPrg16(1, -1); return m }
func (m *m94) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg16(0, ((v&m.cpuRead(a))>>2)&7)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 107 — Magic Dragon
type m107 struct{ *base }

func (m *m107) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32(v >> 1)
		m.setChr8(v)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 111 — GTROM / Cheapocabra (homebrew): CHR RAM and nametable RAM banks on the cartridge
type m111 struct {
	*base
	ntRam  []byte
	ntPage int
}

func newM111(n *NES, c *cart) *m111 {
	m := &m111{base: newBase(n, c), ntRam: make([]byte, 0x2000)}
	if len(m.chr) < 0x4000 {
		m.chr = make([]byte, 0x4000)
		m.chrRam = true
	}
	m.setPrg32(0)
	m.setChr8(0)
	return m
}
func (m *m111) cpuWrite(a, v int) {
	if (a >= 0x5000 && a < 0x6000) || (a >= 0x7000 && a < 0x8000) {
		m.setPrg32(v & 0x0f)
		m.setChr8((v >> 4) & 1)
		m.ntPage = (v >> 5) & 1
	}
}
func (m *m111) ntRead(a int) int { return int(m.ntRam[m.ntPage<<12|a&0xfff]) }
func (m *m111) ntWrite(a, v int) { m.ntRam[m.ntPage<<12|a&0xfff] = byte(v) }
func (m *m111) state(s *sio)     { m.baseState(s); s.Bytes(m.ntRam); s.Int(&m.ntPage) }

// 112 — Asder / NTDEC (Huang Di, Master Shooter)
type m112 struct {
	*base
	sel int
	r   [8]int
}

func newM112(n *NES, c *cart) *m112 { m := &m112{base: newBase(n, c)}; m.update(); return m }
func (m *m112) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch a & 0xe000 {
	case 0x8000:
		m.sel = v & 7
	case 0xa000:
		m.r[m.sel] = v
	case 0xe000:
		m.mirror = hOrV(v&1 != 0)
	}
	m.update()
}
func (m *m112) update() {
	r := &m.r
	m.setPrg8(0, r[0])
	m.setPrg8(1, r[1])
	m.setPrg8(2, -2)
	m.setPrg8(3, -1)
	m.setChr2(0, r[2]>>1)
	m.setChr2(1, r[3]>>1)
	for i := 0; i < 4; i++ {
		m.setChr1(4+i, r[4+i])
	}
}
func (m *m112) state(s *sio) { m.baseState(s); s.Int(&m.sel); s.IntSlice(m.r[:]) }

// 132 — TXC 22211A
type m132 struct {
	*base
	r [4]int
}

func (m *m132) cpuRead(a int) int {
	if a&0xe103 == 0x4100 {
		return m.openBus()&0xf0 | (m.r[1]^m.r[2])&0x0f | 0x40
	}
	return m.base.cpuRead(a)
}
func (m *m132) cpuWrite(a, v int) {
	if a >= 0x4100 && a < 0x6000 {
		if a&0xe100 == 0x4100 {
			m.r[a&3] = v
		}
		return
	}
	if a >= 0x8000 {
		m.setPrg32((m.r[2] >> 2) & 1)
		m.setChr8(m.r[2] & 3)
		return
	}
	m.base.cpuWrite(a, v)
}
func (m *m132) state(s *sio) { m.baseState(s); s.IntSlice(m.r[:]) }

// 133 — Sachen 3009 / SA-72008; 145 — Sachen SA-72007; 147 — Sachen 3018; 148 — Sachen SA-0037; 149 — Sachen SA-0036
type m133 struct{ *base }

func (m *m133) cpuWrite(a, v int) {
	if a >= 0x4100 && a < 0x6000 && a&0x4100 == 0x4100 {
		m.setPrg32((v >> 2) & 1)
		m.setChr8(v & 3)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m145 struct{ *base }

func (m *m145) cpuWrite(a, v int) {
	if a >= 0x4100 && a < 0x6000 && a&0x4100 == 0x4100 {
		m.setChr8(v >> 7)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m147 struct{ *base }

func (m *m147) cpuWrite(a, v int) {
	if (a >= 0x4100 && a < 0x6000 && a&0x4103 == 0x4102) || a >= 0x8000 {
		m.setPrg32((v>>2)&1 | (v>>6)&2)
		m.setChr8((v >> 3) & 0x0f)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m148 struct{ *base }

func (m *m148) cpuWrite(a, v int) {
	if a >= 0x8000 {
		v &= m.cpuRead(a)
		m.setPrg32((v >> 3) & 1)
		m.setChr8(v & 7)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m149 struct{ *base }

func (m *m149) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setChr8((v & m.cpuRead(a)) >> 7)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 143 — Sachen NROM with a copy-protection read at $4100
type m143 struct{ *base }

func (m *m143) cpuRead(a int) int {
	if a >= 0x4100 && a < 0x6000 && a&0x4100 == 0x4100 {
		return ^a&0x3f | 0x40
	}
	return m.base.cpuRead(a)
}

// 137-139, 141 — Sachen 8259 A/B/C/D
type m8259 struct {
	*base
	kind, sel int
	r         [8]int
}

func newM8259(n *NES, c *cart, kind int) *m8259 {
	m := &m8259{base: newBase(n, c), kind: kind}
	m.update()
	return m
}
func (m *m8259) cpuWrite(a, v int) {
	if a >= 0x4100 && a < 0x6000 && a&0x4101 == 0x4100 {
		m.sel = v & 7
		return
	}
	if a >= 0x4100 && a < 0x6000 && a&0x4101 == 0x4101 {
		m.r[m.sel] = v & 7
		m.update()
		return
	}
	m.base.cpuWrite(a, v)
}

var m8259Mirror = [4]int{mirV, mirH, 0, mirOneLo}

func (m *m8259) update() {
	r := &m.r
	simple := r[7]&1 != 0
	m.setPrg32(r[5] & 7)
	switch {
	case m.chrRam:
		m.setChr8(0)
	case m.kind == 137: // 1 KB windows at $0000, last 4 KB fixed at $1000
		m.setChr1(0, r[0])
		m.setChr1(1, r[1]|(r[4]&1)<<4)
		m.setChr1(2, r[2]|(r[4]&2)<<3)
		m.setChr1(3, r[3]|(r[4]&4)<<2|(r[6]&1)<<3)
		for i := 4; i < 8; i++ {
			m.setChr1(i, i-8)
		}
	default:
		for i := 0; i < 4; i++ {
			src := r[i]
			if simple {
				src = r[0]
			}
			b := src&7 | (r[4]&7)<<3
			switch m.kind {
			case 141:
				b = b<<1 | i&1
			case 139:
				b = b<<2 | i&3
			}
			m.setChr2(i, b)
		}
	}
	if simple {
		m.mirror = mirV
	} else {
		mm := (r[7] >> 1) & 3
		if mm == 2 {
			m.setNt(0, 1, 1, 1)
		} else {
			m.mirror = m8259Mirror[mm]
		}
	}
}
func (m *m8259) state(s *sio) { m.baseState(s); s.Int(&m.sel); s.IntSlice(m.r[:]) }

// 168 — Racermate (64 KB CHR RAM)
type m168 struct{ *base }

func newM168(n *NES, c *cart) *m168 {
	m := &m168{newBase(n, c)}
	if len(m.chr) < 0x10000 {
		m.chr = make([]byte, 0x10000)
		m.chrRam = true
	}
	m.setPrg16(1, -1)
	m.setChr4(0, 0)
	m.setChr4(1, 0)
	return m
}
func (m *m168) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg16(0, v>>6)
		m.setChr4(1, v&0x0f)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 171 — Kaiser KS-7058 (Tui Do Woo Ma Jeung)
type m171 struct{ *base }

func (m *m171) cpuWrite(a, v int) {
	switch {
	case a&0xf080 == 0xf000:
		m.setChr4(0, v)
	case a&0xf080 == 0xf080:
		m.setChr4(1, v)
	default:
		m.base.cpuWrite(a, v)
	}
}

// 185 — CNROM with copy protection: the wrong value disables CHR ROM
type m185 struct {
	*base
	on  bool
	sub int
}

func (m *m185) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	v &= m.cpuRead(a)
	if m.sub >= 4 {
		m.on = v&3 == m.sub&3
	} else {
		m.on = v&0x0f != 0 && v != 0x13
	}
}
func (m *m185) ppuRead(a int) int {
	if m.on {
		return m.base.ppuRead(a)
	}
	return 0xff
}
func (m *m185) state(s *sio) { m.baseState(s); s.Bool(&m.on) }

// 190 — Magic Kid GooGoo
type m190 struct{ *base }

func newM190(n *NES, c *cart) *m190 {
	m := &m190{newBase(n, c)}
	m.setPrg16(0, 0)
	m.setPrg16(1, 0)
	return m
}
func (m *m190) cpuWrite(a, v int) {
	switch {
	case a < 0x8000:
		m.base.cpuWrite(a, v)
	case a < 0xa000:
		m.setPrg16(0, v&7)
	case a < 0xc000:
		m.setChr2(a&3, v)
	case a < 0xe000:
		m.setPrg16(0, v&7|8)
	}
}

// 193 — NTDEC TC-112 (Fighting Hero)
type m193 struct{ *base }

func newM193(n *NES, c *cart) *m193 {
	m := &m193{newBase(n, c)}
	m.setPrg8(1, -3)
	m.setPrg8(2, -2)
	m.setPrg8(3, -1)
	return m
}
func (m *m193) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x8000 {
		switch a & 3 {
		case 0:
			m.setChr4(0, v>>2)
		case 1:
			m.setChr2(2, v>>1)
		case 2:
			m.setChr2(3, v>>1)
		case 3:
			m.setPrg8(0, v)
		}
	}
}

// 156 — Daou 306 (Metal Force, Buzz & Waldog)
type m156 struct {
	*base
	lo, hi [8]int
}

func newM156(n *NES, c *cart) *m156 {
	m := &m156{base: newBase(n, c)}
	m.mirror = mirOneLo
	m.setPrg16(1, -1)
	return m
}
func (m *m156) cpuWrite(a, v int) {
	if a < 0xc000 {
		m.base.cpuWrite(a, v)
		return
	}
	switch {
	case a >= 0xc000 && a <= 0xc00f:
		r := a & 0x0f
		slot := r & 3
		if r&8 != 0 {
			slot |= 4
		}
		if r&4 != 0 {
			m.hi[slot] = v
		} else {
			m.lo[slot] = v
		}
		m.setChr1(slot, m.lo[slot]|m.hi[slot]<<8)
	case a == 0xc010:
		m.setPrg16(0, v)
	case a == 0xc014:
		m.mirror = hOrV(v&1 != 0)
	}
}
func (m *m156) state(s *sio) { m.baseState(s); s.IntSlice(m.lo[:]); s.IntSlice(m.hi[:]) }

// 166 / 167 — Subor educational computer cartridges
type m166 struct {
	*base
	alt bool
	r   [4]int
}

func newM166(n *NES, c *cart, alt bool) *m166 {
	m := &m166{base: newBase(n, c), alt: alt}
	m.update()
	return m
}
func (m *m166) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.r[(a>>13)&3] = v
		m.update()
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m166) update() {
	r := &m.r
	base, bank := ((r[0]^r[1])&0x10)<<1, (r[2]^r[3])&0x1f
	switch {
	case r[1]&8 != 0:
		b := (base + bank) &^ 1
		if m.alt {
			m.setPrg16(0, b|1)
			m.setPrg16(1, b)
		} else {
			m.setPrg16(0, b)
			m.setPrg16(1, b|1)
		}
	case r[1]&4 != 0:
		m.setPrg16(0, 0x1f)
		m.setPrg16(1, base+bank)
	default:
		m.setPrg16(0, base+bank)
		if m.alt {
			m.setPrg16(1, 0x20)
		} else {
			m.setPrg16(1, 0x07)
		}
	}
}
func (m *m166) state(s *sio) { m.baseState(s); s.IntSlice(m.r[:]) }

// ===================================================================
// Multicarts
// ===================================================================

// 200 — 1200-in-1 style: address lines pick a mirrored 16 KB bank
type m200 struct{ *base }

func (m *m200) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	m.setPrg16(0, a&7)
	m.setPrg16(1, a&7)
	m.setChr8(a & 7)
	m.mirror = hOrV(a&8 != 0)
}

// 201 — 21-in-1
type m201 struct{ *base }

func (m *m201) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32(a & 0xff)
		m.setChr8(a & 0xff)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 202 — 150-in-1
type m202 struct{ *base }

func (m *m202) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	b := (a >> 1) & 7
	if a&9 == 9 {
		m.setPrg32(b >> 1)
	} else {
		m.setPrg16(0, b)
		m.setPrg16(1, b)
	}
	m.setChr8(b)
	m.mirror = hOrV(a&1 != 0)
}

// 203 — 35-in-1
type m203 struct{ *base }

func (m *m203) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	m.setPrg16(0, v>>2)
	m.setPrg16(1, v>>2)
	m.setChr8(v & 3)
}

// 225 / 255 — 52-in-1, 64-in-1, 110-in-1
type m225 struct {
	*base
	nib [4]int
}

func newM225(n *NES, c *cart) *m225 { m := &m225{base: newBase(n, c)}; m.setPrg32(0); return m }
func (m *m225) cpuRead(a int) int {
	if a >= 0x5800 && a < 0x6000 {
		return m.openBus()&0xf0 | m.nib[a&3]
	}
	return m.base.cpuRead(a)
}
func (m *m225) cpuWrite(a, v int) {
	if a >= 0x5800 && a < 0x6000 {
		m.nib[a&3] = v & 0x0f
		return
	}
	if a < 0x8000 {
		return
	}
	hi := (a >> 8) & 0x40
	p := (a>>6)&0x3f | hi
	if a&0x1000 != 0 {
		m.setPrg16(0, p)
		m.setPrg16(1, p)
	} else {
		m.setPrg32(p >> 1)
	}
	m.setChr8(a&0x3f | hi)
	m.mirror = hOrV(a&0x2000 != 0)
}
func (m *m225) state(s *sio) { m.baseState(s); s.IntSlice(m.nib[:]) }

// 226 — 76-in-1, Super 42-in-1
type m226 struct {
	*base
	r [2]int
}

func newM226(n *NES, c *cart) *m226 { m := &m226{base: newBase(n, c)}; m.update(); return m }
func (m *m226) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.r[a&1] = v
		m.update()
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m226) update() {
	r := &m.r
	p := r[0]&0x1f | (r[0]&0x80)>>2 | (r[1]&1)<<6
	if r[0]&0x40 != 0 {
		m.setPrg16(0, p)
		m.setPrg16(1, p)
	} else {
		m.setPrg32(p >> 1)
	}
	m.mirror = vOrH(r[0]&0x20 != 0)
}
func (m *m226) state(s *sio) { m.baseState(s); s.IntSlice(m.r[:]) }

// 227 — 1200-in-1
type m227 struct{ *base }

func newM227(n *NES, c *cart) *m227 { m := &m227{newBase(n, c)}; m.write(0x8000); return m }
func (m *m227) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.write(a)
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m227) write(a int) {
	p, s, last, l := (a>>2)&0x1f|(a>>3)&0x20, a&1, a&0x80, a&0x200
	if last != 0 {
		if s != 0 {
			m.setPrg32(p >> 1)
		} else {
			m.setPrg16(0, p)
			m.setPrg16(1, p)
		}
	} else {
		if s != 0 {
			m.setPrg16(0, p&^1)
		} else {
			m.setPrg16(0, p)
		}
		if l != 0 {
			m.setPrg16(1, p|7)
		} else {
			m.setPrg16(1, p&0x38)
		}
	}
	m.mirror = hOrV(a&2 != 0)
}

// 228 — Active Enterprises (Action 52, Cheetahmen II)
type m228 struct {
	*base
	nib [4]int
}

func newM228(n *NES, c *cart) *m228 { m := &m228{base: newBase(n, c)}; m.write(0x8000, 0); return m }
func (m *m228) cpuRead(a int) int {
	if a >= 0x5ff0 && a < 0x6000 {
		return m.nib[a&3]
	}
	return m.base.cpuRead(a)
}
func (m *m228) cpuWrite(a, v int) {
	if a >= 0x5ff0 && a < 0x6000 {
		m.nib[a&3] = v & 0x0f
		return
	}
	if a >= 0x8000 {
		m.write(a, v)
	}
}
func (m *m228) write(a, v int) {
	chip := (a >> 11) & 3
	if chip == 3 {
		chip = 2
	}
	p := chip<<5 | (a>>6)&0x1f
	if a&0x20 != 0 {
		m.setPrg16(0, p)
		m.setPrg16(1, p)
	} else {
		m.setPrg32(p >> 1)
	}
	m.setChr8((a&0x0f)<<2 | v&3)
	m.mirror = hOrV(a&0x2000 != 0)
}
func (m *m228) state(s *sio) { m.baseState(s); s.IntSlice(m.nib[:]) }

// 229 — 31-in-1
type m229 struct{ *base }

func (m *m229) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	if a&0x1e == 0 {
		m.setPrg32(0)
	} else {
		m.setPrg16(0, a&0x1f)
		m.setPrg16(1, a&0x1f)
	}
	m.setChr8(a & 0xff)
	m.mirror = hOrV(a&0x20 != 0)
}

// 230 — 22-in-1 (Contra on one side of a reset switch, a menu of games on the other)
type m230 struct {
	*base
	multi, reg int
}

func newM230(n *NES, c *cart) *m230 { m := &m230{base: newBase(n, c)}; m.update(); return m }
func (m *m230) reset()              { m.multi ^= 1; m.reg = 0; m.update() }
func (m *m230) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.reg = v
		m.update()
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m230) update() {
	v := m.reg
	if m.multi == 0 {
		m.setPrg16(0, v&7)
		m.setPrg16(1, 7)
		m.mirror = mirV
	} else {
		p := v&0x1f + 8
		if v&0x20 != 0 {
			m.setPrg16(0, p)
			m.setPrg16(1, p)
		} else {
			m.setPrg16(0, p&^1)
			m.setPrg16(1, p|1)
		}
		m.mirror = vOrH(v&0x40 != 0)
	}
}
func (m *m230) state(s *sio) { m.baseState(s); s.Ints(&m.multi, &m.reg) }

// 231 — 20-in-1
type m231 struct{ *base }

func (m *m231) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	b := a&0x1e | (a>>5)&1
	m.setPrg16(0, a&0x1e)
	m.setPrg16(1, b)
	m.mirror = hOrV(a&0x80 != 0)
}

// 232 — Camerica / Codemasters Quattro carts
type m232 struct {
	*base
	outer, inner int
	aladdin      bool
}

func newM232(n *NES, c *cart) *m232 {
	m := &m232{base: newBase(n, c), aladdin: c.submapper == 1}
	m.update()
	return m
}
func (m *m232) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	if a < 0xc000 {
		if m.aladdin {
			m.outer = (v>>4)&1 | (v>>2)&2
		} else {
			m.outer = (v >> 3) & 3
		}
	} else {
		m.inner = v & 3
	}
	m.update()
}
func (m *m232) update() {
	m.setPrg16(0, m.outer<<2|m.inner)
	m.setPrg16(1, m.outer<<2|3)
}
func (m *m232) state(s *sio) { m.baseState(s); s.Ints(&m.outer, &m.inner) }

// 233 — 42-in-1 with reset switch
type m233 struct {
	*base
	outer, reg int
}

func newM233(n *NES, c *cart) *m233 { m := &m233{base: newBase(n, c)}; m.update(); return m }
func (m *m233) reset()              { m.outer ^= 0x20; m.update() }
func (m *m233) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.reg = v
		m.update()
	} else {
		m.base.cpuWrite(a, v)
	}
}

var m233Mirror = [4]int{mirOneLo, mirV, mirH, mirOneHi}

func (m *m233) update() {
	v := m.reg
	p := v&0x1f | m.outer
	if v&0x20 != 0 {
		m.setPrg16(0, p)
		m.setPrg16(1, p)
	} else {
		m.setPrg32(p >> 1)
	}
	m.mirror = m233Mirror[v>>6]
}
func (m *m233) state(s *sio) { m.baseState(s); s.Ints(&m.outer, &m.reg) }

// 234 — Maxi 15 (AVE D-1012): registers in ROM space, triggered by reads as well as writes
type m234 struct {
	*base
	r0, r1 int
}

func newM234(n *NES, c *cart) *m234 { m := &m234{base: newBase(n, c)}; m.update(); return m }
func (m *m234) touch(a, v int) {
	if a >= 0xff80 && a <= 0xff9f {
		if m.r0&0x3f == 0 {
			m.r0 = v
			m.update()
		}
	} else if a >= 0xffe8 && a <= 0xfff7 {
		m.r1 = v
		m.update()
	}
}
func (m *m234) cpuRead(a int) int {
	v := m.base.cpuRead(a)
	if a >= 0xff80 {
		m.touch(a, v)
	}
	return v
}
func (m *m234) cpuWrite(a, v int) {
	if a >= 0xff80 {
		m.touch(a, v&m.base.cpuRead(a))
	} else {
		m.base.cpuWrite(a, v)
	}
}
func (m *m234) update() {
	r0, r1 := m.r0, m.r1
	if r0&0x40 != 0 {
		m.setPrg32(r0&0x0e | r1&1)
		m.setChr8((r0&0x0e)<<2 | (r1>>4)&7)
	} else {
		m.setPrg32(r0 & 0x0f)
		m.setChr8((r0&0x0f)<<2 | (r1>>4)&3)
	}
	m.mirror = hOrV(r0&0x80 != 0)
}
func (m *m234) state(s *sio) { m.baseState(s); s.Ints(&m.r0, &m.r1) }

// 235 — 150-in-1, 260-in-1
type m235 struct{ *base }

func (m *m235) cpuWrite(a, v int) {
	if a < 0x8000 {
		m.base.cpuWrite(a, v)
		return
	}
	p := a&0x1f | (a>>3)&0x60
	if a&0x800 != 0 {
		b := p<<1 | (a>>12)&1
		m.setPrg16(0, b)
		m.setPrg16(1, b)
	} else {
		m.setPrg32(p)
	}
	if a&0x400 != 0 {
		m.mirror = mirOneLo
	} else {
		m.mirror = hOrV(a&0x2000 != 0)
	}
}

// 240 — C&E (Jing Ke Xin Zhuan); 241 — BNROM-like with RAM; 242 — Wai Xing Zhan Shi
type m240 struct{ *base }

func (m *m240) cpuWrite(a, v int) {
	if a >= 0x4020 && a < 0x6000 {
		m.setPrg32(v >> 4)
		m.setChr8(v & 0x0f)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m241 struct{ *base }

func (m *m241) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32(v)
	} else {
		m.base.cpuWrite(a, v)
	}
}

type m242 struct{ *base }

func (m *m242) cpuWrite(a, v int) {
	if a >= 0x8000 {
		m.setPrg32((a >> 3) & 0x0f)
		m.mirror = hOrV(a&2 != 0)
	} else {
		m.base.cpuWrite(a, v)
	}
}

// 243 — Sachen SA-020A (Poker III)
type m243 struct {
	*base
	sel int
	r   [8]int
}

var m243Mirror = [4]int{mirH, mirV, mirH, mirOneHi}

func (m *m243) cpuWrite(a, v int) {
	if a >= 0x4100 && a < 0x6000 {
		if a&0xc101 == 0x4100 {
			m.sel = v & 7
		} else if a&0xc101 == 0x4101 {
			m.r[m.sel] = v & 7
			r := &m.r
			m.setPrg32(r[5] & 3)
			m.setChr8(r[2]&1 | (r[4]&1)<<1 | (r[6]&3)<<2)
			m.mirror = m243Mirror[(r[7]>>1)&3]
		}
		return
	}
	m.base.cpuWrite(a, v)
}
func (m *m243) state(s *sio) { m.baseState(s); s.Int(&m.sel); s.IntSlice(m.r[:]) }

// 244 — C&E Decathlon: bank numbers are scrambled through fixed tables
var (
	p244 = [4][4]int{{0, 1, 2, 3}, {3, 2, 1, 0}, {0, 2, 1, 3}, {3, 1, 2, 0}}
	c244 = [8][8]int{{0, 1, 2, 3, 4, 5, 6, 7}, {0, 2, 1, 3, 4, 6, 5, 7}, {0, 1, 4, 5, 2, 3, 6, 7}, {0, 4, 1, 5, 2, 6, 3, 7},
		{0, 4, 2, 6, 1, 5, 3, 7}, {0, 2, 4, 6, 1, 3, 5, 7}, {7, 6, 5, 4, 3, 2, 1, 0}, {7, 6, 5, 4, 3, 2, 1, 0}}
)

type m244 struct{ *base }

func (m *m244) cpuWrite(a, v int) {
	switch {
	case a >= 0x8065 && a <= 0x80a4:
		o := a - 0x8065
		m.setPrg32(p244[(o>>4)&3][o&3])
	case a >= 0x80a5 && a <= 0x80e4:
		o := a - 0x80a5
		m.setChr8(c244[(o>>3)&7][o&7])
	default:
		m.base.cpuWrite(a, v)
	}
}

// 246 — C&E (Fong Shen Bang)
type m246 struct{ *base }

func newM246(n *NES, c *cart) *m246 {
	m := &m246{newBase(n, c)}
	for i := 0; i < 4; i++ {
		m.setPrg8(i, i-4)
	}
	return m
}
func (m *m246) cpuRead(a int) int {
	if a >= 0x6000 && a < 0x6800 {
		return -1
	}
	return m.base.cpuRead(a)
}
func (m *m246) cpuWrite(a, v int) {
	if a >= 0x6000 && a < 0x6008 {
		if a&4 != 0 {
			m.setChr2(a&3, v)
		} else {
			m.setPrg8(a&3, v)
		}
		return
	}
	if a >= 0x6800 && a < 0x8000 {
		m.base.cpuWrite(a, v)
	}
}

// ===================================================================
// The board list
// ===================================================================

type mapperCtor func(n *NES, c *cart) mapper

func plain[T any](wrap func(*base) T) mapperCtor {
	return func(n *NES, c *cart) mapper { return any(wrap(newBase(n, c))).(mapper) }
}

var mappers = map[int]mapperCtor{
	0:   plain(func(b *base) *m0 { return &m0{b} }),
	1:   func(n *NES, c *cart) mapper { return newM1(n, c) },
	2:   func(n *NES, c *cart) mapper { return newM2(n, c) },
	3:   plain(func(b *base) *m3 { return &m3{b} }),
	4:   func(n *NES, c *cart) mapper { return newM4(n, c) },
	5:   func(n *NES, c *cart) mapper { return newM5(n, c) },
	7:   func(n *NES, c *cart) mapper { return newM7(n, c) },
	9:   func(n *NES, c *cart) mapper { return newM9(n, c, false) },
	10:  func(n *NES, c *cart) mapper { return newM9(n, c, true) },
	11:  plain(func(b *base) *m11 { return &m11{b} }),
	13:  func(n *NES, c *cart) mapper { return newM13(n, c) },
	34:  func(n *NES, c *cart) mapper { return newM34(n, c) },
	66:  plain(func(b *base) *m66 { return &m66{b} }),
	69:  func(n *NES, c *cart) mapper { return newM69(n, c) },
	71:  func(n *NES, c *cart) mapper { return newM71(n, c) },
	140: plain(func(b *base) *m140 { return &m140{b} }),
	180: func(n *NES, c *cart) mapper { return newM180(n, c) },
	206: func(n *NES, c *cart) mapper { return &m206{newM4(n, c)} },

	15:  func(n *NES, c *cart) mapper { return newM15(n, c) },
	16:  func(n *NES, c *cart) mapper { return newM16(n, c, 16) },
	153: func(n *NES, c *cart) mapper { return newM16(n, c, 153) },
	159: func(n *NES, c *cart) mapper { return newM16(n, c, 159) },
	18:  func(n *NES, c *cart) mapper { return newM18(n, c) },
	19:  func(n *NES, c *cart) mapper { return newM19(n, c, 163) },
	210: func(n *NES, c *cart) mapper {
		k := 0
		switch c.submapper {
		case 1:
			k = 175
		case 2:
			k = 340
		}
		return newM19(n, c, k)
	},
	21:  func(n *NES, c *cart) mapper { return newVRC24(n, c, 21) },
	22:  func(n *NES, c *cart) mapper { return newVRC24(n, c, 22) },
	23:  func(n *NES, c *cart) mapper { return newVRC24(n, c, 23) },
	25:  func(n *NES, c *cart) mapper { return newVRC24(n, c, 25) },
	24:  func(n *NES, c *cart) mapper { return newM24(n, c, false) },
	26:  func(n *NES, c *cart) mapper { return newM24(n, c, true) },
	85:  func(n *NES, c *cart) mapper { return newM85(n, c) },
	73:  func(n *NES, c *cart) mapper { return newM73(n, c) },
	75:  func(n *NES, c *cart) mapper { return newM75(n, c) },
	28:  func(n *NES, c *cart) mapper { return newM28(n, c) },
	30:  func(n *NES, c *cart) mapper { return newM30(n, c) },
	32:  func(n *NES, c *cart) mapper { return newM32(n, c) },
	33:  func(n *NES, c *cart) mapper { return newM33(n, c, false) },
	48:  func(n *NES, c *cart) mapper { return newM33(n, c, true) },
	37:  func(n *NES, c *cart) mapper { return newM47(n, c, true) },
	47:  func(n *NES, c *cart) mapper { return newM47(n, c, false) },
	38:  plain(func(b *base) *m38 { return &m38{b} }),
	41:  plain(func(b *base) *m41 { return &m41{base: b} }),
	64:  func(n *NES, c *cart) mapper { return newM64(n, c) },
	65:  func(n *NES, c *cart) mapper { return newM65(n, c) },
	67:  func(n *NES, c *cart) mapper { return newM67(n, c) },
	68:  func(n *NES, c *cart) mapper { return newM68(n, c) },
	70:  func(n *NES, c *cart) mapper { return newM70(n, c, false) },
	152: func(n *NES, c *cart) mapper { return newM70(n, c, true) },
	72:  func(n *NES, c *cart) mapper { return newM72(n, c, false) },
	92:  func(n *NES, c *cart) mapper { return newM72(n, c, true) },
	76:  func(n *NES, c *cart) mapper { return newM76(n, c) },
	77:  func(n *NES, c *cart) mapper { return newM77(n, c) },
	78:  func(n *NES, c *cart) mapper { return newM78(n, c) },
	79:  plain(func(b *base) *m79 { return &m79{b, false} }),
	146: plain(func(b *base) *m79 { return &m79{b, false} }),
	113: plain(func(b *base) *m79 { return &m79{b, true} }),
	80:  func(n *NES, c *cart) mapper { return newM80(n, c, false) },
	207: func(n *NES, c *cart) mapper { return newM80(n, c, true) },
	82:  func(n *NES, c *cart) mapper { return newM82(n, c) },
	86:  plain(func(b *base) *m86 { return &m86{b} }),
	87:  plain(func(b *base) *m87 { return &m87{b} }),
	88:  func(n *NES, c *cart) mapper { return newN108(n, c, 88) },
	154: func(n *NES, c *cart) mapper { return newN108(n, c, 154) },
	95:  func(n *NES, c *cart) mapper { return newN108(n, c, 95) },
	89:  func(n *NES, c *cart) mapper { return newM89(n, c) },
	93:  func(n *NES, c *cart) mapper { return newM93(n, c) },
	94:  func(n *NES, c *cart) mapper { return newM94(n, c) },
	97:  func(n *NES, c *cart) mapper { return newM97(n, c) },
	105: func(n *NES, c *cart) mapper { return newM105(n, c) },
	107: plain(func(b *base) *m107 { return &m107{b} }),
	111: func(n *NES, c *cart) mapper { return newM111(n, c) },
	112: func(n *NES, c *cart) mapper { return newM112(n, c) },
	118: func(n *NES, c *cart) mapper { return newM118(n, c) },
	119: func(n *NES, c *cart) mapper { return newM119(n, c, nil) },
	74:  func(n *NES, c *cart) mapper { return newM119(n, c, func(b int) bool { return b == 8 || b == 9 }) },
	191: func(n *NES, c *cart) mapper { return newM119(n, c, func(b int) bool { return b&0x80 != 0 }) },
	192: func(n *NES, c *cart) mapper { return newM119(n, c, func(b int) bool { return b >= 8 && b <= 11 }) },
	194: func(n *NES, c *cart) mapper { return newM119(n, c, func(b int) bool { return b <= 1 }) },
	195: func(n *NES, c *cart) mapper { return newM119(n, c, func(b int) bool { return b <= 3 }) },
	132: plain(func(b *base) *m132 { return &m132{base: b} }),
	133: plain(func(b *base) *m133 { return &m133{b} }),
	137: func(n *NES, c *cart) mapper { return newM8259(n, c, 137) },
	138: func(n *NES, c *cart) mapper { return newM8259(n, c, 138) },
	139: func(n *NES, c *cart) mapper { return newM8259(n, c, 139) },
	141: func(n *NES, c *cart) mapper { return newM8259(n, c, 141) },
	143: plain(func(b *base) *m143 { return &m143{b} }),
	145: plain(func(b *base) *m145 { return &m145{b} }),
	147: plain(func(b *base) *m147 { return &m147{b} }),
	148: plain(func(b *base) *m148 { return &m148{b} }),
	149: plain(func(b *base) *m149 { return &m149{b} }),
	155: func(n *NES, c *cart) mapper { return newM1(n, c) },
	156: func(n *NES, c *cart) mapper { return newM156(n, c) },
	166: func(n *NES, c *cart) mapper { return newM166(n, c, false) },
	167: func(n *NES, c *cart) mapper { return newM166(n, c, true) },
	168: func(n *NES, c *cart) mapper { return newM168(n, c) },
	171: plain(func(b *base) *m171 { return &m171{b} }),
	184: plain(func(b *base) *m184 { return &m184{b} }),
	185: plain(func(b *base) *m185 { return &m185{b, true, b.cart.submapper} }),
	190: func(n *NES, c *cart) mapper { return newM190(n, c) },
	193: func(n *NES, c *cart) mapper { return newM193(n, c) },
	200: plain(func(b *base) *m200 { return &m200{b} }),
	201: plain(func(b *base) *m201 { return &m201{b} }),
	202: plain(func(b *base) *m202 { return &m202{b} }),
	203: plain(func(b *base) *m203 { return &m203{b} }),
	225: func(n *NES, c *cart) mapper { return newM225(n, c) },
	255: func(n *NES, c *cart) mapper { return newM225(n, c) },
	226: func(n *NES, c *cart) mapper { return newM226(n, c) },
	227: func(n *NES, c *cart) mapper { return newM227(n, c) },
	228: func(n *NES, c *cart) mapper { return newM228(n, c) },
	229: plain(func(b *base) *m229 { return &m229{b} }),
	230: func(n *NES, c *cart) mapper { return newM230(n, c) },
	231: plain(func(b *base) *m231 { return &m231{b} }),
	232: func(n *NES, c *cart) mapper { return newM232(n, c) },
	233: func(n *NES, c *cart) mapper { return newM233(n, c) },
	234: func(n *NES, c *cart) mapper { return newM234(n, c) },
	235: plain(func(b *base) *m235 { return &m235{b} }),
	240: plain(func(b *base) *m240 { return &m240{b} }),
	241: plain(func(b *base) *m241 { return &m241{b} }),
	242: plain(func(b *base) *m242 { return &m242{b} }),
	243: plain(func(b *base) *m243 { return &m243{base: b} }),
	244: plain(func(b *base) *m244 { return &m244{b} }),
	245: func(n *NES, c *cart) mapper { return newM245(n, c) },
	246: func(n *NES, c *cart) mapper { return newM246(n, c) },
}
