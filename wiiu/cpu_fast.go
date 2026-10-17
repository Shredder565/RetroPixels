package wiiu

// The decoded-instruction cache: each instruction is decoded once, the
// first time it runs, into the Go function that runs it. The common
// instructions get small functions of their own; the rest go to the full
// interpreter (exec).
//
// Code that changes after it has run must be invalidated (icbi,
// ICInvalidateRange, a module loaded over old code): Memory.InvalidateCode.

import "math/bits"

type opFn func(c *CPU, inst uint32)

type decoded struct {
	fn   opFn
	inst uint32
}

type codePage [pageSize / 4]decoded

// InvalidateCode forgets the decoded instructions of n bytes at addr.
func (m *Memory) InvalidateCode(addr, n uint32) {
	if n == 0 {
		return
	}
	first, last := addr>>pageBits, (addr+n-1)>>pageBits
	if last < first { // wrapped
		last = numPages - 1
	}
	for p := first; p <= last; p++ {
		m.code[p] = nil
	}
}

// decodeAt decodes the instruction at pc into the cache.
func (c *CPU) decodeAt(pc uint32) *decoded {
	m := c.Mem
	cp := m.code[pc>>pageBits]
	if cp == nil {
		cp = new(codePage)
		m.code[pc>>pageBits] = cp
	}
	d := &cp[pc&pageMask>>2]
	d.inst = m.Read32(pc)
	d.fn = decodeOp(d.inst)
	return d
}

func generic(c *CPU, inst uint32) { c.exec(inst) }

// decodeOp picks the function for an instruction.
func decodeOp(inst uint32) opFn {
	op := inst >> 26
	ra := inst >> 16 & 31
	switch op {
	case 7:
		return fMulli
	case 10:
		return fCmpli
	case 11:
		return fCmpi
	case 14:
		if ra == 0 {
			return fLi
		}
		return fAddi
	case 15:
		if ra == 0 {
			return fLis
		}
		return fAddis
	case 16:
		if inst>>21&31 == 20 && inst&3 == 0 { // branch always
			return fBAlways
		}
		return fBc
	case 18:
		if inst&3 == 0 {
			return fB
		}
		if inst&3 == 1 {
			return fBl
		}
	case 19:
		switch inst >> 1 & 0x3ff {
		case 16:
			if inst>>21&31 == 20 && inst&1 == 0 {
				return fBlr
			}
		case 528:
			if inst>>21&31 == 20 {
				return fBcctrAlways
			}
		}
	case 21:
		if inst&1 == 0 {
			return fRlwinm
		}
	case 24:
		return fOri
	case 25:
		return fOris
	case 28:
		return fAndi
	case 32:
		if ra == 0 {
			break
		}
		return fLwz
	case 33:
		return fLwzu
	case 34:
		if ra == 0 {
			break
		}
		return fLbz
	case 36:
		if ra == 0 {
			break
		}
		return fStw
	case 37:
		return fStwu
	case 38:
		if ra == 0 {
			break
		}
		return fStb
	case 40:
		if ra == 0 {
			break
		}
		return fLhz
	case 44:
		if ra == 0 {
			break
		}
		return fSth
	case 31:
		if f := decode31(inst); f != nil {
			return f
		}
	}
	return generic
}

func decode31(inst uint32) opFn {
	rc := inst&1 != 0
	oe := inst&0x400 != 0
	xo := inst >> 1 & 0x3ff
	ra := inst >> 16 & 31
	spr := inst>>16&31 | (inst>>11&31)<<5
	if rc {
		switch xo {
		case 0, 32: // cmp, cmpl have no Rc
		default:
			return nil
		}
	}
	switch xo {
	case 266:
		if !oe {
			return fAdd
		}
	case 40:
		if !oe {
			return fSubf
		}
	case 104:
		if !oe {
			return fNeg
		}
	case 235:
		if !oe {
			return fMullw
		}
	case 0:
		return fCmp
	case 32:
		return fCmpl
	case 23:
		if ra != 0 {
			return fLwzx
		}
	case 151:
		if ra != 0 {
			return fStwx
		}
	case 87:
		if ra != 0 {
			return fLbzx
		}
	case 444:
		if inst>>21&31 == inst>>11&31 {
			return fMr
		}
		return fOr
	case 28:
		return fAnd
	case 316:
		return fXor
	case 24:
		return fSlw
	case 536:
		return fSrw
	case 824:
		return fSrawi
	case 922:
		return fExtsh
	case 954:
		return fExtsb
	case 339:
		switch spr {
		case 8:
			return fMflr
		case 9:
			return fMfctr
		}
	case 467:
		switch spr {
		case 8:
			return fMtlr
		case 9:
			return fMtctr
		}
	}
	return nil
}

func simm(inst uint32) uint32 { return uint32(int32(int16(inst))) }

func fMulli(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = uint32(int32(c.GPR[inst>>16&31]) * int32(simm(inst)))
}
func fCmpli(c *CPU, inst uint32) { c.cmpU(inst>>23&7, c.GPR[inst>>16&31], inst&0xffff) }
func fCmpi(c *CPU, inst uint32)  { c.cmpS(inst>>23&7, int32(c.GPR[inst>>16&31]), int32(int16(inst))) }
func fLi(c *CPU, inst uint32)    { c.GPR[inst>>21&31] = simm(inst) }
func fAddi(c *CPU, inst uint32)  { c.GPR[inst>>21&31] = c.GPR[inst>>16&31] + simm(inst) }
func fLis(c *CPU, inst uint32)   { c.GPR[inst>>21&31] = inst << 16 }
func fAddis(c *CPU, inst uint32) { c.GPR[inst>>21&31] = c.GPR[inst>>16&31] + inst<<16 }

func fBc(c *CPU, inst uint32) {
	bo, bi := inst>>21&31, inst>>16&31
	ctx := c.Context
	if bo&4 == 0 {
		ctx.CTR--
		if (ctx.CTR != 0) == (bo&2 != 0) {
			return
		}
	}
	if bo&0x10 == 0 && (ctx.CR>>(31-bi)&1 != 0) != (bo&8 != 0) {
		return
	}
	if inst&1 != 0 {
		ctx.LR = ctx.PC
	}
	bd := uint32(int32(int16(inst & 0xfffc)))
	if inst&2 != 0 {
		ctx.PC = bd
	} else {
		ctx.PC = ctx.PC - 4 + bd
	}
}

func fBAlways(c *CPU, inst uint32) { c.PC = c.PC - 4 + uint32(int32(int16(inst&0xfffc))) }

func branchOffset(inst uint32) uint32 {
	li := inst & 0x03fffffc
	if li&0x02000000 != 0 {
		li |= 0xfc000000
	}
	return li
}

func fB(c *CPU, inst uint32) { c.PC = c.PC - 4 + branchOffset(inst) }
func fBl(c *CPU, inst uint32) {
	c.LR = c.PC
	c.PC = c.PC - 4 + branchOffset(inst)
}
func fBlr(c *CPU, inst uint32) { c.PC = c.LR &^ 3 }
func fBcctrAlways(c *CPU, inst uint32) {
	if inst&1 != 0 {
		c.LR = c.PC
	}
	c.PC = c.CTR &^ 3
}

func fRlwinm(c *CPU, inst uint32) {
	c.GPR[inst>>16&31] = bits.RotateLeft32(c.GPR[inst>>21&31], int(inst>>11&31)) & rotMask(inst>>6&31, inst>>1&31)
}
func fOri(c *CPU, inst uint32)  { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] | inst&0xffff }
func fOris(c *CPU, inst uint32) { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] | inst<<16 }
func fAndi(c *CPU, inst uint32) {
	r := c.GPR[inst>>21&31] & (inst & 0xffff)
	c.GPR[inst>>16&31] = r
	c.setCR0(r)
}

func fLwz(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = c.Mem.Read32(c.GPR[inst>>16&31] + simm(inst))
}
func fLwzu(c *CPU, inst uint32) {
	ra := inst >> 16 & 31
	ea := c.GPR[ra] + simm(inst)
	c.GPR[inst>>21&31] = c.Mem.Read32(ea)
	c.GPR[ra] = ea
}
func fLbz(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = uint32(c.Mem.Read8(c.GPR[inst>>16&31] + simm(inst)))
}
func fLhz(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = uint32(c.Mem.Read16(c.GPR[inst>>16&31] + simm(inst)))
}
func fStw(c *CPU, inst uint32) {
	c.Mem.Write32(c.GPR[inst>>16&31]+simm(inst), c.GPR[inst>>21&31])
}
func fStwu(c *CPU, inst uint32) {
	ra := inst >> 16 & 31
	ea := c.GPR[ra] + simm(inst)
	c.Mem.Write32(ea, c.GPR[inst>>21&31])
	c.GPR[ra] = ea
}
func fStb(c *CPU, inst uint32) {
	c.Mem.Write8(c.GPR[inst>>16&31]+simm(inst), uint8(c.GPR[inst>>21&31]))
}
func fSth(c *CPU, inst uint32) {
	c.Mem.Write16(c.GPR[inst>>16&31]+simm(inst), uint16(c.GPR[inst>>21&31]))
}

func fAdd(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = c.GPR[inst>>16&31] + c.GPR[inst>>11&31]
}
func fSubf(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = c.GPR[inst>>11&31] - c.GPR[inst>>16&31]
}
func fNeg(c *CPU, inst uint32) { c.GPR[inst>>21&31] = -c.GPR[inst>>16&31] }
func fMullw(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = uint32(int32(c.GPR[inst>>16&31]) * int32(c.GPR[inst>>11&31]))
}
func fCmp(c *CPU, inst uint32) {
	c.cmpS(inst>>23&7, int32(c.GPR[inst>>16&31]), int32(c.GPR[inst>>11&31]))
}
func fCmpl(c *CPU, inst uint32) { c.cmpU(inst>>23&7, c.GPR[inst>>16&31], c.GPR[inst>>11&31]) }
func fLwzx(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = c.Mem.Read32(c.GPR[inst>>16&31] + c.GPR[inst>>11&31])
}
func fLbzx(c *CPU, inst uint32) {
	c.GPR[inst>>21&31] = uint32(c.Mem.Read8(c.GPR[inst>>16&31] + c.GPR[inst>>11&31]))
}
func fStwx(c *CPU, inst uint32) {
	c.Mem.Write32(c.GPR[inst>>16&31]+c.GPR[inst>>11&31], c.GPR[inst>>21&31])
}
func fMr(c *CPU, inst uint32)  { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] }
func fOr(c *CPU, inst uint32)  { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] | c.GPR[inst>>11&31] }
func fAnd(c *CPU, inst uint32) { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] & c.GPR[inst>>11&31] }
func fXor(c *CPU, inst uint32) { c.GPR[inst>>16&31] = c.GPR[inst>>21&31] ^ c.GPR[inst>>11&31] }
func fSlw(c *CPU, inst uint32) {
	s := c.GPR[inst>>11&31] & 63
	r := uint32(0)
	if s < 32 {
		r = c.GPR[inst>>21&31] << s
	}
	c.GPR[inst>>16&31] = r
}
func fSrw(c *CPU, inst uint32) {
	s := c.GPR[inst>>11&31] & 63
	r := uint32(0)
	if s < 32 {
		r = c.GPR[inst>>21&31] >> s
	}
	c.GPR[inst>>16&31] = r
}
func fSrawi(c *CPU, inst uint32) {
	s := inst >> 11 & 31
	v := int32(c.GPR[inst>>21&31])
	c.GPR[inst>>16&31] = uint32(v >> s)
	c.setCA(v < 0 && uint32(v)&(1<<s-1) != 0)
}
func fExtsh(c *CPU, inst uint32) { c.GPR[inst>>16&31] = uint32(int32(int16(c.GPR[inst>>21&31]))) }
func fExtsb(c *CPU, inst uint32) { c.GPR[inst>>16&31] = uint32(int32(int8(c.GPR[inst>>21&31]))) }
func fMflr(c *CPU, inst uint32)  { c.GPR[inst>>21&31] = c.LR }
func fMfctr(c *CPU, inst uint32) { c.GPR[inst>>21&31] = c.CTR }
func fMtlr(c *CPU, inst uint32)  { c.LR = c.GPR[inst>>21&31] }
func fMtctr(c *CPU, inst uint32) { c.CTR = c.GPR[inst>>21&31] }
