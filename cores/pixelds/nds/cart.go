package nds

import "math/bits"

// cart is the game card: the ROM behind the card bus (commands written to
// 0x040001A8, data read from 0x04100010) and the save chip behind the
// card's SPI bus (0x040001A0 / 0x040001A2). The card is used the way it is
// after the boot menu has started the game, so the program's reads (command
// B7) see plain data.
type cart struct {
	m      *Machine
	rom    []byte
	chipID uint32

	cmd       [8]uint8
	romctrl   uint32
	auxspicnt uint16
	spiData   uint8

	addr      uint32
	cmdByte   uint8
	wordsLeft int
	doneAt    int64

	backup *backup
}

func newCart(m *Machine, rom []byte, save []byte) *cart {
	mb := (len(rom) + (1<<20 - 1)) >> 20
	if mb < 1 {
		mb = 1
	}
	c := &cart{m: m, rom: rom, doneAt: never}
	c.chipID = 0xc2 | uint32(mb-1)&0xff<<8
	c.backup = newBackup(save)
	return c
}

// arm7Owns: EXMEMCNT gives the card to the ARM7.
func (c *cart) arm7Owns() bool { return c.m.exmemcnt&(1<<11) != 0 }

func (c *cart) irq() *irqCtl {
	if c.arm7Owns() {
		return &c.m.irq7
	}
	return &c.m.irq9
}

func (c *cart) writeCtrl(v uint32) {
	start := v&(1<<31) != 0 && c.romctrl&(1<<31) == 0
	c.romctrl = c.romctrl&(1<<23) | v&^(1<<23)
	if !start {
		return
	}
	size := 0
	switch n := v >> 24 & 7; n {
	case 0:
	case 7:
		size = 4
	default:
		size = 0x100 << n
	}
	c.cmdByte = c.cmd[0]
	c.addr = uint32(c.cmd[1])<<24 | uint32(c.cmd[2])<<16 | uint32(c.cmd[3])<<8 | uint32(c.cmd[4])
	if c.cmdByte == 0xb7 {
		if len(c.rom) > 0 {
			c.addr &= uint32(roundPow2(len(c.rom)) - 1)
		}
		if c.addr < 0x8000 {
			c.addr = 0x8000 + c.addr&0x1ff
		}
	}
	c.wordsLeft = size / 4
	if c.wordsLeft == 0 {
		c.doneAt = c.m.now + 16
		return
	}
	c.romctrl |= 1 << 23
	c.feedDMA()
}

func roundPow2(n int) int {
	if n <= 1 {
		return 1
	}
	return 1 << bits.Len(uint(n-1))
}

func (c *cart) wordReady() bool { return c.wordsLeft > 0 }

// feedDMA lets a DMA channel waiting on the card take the words.
func (c *cart) feedDMA() {
	chans := &c.m.dma9
	if c.arm7Owns() {
		chans = &c.m.dma7
	}
	for i := range chans {
		d := &chans[i]
		for c.wordsLeft > 0 && d.cnt&(1<<31) != 0 && d.mode() == dmaCart {
			d.run()
		}
	}
}

func (c *cart) readData() uint32 {
	if c.wordsLeft == 0 {
		return 0xffffffff
	}
	var v uint32
	switch c.cmdByte {
	case 0xb7:
		for i := uint32(0); i < 4; i++ {
			a := c.addr + i
			b := uint32(0xff)
			if int(a) < len(c.rom) {
				b = uint32(c.rom[a])
			}
			v |= b << (8 * i)
		}
		// reads stay within a 4 KB page
		c.addr = c.addr&^0xfff | (c.addr+4)&0xfff
	case 0xb8, 0x90:
		v = c.chipID
	case 0x00:
		a := c.addr & 0xfff
		for i := uint32(0); i < 4; i++ {
			v |= uint32(c.rom[(a+i)&0x1ff]) << (8 * i)
		}
		c.addr += 4
	default:
		v = 0xffffffff
	}
	c.wordsLeft--
	if c.wordsLeft == 0 {
		c.finish()
	}
	return v
}

// finish ends a transfer: the busy bit clears and the interrupt is sent.
func (c *cart) finish() {
	c.doneAt = never
	c.romctrl &^= 1<<31 | 1<<23
	if c.auxspicnt&(1<<14) != 0 {
		c.irq().raise(irqCartDone)
	}
}

func (c *cart) spiWrite(v uint8) {
	if c.auxspicnt&(1<<13) == 0 {
		return
	}
	c.spiData = c.backup.transfer(v)
	if c.auxspicnt&(1<<6) == 0 {
		c.backup.deselect()
	}
}

// ---------------- the save chip ----------------

// backup is the card's save chip: an EEPROM, FRAM or flash chip. Which one
// isn't written anywhere on the card, so it's worked out from the save's
// size, or else from how the game first writes to it.
type backup struct {
	mem       []byte
	addrBytes int // 0 = not known yet

	cmd    uint8
	pos    int
	addr   uint32
	wel    bool
	record []uint8 // the first write, before the chip's type is known
	dirty  bool
}

func newBackup(save []byte) *backup {
	b := &backup{}
	if len(save) > 0 {
		b.mem = append([]byte(nil), save...)
		switch n := len(save); {
		case n <= 512:
			b.addrBytes = 1
		case n <= 64<<10:
			b.addrBytes = 2
		default:
			b.addrBytes = 3
		}
	}
	return b
}

func (b *backup) data() []byte { return b.mem }

func (b *backup) takeDirty() bool {
	d := b.dirty
	b.dirty = false
	return d
}

func (b *backup) size() int { return len(b.mem) }

func (b *backup) ensure(addr uint32) {
	if int(addr) < len(b.mem) {
		return
	}
	n := len(b.mem)
	if n == 0 {
		n = 512
	}
	for n <= int(addr) {
		n *= 2
	}
	nm := make([]byte, n)
	for i := range nm {
		nm[i] = 0xff
	}
	copy(nm, b.mem)
	b.mem = nm
}

func (b *backup) read(addr uint32) uint8 {
	if int(addr) < len(b.mem) {
		return b.mem[addr]
	}
	return 0xff
}

func (b *backup) write(addr uint32, v uint8) {
	b.ensure(addr)
	b.mem[addr] = v
	b.dirty = true
}

func (b *backup) transfer(v uint8) uint8 {
	defer func() { b.pos++ }()
	if b.pos == 0 {
		b.cmd = v
		b.addr = 0
		if b.addrBytes == 0 {
			b.record = b.record[:0]
		}
		switch v {
		case 0x06:
			b.wel = true
		case 0x04:
			b.wel = false
		}
		return 0xff
	}
	switch b.cmd {
	case 0x05: // read status
		if b.wel {
			return 2
		}
		return 0
	case 0x9f: // read ID (flash)
		switch b.pos {
		case 1:
			return 0x20
		case 2:
			return 0x40
		case 3:
			return 0x12
		}
		return 0xff
	case 0x03, 0x0b, 0x02, 0x0a, 0xdb, 0xd8:
	default:
		return 0xff
	}
	if b.addrBytes == 0 {
		b.record = append(b.record, v)
		return 0xff
	}
	ab := b.addrBytes
	if b.pos <= ab {
		b.addr = b.addr<<8 | uint32(v)
		if b.pos == ab && ab == 1 && b.cmd&8 != 0 {
			b.addr |= 0x100 // the small EEPROM's upper half
		}
		if b.pos == ab {
			b.addr = b.normalize(b.addr)
			switch b.cmd {
			case 0xdb: // erase a page
				for i := uint32(0); i < 256; i++ {
					b.write(b.addr&^0xff+i, 0xff)
				}
			case 0xd8: // erase a sector
				for i := uint32(0); i < 0x10000; i++ {
					b.write(b.addr&^0xffff+i, 0xff)
				}
			}
		}
		return 0xff
	}
	switch b.cmd {
	case 0x03:
		r := b.read(b.addr)
		b.addr++
		return r
	case 0x0b:
		if ab == 1 {
			r := b.read(b.addr)
			b.addr++
			return r
		}
		if b.pos == ab+1 { // fast read's dummy byte
			return 0xff
		}
		r := b.read(b.addr)
		b.addr++
		return r
	case 0x02, 0x0a:
		b.write(b.addr, v)
		b.addr++
	}
	return 0xff
}

// normalize: a small EEPROM's address wraps at 512 bytes.
func (b *backup) normalize(a uint32) uint32 {
	if b.addrBytes == 1 {
		return a & 0x1ff
	}
	return a
}

func (b *backup) deselect() {
	if b.addrBytes == 0 && (b.cmd == 0x02 || b.cmd == 0x0a) && len(b.record) > 1 {
		b.learn()
	}
	if b.cmd == 0x02 || b.cmd == 0x0a || b.cmd == 0xdb || b.cmd == 0xd8 {
		b.wel = false
	}
	b.pos = 0
}

// learn works out the chip from the game's first write: the address bytes
// come first, then a page of data (a power of two in size).
func (b *backup) learn() {
	n := len(b.record)
	pow2 := func(x int) bool { return x > 0 && x&(x-1) == 0 }
	ab := 0
	for _, a := range []int{2, 3, 1} {
		if pow2(n - a) {
			ab = a
			break
		}
	}
	if ab == 0 {
		ab = 2
		if n > 130 {
			ab = 3
		}
	}
	if b.cmd == 0x0a && n > 17 {
		ab = 3
	}
	b.addrBytes = ab
	switch ab {
	case 1:
		b.ensure(511)
	case 2:
		b.ensure(64<<10 - 1)
	case 3:
		b.ensure(512<<10 - 1)
	}
	// replay the write
	rec := append([]uint8(nil), b.record...)
	b.record = nil
	b.pos = 1
	for _, v := range rec {
		b.transfer(v)
	}
}
