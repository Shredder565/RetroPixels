package arm

import "math/bits"

// armTable is indexed by instruction bits 27-20 and 7-4.
var armTable [4096]func(c *CPU, op uint32)

func init() {
	for i := 0; i < 4096; i++ {
		armTable[i] = decodeARM(uint32(i>>4), uint32(i&15))
	}
	for i := 0; i < 1024; i++ {
		thumbTable[i] = decodeThumb(uint32(i))
	}
}

func decodeARM(hi, lo uint32) func(*CPU, uint32) {
	switch hi >> 5 {
	case 0:
		switch {
		case lo == 9:
			switch {
			case hi&0xfc == 0x00:
				return armMul
			case hi&0xfe == 0x04:
				return armUMAAL
			case hi&0xf8 == 0x08:
				return armMulLong
			case hi&0xfb == 0x10:
				return armSwap
			case hi&0xf8 == 0x18:
				return armExclusive
			}
			return armUndef
		case lo&9 == 9:
			return armHalf
		case hi&0xf9 == 0x10:
			switch {
			case lo == 0:
				if hi&2 != 0 {
					return armMSRReg
				}
				return armMRS
			case lo == 1 && hi == 0x12:
				return armBX
			case lo == 1 && hi == 0x16:
				return armCLZ
			case lo == 3 && hi == 0x12:
				return armBLXReg
			case lo == 5:
				return armQArith
			case lo == 7 && hi == 0x12:
				return armBKPT
			case lo&9 == 8:
				return armDSPMul
			}
			return armUndef
		}
		if lo&1 == 0 {
			return armDPImmShift
		}
		return armDPRegShift
	case 1:
		if hi&0xfb == 0x32 {
			return armMSRImm
		}
		if hi&0xfb == 0x30 {
			return armUndef
		}
		return armDPImm
	case 2:
		return armSDT
	case 3:
		if lo&1 != 0 {
			return armMedia
		}
		return armSDT
	case 4:
		return armBlock
	case 5:
		return armBranch
	case 6:
		return armLDC // LDC/STC: the ARM11's VFP loads and stores
	case 7:
		if hi&0x10 != 0 {
			return armSWI
		}
		if lo&1 != 0 {
			return armCoproc
		}
		return armCDP
	}
	return armUndef
}

func armUndef(c *CPU, op uint32) { c.undefined(op) }

// ---------------- data processing ----------------

func (c *CPU) shiftImm(op uint32) (uint32, bool) {
	v := c.R[op&15]
	amt := op >> 7 & 31
	cf := c.CPSR&FlagC != 0
	switch op >> 5 & 3 {
	case 0: // LSL
		if amt == 0 {
			return v, cf
		}
		return v << amt, v>>(32-amt)&1 != 0
	case 1: // LSR
		if amt == 0 {
			return 0, v>>31 != 0
		}
		return v >> amt, v>>(amt-1)&1 != 0
	case 2: // ASR
		if amt == 0 {
			return uint32(int32(v) >> 31), v>>31 != 0
		}
		return uint32(int32(v) >> amt), v>>(amt-1)&1 != 0
	default: // ROR / RRX
		if amt == 0 {
			r := v >> 1
			if cf {
				r |= 1 << 31
			}
			return r, v&1 != 0
		}
		return bits.RotateLeft32(v, -int(amt)), v>>(amt-1)&1 != 0
	}
}

func (c *CPU) shiftReg(op uint32) (uint32, bool) {
	c.R[15] += 4 // the PC reads 12 ahead when a register gives the shift
	v := c.R[op&15]
	amt := c.R[op>>8&15] & 0xff
	c.R[15] -= 4
	c.Cycles++
	cf := c.CPSR&FlagC != 0
	if amt == 0 {
		return v, cf
	}
	switch op >> 5 & 3 {
	case 0:
		switch {
		case amt < 32:
			return v << amt, v>>(32-amt)&1 != 0
		case amt == 32:
			return 0, v&1 != 0
		}
		return 0, false
	case 1:
		switch {
		case amt < 32:
			return v >> amt, v>>(amt-1)&1 != 0
		case amt == 32:
			return 0, v>>31 != 0
		}
		return 0, false
	case 2:
		if amt >= 32 {
			return uint32(int32(v) >> 31), v>>31 != 0
		}
		return uint32(int32(v) >> amt), v>>(amt-1)&1 != 0
	default:
		amt &= 31
		if amt == 0 {
			return v, v>>31 != 0
		}
		return bits.RotateLeft32(v, -int(amt)), v>>(amt-1)&1 != 0
	}
}

func armDPImm(c *CPU, op uint32) {
	rot := op >> 7 & 30
	v := bits.RotateLeft32(op&0xff, -int(rot))
	cf := c.CPSR&FlagC != 0
	if rot != 0 {
		cf = v>>31 != 0
	}
	c.alu(op, c.R[op>>16&15], v, cf)
}

func armDPImmShift(c *CPU, op uint32) {
	v, cf := c.shiftImm(op)
	c.alu(op, c.R[op>>16&15], v, cf)
}

func armDPRegShift(c *CPU, op uint32) {
	v, cf := c.shiftReg(op)
	rn := op >> 16 & 15
	a := c.R[rn]
	if rn == 15 {
		a += 4
	}
	c.alu(op, a, v, cf)
}

func (c *CPU) setNZ(r uint32) {
	c.CPSR = c.CPSR&^(FlagN|FlagZ) | r&FlagN
	if r == 0 {
		c.CPSR |= FlagZ
	}
}

func (c *CPU) setNZCV(r uint32, cf, vf bool) {
	f := r & FlagN
	if r == 0 {
		f |= FlagZ
	}
	if cf {
		f |= FlagC
	}
	if vf {
		f |= FlagV
	}
	c.CPSR = c.CPSR&0x0fffffff | f
}

func addFlags(a, b, cin uint32) (uint32, bool, bool) {
	r64 := uint64(a) + uint64(b) + uint64(cin)
	r := uint32(r64)
	return r, r64>>32 != 0, (^(a^b)&(a^r))>>31 != 0
}

func subFlags(a, b, cin uint32) (uint32, bool, bool) {
	// a - b - !cin, as a + ^b + cin
	return addFlags(a, ^b, cin)
}

func (c *CPU) alu(op, a, b uint32, cf bool) {
	s := op&(1<<20) != 0
	rd := op >> 12 & 15
	var r uint32
	logic := false
	var co, vo bool
	write := true
	switch op >> 21 & 15 {
	case 0: // AND
		r, logic = a&b, true
	case 1: // EOR
		r, logic = a^b, true
	case 2: // SUB
		r, co, vo = subFlags(a, b, 1)
	case 3: // RSB
		r, co, vo = subFlags(b, a, 1)
	case 4: // ADD
		r, co, vo = addFlags(a, b, 0)
	case 5: // ADC
		r, co, vo = addFlags(a, b, c.CPSR>>29&1)
	case 6: // SBC
		r, co, vo = subFlags(a, b, c.CPSR>>29&1)
	case 7: // RSC
		r, co, vo = subFlags(b, a, c.CPSR>>29&1)
	case 8: // TST
		r, logic, write = a&b, true, false
	case 9: // TEQ
		r, logic, write = a^b, true, false
	case 10: // CMP
		r, co, vo = subFlags(a, b, 1)
		write = false
	case 11: // CMN
		r, co, vo = addFlags(a, b, 0)
		write = false
	case 12: // ORR
		r, logic = a|b, true
	case 13: // MOV
		r, logic = b, true
	case 14: // BIC
		r, logic = a&^b, true
	case 15: // MVN
		r, logic = ^b, true
	}
	if s {
		if rd == 15 && write {
			if c.hasSPSR() {
				c.SetCPSR(c.SPSR)
			}
		} else if logic {
			c.setNZ(r)
			if cf {
				c.CPSR |= FlagC
			} else {
				c.CPSR &^= FlagC
			}
		} else {
			c.setNZCV(r, co, vo)
		}
	}
	if write {
		if rd == 15 {
			c.SetPC(r)
			c.Cycles += 2
			return
		}
		c.R[rd] = r
	}
}

// ---------------- PSR transfer ----------------

func armMRS(c *CPU, op uint32) {
	v := c.CPSR
	if op&(1<<22) != 0 {
		v = c.SPSR
		if !c.hasSPSR() {
			v = c.CPSR
		}
	}
	c.R[op>>12&15] = v
}

func (c *CPU) msr(op, v uint32) {
	var mask uint32
	if op&(1<<19) != 0 {
		mask |= 0xff000000
	}
	if op&(1<<18) != 0 {
		mask |= 0x00ff0000
	}
	if op&(1<<17) != 0 {
		mask |= 0x0000ff00
	}
	if op&(1<<16) != 0 {
		mask |= 0x000000ff
	}
	if op&(1<<22) != 0 {
		if c.hasSPSR() {
			c.SPSR = c.SPSR&^mask | v&mask
		}
		return
	}
	if c.CPSR&0x1f == ModeUSR {
		mask &= 0xff000000
	}
	mask &^= FlagT // MSR can't change the instruction set
	nv := c.CPSR&^mask | v&mask
	if mask&0xff != 0 {
		c.SetMode(nv & 0x1f)
	}
	c.CPSR = nv
}

func armMSRReg(c *CPU, op uint32) { c.msr(op, c.R[op&15]) }
func armMSRImm(c *CPU, op uint32) {
	c.msr(op, bits.RotateLeft32(op&0xff, -int(op>>7&30)))
}

// ---------------- branches ----------------

func armBX(c *CPU, op uint32) {
	c.JumpX(c.R[op&15])
	c.Cycles += 2
}

func armBLXReg(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	t := c.R[op&15]
	c.R[14] = c.next
	c.JumpX(t)
	c.Cycles += 2
}

func armBranch(c *CPU, op uint32) {
	off := uint32(int32(op<<8) >> 6)
	if op&(1<<24) != 0 {
		c.R[14] = c.next
	}
	c.next = c.R[15] + off
	c.Cycles += 2
}

func armSWI(c *CPU, op uint32) { c.swi(op >> 16 & 0xff) }

func armBKPT(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	c.exception(VecPrefetch, ModeABT, c.next)
}

// armUncond: instructions with condition field 15 (ARMv5 and later).
func (c *CPU) armUncond(op uint32) {
	if c.Arch < V5TE {
		return // NV: never executes on the ARM7
	}
	switch {
	case op&0x0e000000 == 0x0a000000: // BLX immediate
		off := uint32(int32(op<<8)>>6) | op>>23&2
		c.R[14] = c.next
		c.CPSR |= FlagT
		c.next = c.R[15] + off
		c.Cycles += 2
	case op&0x0d70f000 == 0x0550f000: // PLD
	case c.Arch >= V6K && op&0x0ff00000 == 0x01000000: // CPS / SETEND
		c.v6CPS(op)
	case c.Arch >= V6K && op == 0xf57ff01f: // CLREX
		c.exclusive = false
	case c.Arch >= V6K && op&0x0e5f0f00 == 0x084d0500: // SRS
		c.v6SRS(op)
	case c.Arch >= V6K && op&0x0e50ff00 == 0x08100a00: // RFE
		c.v6RFE(op)
	case c.Arch >= V6K && op&0x0ff000f0 == 0x05700040, op&0x0ff000f0 == 0x05700050: // DSB / DMB
	default:
		c.undefined(op)
	}
}

// ---------------- multiplies ----------------

func mulCycles(v uint32) int64 {
	switch {
	case v>>8 == 0 || v>>8 == 0xffffff:
		return 1
	case v>>16 == 0 || v>>16 == 0xffff:
		return 2
	case v>>24 == 0 || v>>24 == 0xff:
		return 3
	}
	return 4
}

func armMul(c *CPU, op uint32) {
	rd, rn, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	r := c.R[rm] * c.R[rs]
	c.Cycles += mulCycles(c.R[rs])
	if op&(1<<21) != 0 {
		r += c.R[rn]
		c.Cycles++
	}
	c.R[rd] = r
	if op&(1<<20) != 0 {
		c.setNZ(r)
	}
}

func armMulLong(c *CPU, op uint32) {
	hi, lo, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	var r uint64
	if op&(1<<22) != 0 {
		r = uint64(int64(int32(c.R[rm])) * int64(int32(c.R[rs])))
	} else {
		r = uint64(c.R[rm]) * uint64(c.R[rs])
	}
	c.Cycles += mulCycles(c.R[rs]) + 1
	if op&(1<<21) != 0 {
		r += uint64(c.R[hi])<<32 | uint64(c.R[lo])
		c.Cycles++
	}
	c.R[lo], c.R[hi] = uint32(r), uint32(r>>32)
	if op&(1<<20) != 0 {
		c.CPSR &^= FlagN | FlagZ
		if r == 0 {
			c.CPSR |= FlagZ
		}
		if r>>63 != 0 {
			c.CPSR |= FlagN
		}
	}
}

func armUMAAL(c *CPU, op uint32) {
	if c.Arch < V6K {
		c.undefined(op)
		return
	}
	hi, lo, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	r := uint64(c.R[rm])*uint64(c.R[rs]) + uint64(c.R[hi]) + uint64(c.R[lo])
	c.R[lo], c.R[hi] = uint32(r), uint32(r>>32)
	c.Cycles += 3
}

func clampS32(v int64) (uint32, bool) {
	if v > 0x7fffffff {
		return 0x7fffffff, true
	}
	if v < -0x80000000 {
		return 0x80000000, true
	}
	return uint32(int32(v)), false
}

func armCLZ(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	c.R[op>>12&15] = uint32(bits.LeadingZeros32(c.R[op&15]))
}

func armQArith(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	rn, rd, rm := op>>16&15, op>>12&15, op&15
	a, b := int64(int32(c.R[rm])), int64(int32(c.R[rn]))
	sat := false
	if op&(1<<22) != 0 { // doubling
		var d uint32
		d, sat = clampS32(b * 2)
		b = int64(int32(d))
	}
	var r uint32
	var s2 bool
	if op&(1<<21) != 0 {
		r, s2 = clampS32(a - b)
	} else {
		r, s2 = clampS32(a + b)
	}
	if sat || s2 {
		c.CPSR |= FlagQ
	}
	c.R[rd] = r
}

func half(v uint32, top bool) int64 {
	if top {
		return int64(int16(v >> 16))
	}
	return int64(int16(v))
}

func armDSPMul(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	rd, rn, rs, rm := op>>16&15, op>>12&15, op>>8&15, op&15
	x, y := op&(1<<5) != 0, op&(1<<6) != 0
	switch op >> 21 & 3 {
	case 0: // SMLAxy
		p := half(c.R[rm], x) * half(c.R[rs], y)
		r := int64(int32(p)) + int64(int32(c.R[rn]))
		if r != int64(int32(r)) {
			c.CPSR |= FlagQ
		}
		c.R[rd] = uint32(r)
	case 1: // SMLAWy / SMULWy
		p := int64(int32(c.R[rm])) * half(c.R[rs], y) >> 16
		if !x {
			r := p + int64(int32(c.R[rn]))
			if r != int64(int32(r)) {
				c.CPSR |= FlagQ
			}
			c.R[rd] = uint32(r)
		} else {
			c.R[rd] = uint32(p)
		}
	case 2: // SMLALxy
		p := half(c.R[rm], x) * half(c.R[rs], y)
		acc := int64(uint64(c.R[rd])<<32 | uint64(c.R[rn]))
		acc += p
		c.R[rn], c.R[rd] = uint32(acc), uint32(uint64(acc)>>32)
		c.Cycles++
	case 3: // SMULxy
		c.R[rd] = uint32(half(c.R[rm], x) * half(c.R[rs], y))
	}
}

// ---------------- loads and stores ----------------

func armSwap(c *CPU, op uint32) {
	rn, rd, rm := op>>16&15, op>>12&15, op&15
	a := c.R[rn]
	if op&(1<<22) != 0 {
		v := c.Bus.Read8(a)
		c.Bus.Write8(a, c.R[rm])
		c.R[rd] = v
	} else {
		v := bits.RotateLeft32(c.Bus.Read32(a), -int(a&3)*8)
		c.Bus.Write32(a, c.R[rm])
		c.R[rd] = v
	}
	c.Cycles += 3
}

// load32 is an LDR: unaligned addresses rotate the word.
func (c *CPU) load32(a uint32) uint32 {
	return bits.RotateLeft32(c.Bus.Read32(a), -int(a&3)*8)
}

// loadPC puts a loaded value in the PC; ARMv5 switches to Thumb on bit 0.
func (c *CPU) loadPC(v uint32) {
	if c.Arch >= V5TE {
		c.JumpX(v)
	} else {
		c.SetPC(v)
	}
	c.Cycles += 2
}

func armSDT(c *CPU, op uint32) {
	rn, rd := op>>16&15, op>>12&15
	var off uint32
	if op&(1<<25) == 0 {
		off = op & 0xfff
	} else {
		off, _ = c.shiftImm(op)
	}
	pre, up, wb := op&(1<<24) != 0, op&(1<<23) != 0, op&(1<<21) != 0
	base := c.R[rn]
	a := base
	if !up {
		off = -off
	}
	if pre {
		a += off
	}
	load, byt := op&(1<<20) != 0, op&(1<<22) != 0
	c.Cycles++
	if load {
		var v uint32
		if byt {
			v = c.Bus.Read8(a)
		} else {
			v = c.load32(a)
		}
		if !pre {
			c.R[rn] = base + off
		} else if wb {
			c.R[rn] = a
		}
		if rd == 15 {
			c.loadPC(v)
		} else {
			c.R[rd] = v
		}
		return
	}
	v := c.R[rd]
	if rd == 15 {
		v += 4
	}
	if byt {
		c.Bus.Write8(a, v)
	} else {
		c.Bus.Write32(a, v)
	}
	if !pre {
		c.R[rn] = base + off
	} else if wb {
		c.R[rn] = a
	}
}

func armHalf(c *CPU, op uint32) {
	rn, rd := op>>16&15, op>>12&15
	var off uint32
	if op&(1<<22) != 0 {
		off = op>>4&0xf0 | op&15
	} else {
		off = c.R[op&15]
	}
	pre, up, wb, load := op&(1<<24) != 0, op&(1<<23) != 0, op&(1<<21) != 0, op&(1<<20) != 0
	if !up {
		off = -off
	}
	base := c.R[rn]
	a := base
	if pre {
		a += off
	}
	sh := op >> 5 & 3
	c.Cycles++
	writeback := func() {
		if !pre {
			c.R[rn] = base + off
		} else if wb {
			c.R[rn] = a
		}
	}
	if !load && sh != 1 { // LDRD / STRD (ARMv5TE)
		if c.Arch < V5TE {
			c.undefined(op)
			return
		}
		if sh == 2 {
			lo, hi := c.Bus.Read32(a), c.Bus.Read32(a+4)
			writeback()
			c.R[rd&^1] = lo
			if rd|1 == 15 {
				c.loadPC(hi)
			} else {
				c.R[rd|1] = hi
			}
		} else {
			c.Bus.Write32(a, c.R[rd&^1])
			c.Bus.Write32(a+4, c.R[rd|1])
			writeback()
		}
		c.Cycles++
		return
	}
	if !load { // STRH
		v := c.R[rd]
		if rd == 15 {
			v += 4
		}
		c.Bus.Write16(a, v)
		writeback()
		return
	}
	var v uint32
	switch sh {
	case 1: // LDRH
		v = c.Bus.Read16(a)
		if c.Arch == V4T && a&1 != 0 {
			v = bits.RotateLeft32(v, -8)
		}
	case 2: // LDRSB
		v = uint32(int32(int8(c.Bus.Read8(a))))
	case 3: // LDRSH
		if c.Arch == V4T && a&1 != 0 {
			v = uint32(int32(int8(c.Bus.Read8(a))))
		} else {
			v = uint32(int32(int16(c.Bus.Read16(a))))
		}
	}
	writeback()
	if rd == 15 {
		c.loadPC(v)
	} else {
		c.R[rd] = v
	}
}

func armBlock(c *CPU, op uint32) {
	rn := op >> 16 & 15
	list := op & 0xffff
	pre, up, sbit, wb, load := op&(1<<24) != 0, op&(1<<23) != 0, op&(1<<22) != 0, op&(1<<21) != 0, op&(1<<20) != 0
	n := uint32(bits.OnesCount32(list))
	base := c.R[rn]
	empty := list == 0
	if empty {
		n = 16
		if c.Arch == V4T {
			list = 1 << 15
		}
	}
	// lowest address first, whatever the direction
	var start, end uint32
	if up {
		start, end = base, base+4*n
		if pre {
			start += 4
		}
	} else {
		start, end = base-4*n, base-4*n
		if !pre {
			start += 4
		}
	}
	c.Cycles += int64(n)
	userBank := sbit && (!load || list&(1<<15) == 0)
	a := start
	if load {
		for i := 0; i < 15; i++ {
			if list&(1<<i) != 0 {
				v := c.Bus.Read32(a)
				if userBank {
					c.setUserReg(i, v)
				} else {
					c.R[i] = v
				}
				a += 4
			}
		}
		if wb {
			// ARMv4 never writes back a base that's in the list; ARMv5 does
			// when it's the only register or not the last one.
			inList := list&(1<<rn) != 0
			if !inList || c.Arch >= V5TE && (list == 1<<rn || list>>(rn+1) != 0) {
				c.R[rn] = end
			}
		}
		if list&(1<<15) != 0 {
			v := c.Bus.Read32(a)
			if sbit && c.hasSPSR() {
				c.SetCPSR(c.SPSR)
				c.SetPC(v)
				c.Cycles += 2
			} else {
				c.loadPC(v)
			}
		}
		return
	}
	first := true
	for i := 0; i < 16; i++ {
		if list&(1<<i) != 0 {
			var v uint32
			if userBank {
				v = c.userReg(i)
			} else {
				v = c.R[i]
			}
			if i == 15 {
				v += 4
			}
			if i == int(rn) && wb && !first && c.Arch == V4T {
				v = end // ARMv4 stores the new base unless it's first
			}
			c.Bus.Write32(a, v)
			a += 4
			first = false
		}
	}
	if wb {
		c.R[rn] = end
	}
}

// ---------------- coprocessors ----------------

func armLDC(c *CPU, op uint32) {
	if c.Arch < V6K || !isVFP(op) {
		c.undefined(op)
		return
	}
	if op&0x0fe00000 == 0x0c400000 { // MCRR / MRRC
		c.vfpTwoReg(op)
		return
	}
	c.vfpLoadStore(op)
}

func armCDP(c *CPU, op uint32) {
	if c.Arch < V6K || !isVFP(op) {
		c.undefined(op)
		return
	}
	c.vfpDataProc(op)
}

func armCoproc(c *CPU, op uint32) {
	cp := op >> 8 & 15
	if c.Arch >= V6K && (cp == 10 || cp == 11) {
		c.vfpTransfer(op)
		return
	}
	if cp != 15 || c.CP15 == nil {
		c.undefined(op)
		return
	}
	op1, cn, cm, op2 := op>>21&7, op>>16&15, op&15, op>>5&7
	rd := op >> 12 & 15
	if op&(1<<20) != 0 {
		v := c.CP15.MRC(op1, cn, cm, op2)
		if rd == 15 {
			c.CPSR = c.CPSR&0x0fffffff | v&0xf0000000
		} else {
			c.R[rd] = v
		}
	} else {
		v := c.R[rd]
		if rd == 15 {
			v += 4
		}
		c.CP15.MCR(op1, cn, cm, op2, v)
	}
	c.Cycles++
}
