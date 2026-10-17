package sh2

import "encoding/binary"

// The SH7604's on-chip modules at 0xfffffe00-0xffffffff: serial port (as an
// idle port), free-running timer (FRT), interrupt controller registers,
// watchdog timer (interval mode), cache control, division unit (DIVU), DMA
// controller and the bus controller's registers (kept, no effect).

// Register offsets from 0xfffffe00.
const (
	rSSR    = 0x04
	rTIER   = 0x10
	rFTCSR  = 0x11
	rFRCH   = 0x12
	rFRCL   = 0x13
	rOCRH   = 0x14
	rOCRL   = 0x15
	rTCR    = 0x16
	rTOCR   = 0x17
	rICRH   = 0x18
	rICRL   = 0x19
	rIPRB   = 0x60
	rVCRA   = 0x62
	rVCRB   = 0x64
	rVCRC   = 0x66
	rVCRD   = 0x68
	rWTCSR  = 0x80
	rWTCNT  = 0x81
	rRSTCSR = 0x83
	rCCR    = 0x92
	rICR    = 0xe0
	rIPRA   = 0xe2
	rVCRWDT = 0xe4
	rDVSR   = 0x100
	rDVDNT  = 0x104
	rDVCR   = 0x108
	rVCRDIV = 0x10c
	rDVDNTH = 0x110
	rDVDNTL = 0x114
	rSAR0   = 0x180
	rDMAOR  = 0x1b0
)

// interrupt sources of the on-chip modules
const (
	srcDIVU = iota
	srcDMA0
	srcDMA1
	srcWDT
	srcFRTICI
	srcFRTOCI
	srcFRTOVI
	nSources
)

type onchip struct {
	c    *CPU
	regs [0x200]uint8 // register values as read (big-endian)

	icr uint16

	// FRT: the counter is frc at cycle frcAt, counting every frcDiv cycles
	frc       uint16
	frcAt     int64
	ocra      uint16
	ocrb      uint16
	ftcsr     uint8
	frcTemp   uint8 // the latched low byte of FRC reads/writes
	ocrTemp   uint8
	nextEvent int64

	// WDT
	wtcnt   uint8
	wtAt    int64
	wtcsr   uint8
	rstcsr  uint8
	pendSrc [nSources]bool

	cacheRAM [0x1000]uint8
}

func (o *onchip) reset() {
	o.regs = [0x200]uint8{}
	o.icr = 0
	o.frc, o.frcAt, o.ocra, o.ocrb = 0, o.c.Cycles, 0xffff, 0xffff
	o.ftcsr, o.frcTemp, o.ocrTemp = 0, 0, 0
	o.regs[rTCR] = 0
	o.regs[rTOCR] = 0xe0
	o.regs[rSSR] = 0x84
	o.wtcnt, o.wtAt, o.wtcsr, o.rstcsr = 0, o.c.Cycles, 0x18, 0x1f
	o.pendSrc = [nSources]bool{}
	binary.BigEndian.PutUint32(o.regs[rDMAOR:], 0)
	o.schedule()
}

func (o *onchip) r16(off int) uint16 { return binary.BigEndian.Uint16(o.regs[off:]) }
func (o *onchip) r32(off int) uint32 { return binary.BigEndian.Uint32(o.regs[off:]) }
func (o *onchip) w32(off int, v uint32) {
	binary.BigEndian.PutUint32(o.regs[off:], v)
}

// ---------------------------------------------------------------- access

func (o *onchip) read(a uint32, size int) uint32 {
	off := int(a - 0xfffffe00)
	if off >= 0x100 {
		// 32-bit registers (DIVU at 0x100-0x13f mirrors every 0x20, DMAC, BSC)
		if off < 0x140 {
			off = 0x100 + off&0x1f
		}
		v := o.r32(off &^ 3)
		switch size {
		case 1:
			return v >> (8 * (3 - off&3)) & 0xff
		case 2:
			return v >> (8 * (2 - off&2)) & 0xffff
		}
		return v
	}
	switch size {
	case 1:
		return uint32(o.read8(off))
	case 2:
		return uint32(o.read8(off))<<8 | uint32(o.read8(off+1))
	}
	return uint32(o.read8(off))<<24 | uint32(o.read8(off+1))<<16 | uint32(o.read8(off+2))<<8 | uint32(o.read8(off+3))
}

func (o *onchip) read8(off int) uint8 {
	switch off {
	case rFTCSR:
		o.frtSync()
		return o.ftcsr
	case rFRCH:
		o.frtSync()
		o.frcTemp = uint8(o.frc)
		return uint8(o.frc >> 8)
	case rFRCL:
		return o.frcTemp
	case rOCRH:
		if o.regs[rTOCR]&0x10 != 0 {
			return uint8(o.ocrb >> 8)
		}
		return uint8(o.ocra >> 8)
	case rOCRL:
		if o.regs[rTOCR]&0x10 != 0 {
			return uint8(o.ocrb)
		}
		return uint8(o.ocra)
	case rWTCSR:
		o.wdtSync()
		return o.wtcsr
	case rWTCNT:
		o.wdtSync()
		return o.wtcnt
	case rRSTCSR:
		return o.rstcsr
	case rICR:
		return uint8(o.icr >> 8)
	case rICR + 1:
		return uint8(o.icr)
	}
	return o.regs[off]
}

func (o *onchip) write(a, v uint32, size int) {
	off := int(a - 0xfffffe00)
	if off >= 0x100 {
		if off < 0x140 {
			off = 0x100 + off&0x1f
		}
		base := off &^ 3
		old := o.r32(base)
		var nv uint32
		switch size {
		case 1:
			sh := 8 * uint(3-off&3)
			nv = old&^(0xff<<sh) | (v&0xff)<<sh
		case 2:
			sh := 8 * uint(2-off&2)
			nv = old&^(0xffff<<sh) | (v&0xffff)<<sh
		default:
			nv = v
		}
		o.write32(base, nv, old)
		return
	}
	switch size {
	case 1:
		o.write8(off, uint8(v))
	case 2:
		if off == rWTCSR {
			o.wdtWrite(uint16(v))
			return
		}
		if off == rRSTCSR-1 {
			// RSTCSR: 0xa5 writes WOVF clear, 0x5a writes RSTE/RSTS
			if v>>8 == 0x5a {
				o.rstcsr = o.rstcsr&0x80 | uint8(v)&0x60 | 0x1f
			} else if v>>8 == 0xa5 && v&0x80 == 0 {
				o.rstcsr &^= 0x80
			}
			return
		}
		o.write8(off, uint8(v>>8))
		o.write8(off+1, uint8(v))
	default:
		o.write8(off, uint8(v>>24))
		o.write8(off+1, uint8(v>>16))
		o.write8(off+2, uint8(v>>8))
		o.write8(off+3, uint8(v))
	}
}

func (o *onchip) write8(off int, v uint8) {
	switch off {
	case rTIER:
		o.regs[off] = v | 0x01
		o.updateIRQ()
	case rFTCSR:
		o.frtSync()
		// flags are cleared by writing 0 after reading 1; bit 0 is CCLRA
		o.ftcsr = o.ftcsr&(v|0x01)&0x8e | v&0x01
		o.updateIRQ()
	case rFRCH:
		o.frcTemp = v
	case rFRCL:
		o.frtSync()
		o.frc = uint16(o.frcTemp)<<8 | uint16(v)
		o.schedule()
	case rOCRH:
		o.ocrTemp = v
	case rOCRL:
		o.frtSync()
		if o.regs[rTOCR]&0x10 != 0 {
			o.ocrb = uint16(o.ocrTemp)<<8 | uint16(v)
		} else {
			o.ocra = uint16(o.ocrTemp)<<8 | uint16(v)
		}
		o.schedule()
	case rTCR:
		o.frtSync()
		o.regs[off] = v
		o.schedule()
	case rTOCR:
		o.regs[off] = v | 0xe0
	case rICR:
		o.icr = o.icr&0x00ff | uint16(v)<<8
	case rICR + 1:
		o.icr = o.icr&0xff00 | uint16(v)
	case rWTCSR, rWTCNT, rRSTCSR, rRSTCSR - 1:
		// byte writes to the watchdog are ignored (it needs the 16-bit keys)
	case rCCR:
		o.regs[off] = v &^ 0x10 // CP (cache purge) reads 0
	default:
		o.regs[off] = v
		if off >= rIPRB && off < rVCRD+2 || off >= rIPRA && off < rVCRWDT+2 {
			o.c.irqCheck = true
		}
	}
}

// write32: the 32-bit registers.
func (o *onchip) write32(off int, v, old uint32) {
	switch off {
	case rDVSR, rVCRDIV, rDVDNTH:
		o.w32(off, v)
	case rDVCR:
		o.w32(off, v&3)
		o.pendSrc[srcDIVU] = v&3 == 3
		o.c.irqCheck = true
	case rDVDNT:
		o.w32(off, v)
		o.divide32()
	case rDVDNTL:
		o.w32(off, v)
		o.divide64()
	case 0x118, 0x11c: // DVDNTUH/UL: mirrors of DVDNTH/L on reads
		o.w32(off, v)
	case rDMAOR:
		// AE and NMIF are cleared by writing 0 after reading 1
		v = v&0x0f&^0x06 | v&old&0x06
		o.w32(off, v)
		o.dmaRun(0)
		o.dmaRun(1)
	case rSAR0 + 0x0c, rSAR0 + 0x1c: // CHCR0/1
		ch := (off - rSAR0) >> 4
		// TE is cleared by writing 0 after reading 1
		v = v&0xfffd | v&old&0x02
		o.w32(off, v)
		o.pendSrc[srcDMA0+ch] = v&6 == 6
		o.c.irqCheck = true
		o.dmaRun(ch)
	case rSAR0 + 0x08, rSAR0 + 0x18: // TCR0/1
		o.w32(off, v&0xffffff)
	case 0x1e0, 0x1e4, 0x1e8, 0x1ec, 0x1f0, 0x1f4, 0x1f8:
		// BSC: 32-bit writes need 0xa55a in the upper half
		if v>>16 == 0xa55a {
			o.w32(off, v&0xffff)
		}
	default:
		o.w32(off, v)
	}
}

// ---------------------------------------------------------------- division unit

func (o *onchip) divide32() {
	o.c.Cycles += 0 // the CPU only waits if it reads too soon; not modelled
	dvsr := int32(o.r32(rDVSR))
	dvdnt := int32(o.r32(rDVDNT))
	if dvsr == 0 || (dvdnt == -0x80000000 && dvsr == -1) {
		o.divOverflow(dvdnt < 0 != (dvsr < 0))
		return
	}
	q, r := dvdnt/dvsr, dvdnt%dvsr
	o.w32(rDVDNT, uint32(q))
	o.w32(rDVDNTL, uint32(q))
	o.w32(rDVDNTH, uint32(r))
	o.w32(0x118, uint32(r))
	o.w32(0x11c, uint32(q))
}

func (o *onchip) divide64() {
	dvsr := int64(int32(o.r32(rDVSR)))
	dvdnt := int64(uint64(o.r32(rDVDNTH))<<32 | uint64(o.r32(rDVDNTL)))
	if dvsr == 0 {
		o.divOverflow(dvdnt < 0)
		return
	}
	q, r := dvdnt/dvsr, dvdnt%dvsr
	if q > 0x7fffffff || q < -0x80000000 {
		o.divOverflow((dvdnt < 0) != (dvsr < 0))
		return
	}
	o.w32(rDVDNT, uint32(q))
	o.w32(rDVDNTL, uint32(q))
	o.w32(rDVDNTH, uint32(r))
	o.w32(0x118, uint32(r))
	o.w32(0x11c, uint32(q))
}

// divOverflow: the quotient saturates and DVCR's OVF is set.
func (o *onchip) divOverflow(neg bool) {
	q := uint32(0x7fffffff)
	if neg {
		q = 0x80000000
	}
	o.w32(rDVDNT, q)
	o.w32(rDVDNTL, q)
	o.w32(0x11c, q)
	dvcr := o.r32(rDVCR) | 1
	o.w32(rDVCR, dvcr)
	if dvcr&2 != 0 {
		o.pendSrc[srcDIVU] = true
		o.c.irqCheck = true
	}
}

// ---------------------------------------------------------------- DMA

// dmaRun does a channel's auto-request transfer at once when it is enabled.
func (o *onchip) dmaRun(ch int) {
	base := rSAR0 + ch*0x10
	chcr := o.r32(base + 0x0c)
	dmaor := o.r32(rDMAOR)
	if chcr&3 != 1 || dmaor&7 != 1 || chcr&0x200 == 0 {
		return
	}
	sar, dar := o.r32(base), o.r32(base+4)
	n := o.r32(base + 8)
	if n == 0 {
		n = 0x1000000
	}
	unit := [4]uint32{1, 2, 4, 16}[chcr>>10&3]
	step := func(mode, sz uint32) uint32 {
		switch mode {
		case 1:
			return sz
		case 2:
			return -sz
		}
		return 0
	}
	ds, ss := step(chcr>>14&3, unit), step(chcr>>12&3, unit)
	if unit == 16 {
		// 16-byte units move as four longwords; the source always counts up
		ss = 16
	}
	c := o.c
	for ; n > 0; n-- {
		switch unit {
		case 1:
			c.write8(dar, c.read8(sar))
		case 2:
			c.write16(dar, c.read16(sar))
		case 4:
			c.write32(dar, c.read32(sar))
		default:
			for k := uint32(0); k < 16; k += 4 {
				c.write32(dar+k, c.read32(sar+k))
			}
			if n >= 4 {
				n -= 3
			} else {
				n = 1
			}
		}
		sar += ss
		dar += ds
	}
	o.w32(base, sar)
	o.w32(base+4, dar)
	o.w32(base+8, 0)
	o.w32(base+0x0c, chcr|2)
	if chcr&4 != 0 {
		o.pendSrc[srcDMA0+ch] = true
		c.irqCheck = true
	}
}

// ---------------------------------------------------------------- FRT

// frtDiv: CPU cycles per count (TCR CKS: phi/8, /32, /128; external clock
// does not count).
func (o *onchip) frtDiv() int64 {
	switch o.regs[rTCR] & 3 {
	case 0:
		return 8
	case 1:
		return 32
	case 2:
		return 128
	}
	return 0
}

// frtSync brings the counter up to date, setting the compare and overflow
// flags it passes.
func (o *onchip) frtSync() {
	div := o.frtDiv()
	now := o.c.Cycles
	if div == 0 {
		o.frcAt = now
		return
	}
	n := (now - o.frcAt) / div
	if n <= 0 {
		return
	}
	o.frcAt += n * div
	for n > 0 {
		// count to the next interesting value at most
		next := int64(0x10000) - int64(o.frc) // to overflow
		da := int64(o.ocra) - int64(o.frc)
		if da <= 0 {
			da += 0x10000
		}
		db := int64(o.ocrb) - int64(o.frc)
		if db <= 0 {
			db += 0x10000
		}
		step := min(next, da, db, n)
		o.frc += uint16(step)
		n -= step
		if step == da {
			o.ftcsr |= 0x08
			if o.ftcsr&1 != 0 {
				o.frc = 0
			}
		}
		if step == db {
			o.ftcsr |= 0x04
		}
		if step == next && !(step == da && o.ftcsr&1 != 0) {
			o.ftcsr |= 0x02
		}
	}
	o.updateIRQ()
}

// schedule finds when the next timer event (FRT match or overflow, WDT
// overflow) happens.
func (o *onchip) schedule() {
	next := int64(1) << 62
	if div := o.frtDiv(); div != 0 {
		d := int64(0x10000) - int64(o.frc)
		da := int64(o.ocra) - int64(o.frc)
		if da <= 0 {
			da += 0x10000
		}
		db := int64(o.ocrb) - int64(o.frc)
		if db <= 0 {
			db += 0x10000
		}
		next = o.frcAt + min(d, da, db)*div
	}
	if o.wtcsr&0x20 != 0 {
		next = min(next, o.wtAt+int64(0x100-int(o.wtcnt))*o.wdtDiv())
	}
	o.nextEvent = next
	// make Run look at the new time
	o.c.irqCheck = true
}

// update runs the timers to now (called when nextEvent is reached).
func (o *onchip) update() {
	o.frtSync()
	o.wdtSync()
	o.schedule()
}

// ---------------------------------------------------------------- WDT

func (o *onchip) wdtDiv() int64 {
	return [8]int64{2, 64, 128, 256, 512, 1024, 4096, 8192}[o.wtcsr&7]
}

func (o *onchip) wdtSync() {
	now := o.c.Cycles
	if o.wtcsr&0x20 == 0 {
		o.wtAt = now
		return
	}
	div := o.wdtDiv()
	n := (now - o.wtAt) / div
	if n <= 0 {
		return
	}
	o.wtAt += n * div
	total := int64(o.wtcnt) + n
	if total >= 0x100 {
		if o.wtcsr&0x40 == 0 {
			// interval timer: overflow interrupt
			o.wtcsr |= 0x80
			o.pendSrc[srcWDT] = true
			o.c.irqCheck = true
		} else {
			// watchdog mode: the overflow would reset the chip; flag it only
			o.rstcsr |= 0x80
		}
	}
	o.wtcnt = uint8(total)
}

// wdtWrite: 16-bit writes with key 0xa5 set WTCSR, 0x5a set WTCNT.
func (o *onchip) wdtWrite(v uint16) {
	o.wdtSync()
	switch v >> 8 {
	case 0xa5:
		// OVF is cleared by writing 0 after reading 1
		nv := uint8(v)&0x67 | 0x18
		nv |= o.wtcsr & uint8(v) & 0x80
		o.wtcsr = nv
		if nv&0x80 == 0 {
			o.pendSrc[srcWDT] = false
		}
		o.wtAt = o.c.Cycles
	case 0x5a:
		o.wtcnt = uint8(v)
		o.wtAt = o.c.Cycles
	}
	o.schedule()
}

// ---------------------------------------------------------------- interrupts

// updateIRQ recomputes the FRT's interrupt requests from its flags.
func (o *onchip) updateIRQ() {
	tier := o.regs[rTIER]
	o.pendSrc[srcFRTICI] = tier&0x80 != 0 && o.ftcsr&0x80 != 0
	o.pendSrc[srcFRTOCI] = tier&0x0c&(o.ftcsr&0x0c) != 0
	o.pendSrc[srcFRTOVI] = tier&0x02 != 0 && o.ftcsr&0x02 != 0
	o.c.irqCheck = true
}

// level and vector of each source.
func (o *onchip) source(s int) (level, vec int) {
	ipra, iprb := o.r16(rIPRA), o.r16(rIPRB)
	switch s {
	case srcDIVU:
		return int(ipra >> 12), int(o.r32(rVCRDIV) & 0x7f)
	case srcDMA0:
		return int(ipra >> 8 & 15), int(o.r32(0x1a0) & 0x7f)
	case srcDMA1:
		return int(ipra >> 8 & 15), int(o.r32(0x1a8) & 0x7f)
	case srcWDT:
		return int(ipra >> 4 & 15), int(o.r16(rVCRWDT) >> 8 & 0x7f)
	case srcFRTICI:
		return int(iprb >> 8 & 15), int(o.r16(rVCRC) >> 8 & 0x7f)
	case srcFRTOCI:
		return int(iprb >> 8 & 15), int(o.r16(rVCRC) & 0x7f)
	case srcFRTOVI:
		return int(iprb >> 8 & 15), int(o.r16(rVCRD) >> 8 & 0x7f)
	}
	return 0, 0
}

// pending returns the highest-priority on-chip request (level 0 = none).
func (o *onchip) pending() (level, vec int) {
	for s := 0; s < nSources; s++ {
		if !o.pendSrc[s] {
			continue
		}
		l, v := o.source(s)
		if l > level {
			level, vec = l, v
		}
	}
	return
}

// ack: taking an on-chip interrupt does not clear its flag (the handler
// does), except that DMA and DIVU requests are remembered until their flags
// are cleared; nothing to do here.
func (o *onchip) ack(vec int) {}
