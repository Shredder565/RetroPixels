package nds

import (
	"math/bits"

	"retropixels/cores/pixelds/arm"
)

// Interrupt bits.
const (
	irqVBlank    = 1 << 0
	irqHBlank    = 1 << 1
	irqVCount    = 1 << 2
	irqTimer0    = 1 << 3
	irqDMA0      = 1 << 8
	irqKeypad    = 1 << 12
	irqIPCSync   = 1 << 16
	irqIPCSend   = 1 << 17
	irqIPCRecv   = 1 << 18
	irqCartDone  = 1 << 19
	irqGXFIFO    = 1 << 21
	irqSPI       = 1 << 23
	irqRTC       = 1 << 7
	irqUnfolding = 1 << 22
)

// ---------------- interrupts ----------------

type irqCtl struct {
	cpu     *arm.CPU
	ime     uint32
	ie, ifl uint32
}

func (q *irqCtl) update() {
	q.cpu.IRQLine = q.ime&1 != 0 && q.ie&q.ifl != 0
	if q.ie&q.ifl != 0 {
		q.cpu.Halted = false
	}
}

func (q *irqCtl) raise(bits uint32) {
	q.ifl |= bits
	q.update()
}

// ---------------- timers ----------------

var prescale = [4]int64{2, 128, 512, 2048} // in ARM9 cycles

type timer struct {
	m          *Machine
	irq        *irqCtl
	n          int
	reload     uint16
	ctrl       uint16
	base       uint16 // counter value at start
	start      int64
	overflowAt int64
}

func (t *timer) clock() int64 {
	if t.irq == &t.m.irq9 {
		return t.m.arm9.Cycles
	}
	return t.m.arm7.Cycles * 2
}

func (t *timer) group() *[4]timer {
	if t.irq == &t.m.irq9 {
		return &t.m.timers9
	}
	return &t.m.timers7
}

func (t *timer) running() bool { return t.ctrl&0x80 != 0 && !(t.n > 0 && t.ctrl&4 != 0) }

func (t *timer) counter() uint16 {
	if !t.running() {
		return t.base
	}
	p := prescale[t.ctrl&3]
	el := (t.clock() - t.start) / p
	if el < 0 {
		el = 0
	}
	v := int64(t.base) + el
	if v > 0xffff {
		span := int64(0x10000 - int(t.reload))
		v = int64(t.reload) + (v-0x10000)%span
	}
	return uint16(v)
}

func (t *timer) schedule() {
	if !t.running() {
		t.overflowAt = never
		return
	}
	t.overflowAt = t.start + int64(0x10000-int(t.base))*prescale[t.ctrl&3]
}

func (t *timer) writeCtrl(v uint16) {
	was := t.ctrl & 0x80
	if t.running() {
		t.base = t.counter()
		t.start = t.clock()
	}
	t.ctrl = v & 0xc7
	if was == 0 && v&0x80 != 0 {
		t.base = t.reload
		t.start = t.clock() + 2
	} else {
		t.start = t.clock()
	}
	t.schedule()
}

func (t *timer) overflow() {
	t.start = t.overflowAt
	t.base = t.reload
	t.schedule()
	t.fire()
}

func (t *timer) fire() {
	if t.ctrl&0x40 != 0 {
		t.irq.raise(irqTimer0 << t.n)
	}
	if t.n < 3 {
		nt := &t.group()[t.n+1]
		if nt.ctrl&0x84 == 0x84 {
			nt.base++
			if nt.base == 0 {
				nt.base = nt.reload
				nt.fire()
			}
		}
	}
}

// ---------------- DMA ----------------

const (
	dmaImmediate = iota
	dmaVBlank
	dmaHBlank
	dmaDisplayStart
	dmaMainDisplay
	dmaCart
	dmaGBACart
	dmaGXFIFO
	dmaWifi
)

type dma struct {
	m        *Machine
	arm9     bool
	n        int
	sad, dad uint32
	cnt      uint32
	src, dst uint32
	left     uint32
}

func (d *dma) mode() int {
	if d.arm9 {
		return int(d.cnt >> 27 & 7)
	}
	switch d.cnt >> 28 & 3 {
	case 0:
		return dmaImmediate
	case 1:
		return dmaVBlank
	case 2:
		return dmaCart
	}
	return dmaWifi
}

func (d *dma) units() uint32 {
	var n uint32
	if d.arm9 {
		n = d.cnt & 0x1fffff
		if n == 0 {
			n = 0x200000
		}
	} else {
		n = d.cnt & 0xffff
		if d.n < 3 {
			n &= 0x3fff
		}
		if n == 0 {
			if d.n == 3 {
				n = 0x10000
			} else {
				n = 0x4000
			}
		}
	}
	return n
}

func (d *dma) writeCnt(v uint32) {
	was := d.cnt & (1 << 31)
	d.cnt = v
	if was == 0 && v&(1<<31) != 0 {
		d.src, d.dst = d.sad, d.dad
		d.left = d.units()
		if d.mode() == dmaImmediate {
			d.run()
		} else if d.mode() == dmaGXFIFO {
			d.run()
		} else if d.mode() == dmaCart && d.m.cart.wordReady() {
			d.m.cart.feedDMA()
		}
	}
}

// run transfers everything left (one word for card DMA).
func (d *dma) run() {
	if d.cnt&(1<<31) == 0 {
		return
	}
	var bus interface {
		Read16(uint32) uint32
		Read32(uint32) uint32
		Write16(uint32, uint32)
		Write32(uint32, uint32)
	} = d.m.bus7
	if d.arm9 {
		bus = d.m.bus9
	}
	word := d.cnt&(1<<26) != 0
	step := uint32(2)
	if word {
		step = 4
	}
	srcCtl, dstCtl := d.cnt>>23&3, d.cnt>>21&3
	n := d.left
	if d.mode() == dmaCart {
		n = 1
	}
	for i := uint32(0); i < n; i++ {
		if word {
			bus.Write32(d.dst, bus.Read32(d.src))
		} else {
			bus.Write16(d.dst, bus.Read16(d.src))
		}
		switch srcCtl {
		case 0:
			d.src += step
		case 1:
			d.src -= step
		}
		switch dstCtl {
		case 0, 3:
			d.dst += step
		case 1:
			d.dst -= step
		}
	}
	cost := int64(n) * 2
	if d.arm9 {
		d.m.arm9.Cycles += cost
	} else {
		d.m.arm7.Cycles += cost / 2
	}
	d.left -= n
	if d.left > 0 {
		return
	}
	if d.cnt&(1<<30) != 0 {
		if d.arm9 {
			d.m.irq9.raise(irqDMA0 << d.n)
		} else {
			d.m.irq7.raise(irqDMA0 << d.n)
		}
	}
	if d.cnt&(1<<25) != 0 && d.mode() != dmaImmediate {
		d.left = d.units()
		if dstCtl == 3 {
			d.dst = d.dad
		}
		return
	}
	d.cnt &^= 1 << 31
}

func (m *Machine) dmaEvent9(mode int) {
	for i := range m.dma9 {
		if d := &m.dma9[i]; d.cnt&(1<<31) != 0 && d.mode() == mode {
			d.run()
		}
	}
}

func (m *Machine) dmaEvent7(mode int) {
	for i := range m.dma7 {
		if d := &m.dma7[i]; d.cnt&(1<<31) != 0 && d.mode() == mode {
			d.run()
		}
	}
}

// ---------------- IPC ----------------

type fifo struct {
	data  [16]uint32
	n, at int
}

func (f *fifo) push(v uint32) bool {
	if f.n == 16 {
		return false
	}
	f.data[(f.at+f.n)&15] = v
	f.n++
	return true
}

func (f *fifo) pop() (uint32, bool) {
	if f.n == 0 {
		return 0, false
	}
	v := f.data[f.at]
	f.at = (f.at + 1) & 15
	f.n--
	return v, true
}

func (f *fifo) front() uint32 { return f.data[f.at] }

type ipc struct {
	sync9, sync7 uint16 // each side's IPCSYNC (output bits 8-11, IRQ enable 14)
	cnt9, cnt7   uint16 // IPCFIFOCNT control bits (2, 10, 14, 15)
	to7, to9     fifo   // ARM9 -> ARM7, ARM7 -> ARM9
	last9, last7 uint32 // last word each side read
}

func (m *Machine) ipcSyncRead(arm9 bool) uint16 {
	p := &m.ipc
	if arm9 {
		return p.sync9&0x4f00 | p.sync7>>8&0xf
	}
	return p.sync7&0x4f00 | p.sync9>>8&0xf
}

func (m *Machine) ipcSyncWrite(arm9 bool, v uint16) {
	p := &m.ipc
	if arm9 {
		p.sync9 = v & 0x4f00
		if v&0x2000 != 0 && p.sync7&0x4000 != 0 {
			m.irq7.raise(irqIPCSync)
		}
	} else {
		p.sync7 = v & 0x4f00
		if v&0x2000 != 0 && p.sync9&0x4000 != 0 {
			m.irq9.raise(irqIPCSync)
		}
	}
}

func (m *Machine) ipcCntRead(arm9 bool) uint16 {
	p := &m.ipc
	send, recv, c := &p.to7, &p.to9, p.cnt9
	if !arm9 {
		send, recv, c = &p.to9, &p.to7, p.cnt7
	}
	v := c & 0xc404
	if send.n == 0 {
		v |= 1
	}
	if send.n == 16 {
		v |= 2
	}
	if recv.n == 0 {
		v |= 0x100
	}
	if recv.n == 16 {
		v |= 0x200
	}
	return v
}

func (m *Machine) ipcCntWrite(arm9 bool, v uint16) {
	p := &m.ipc
	c, send, recv, me := &p.cnt9, &p.to7, &p.to9, &m.irq9
	if !arm9 {
		c, send, recv, me = &p.cnt7, &p.to9, &p.to7, &m.irq7
	}
	if v&8 != 0 {
		send.n, send.at = 0, 0
	}
	if v&0x4000 != 0 {
		*c &^= 0x4000
	}
	old := *c
	*c = *c&0x4000 | v&0x8404
	if old&4 == 0 && v&4 != 0 && send.n == 0 {
		me.raise(irqIPCSend)
	}
	if old&0x400 == 0 && v&0x400 != 0 && recv.n != 0 {
		me.raise(irqIPCRecv)
	}
}

func (m *Machine) ipcSend(arm9 bool, v uint32) {
	p := &m.ipc
	c, send, other, otherCnt := &p.cnt9, &p.to7, &m.irq7, p.cnt7
	if !arm9 {
		c, send, other, otherCnt = &p.cnt7, &p.to9, &m.irq9, p.cnt9
	}
	if *c&0x8000 == 0 {
		return
	}
	wasEmpty := send.n == 0
	if !send.push(v) {
		*c |= 0x4000
		return
	}
	if wasEmpty && otherCnt&0x400 != 0 {
		other.raise(irqIPCRecv)
	}
}

func (m *Machine) ipcRecv(arm9 bool) uint32 {
	p := &m.ipc
	c, recv, last, other, otherCnt := &p.cnt9, &p.to9, &p.last9, &m.irq7, p.cnt7
	if !arm9 {
		c, recv, last, other, otherCnt = &p.cnt7, &p.to7, &p.last7, &m.irq9, p.cnt9
	}
	if *c&0x8000 == 0 {
		if recv.n > 0 {
			return recv.front()
		}
		return *last
	}
	v, ok := recv.pop()
	if !ok {
		*c |= 0x4000
		return *last
	}
	*last = v
	if recv.n == 0 && otherCnt&4 != 0 {
		other.raise(irqIPCSend)
	}
	return v
}

// ---------------- divider and square root ----------------

type mathUnit struct {
	divcnt       uint16
	numer, denom uint64
	quot, rem    uint64
	sqrtcnt      uint16
	sqrtParam    uint64
	sqrtResult   uint32
}

func (u *mathUnit) divide() {
	mode := u.divcnt & 3
	var n, d int64
	switch mode {
	case 0:
		n, d = int64(int32(u.numer)), int64(int32(u.denom))
	case 1, 3:
		n, d = int64(u.numer), int64(int32(u.denom))
	case 2:
		n, d = int64(u.numer), int64(u.denom)
	}
	if u.denom == 0 {
		u.divcnt |= 0x4000
	} else {
		u.divcnt &^= 0x4000
	}
	if d == 0 {
		u.rem = uint64(n)
		if n < 0 {
			u.quot = 1
		} else {
			u.quot = ^uint64(0)
		}
		if mode == 0 {
			u.quot ^= 0xffffffff00000000
		}
		return
	}
	if n == -1<<63 && d == -1 {
		u.quot, u.rem = uint64(n), 0
		return
	}
	if mode == 0 && n == -1<<31 && d == -1 {
		u.quot, u.rem = 0x80000000, 0
		return
	}
	u.quot, u.rem = uint64(n/d), uint64(n%d)
}

func (u *mathUnit) sqrt() {
	v := u.sqrtParam
	if u.sqrtcnt&1 == 0 {
		v &= 0xffffffff
	}
	// integer square root
	r := uint64(0)
	if v > 0 {
		r = uint64(1) << ((bits.Len64(v) + 1) / 2)
		for {
			nr := (r + v/r) / 2
			if nr >= r {
				break
			}
			r = nr
		}
		for r*r > v {
			r--
		}
		for (r+1)*(r+1) <= v && r+1 <= 0xffffffff {
			r++
		}
	}
	u.sqrtResult = uint32(r)
}
