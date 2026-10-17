package sh2

// The instructions. While an instruction runs, c.PC is its address + 2, so
// the "PC" of PC-relative addressing (the address + 4) is c.pcRel().

type opFunc func(c *CPU, op uint16)

// opTable maps every 16-bit opcode to its handler; isBranch marks the
// instructions not allowed in a delay slot.
var (
	opTable  [0x10000]opFunc
	isBranch [0x10000]bool
)

func rn(op uint16) int { return int(op>>8) & 15 }
func rm(op uint16) int { return int(op>>4) & 15 }

func sx8(v uint32) uint32  { return uint32(int32(int8(v))) }
func sx16(v uint32) uint32 { return uint32(int32(int16(v))) }

func (c *CPU) setT(b bool) {
	if b {
		c.SR |= srT
	} else {
		c.SR &^= srT
	}
}

func (c *CPU) t() uint32 { return c.SR & srT }

func init() {
	for i := range opTable {
		op := uint16(i)
		opTable[i] = decode(op)
		switch {
		case op&0xf0ff == 0x0003, op&0xf0ff == 0x0023, // BSRF, BRAF
			op == 0x000b, op == 0x002b, // RTS, RTE
			op&0xf0ff == 0x400b, op&0xf0ff == 0x402b, // JSR, JMP
			op&0xff00 == 0x8900, op&0xff00 == 0x8b00, op&0xff00 == 0x8d00, op&0xff00 == 0x8f00, // BT BF BT/S BF/S
			op&0xf000 == 0xa000, op&0xf000 == 0xb000, // BRA, BSR
			op&0xff00 == 0xc300: // TRAPA
			isBranch[i] = true
		}
	}
}

// decode picks the handler of one opcode.
func decode(op uint16) opFunc {
	switch op >> 12 {
	case 0:
		switch op & 0xf {
		case 2:
			switch op & 0xf0 {
			case 0x00:
				return opSTCSR
			case 0x10:
				return opSTCGBR
			case 0x20:
				return opSTCVBR
			}
		case 3:
			switch op & 0xf0 {
			case 0x00:
				return opBSRF
			case 0x20:
				return opBRAF
			}
		case 4:
			return opMOVBS0
		case 5:
			return opMOVWS0
		case 6:
			return opMOVLS0
		case 7:
			return opMULL
		case 8:
			switch op & 0xfff0 {
			case 0x0000:
				return opCLRT
			case 0x0010:
				return opSETT
			case 0x0020:
				return opCLRMAC
			}
		case 9:
			switch {
			case op == 0x0009:
				return opNOP
			case op == 0x0019:
				return opDIV0U
			case op&0xf0 == 0x20:
				return opMOVT
			}
		case 0xa:
			switch op & 0xf0 {
			case 0x00:
				return opSTSMACH
			case 0x10:
				return opSTSMACL
			case 0x20:
				return opSTSPR
			}
		case 0xb:
			switch op {
			case 0x000b:
				return opRTS
			case 0x001b:
				return opSLEEP
			case 0x002b:
				return opRTE
			}
		case 0xc:
			return opMOVBL0
		case 0xd:
			return opMOVWL0
		case 0xe:
			return opMOVLL0
		case 0xf:
			return opMACL
		}
	case 1:
		return opMOVLS4
	case 2:
		switch op & 0xf {
		case 0:
			return opMOVBS
		case 1:
			return opMOVWS
		case 2:
			return opMOVLS
		case 4:
			return opMOVBM
		case 5:
			return opMOVWM
		case 6:
			return opMOVLM
		case 7:
			return opDIV0S
		case 8:
			return opTST
		case 9:
			return opAND
		case 0xa:
			return opXOR
		case 0xb:
			return opOR
		case 0xc:
			return opCMPSTR
		case 0xd:
			return opXTRCT
		case 0xe:
			return opMULU
		case 0xf:
			return opMULS
		}
	case 3:
		switch op & 0xf {
		case 0:
			return opCMPEQ
		case 2:
			return opCMPHS
		case 3:
			return opCMPGE
		case 4:
			return opDIV1
		case 5:
			return opDMULU
		case 6:
			return opCMPHI
		case 7:
			return opCMPGT
		case 8:
			return opSUB
		case 0xa:
			return opSUBC
		case 0xb:
			return opSUBV
		case 0xc:
			return opADD
		case 0xd:
			return opDMULS
		case 0xe:
			return opADDC
		case 0xf:
			return opADDV
		}
	case 4:
		if op&0xf == 0xf {
			return opMACW
		}
		switch op & 0xff {
		case 0x00:
			return opSHLL
		case 0x01:
			return opSHLR
		case 0x02:
			return opSTSMMACH
		case 0x03:
			return opSTCMSR
		case 0x04:
			return opROTL
		case 0x05:
			return opROTR
		case 0x06:
			return opLDSMMACH
		case 0x07:
			return opLDCMSR
		case 0x08:
			return opSHLL2
		case 0x09:
			return opSHLR2
		case 0x0a:
			return opLDSMACH
		case 0x0b:
			return opJSR
		case 0x0e:
			return opLDCSR
		case 0x10:
			return opDT
		case 0x11:
			return opCMPPZ
		case 0x12:
			return opSTSMMACL
		case 0x13:
			return opSTCMGBR
		case 0x15:
			return opCMPPL
		case 0x16:
			return opLDSMMACL
		case 0x17:
			return opLDCMGBR
		case 0x18:
			return opSHLL8
		case 0x19:
			return opSHLR8
		case 0x1a:
			return opLDSMACL
		case 0x1b:
			return opTAS
		case 0x1e:
			return opLDCGBR
		case 0x20:
			return opSHAL
		case 0x21:
			return opSHAR
		case 0x22:
			return opSTSMPR
		case 0x23:
			return opSTCMVBR
		case 0x24:
			return opROTCL
		case 0x25:
			return opROTCR
		case 0x26:
			return opLDSMPR
		case 0x27:
			return opLDCMVBR
		case 0x28:
			return opSHLL16
		case 0x29:
			return opSHLR16
		case 0x2a:
			return opLDSPR
		case 0x2b:
			return opJMP
		case 0x2e:
			return opLDCVBR
		}
	case 5:
		return opMOVLL4
	case 6:
		switch op & 0xf {
		case 0:
			return opMOVBL
		case 1:
			return opMOVWL
		case 2:
			return opMOVLL
		case 3:
			return opMOV
		case 4:
			return opMOVBP
		case 5:
			return opMOVWP
		case 6:
			return opMOVLP
		case 7:
			return opNOT
		case 8:
			return opSWAPB
		case 9:
			return opSWAPW
		case 0xa:
			return opNEGC
		case 0xb:
			return opNEG
		case 0xc:
			return opEXTUB
		case 0xd:
			return opEXTUW
		case 0xe:
			return opEXTSB
		case 0xf:
			return opEXTSW
		}
	case 7:
		return opADDI
	case 8:
		switch op >> 8 & 0xf {
		case 0x0:
			return opMOVBS4
		case 0x1:
			return opMOVWS4
		case 0x4:
			return opMOVBL4
		case 0x5:
			return opMOVWL4
		case 0x8:
			return opCMPIM
		case 0x9:
			return opBT
		case 0xb:
			return opBF
		case 0xd:
			return opBTS
		case 0xf:
			return opBFS
		}
	case 9:
		return opMOVWI
	case 0xa:
		return opBRA
	case 0xb:
		return opBSR
	case 0xc:
		switch op >> 8 & 0xf {
		case 0x0:
			return opMOVBSG
		case 0x1:
			return opMOVWSG
		case 0x2:
			return opMOVLSG
		case 0x3:
			return opTRAPA
		case 0x4:
			return opMOVBLG
		case 0x5:
			return opMOVWLG
		case 0x6:
			return opMOVLLG
		case 0x7:
			return opMOVA
		case 0x8:
			return opTSTI
		case 0x9:
			return opANDI
		case 0xa:
			return opXORI
		case 0xb:
			return opORI
		case 0xc:
			return opTSTM
		case 0xd:
			return opANDM
		case 0xe:
			return opXORM
		case 0xf:
			return opORM
		}
	case 0xd:
		return opMOVLI
	case 0xe:
		return opMOVI
	}
	return opIllegal
}

// ---------------------------------------------------------------- data moves

func opMOV(c *CPU, op uint16)  { c.R[rn(op)] = c.R[rm(op)] }
func opMOVI(c *CPU, op uint16) { c.R[rn(op)] = sx8(uint32(op)) }
func opMOVWI(c *CPU, op uint16) {
	c.R[rn(op)] = sx16(c.read16(c.pcRel() + uint32(op&0xff)*2))
}
func opMOVLI(c *CPU, op uint16) {
	c.R[rn(op)] = c.read32(c.pcRel()&^3 + uint32(op&0xff)*4)
}
func opMOVA(c *CPU, op uint16) { c.R[0] = c.pcRel()&^3 + uint32(op&0xff)*4 }

func opMOVBS(c *CPU, op uint16) { c.write8(c.R[rn(op)], c.R[rm(op)]) }
func opMOVWS(c *CPU, op uint16) { c.write16(c.R[rn(op)], c.R[rm(op)]) }
func opMOVLS(c *CPU, op uint16) { c.write32(c.R[rn(op)], c.R[rm(op)]) }
func opMOVBL(c *CPU, op uint16) { c.R[rn(op)] = sx8(c.read8(c.R[rm(op)])) }
func opMOVWL(c *CPU, op uint16) { c.R[rn(op)] = sx16(c.read16(c.R[rm(op)])) }
func opMOVLL(c *CPU, op uint16) { c.R[rn(op)] = c.read32(c.R[rm(op)]) }

// @-Rn: the value is read before Rn changes (MOV Rn,@-Rn stores the old Rn).
func opMOVBM(c *CPU, op uint16) {
	n, v := rn(op), c.R[rm(op)]
	c.R[n]--
	c.write8(c.R[n], v)
}
func opMOVWM(c *CPU, op uint16) {
	n, v := rn(op), c.R[rm(op)]
	c.R[n] -= 2
	c.write16(c.R[n], v)
}
func opMOVLM(c *CPU, op uint16) {
	n, v := rn(op), c.R[rm(op)]
	c.R[n] -= 4
	c.write32(c.R[n], v)
}

// @Rm+: when n == m the loaded value wins.
func opMOVBP(c *CPU, op uint16) {
	n, m := rn(op), rm(op)
	v := sx8(c.read8(c.R[m]))
	c.R[m]++
	c.R[n] = v
}
func opMOVWP(c *CPU, op uint16) {
	n, m := rn(op), rm(op)
	v := sx16(c.read16(c.R[m]))
	c.R[m] += 2
	c.R[n] = v
}
func opMOVLP(c *CPU, op uint16) {
	n, m := rn(op), rm(op)
	v := c.read32(c.R[m])
	c.R[m] += 4
	c.R[n] = v
}

func opMOVBS0(c *CPU, op uint16) { c.write8(c.R[rn(op)]+c.R[0], c.R[rm(op)]) }
func opMOVWS0(c *CPU, op uint16) { c.write16(c.R[rn(op)]+c.R[0], c.R[rm(op)]) }
func opMOVLS0(c *CPU, op uint16) { c.write32(c.R[rn(op)]+c.R[0], c.R[rm(op)]) }
func opMOVBL0(c *CPU, op uint16) { c.R[rn(op)] = sx8(c.read8(c.R[rm(op)] + c.R[0])) }
func opMOVWL0(c *CPU, op uint16) { c.R[rn(op)] = sx16(c.read16(c.R[rm(op)] + c.R[0])) }
func opMOVLL0(c *CPU, op uint16) { c.R[rn(op)] = c.read32(c.R[rm(op)] + c.R[0]) }

func opMOVLS4(c *CPU, op uint16) { c.write32(c.R[rn(op)]+uint32(op&15)*4, c.R[rm(op)]) }
func opMOVLL4(c *CPU, op uint16) { c.R[rn(op)] = c.read32(c.R[rm(op)] + uint32(op&15)*4) }
func opMOVBS4(c *CPU, op uint16) { c.write8(c.R[rm(op)]+uint32(op&15), c.R[0]) }
func opMOVWS4(c *CPU, op uint16) { c.write16(c.R[rm(op)]+uint32(op&15)*2, c.R[0]) }
func opMOVBL4(c *CPU, op uint16) { c.R[0] = sx8(c.read8(c.R[rm(op)] + uint32(op&15))) }
func opMOVWL4(c *CPU, op uint16) { c.R[0] = sx16(c.read16(c.R[rm(op)] + uint32(op&15)*2)) }

func opMOVBSG(c *CPU, op uint16) { c.write8(c.GBR+uint32(op&0xff), c.R[0]) }
func opMOVWSG(c *CPU, op uint16) { c.write16(c.GBR+uint32(op&0xff)*2, c.R[0]) }
func opMOVLSG(c *CPU, op uint16) { c.write32(c.GBR+uint32(op&0xff)*4, c.R[0]) }
func opMOVBLG(c *CPU, op uint16) { c.R[0] = sx8(c.read8(c.GBR + uint32(op&0xff))) }
func opMOVWLG(c *CPU, op uint16) { c.R[0] = sx16(c.read16(c.GBR + uint32(op&0xff)*2)) }
func opMOVLLG(c *CPU, op uint16) { c.R[0] = c.read32(c.GBR + uint32(op&0xff)*4) }

func opMOVT(c *CPU, op uint16) { c.R[rn(op)] = c.t() }

func opSWAPB(c *CPU, op uint16) {
	v := c.R[rm(op)]
	c.R[rn(op)] = v&0xffff0000 | (v&0xff)<<8 | (v>>8)&0xff
}
func opSWAPW(c *CPU, op uint16) {
	v := c.R[rm(op)]
	c.R[rn(op)] = v<<16 | v>>16
}
func opXTRCT(c *CPU, op uint16) {
	n := rn(op)
	c.R[n] = c.R[rm(op)]<<16 | c.R[n]>>16
}

// ---------------------------------------------------------------- arithmetic

func opADD(c *CPU, op uint16)  { c.R[rn(op)] += c.R[rm(op)] }
func opADDI(c *CPU, op uint16) { c.R[rn(op)] += sx8(uint32(op)) }
func opADDC(c *CPU, op uint16) {
	n := rn(op)
	a, b := c.R[n], c.R[rm(op)]
	s := a + b
	r := s + c.t()
	c.R[n] = r
	c.setT(s < a || r < s)
}
func opADDV(c *CPU, op uint16) {
	n := rn(op)
	a, b := c.R[n], c.R[rm(op)]
	r := a + b
	c.R[n] = r
	c.setT(int32((a^r)&(b^r)) < 0)
}
func opSUB(c *CPU, op uint16) { c.R[rn(op)] -= c.R[rm(op)] }
func opSUBC(c *CPU, op uint16) {
	n := rn(op)
	a, b := c.R[n], c.R[rm(op)]
	d := a - b
	r := d - c.t()
	c.R[n] = r
	c.setT(a < b || d < r)
}
func opSUBV(c *CPU, op uint16) {
	n := rn(op)
	a, b := c.R[n], c.R[rm(op)]
	r := a - b
	c.R[n] = r
	c.setT(int32((a^b)&(a^r)) < 0)
}
func opNEG(c *CPU, op uint16) { c.R[rn(op)] = -c.R[rm(op)] }
func opNEGC(c *CPU, op uint16) {
	m := c.R[rm(op)]
	t := -m
	r := t - c.t()
	c.R[rn(op)] = r
	c.setT(t != 0 || r > t)
}

func opCMPEQ(c *CPU, op uint16) { c.setT(c.R[rn(op)] == c.R[rm(op)]) }
func opCMPHS(c *CPU, op uint16) { c.setT(c.R[rn(op)] >= c.R[rm(op)]) }
func opCMPGE(c *CPU, op uint16) { c.setT(int32(c.R[rn(op)]) >= int32(c.R[rm(op)])) }
func opCMPHI(c *CPU, op uint16) { c.setT(c.R[rn(op)] > c.R[rm(op)]) }
func opCMPGT(c *CPU, op uint16) { c.setT(int32(c.R[rn(op)]) > int32(c.R[rm(op)])) }
func opCMPPZ(c *CPU, op uint16) { c.setT(int32(c.R[rn(op)]) >= 0) }
func opCMPPL(c *CPU, op uint16) { c.setT(int32(c.R[rn(op)]) > 0) }
func opCMPIM(c *CPU, op uint16) { c.setT(c.R[0] == sx8(uint32(op))) }
func opCMPSTR(c *CPU, op uint16) {
	x := c.R[rn(op)] ^ c.R[rm(op)]
	c.setT(x&0xff000000 == 0 || x&0xff0000 == 0 || x&0xff00 == 0 || x&0xff == 0)
}

func opDT(c *CPU, op uint16) {
	n := rn(op)
	c.R[n]--
	c.setT(c.R[n] == 0)
}

func opEXTSB(c *CPU, op uint16) { c.R[rn(op)] = sx8(c.R[rm(op)]) }
func opEXTSW(c *CPU, op uint16) { c.R[rn(op)] = sx16(c.R[rm(op)]) }
func opEXTUB(c *CPU, op uint16) { c.R[rn(op)] = c.R[rm(op)] & 0xff }
func opEXTUW(c *CPU, op uint16) { c.R[rn(op)] = c.R[rm(op)] & 0xffff }

// ---------------------------------------------------------------- multiply / divide

func opMULL(c *CPU, op uint16) {
	c.MACL = c.R[rn(op)] * c.R[rm(op)]
	c.Cycles++
}
func opMULS(c *CPU, op uint16) {
	c.MACL = uint32(int32(int16(c.R[rn(op)])) * int32(int16(c.R[rm(op)])))
}
func opMULU(c *CPU, op uint16) {
	c.MACL = (c.R[rn(op)] & 0xffff) * (c.R[rm(op)] & 0xffff)
}
func opDMULS(c *CPU, op uint16) {
	r := uint64(int64(int32(c.R[rn(op)])) * int64(int32(c.R[rm(op)])))
	c.MACH, c.MACL = uint32(r>>32), uint32(r)
	c.Cycles++
}
func opDMULU(c *CPU, op uint16) {
	r := uint64(c.R[rn(op)]) * uint64(c.R[rm(op)])
	c.MACH, c.MACL = uint32(r>>32), uint32(r)
	c.Cycles++
}

func opCLRMAC(c *CPU, op uint16) { c.MACH, c.MACL = 0, 0 }

// MAC.W: MAC += @Rn * @Rm (16-bit, signed); with S set the sum saturates at
// 32 bits and MACH's bit 0 marks an overflow.
func opMACW(c *CPU, op uint16) {
	n, m := rn(op), rm(op)
	b := int64(int16(c.read16(c.R[n])))
	c.R[n] += 2
	a := int64(int16(c.read16(c.R[m])))
	c.R[m] += 2
	p := a * b
	if c.SR&srS != 0 {
		s := int64(int32(c.MACL)) + p
		switch {
		case s > 0x7fffffff:
			c.MACL, c.MACH = 0x7fffffff, c.MACH|1
		case s < -0x80000000:
			c.MACL, c.MACH = 0x80000000, c.MACH|1
		default:
			c.MACL = uint32(s)
		}
	} else {
		s := int64(uint64(c.MACH)<<32|uint64(c.MACL)) + p
		c.MACH, c.MACL = uint32(uint64(s)>>32), uint32(s)
	}
	c.Cycles++
}

// MAC.L: MAC += @Rn * @Rm (32-bit, signed); with S set the sum saturates at
// 48 bits.
func opMACL(c *CPU, op uint16) {
	n, m := rn(op), rm(op)
	b := int64(int32(c.read32(c.R[n])))
	c.R[n] += 4
	a := int64(int32(c.read32(c.R[m])))
	c.R[m] += 4
	hi, lo := mul64(a, b)
	// 128-bit add of the old MAC (sign-extended) to hi:lo
	mac := int64(uint64(c.MACH)<<32 | uint64(c.MACL))
	mhi := uint64(0)
	if mac < 0 {
		mhi = ^uint64(0)
	}
	sl := lo + uint64(mac)
	carry := uint64(0)
	if sl < lo {
		carry = 1
	}
	sh := hi + mhi + carry
	if c.SR&srS != 0 {
		max48, min48 := int64(0x00007fffffffffff), -int64(0x0000800000000000)
		neg := int64(sh) < 0
		switch {
		case !neg && (sh != 0 || sl > uint64(max48)):
			sl = uint64(max48)
		case neg && (sh != ^uint64(0) || int64(sl) >= 0 || int64(sl) < min48):
			sl = uint64(min48)
		}
	}
	c.MACH, c.MACL = uint32(sl>>32), uint32(sl)
	c.Cycles += 2
}

// mul64 is the signed 128-bit product of a and b as hi:lo.
func mul64(a, b int64) (hi, lo uint64) {
	neg := (a < 0) != (b < 0)
	ua, ub := uint64(a), uint64(b)
	if a < 0 {
		ua = uint64(-a)
	}
	if b < 0 {
		ub = uint64(-b)
	}
	a0, a1 := ua&0xffffffff, ua>>32
	b0, b1 := ub&0xffffffff, ub>>32
	p00 := a0 * b0
	p01 := a0 * b1
	p10 := a1 * b0
	p11 := a1 * b1
	mid := p00>>32 + p01&0xffffffff + p10&0xffffffff
	lo = p00&0xffffffff | mid<<32
	hi = p11 + p01>>32 + p10>>32 + mid>>32
	if neg {
		lo = ^lo + 1
		hi = ^hi
		if lo == 0 {
			hi++
		}
	}
	return
}

func opDIV0U(c *CPU, op uint16) { c.SR &^= srM | srQ | srT }
func opDIV0S(c *CPU, op uint16) {
	q := c.R[rn(op)] >> 31
	m := c.R[rm(op)] >> 31
	c.SR = c.SR&^(srM|srQ|srT) | q<<8 | m<<9 | (q ^ m)
}

// DIV1 is one step of the non-restoring division.
func opDIV1(c *CPU, op uint16) {
	n := rn(op)
	divisor := c.R[rm(op)]
	oldQ := c.SR >> 8 & 1
	m := c.SR >> 9 & 1
	q := c.R[n] >> 31
	r := c.R[n]<<1 | c.t()
	var carry uint32
	if oldQ == m {
		res := r - divisor
		if res > r {
			carry = 1
		}
		r = res
	} else {
		res := r + divisor
		if res < r {
			carry = 1
		}
		r = res
	}
	// the new Q is the 33rd bit of the partial remainder
	q = q ^ carry ^ m
	c.R[n] = r
	c.SR = c.SR&^(srQ|srT) | q<<8
	if q == m {
		c.SR |= srT
	}
}

// ---------------------------------------------------------------- logic

func opAND(c *CPU, op uint16)  { c.R[rn(op)] &= c.R[rm(op)] }
func opOR(c *CPU, op uint16)   { c.R[rn(op)] |= c.R[rm(op)] }
func opXOR(c *CPU, op uint16)  { c.R[rn(op)] ^= c.R[rm(op)] }
func opNOT(c *CPU, op uint16)  { c.R[rn(op)] = ^c.R[rm(op)] }
func opTST(c *CPU, op uint16)  { c.setT(c.R[rn(op)]&c.R[rm(op)] == 0) }
func opANDI(c *CPU, op uint16) { c.R[0] &= uint32(op & 0xff) }
func opORI(c *CPU, op uint16)  { c.R[0] |= uint32(op & 0xff) }
func opXORI(c *CPU, op uint16) { c.R[0] ^= uint32(op & 0xff) }
func opTSTI(c *CPU, op uint16) { c.setT(c.R[0]&uint32(op&0xff) == 0) }

func opANDM(c *CPU, op uint16) {
	a := c.GBR + c.R[0]
	c.write8(a, c.read8(a)&uint32(op&0xff))
	c.Cycles += 2
}
func opORM(c *CPU, op uint16) {
	a := c.GBR + c.R[0]
	c.write8(a, c.read8(a)|uint32(op&0xff))
	c.Cycles += 2
}
func opXORM(c *CPU, op uint16) {
	a := c.GBR + c.R[0]
	c.write8(a, c.read8(a)^uint32(op&0xff))
	c.Cycles += 2
}
func opTSTM(c *CPU, op uint16) {
	c.setT(c.read8(c.GBR+c.R[0])&uint32(op&0xff) == 0)
	c.Cycles += 2
}

func opTAS(c *CPU, op uint16) {
	a := c.R[rn(op)]
	v := c.read8(a)
	c.setT(v == 0)
	c.write8(a, v|0x80)
	c.Cycles += 3
}

// ---------------------------------------------------------------- shifts

func opSHLL(c *CPU, op uint16) {
	n := rn(op)
	c.setT(c.R[n]&0x80000000 != 0)
	c.R[n] <<= 1
}
func opSHAL(c *CPU, op uint16) { opSHLL(c, op) }
func opSHLR(c *CPU, op uint16) {
	n := rn(op)
	c.setT(c.R[n]&1 != 0)
	c.R[n] >>= 1
}
func opSHAR(c *CPU, op uint16) {
	n := rn(op)
	c.setT(c.R[n]&1 != 0)
	c.R[n] = uint32(int32(c.R[n]) >> 1)
}
func opROTL(c *CPU, op uint16) {
	n := rn(op)
	t := c.R[n] >> 31
	c.R[n] = c.R[n]<<1 | t
	c.setT(t != 0)
}
func opROTR(c *CPU, op uint16) {
	n := rn(op)
	t := c.R[n] & 1
	c.R[n] = c.R[n]>>1 | t<<31
	c.setT(t != 0)
}
func opROTCL(c *CPU, op uint16) {
	n := rn(op)
	t := c.R[n] >> 31
	c.R[n] = c.R[n]<<1 | c.t()
	c.setT(t != 0)
}
func opROTCR(c *CPU, op uint16) {
	n := rn(op)
	t := c.R[n] & 1
	c.R[n] = c.R[n]>>1 | c.t()<<31
	c.setT(t != 0)
}
func opSHLL2(c *CPU, op uint16)  { c.R[rn(op)] <<= 2 }
func opSHLR2(c *CPU, op uint16)  { c.R[rn(op)] >>= 2 }
func opSHLL8(c *CPU, op uint16)  { c.R[rn(op)] <<= 8 }
func opSHLR8(c *CPU, op uint16)  { c.R[rn(op)] >>= 8 }
func opSHLL16(c *CPU, op uint16) { c.R[rn(op)] <<= 16 }
func opSHLR16(c *CPU, op uint16) { c.R[rn(op)] >>= 16 }

// ---------------------------------------------------------------- branches

func disp8(op uint16) uint32  { return sx8(uint32(op)) * 2 }
func disp12(op uint16) uint32 { return uint32(int32(int16(op<<4))>>4) * 2 }

func opBT(c *CPU, op uint16) {
	if c.SR&srT != 0 {
		c.PC = c.PC + 2 + disp8(op)
		c.Cycles += 2
	}
}
func opBF(c *CPU, op uint16) {
	if c.SR&srT == 0 {
		c.PC = c.PC + 2 + disp8(op)
		c.Cycles += 2
	}
}
func opBTS(c *CPU, op uint16) {
	if c.SR&srT != 0 {
		c.Cycles++
		c.delay(c.PC + 2 + disp8(op))
	}
}
func opBFS(c *CPU, op uint16) {
	if c.SR&srT == 0 {
		c.Cycles++
		c.delay(c.PC + 2 + disp8(op))
	}
}
func opBRA(c *CPU, op uint16) {
	c.Cycles++
	c.delay(c.PC + 2 + disp12(op))
}
func opBSR(c *CPU, op uint16) {
	c.PR = c.PC + 2
	c.Cycles++
	c.delay(c.PC + 2 + disp12(op))
}
func opBRAF(c *CPU, op uint16) {
	c.Cycles++
	c.delay(c.PC + 2 + c.R[rn(op)])
}
func opBSRF(c *CPU, op uint16) {
	c.PR = c.PC + 2
	c.Cycles++
	c.delay(c.PC + 2 + c.R[rn(op)])
}
func opJMP(c *CPU, op uint16) {
	c.Cycles++
	c.delay(c.R[rn(op)])
}
func opJSR(c *CPU, op uint16) {
	t := c.R[rn(op)]
	c.PR = c.PC + 2
	c.Cycles++
	c.delay(t)
}
func opRTS(c *CPU, op uint16) {
	c.Cycles++
	c.delay(c.PR)
}
func opRTE(c *CPU, op uint16) {
	t := c.read32(c.R[15])
	c.R[15] += 4
	c.SR = c.read32(c.R[15]) & srMask
	c.R[15] += 4
	c.Cycles += 3
	c.srChanged()
	c.delay(t)
}

func opTRAPA(c *CPU, op uint16) {
	c.exception(uint32(op&0xff), c.PC)
	c.Cycles += 7
}

func opIllegal(c *CPU, op uint16) {
	if c.inSlot {
		// in a delay slot: the branch's own address is pushed
		c.slotFault = true
		c.exception(6, c.PC-4)
		return
	}
	c.exception(4, c.PC-2)
	c.Cycles += 7
}

// ---------------------------------------------------------------- system registers

func opCLRT(c *CPU, op uint16)  { c.SR &^= srT }
func opSETT(c *CPU, op uint16)  { c.SR |= srT }
func opNOP(c *CPU, op uint16)   {}
func opSLEEP(c *CPU, op uint16) { c.sleeping = true; c.Cycles += 2; c.irqCheck = true }

func opSTCSR(c *CPU, op uint16)     { c.R[rn(op)] = c.SR }
func opSTCGBR(c *CPU, op uint16)    { c.R[rn(op)] = c.GBR }
func opSTCVBR(c *CPU, op uint16)    { c.R[rn(op)] = c.VBR }
func opSTSMACH(c *CPU, op uint16)   { c.R[rn(op)] = c.MACH }
func opSTSMACL(c *CPU, op uint16)   { c.R[rn(op)] = c.MACL }
func opSTSPR(c *CPU, op uint16)     { c.R[rn(op)] = c.PR }
func opLDCSR(c *CPU, op uint16)     { c.SR = c.R[rn(op)] & srMask; c.srChanged() }
func opLDCGBR(c *CPU, op uint16)    { c.GBR = c.R[rn(op)] }
func opLDCVBR(c *CPU, op uint16)    { c.VBR = c.R[rn(op)] }
func opLDSMACH(c *CPU, op uint16)   { c.MACH = c.R[rn(op)] }
func opLDSMACL(c *CPU, op uint16)   { c.MACL = c.R[rn(op)] }
func opLDSPR(c *CPU, op uint16)     { c.PR = c.R[rn(op)] }
func (c *CPU) push(n int, v uint32) { c.R[n] -= 4; c.write32(c.R[n], v) }
func (c *CPU) pop(n int) uint32 {
	v := c.read32(c.R[n])
	c.R[n] += 4
	return v
}
func opSTCMSR(c *CPU, op uint16)   { c.push(rn(op), c.SR); c.Cycles++ }
func opSTCMGBR(c *CPU, op uint16)  { c.push(rn(op), c.GBR); c.Cycles++ }
func opSTCMVBR(c *CPU, op uint16)  { c.push(rn(op), c.VBR); c.Cycles++ }
func opSTSMMACH(c *CPU, op uint16) { c.push(rn(op), c.MACH) }
func opSTSMMACL(c *CPU, op uint16) { c.push(rn(op), c.MACL) }
func opSTSMPR(c *CPU, op uint16)   { c.push(rn(op), c.PR) }
func opLDCMSR(c *CPU, op uint16) {
	c.SR = c.pop(rn(op)) & srMask
	c.Cycles += 2
	c.srChanged()
}
func opLDCMGBR(c *CPU, op uint16)  { c.GBR = c.pop(rn(op)); c.Cycles += 2 }
func opLDCMVBR(c *CPU, op uint16)  { c.VBR = c.pop(rn(op)); c.Cycles += 2 }
func opLDSMMACH(c *CPU, op uint16) { c.MACH = c.pop(rn(op)) }
func opLDSMMACL(c *CPU, op uint16) { c.MACL = c.pop(rn(op)) }
func opLDSMPR(c *CPU, op uint16)   { c.PR = c.pop(rn(op)) }
