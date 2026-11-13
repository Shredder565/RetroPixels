package nds

// The I/O registers of both processors (0x04000000-0x04FFFFFF).

// keyinput: KEYINPUT, low = held.
func (m *Machine) keyinput() uint16 { return ^uint16(m.keys) & 0x3ff }

// extkeyin: the ARM7's EXTKEYIN (X, Y, pen down, hinge).
func (m *Machine) extkeyin() uint16 {
	v := uint16(0x7f)
	if m.keys&(1<<10) != 0 {
		v &^= 1
	}
	if m.keys&(1<<11) != 0 {
		v &^= 2
	}
	if m.touching {
		v &^= 0x40
	}
	if m.keys&(1<<12) != 0 { // the lid is closed
		v |= 0x80
	}
	return v
}

func (m *Machine) dispstatRead(arm9 bool) uint16 {
	if arm9 {
		return m.dispstat9
	}
	return m.dispstat7
}

func (m *Machine) dispstatWrite(arm9 bool, v uint16) {
	p := &m.dispstat7
	if arm9 {
		p = &m.dispstat9
	}
	*p = *p&7 | v&0xffb8
	m.checkVCount()
}

func (m *Machine) dmaRead(d *[4]dma, off uint32) uint32 {
	ch := &d[(off-0xb0)/12]
	switch (off - 0xb0) % 12 {
	case 0:
		return ch.sad
	case 4:
		return ch.dad
	}
	return ch.cnt
}

func (m *Machine) dmaWrite(d *[4]dma, off, v uint32, mask uint32) {
	ch := &d[(off-0xb0)/12]
	switch (off - 0xb0) % 12 {
	case 0:
		ch.sad = ch.sad&^mask | v&mask
		if !ch.arm9 {
			ch.sad &= 0x07ffffff
		}
	case 4:
		ch.dad = ch.dad&^mask | v&mask
		if !ch.arm9 {
			ch.dad &= 0x07ffffff
		}
	case 8:
		ch.writeCnt(ch.cnt&^mask | v&mask)
	}
}

func (m *Machine) timerRead(t *[4]timer, off uint32) uint16 {
	tm := &t[(off-0x100)/4]
	if off&2 == 0 {
		return tm.counter()
	}
	return tm.ctrl
}

func (m *Machine) timerWrite(t *[4]timer, off uint32, v uint16) {
	tm := &t[(off-0x100)/4]
	if off&2 == 0 {
		tm.reload = v
	} else {
		tm.writeCtrl(v)
	}
}

// ---------------- ARM9 ----------------

func (m *Machine) io9Read8(a uint32) uint8 {
	switch a {
	case 0x04000240, 0x04000241, 0x04000242, 0x04000243, 0x04000244, 0x04000245, 0x04000246:
		return m.vram.cnt[a-0x04000240]
	case 0x04000247:
		return m.wramcnt
	case 0x04000248, 0x04000249:
		return m.vram.cnt[a-0x04000248+7]
	case 0x04000300:
		return m.postflg9
	}
	if a >= 0x040001a8 && a < 0x040001b0 {
		return m.cart.cmd[a-0x040001a8]
	}
	v := m.io9Read16(a &^ 1)
	return uint8(v >> (a & 1 * 8))
}

func (m *Machine) io9Read16(a uint32) uint16 {
	off := a - 0x04000000
	switch {
	case off < 0x70:
		switch off {
		case 0x04:
			return m.dispstatRead(true)
		case 0x06:
			return uint16(m.vcount)
		case 0x64:
			return uint16(m.dispCapCnt)
		case 0x66:
			return uint16(m.dispCapCnt >> 16)
		}
		return m.gpuA.read16(off)
	case off >= 0x1000 && off < 0x1070:
		return m.gpuB.read16(off - 0x1000)
	case off >= 0xb0 && off < 0xe0:
		v := m.dmaRead(&m.dma9, off&^3)
		return uint16(v >> (off & 2 * 8))
	case off >= 0xe0 && off < 0xf0:
		return uint16(m.dmaFill[(off-0xe0)/4] >> (off & 2 * 8))
	case off >= 0x100 && off < 0x110:
		return m.timerRead(&m.timers9, off)
	case off >= 0x320 && off < 0x6a4:
		return uint16(m.gpu3d.read32(a&^3) >> (off & 2 * 8))
	case off >= 0x280 && off < 0x2c0:
		return uint16(m.io9Read32(a&^3) >> (off & 2 * 8))
	}
	switch off {
	case 0x130:
		return m.keyinput()
	case 0x132:
		return m.keycnt9
	case 0x180:
		return m.ipcSyncRead(true)
	case 0x184:
		return m.ipcCntRead(true)
	case 0x1a0:
		return m.cart.auxspicnt
	case 0x1a2:
		return uint16(m.cart.spiData)
	case 0x1a4, 0x1a6:
		return uint16(m.cart.romctrl >> (off & 2 * 8))
	case 0x204:
		return m.exmemcnt
	case 0x208:
		return uint16(m.irq9.ime)
	case 0x210, 0x212:
		return uint16(m.irq9.ie >> (off & 2 * 8))
	case 0x214, 0x216:
		return uint16(m.irq9.ifl >> (off & 2 * 8))
	case 0x240, 0x242, 0x244, 0x246, 0x248:
		return uint16(m.io9Read8(a)) | uint16(m.io9Read8(a+1))<<8
	case 0x300:
		return uint16(m.postflg9)
	case 0x304:
		return m.powcnt1
	}
	return 0
}

func (m *Machine) io9Read32(a uint32) uint32 {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		return m.dmaRead(&m.dma9, off)
	case off >= 0x320 && off < 0x6a4:
		return m.gpu3d.read32(a)
	}
	switch off {
	case 0x000:
		return m.gpuA.dispcnt
	case 0x1000:
		return m.gpuB.dispcnt
	case 0x064:
		return m.dispCapCnt
	case 0x1a4:
		return m.cart.romctrl
	case 0x210:
		return m.irq9.ie
	case 0x214:
		return m.irq9.ifl
	case 0x280:
		return uint32(m.math.divcnt)
	case 0x290:
		return uint32(m.math.numer)
	case 0x294:
		return uint32(m.math.numer >> 32)
	case 0x298:
		return uint32(m.math.denom)
	case 0x29c:
		return uint32(m.math.denom >> 32)
	case 0x2a0:
		return uint32(m.math.quot)
	case 0x2a4:
		return uint32(m.math.quot >> 32)
	case 0x2a8:
		return uint32(m.math.rem)
	case 0x2ac:
		return uint32(m.math.rem >> 32)
	case 0x2b0:
		return uint32(m.math.sqrtcnt)
	case 0x2b4:
		return m.math.sqrtResult
	case 0x2b8:
		return uint32(m.math.sqrtParam)
	case 0x2bc:
		return uint32(m.math.sqrtParam >> 32)
	case 0x100000:
		return m.ipcRecv(true)
	case 0x100010:
		return m.cart.readData()
	}
	return uint32(m.io9Read16(a)) | uint32(m.io9Read16(a+2))<<16
}

func (m *Machine) io9Write8(a uint32, v uint8) {
	switch {
	case a >= 0x04000240 && a <= 0x04000246:
		m.vram.setCnt(int(a-0x04000240), v)
		return
	case a == 0x04000247:
		m.wramcnt = v & 3
		m.vram.wramcnt = v & 3
		return
	case a == 0x04000248 || a == 0x04000249:
		m.vram.setCnt(int(a-0x04000248+7), v)
		return
	case a == 0x04000300:
		m.postflg9 = m.postflg9&1 | v&3
		return
	case a >= 0x040001a8 && a < 0x040001b0:
		m.cart.cmd[a-0x040001a8] = v
		return
	case a == 0x040001a2:
		m.cart.spiWrite(v)
		return
	case a == 0x04000208:
		m.irq9.ime = uint32(v & 1)
		m.irq9.update()
		return
	case a >= 0x04000320 && a < 0x040006a4:
		m.gpu3d.write8(a, v)
		return
	}
	// read-modify-write the halfword
	h := a &^ 1
	old := m.io9Read16(h)
	if a&1 == 0 {
		m.io9Write16Mask(h, uint16(v), 0x00ff, old)
	} else {
		m.io9Write16Mask(h, uint16(v)<<8, 0xff00, old)
	}
}

func (m *Machine) io9Write16Mask(a uint32, v, mask, old uint16) {
	off := a - 0x04000000
	// registers where writing a byte must not touch the other byte's side
	// effects
	switch {
	case off == 0x214 || off == 0x216:
		m.irq9.ifl &^= uint32(v&mask) << (off & 2 * 8)
		m.irq9.update()
		return
	}
	m.io9Write16(a, old&^mask|v&mask)
}

func (m *Machine) io9Write16(a uint32, v uint16) {
	off := a - 0x04000000
	switch {
	case off < 0x70:
		switch off {
		case 0x04:
			m.dispstatWrite(true, v)
		case 0x06:
		case 0x64:
			m.dispCapCnt = m.dispCapCnt&0xffff0000 | uint32(v)
		case 0x66:
			m.dispCapCnt = m.dispCapCnt&0xffff | uint32(v)<<16
		default:
			m.gpuA.write16(off, v)
		}
		return
	case off >= 0x1000 && off < 0x1070:
		m.gpuB.write16(off-0x1000, v)
		return
	case off >= 0xb0 && off < 0xe0:
		m.dmaWrite(&m.dma9, off&^3, uint32(v)<<(off&2*8), 0xffff<<(off&2*8))
		return
	case off >= 0xe0 && off < 0xf0:
		i := (off - 0xe0) / 4
		sh := off & 2 * 8
		m.dmaFill[i] = m.dmaFill[i]&^(0xffff<<sh) | uint32(v)<<sh
		return
	case off >= 0x100 && off < 0x110:
		m.timerWrite(&m.timers9, off, v)
		return
	case off >= 0x320 && off < 0x6a4:
		m.gpu3d.write16(a, v)
		return
	case off >= 0x280 && off < 0x2c0:
		sh := off & 2 * 8
		old := m.io9Read32(a &^ 3)
		m.io9Write32(a&^3, old&^(0xffff<<sh)|uint32(v)<<sh)
		return
	}
	switch off {
	case 0x132:
		m.keycnt9 = v
	case 0x180:
		m.ipcSyncWrite(true, v)
	case 0x184:
		m.ipcCntWrite(true, v)
	case 0x1a0:
		m.cart.auxspicnt = v
	case 0x1a2:
		m.cart.spiWrite(uint8(v))
	case 0x1a4:
		m.cart.writeCtrl(m.cart.romctrl&0xffff0000 | uint32(v))
	case 0x1a6:
		m.cart.writeCtrl(m.cart.romctrl&0xffff | uint32(v)<<16)
	case 0x1a8, 0x1aa, 0x1ac, 0x1ae:
		m.cart.cmd[off-0x1a8] = uint8(v)
		m.cart.cmd[off-0x1a8+1] = uint8(v >> 8)
	case 0x204:
		m.exmemcnt = v
	case 0x208:
		m.irq9.ime = uint32(v & 1)
		m.irq9.update()
	case 0x210:
		m.irq9.ie = m.irq9.ie&0xffff0000 | uint32(v)
		m.irq9.update()
	case 0x212:
		m.irq9.ie = m.irq9.ie&0xffff | uint32(v)<<16
		m.irq9.update()
	case 0x214:
		m.irq9.ifl &^= uint32(v)
		m.irq9.update()
	case 0x216:
		m.irq9.ifl &^= uint32(v) << 16
		m.irq9.update()
	case 0x240, 0x242, 0x244, 0x246, 0x248:
		m.io9Write8(a, uint8(v))
		m.io9Write8(a+1, uint8(v>>8))
	case 0x300:
		m.postflg9 = m.postflg9&1 | uint8(v&3)
	case 0x304:
		m.powcnt1 = v & 0x820f
	}
}

func (m *Machine) io9Write32(a, v uint32) {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		m.dmaWrite(&m.dma9, off, v, 0xffffffff)
		return
	case off >= 0xe0 && off < 0xf0:
		m.dmaFill[(off-0xe0)/4] = v
		return
	case off >= 0x320 && off < 0x6a4:
		m.gpu3d.write32(a, v)
		return
	}
	switch off {
	case 0x000:
		m.gpuA.dispcnt = v
		return
	case 0x1000:
		m.gpuB.dispcnt = v
		return
	case 0x064:
		m.dispCapCnt = v
		return
	case 0x188:
		m.ipcSend(true, v)
		return
	case 0x1a4:
		m.cart.writeCtrl(v)
		return
	case 0x210:
		m.irq9.ie = v
		m.irq9.update()
		return
	case 0x214:
		m.irq9.ifl &^= v
		m.irq9.update()
		return
	case 0x280:
		m.math.divcnt = m.math.divcnt&0x4000 | uint16(v&3)
		m.math.divide()
		return
	case 0x290:
		m.math.numer = m.math.numer&^0xffffffff | uint64(v)
		m.math.divide()
		return
	case 0x294:
		m.math.numer = m.math.numer&0xffffffff | uint64(v)<<32
		m.math.divide()
		return
	case 0x298:
		m.math.denom = m.math.denom&^0xffffffff | uint64(v)
		m.math.divide()
		return
	case 0x29c:
		m.math.denom = m.math.denom&0xffffffff | uint64(v)<<32
		m.math.divide()
		return
	case 0x2b0:
		m.math.sqrtcnt = uint16(v & 1)
		m.math.sqrt()
		return
	case 0x2b8:
		m.math.sqrtParam = m.math.sqrtParam&^0xffffffff | uint64(v)
		m.math.sqrt()
		return
	case 0x2bc:
		m.math.sqrtParam = m.math.sqrtParam&0xffffffff | uint64(v)<<32
		m.math.sqrt()
		return
	case 0x100010:
		return
	}
	if off >= 0x08 && off < 0x70 || off >= 0x1008 && off < 0x1070 {
		// the 2D engines' registers take 32-bit writes as two halves
		m.io9Write16(a, uint16(v))
		m.io9Write16(a+2, uint16(v>>16))
		return
	}
	m.io9Write16(a, uint16(v))
	m.io9Write16(a+2, uint16(v>>16))
}

// ---------------- ARM7 ----------------

func (m *Machine) io7Read8(a uint32) uint8 {
	switch {
	case a == 0x04000240:
		return m.vram.arm7Stat()
	case a == 0x04000241:
		return m.wramcnt
	case a == 0x04000300:
		return m.postflg7
	case a == 0x04000301:
		return m.haltcnt
	case a >= 0x040001a8 && a < 0x040001b0:
		return m.cart.cmd[a-0x040001a8]
	case a >= 0x04000400 && a < 0x04000520:
		return m.spu.read8(a - 0x04000400)
	}
	v := m.io7Read16(a &^ 1)
	return uint8(v >> (a & 1 * 8))
}

func (m *Machine) io7Read16(a uint32) uint16 {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		return uint16(m.dmaRead(&m.dma7, off&^3) >> (off & 2 * 8))
	case off >= 0x100 && off < 0x110:
		return m.timerRead(&m.timers7, off)
	case off >= 0x400 && off < 0x520:
		return uint16(m.spu.read8(off-0x400)) | uint16(m.spu.read8(off-0x400+1))<<8
	}
	switch off {
	case 0x04:
		return m.dispstatRead(false)
	case 0x06:
		return uint16(m.vcount)
	case 0x130:
		return m.keyinput()
	case 0x132:
		return m.keycnt7
	case 0x134:
		return m.rcnt
	case 0x136:
		return m.extkeyin()
	case 0x138:
		return m.rtc.read()
	case 0x180:
		return m.ipcSyncRead(false)
	case 0x184:
		return m.ipcCntRead(false)
	case 0x1a0:
		return m.cart.auxspicnt
	case 0x1a2:
		return uint16(m.cart.spiData)
	case 0x1a4, 0x1a6:
		return uint16(m.cart.romctrl >> (off & 2 * 8))
	case 0x1c0:
		return m.spi.cnt
	case 0x1c2:
		return uint16(m.spi.data)
	case 0x204:
		return m.exmemcnt&0x7f | 0x7f80&m.exmemcnt7
	case 0x208:
		return uint16(m.irq7.ime)
	case 0x210, 0x212:
		return uint16(m.irq7.ie >> (off & 2 * 8))
	case 0x214, 0x216:
		return uint16(m.irq7.ifl >> (off & 2 * 8))
	case 0x240:
		return uint16(m.vram.arm7Stat()) | uint16(m.wramcnt)<<8
	case 0x300:
		return uint16(m.postflg7) | uint16(m.haltcnt)<<8
	case 0x304:
		return m.powcnt2
	case 0x308:
		return m.bios7prot
	}
	return 0
}

func (m *Machine) io7Read32(a uint32) uint32 {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		return m.dmaRead(&m.dma7, off)
	case off >= 0x400 && off < 0x520:
		return m.spu.read32(off - 0x400)
	}
	switch off {
	case 0x1a4:
		return m.cart.romctrl
	case 0x210:
		return m.irq7.ie
	case 0x214:
		return m.irq7.ifl
	case 0x100000:
		return m.ipcRecv(false)
	case 0x100010:
		return m.cart.readData()
	}
	return uint32(m.io7Read16(a)) | uint32(m.io7Read16(a+2))<<16
}

func (m *Machine) io7Write8(a uint32, v uint8) {
	switch {
	case a == 0x04000301:
		m.haltcnt = v
		switch v >> 6 {
		case 2, 3: // halt (sleep is treated as halt)
			if m.irq7.ie&m.irq7.ifl == 0 {
				m.arm7.Halted = true
			}
		}
		return
	case a == 0x04000300:
		m.postflg7 = m.postflg7&1 | v&1
		return
	case a == 0x04000208:
		m.irq7.ime = uint32(v & 1)
		m.irq7.update()
		return
	case a >= 0x040001a8 && a < 0x040001b0:
		m.cart.cmd[a-0x040001a8] = v
		return
	case a == 0x040001a2:
		m.cart.spiWrite(v)
		return
	case a == 0x040001c2:
		m.spi.write(v)
		return
	case a == 0x04000138:
		m.rtc.write(uint16(v), 0xff)
		return
	case a >= 0x04000400 && a < 0x04000520:
		m.spu.write8(a-0x04000400, v)
		return
	}
	h := a &^ 1
	old := m.io7Read16(h)
	off := h - 0x04000000
	if off == 0x214 || off == 0x216 {
		sh := (off&2)*8 + (a&1)*8
		m.irq7.ifl &^= uint32(v) << sh
		m.irq7.update()
		return
	}
	if a&1 == 0 {
		m.io7Write16(h, old&0xff00|uint16(v))
	} else {
		m.io7Write16(h, old&0xff|uint16(v)<<8)
	}
}

func (m *Machine) io7Write16(a uint32, v uint16) {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		m.dmaWrite(&m.dma7, off&^3, uint32(v)<<(off&2*8), 0xffff<<(off&2*8))
		return
	case off >= 0x100 && off < 0x110:
		m.timerWrite(&m.timers7, off, v)
		return
	case off >= 0x400 && off < 0x520:
		m.spu.write8(off-0x400, uint8(v))
		m.spu.write8(off-0x400+1, uint8(v>>8))
		return
	}
	switch off {
	case 0x04:
		m.dispstatWrite(false, v)
	case 0x132:
		m.keycnt7 = v
	case 0x134:
		m.rcnt = v
	case 0x138:
		m.rtc.write(v, 0xffff)
	case 0x180:
		m.ipcSyncWrite(false, v)
	case 0x184:
		m.ipcCntWrite(false, v)
	case 0x1a0:
		m.cart.auxspicnt = v
	case 0x1a2:
		m.cart.spiWrite(uint8(v))
	case 0x1a4:
		m.cart.writeCtrl(m.cart.romctrl&0xffff0000 | uint32(v))
	case 0x1a6:
		m.cart.writeCtrl(m.cart.romctrl&0xffff | uint32(v)<<16)
	case 0x1a8, 0x1aa, 0x1ac, 0x1ae:
		m.cart.cmd[off-0x1a8] = uint8(v)
		m.cart.cmd[off-0x1a8+1] = uint8(v >> 8)
	case 0x1c0:
		m.spi.writeCnt(v)
	case 0x1c2:
		m.spi.write(uint8(v))
	case 0x204:
		m.exmemcnt7 = v
	case 0x208:
		m.irq7.ime = uint32(v & 1)
		m.irq7.update()
	case 0x210:
		m.irq7.ie = m.irq7.ie&0xffff0000 | uint32(v)
		m.irq7.update()
	case 0x212:
		m.irq7.ie = m.irq7.ie&0xffff | uint32(v)<<16
		m.irq7.update()
	case 0x214:
		m.irq7.ifl &^= uint32(v)
		m.irq7.update()
	case 0x216:
		m.irq7.ifl &^= uint32(v) << 16
		m.irq7.update()
	case 0x300:
		m.io7Write8(a, uint8(v))
		m.io7Write8(a+1, uint8(v>>8))
	case 0x304:
		m.powcnt2 = v & 3
	case 0x308:
		m.bios7prot = v
	}
}

func (m *Machine) io7Write32(a, v uint32) {
	off := a - 0x04000000
	switch {
	case off >= 0xb0 && off < 0xe0:
		m.dmaWrite(&m.dma7, off, v, 0xffffffff)
		return
	case off >= 0x400 && off < 0x520:
		m.spu.write32(off-0x400, v)
		return
	}
	switch off {
	case 0x188:
		m.ipcSend(false, v)
		return
	case 0x1a4:
		m.cart.writeCtrl(v)
		return
	case 0x210:
		m.irq7.ie = v
		m.irq7.update()
		return
	case 0x214:
		m.irq7.ifl &^= v
		m.irq7.update()
		return
	}
	m.io7Write16(a, uint16(v))
	m.io7Write16(a+2, uint16(v>>16))
}
