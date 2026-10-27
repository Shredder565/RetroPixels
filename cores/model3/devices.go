package model3

import (
	"time"
)

// ---------------------------------------------------------------- IRQ controller

// The interrupt controller at 0xF0100014 (enable) / 0xF0100018 (pending).
// Bits 0-7 are maskable; the bits above (the Real3D DMA) always interrupt.
const (
	irqVBlank = 0x02
	irqSound  = 0x40
	irqDMA    = 0x100 // non-maskable
)

type irqCtl struct {
	enable  uint32
	pending uint32
	line    func(on bool)
}

func (q *irqCtl) update() {
	q.line(q.pending&q.enable&0xFF != 0 || q.pending&^0xFF != 0)
}

func (q *irqCtl) assert(bits uint32) {
	q.pending |= bits
	if q.pending&q.enable&0xFF != 0 || q.pending&^0xFF != 0 {
		q.line(true)
	}
}

func (q *irqCtl) deassert(bits uint32) {
	q.pending &^= bits
	if q.pending&q.enable&0xFF == 0 && q.pending&^0xFF == 0 {
		q.line(false)
	}
}

// ---------------------------------------------------------------- 93C46 EEPROM

// The serial EEPROM (64 words of 16 bits) on the I/O board: the game's
// settings. Clocked by the PowerPC through the input bank register.
type eeprom struct {
	Words    [64]uint16
	cs, clk  bool
	di, do   uint32
	in       uint32
	out      uint32
	outBits  int
	reading  bool // shifting a word out
	addr     uint32
	busy     int
	locked   bool
	modified bool
}

func reverse16(v uint16) uint32 {
	var r uint32
	for i := 0; i < 16; i++ {
		r |= uint32(v>>i&1) << (15 - i)
	}
	return r
}

func (e *eeprom) reset() {
	e.reading = false
	e.locked = true
	e.in, e.out = 0, 0
	e.addr = 0
	e.busy = 0
	e.cs = false
}

func (e *eeprom) write(cs, clk, di bool) {
	prev := e.clk
	e.cs, e.clk = cs, clk
	if di {
		e.di = 1
	} else {
		e.di = 0
	}
	if !cs {
		e.in = 0
		e.reading = false
		e.busy = 5
		return
	}
	if prev || !clk {
		return
	}
	if e.reading {
		e.do = e.out & 1
		e.out >>= 1
		e.outBits++
		if e.outBits == 16 {
			e.addr = (e.addr + 1) & 63
			e.out = reverse16(e.Words[e.addr])
			e.outBits = 0
		}
		return
	}
	e.in = e.in<<1 | e.di
	switch {
	case e.in&0xFFFFFFC0 == 0x180: // READ
		e.addr = e.in & 63
		e.out = reverse16(e.Words[e.addr])
		e.outBits = 0
		e.reading = true
	case e.in == 0x13: // write enable
		e.locked = false
	case e.in == 0x10: // write disable
		e.locked = true
	case e.in&0xFFC00000 == 0x01400000: // WRITE
		if !e.locked {
			e.Words[e.in>>16&63] = uint16(e.in)
			e.modified = true
		}
		e.do = 1
	case e.in&0xFFF00000 == 0x01100000: // write all
		if !e.locked {
			for i := range e.Words {
				e.Words[i] = uint16(e.in)
			}
			e.modified = true
		}
		e.do = 1
	case e.in&0xFFFFFFC0 == 0x1C0: // ERASE
		if !e.locked {
			e.Words[e.in&63] = 0xFFFF
			e.modified = true
		}
		e.do = 1
	case e.in&0xFFFFFFF0 == 0x120: // erase all
		if !e.locked {
			for i := range e.Words {
				e.Words[i] = 0xFFFF
			}
			e.modified = true
		}
		e.do = 1
	}
}

func (e *eeprom) read() uint32 {
	if !e.reading {
		if e.busy > 0 {
			e.busy--
			return 0
		}
		return 1
	}
	return e.do
}

// ---------------------------------------------------------------- RTC 72421

// rtcRead: the real-time clock's registers (BCD digits of the local time).
func rtcRead(reg uint32, now time.Time) uint8 {
	switch reg & 15 {
	case 0:
		return uint8(now.Second() % 10)
	case 1:
		return uint8(now.Second() / 10)
	case 2:
		return uint8(now.Minute() % 10)
	case 3:
		return uint8(now.Minute() / 10)
	case 4:
		return uint8(now.Hour() % 10)
	case 5:
		return uint8(now.Hour() / 10)
	case 6:
		return uint8(now.Day() % 10)
	case 7:
		return uint8(now.Day() / 10)
	case 8:
		return uint8(int(now.Month()) % 10)
	case 9:
		return uint8(int(now.Month()) / 10)
	case 10:
		return uint8(now.Year() % 10)
	case 11:
		return uint8(now.Year() % 100 / 10)
	case 12:
		return uint8(now.Weekday())
	}
	return 0
}

// ---------------------------------------------------------------- MPC106 PCI bridge

type pciBridge struct {
	regs                 [256]byte
	device, reg, fn, bus uint32
}

func (p *pciBridge) reset() {
	p.regs = [256]byte{}
	le32 := func(a int, v uint32) {
		p.regs[a], p.regs[a+1], p.regs[a+2], p.regs[a+3] = byte(v), byte(v>>8), byte(v>>16), byte(v>>24)
	}
	p.regs[0], p.regs[1] = 0x57, 0x10 // Motorola
	p.regs[2], p.regs[3] = 0x02, 0x00 // MPC106
	le32(0x04, 0x00800006)
	le32(0x08, 0x00060000)
	le32(0x0C, 0x00000800)
	le32(0x70, 0x00CD0000)
	le32(0xA8, 0x0010FF00)
	le32(0xAC, 0x060C000C)
	le32(0xB8, 0x04000000)
	le32(0xC0, 0x00000100)
	le32(0xE0, 0x00420FFF)
	le32(0xE8, 0x00200000)
	le32(0xF0, 0x0000FF02)
	le32(0xF4, 0x00030000)
	le32(0xFC, 0x00000010)
	p.device, p.reg, p.fn, p.bus = 0, 0, 0, 0
}

func bswap32(v uint32) uint32 { return v>>24 | v>>8&0xFF00 | v<<8&0xFF0000 | v<<24 }

func (p *pciBridge) setAddress(v uint32) {
	d := bswap32(v)
	p.bus = d >> 16 & 0xFF
	p.device = d >> 11 & 0x1F
	p.fn = d >> 8 & 7
	p.reg = d & 0xFF
}
