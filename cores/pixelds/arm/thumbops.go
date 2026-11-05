package arm

import "math/bits"

// thumbTable is indexed by instruction bits 15-6.
var thumbTable [1024]func(c *CPU, op uint32)

func decodeThumb(i uint32) func(*CPU, uint32) {
	top5 := i >> 5
	switch {
	case top5 < 3:
		return thShift
	case top5 == 3:
		return thAddSub
	case top5>>2 == 1:
		return thImm8
	case i>>4 == 0x10:
		return thALU
	case i>>4 == 0x11:
		return thHiReg
	case top5 == 9:
		return thLdrPC
	case top5>>1 == 5:
		return thLdStReg
	case top5>>2 == 3:
		return thLdStImm
	case top5>>1 == 8:
		return thLdStHalf
	case top5>>1 == 9:
		return thLdStSP
	case top5>>1 == 10:
		return thAddPCSP
	case top5>>1 == 11:
		sub := i >> 2 & 0xf // bits 11-8
		switch {
		case sub == 0:
			return thAddSP
		case sub&6 == 4:
			return thPushPop
		case sub == 0xe:
			return thBKPT
		}
		return thMisc
	case top5>>1 == 12:
		return thBlock
	case top5>>1 == 13:
		if i>>2&0xf == 0xf {
			return thSWI
		}
		if i>>2&0xf == 0xe {
			return thUndef
		}
		return thCondBranch
	case top5 == 0x1c:
		return thBranch
	case top5 == 0x1d:
		return thBLXSuffix
	case top5 == 0x1e:
		return thBLPrefix
	case top5 == 0x1f:
		return thBLSuffix
	}
	return thUndef
}

func thUndef(c *CPU, op uint32) { c.undefined(op) }

func thShift(c *CPU, op uint32) {
	rd, rs := op&7, op>>3&7
	v := c.R[rs]
	amt := op >> 6 & 31
	cf := c.CPSR&FlagC != 0
	var r uint32
	switch op >> 11 & 3 {
	case 0:
		if amt == 0 {
			r = v
		} else {
			r, cf = v<<amt, v>>(32-amt)&1 != 0
		}
	case 1:
		if amt == 0 {
			r, cf = 0, v>>31 != 0
		} else {
			r, cf = v>>amt, v>>(amt-1)&1 != 0
		}
	case 2:
		if amt == 0 {
			r, cf = uint32(int32(v)>>31), v>>31 != 0
		} else {
			r, cf = uint32(int32(v)>>amt), v>>(amt-1)&1 != 0
		}
	}
	c.R[rd] = r
	c.setNZ(r)
	if cf {
		c.CPSR |= FlagC
	} else {
		c.CPSR &^= FlagC
	}
}

func thAddSub(c *CPU, op uint32) {
	rd, rs := op&7, op>>3&7
	b := op >> 6 & 7
	if op&(1<<10) == 0 {
		b = c.R[b]
	}
	var r uint32
	var cf, vf bool
	if op&(1<<9) != 0 {
		r, cf, vf = subFlags(c.R[rs], b, 1)
	} else {
		r, cf, vf = addFlags(c.R[rs], b, 0)
	}
	c.R[rd] = r
	c.setNZCV(r, cf, vf)
}

func thImm8(c *CPU, op uint32) {
	rd := op >> 8 & 7
	imm := op & 0xff
	switch op >> 11 & 3 {
	case 0:
		c.R[rd] = imm
		c.setNZ(imm)
	case 1:
		r, cf, vf := subFlags(c.R[rd], imm, 1)
		c.setNZCV(r, cf, vf)
	case 2:
		r, cf, vf := addFlags(c.R[rd], imm, 0)
		c.R[rd] = r
		c.setNZCV(r, cf, vf)
	case 3:
		r, cf, vf := subFlags(c.R[rd], imm, 1)
		c.R[rd] = r
		c.setNZCV(r, cf, vf)
	}
}

func (c *CPU) setC(b bool) {
	if b {
		c.CPSR |= FlagC
	} else {
		c.CPSR &^= FlagC
	}
}

func thALU(c *CPU, op uint32) {
	rd, rs := op&7, op>>3&7
	a, b := c.R[rd], c.R[rs]
	var r uint32
	switch op >> 6 & 15 {
	case 0: // AND
		r = a & b
	case 1: // EOR
		r = a ^ b
	case 2, 3, 4, 7: // LSL, LSR, ASR, ROR by register
		amt := b & 0xff
		c.Cycles++
		r = a
		if amt != 0 {
			switch op >> 6 & 15 {
			case 2:
				switch {
				case amt < 32:
					r = a << amt
					c.setC(a>>(32-amt)&1 != 0)
				case amt == 32:
					r = 0
					c.setC(a&1 != 0)
				default:
					r = 0
					c.setC(false)
				}
			case 3:
				switch {
				case amt < 32:
					r = a >> amt
					c.setC(a>>(amt-1)&1 != 0)
				case amt == 32:
					r = 0
					c.setC(a>>31 != 0)
				default:
					r = 0
					c.setC(false)
				}
			case 4:
				if amt >= 32 {
					r = uint32(int32(a) >> 31)
					c.setC(a>>31 != 0)
				} else {
					r = uint32(int32(a) >> amt)
					c.setC(a>>(amt-1)&1 != 0)
				}
			case 7:
				amt &= 31
				if amt == 0 {
					c.setC(a>>31 != 0)
				} else {
					r = bits.RotateLeft32(a, -int(amt))
					c.setC(a>>(amt-1)&1 != 0)
				}
			}
		}
	case 5: // ADC
		var cf, vf bool
		r, cf, vf = addFlags(a, b, c.CPSR>>29&1)
		c.R[rd] = r
		c.setNZCV(r, cf, vf)
		return
	case 6: // SBC
		var cf, vf bool
		r, cf, vf = subFlags(a, b, c.CPSR>>29&1)
		c.R[rd] = r
		c.setNZCV(r, cf, vf)
		return
	case 8: // TST
		c.setNZ(a & b)
		return
	case 9: // NEG
		var cf, vf bool
		r, cf, vf = subFlags(0, b, 1)
		c.R[rd] = r
		c.setNZCV(r, cf, vf)
		return
	case 10: // CMP
		r, cf, vf := subFlags(a, b, 1)
		c.setNZCV(r, cf, vf)
		return
	case 11: // CMN
		r, cf, vf := addFlags(a, b, 0)
		c.setNZCV(r, cf, vf)
		return
	case 12: // ORR
		r = a | b
	case 13: // MUL
		r = a * b
		c.Cycles += mulCycles(a)
	case 14: // BIC
		r = a &^ b
	case 15: // MVN
		r = ^b
	}
	c.R[rd] = r
	c.setNZ(r)
}

func thHiReg(c *CPU, op uint32) {
	rd := op&7 | op>>4&8
	rs := op >> 3 & 15
	v := c.R[rs]
	switch op >> 8 & 3 {
	case 0: // ADD
		r := c.R[rd] + v
		if rd == 15 {
			c.SetPC(r)
			c.Cycles += 2
		} else {
			c.R[rd] = r
		}
	case 1: // CMP
		r, cf, vf := subFlags(c.R[rd], v, 1)
		c.setNZCV(r, cf, vf)
	case 2: // MOV
		if rd == 15 {
			c.SetPC(v)
			c.Cycles += 2
		} else {
			c.R[rd] = v
		}
	case 3: // BX / BLX
		if op&(1<<7) != 0 {
			if c.Arch < V5TE {
				c.undefined(op)
				return
			}
			c.R[14] = c.next | 1
		}
		c.JumpX(v)
		c.Cycles += 2
	}
}

func thLdrPC(c *CPU, op uint32) {
	a := c.R[15]&^3 + (op&0xff)<<2
	c.R[op>>8&7] = c.Bus.Read32(a)
	c.Cycles++
}

func thLdStReg(c *CPU, op uint32) {
	rd := op & 7
	a := c.R[op>>3&7] + c.R[op>>6&7]
	c.Cycles++
	switch op >> 9 & 7 {
	case 0:
		c.Bus.Write32(a, c.R[rd])
	case 1:
		c.Bus.Write16(a, c.R[rd])
	case 2:
		c.Bus.Write8(a, c.R[rd])
	case 3:
		c.R[rd] = uint32(int32(int8(c.Bus.Read8(a))))
	case 4:
		c.R[rd] = c.load32(a)
	case 5:
		c.R[rd] = c.loadHalf(a)
	case 6:
		c.R[rd] = c.Bus.Read8(a)
	case 7:
		if c.Arch == V4T && a&1 != 0 {
			c.R[rd] = uint32(int32(int8(c.Bus.Read8(a))))
		} else {
			c.R[rd] = uint32(int32(int16(c.Bus.Read16(a))))
		}
	}
}

func (c *CPU) loadHalf(a uint32) uint32 {
	v := c.Bus.Read16(a)
	if c.Arch == V4T && a&1 != 0 {
		v = bits.RotateLeft32(v, -8)
	}
	return v
}

func thLdStImm(c *CPU, op uint32) {
	rd, rb := op&7, op>>3&7
	off := op >> 6 & 31
	c.Cycles++
	switch op >> 11 & 3 {
	case 0:
		c.Bus.Write32(c.R[rb]+off<<2, c.R[rd])
	case 1:
		c.R[rd] = c.load32(c.R[rb] + off<<2)
	case 2:
		c.Bus.Write8(c.R[rb]+off, c.R[rd])
	case 3:
		c.R[rd] = c.Bus.Read8(c.R[rb] + off)
	}
}

func thLdStHalf(c *CPU, op uint32) {
	rd := op & 7
	a := c.R[op>>3&7] + (op>>6&31)<<1
	c.Cycles++
	if op&(1<<11) != 0 {
		c.R[rd] = c.loadHalf(a)
	} else {
		c.Bus.Write16(a, c.R[rd])
	}
}

func thLdStSP(c *CPU, op uint32) {
	rd := op >> 8 & 7
	a := c.R[13] + (op&0xff)<<2
	c.Cycles++
	if op&(1<<11) != 0 {
		c.R[rd] = c.load32(a)
	} else {
		c.Bus.Write32(a, c.R[rd])
	}
}

func thAddPCSP(c *CPU, op uint32) {
	imm := (op & 0xff) << 2
	if op&(1<<11) != 0 {
		c.R[op>>8&7] = c.R[13] + imm
	} else {
		c.R[op>>8&7] = c.R[15]&^3 + imm
	}
}

func thAddSP(c *CPU, op uint32) {
	imm := (op & 0x7f) << 2
	if op&(1<<7) != 0 {
		c.R[13] -= imm
	} else {
		c.R[13] += imm
	}
}

func thPushPop(c *CPU, op uint32) {
	list := op & 0xff
	extra := op&(1<<8) != 0
	n := uint32(bits.OnesCount32(list))
	if extra {
		n++
	}
	c.Cycles += int64(n)
	if op&(1<<11) == 0 { // PUSH
		a := c.R[13] - 4*n
		c.R[13] = a
		for i := 0; i < 8; i++ {
			if list&(1<<i) != 0 {
				c.Bus.Write32(a, c.R[i])
				a += 4
			}
		}
		if extra {
			c.Bus.Write32(a, c.R[14])
		}
		return
	}
	a := c.R[13]
	for i := 0; i < 8; i++ {
		if list&(1<<i) != 0 {
			c.R[i] = c.Bus.Read32(a)
			a += 4
		}
	}
	if extra {
		v := c.Bus.Read32(a)
		a += 4
		c.R[13] = a
		c.loadPC(v)
		return
	}
	c.R[13] = a
}

func thBKPT(c *CPU, op uint32) {
	if c.Arch < V5TE {
		c.undefined(op)
		return
	}
	c.exception(VecPrefetch, ModeABT, c.next)
}

// thMisc: the ARMv6 Thumb additions (sign/zero extend, byte reverse, CPS).
func thMisc(c *CPU, op uint32) {
	if c.Arch < V6K {
		c.undefined(op)
		return
	}
	rd, rm := op&7, op>>3&7
	v := c.R[rm]
	switch op >> 6 & 0x3f {
	case 0x08: // SXTH
		c.R[rd] = uint32(int32(int16(v)))
	case 0x09: // SXTB
		c.R[rd] = uint32(int32(int8(v)))
	case 0x0a: // UXTH
		c.R[rd] = v & 0xffff
	case 0x0b: // UXTB
		c.R[rd] = v & 0xff
	case 0x28: // REV
		c.R[rd] = bits.ReverseBytes32(v)
	case 0x29: // REV16
		c.R[rd] = bits.ReverseBytes32(v)>>16 | bits.ReverseBytes32(v)<<16
	case 0x2b: // REVSH
		c.R[rd] = uint32(int32(int16(bits.ReverseBytes16(uint16(v)))))
	case 0x19: // CPS / SETEND
		if op&0x20 != 0 && op&0x10 == 0 || op&0xffe8 == 0xb660 {
			if c.CPSR&0x1f == ModeUSR {
				return
			}
			var mask uint32
			if op&4 != 0 {
				mask |= 1 << 8
			}
			if op&2 != 0 {
				mask |= FlagI
			}
			if op&1 != 0 {
				mask |= FlagF
			}
			if op&0x10 != 0 {
				c.CPSR |= mask
			} else {
				c.CPSR &^= mask
			}
		}
	default:
		c.undefined(op)
	}
}

func thBlock(c *CPU, op uint32) {
	rb := op >> 8 & 7
	list := op & 0xff
	a := c.R[rb]
	if list == 0 { // empty list: as the ARM version
		if c.Arch == V4T {
			if op&(1<<11) != 0 {
				c.loadPC(c.Bus.Read32(a))
			} else {
				c.Bus.Write32(a, c.R[15]+2)
			}
		}
		c.R[rb] = a + 0x40
		return
	}
	n := uint32(bits.OnesCount32(list))
	c.Cycles += int64(n)
	end := a + 4*n
	if op&(1<<11) != 0 { // LDMIA
		for i := 0; i < 8; i++ {
			if list&(1<<i) != 0 {
				c.R[i] = c.Bus.Read32(a)
				a += 4
			}
		}
		inList := list&(1<<rb) != 0
		if !inList || c.Arch >= V5TE && (list == 1<<rb || list>>(rb+1) != 0) {
			c.R[rb] = end
		}
		return
	}
	first := true
	for i := 0; i < 8; i++ {
		if list&(1<<i) != 0 {
			v := c.R[i]
			if uint32(i) == rb && !first && c.Arch == V4T {
				v = end
			}
			c.Bus.Write32(a, v)
			a += 4
			first = false
		}
	}
	c.R[rb] = end
}

func thCondBranch(c *CPU, op uint32) {
	cond := op >> 8 & 15
	if !condPass[cond<<4|c.CPSR>>28] {
		return
	}
	c.next = c.R[15] + uint32(int32(int8(op)))<<1
	c.Cycles += 2
}

func thSWI(c *CPU, op uint32) { c.swi(op & 0xff) }

func thBranch(c *CPU, op uint32) {
	c.next = c.R[15] + uint32(int32(op<<21)>>20)
	c.Cycles += 2
}

func thBLPrefix(c *CPU, op uint32) {
	c.R[14] = c.R[15] + uint32(int32(op<<21)>>9)
}

func thBLSuffix(c *CPU, op uint32) {
	t := c.R[14] + (op&0x7ff)<<1
	c.R[14] = c.next | 1
	c.next = t &^ 1
	c.Cycles += 2
}

func thBLXSuffix(c *CPU, op uint32) {
	if c.Arch < V5TE || op&1 != 0 {
		c.undefined(op)
		return
	}
	t := c.R[14] + (op&0x7ff)<<1
	c.R[14] = c.next | 1
	c.CPSR &^= FlagT
	c.next = t &^ 3
	c.Cycles += 2
}
