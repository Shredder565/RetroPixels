package arm

// ARMv6 instructions of the 3DS's ARM11. The DS cores never reach these
// (their architecture check sends them to the undefined vector).

func armExclusive(c *CPU, op uint32) {
	if c.Arch < V6K {
		c.undefined(op)
		return
	}
	rn, rd, rm := op>>16&15, op>>12&15, op&15
	a := c.R[rn]
	load := op&(1<<20) != 0
	size := op >> 21 & 3 // 0 word, 1 doubleword, 2 byte, 3 halfword
	if load {
		c.exclusive, c.exclusiveAddr = true, a
		switch size {
		case 0:
			c.R[rd] = c.Bus.Read32(a)
		case 1:
			c.R[rd], c.R[rd+1] = c.Bus.Read32(a), c.Bus.Read32(a+4)
		case 2:
			c.R[rd] = c.Bus.Read8(a)
		case 3:
			c.R[rd] = c.Bus.Read16(a)
		}
		return
	}
	if !c.exclusive || c.exclusiveAddr != a {
		c.R[rd] = 1
		return
	}
	c.exclusive = false
	switch size {
	case 0:
		c.Bus.Write32(a, c.R[rm])
	case 1:
		c.Bus.Write32(a, c.R[rm])
		c.Bus.Write32(a+4, c.R[rm+1])
	case 2:
		c.Bus.Write8(a, c.R[rm])
	case 3:
		c.Bus.Write16(a, c.R[rm])
	}
	c.R[rd] = 0
}

func armMedia(c *CPU, op uint32) {
	if c.Arch < V6K {
		c.undefined(op)
		return
	}
	c.media(op)
}

func (c *CPU) v6CPS(op uint32) {
	if op&(1<<16) != 0 { // SETEND: big-endian data isn't supported
		return
	}
	if c.CPSR&0x1f == ModeUSR {
		return
	}
	imod := op >> 18 & 3
	var mask uint32
	if op&(1<<8) != 0 {
		mask |= 1 << 8
	}
	if op&(1<<7) != 0 {
		mask |= FlagI
	}
	if op&(1<<6) != 0 {
		mask |= FlagF
	}
	switch imod {
	case 2:
		c.CPSR &^= mask
	case 3:
		c.CPSR |= mask
	}
	if op&(1<<17) != 0 {
		c.SetMode(op & 0x1f)
	}
}

func (c *CPU) v6SRS(op uint32) {
	mode := op & 0x1f
	lr, spsr := c.R[14], c.SPSR
	old := c.CPSR
	c.SetMode(mode)
	sp := c.R[13]
	pre, up := op&(1<<24) != 0, op&(1<<23) != 0
	a := sp
	if !up {
		a -= 8
	}
	if pre == up {
		a += 4
	}
	c.Bus.Write32(a, lr)
	c.Bus.Write32(a+4, spsr)
	if op&(1<<21) != 0 {
		if up {
			c.R[13] = sp + 8
		} else {
			c.R[13] = sp - 8
		}
	}
	c.SetMode(old & 0x1f)
}

func (c *CPU) v6RFE(op uint32) {
	rn := op >> 16 & 15
	base := c.R[rn]
	pre, up := op&(1<<24) != 0, op&(1<<23) != 0
	a := base
	if !up {
		a -= 8
	}
	if pre == up {
		a += 4
	}
	pc, psr := c.Bus.Read32(a), c.Bus.Read32(a+4)
	if op&(1<<21) != 0 {
		if up {
			c.R[rn] = base + 8
		} else {
			c.R[rn] = base - 8
		}
	}
	c.SetCPSR(psr)
	c.SetPC(pc)
}
