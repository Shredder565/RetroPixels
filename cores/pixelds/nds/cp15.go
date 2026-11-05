package nds

// cp15 is the ARM946E-S's system control coprocessor: the control
// register, the tightly coupled memories, the protection unit's registers
// (kept so games read back what they wrote; memory isn't protected) and the
// cache operations (nothing to do: caches aren't modelled), including
// "wait for interrupt".
type cp15 struct {
	m       *Machine
	control uint32
	dtcmReg uint32
	itcmReg uint32
	regs    map[uint32]uint32 // everything else, by op1/cn/cm/op2

	itcm, dtcm []byte
	itcmEnd    uint32
	itcmOn     bool
	dtcmBase   uint32
	dtcmSize   uint32
	dtcmOn     bool
}

func newCP15(m *Machine) *cp15 {
	c := &cp15{m: m, itcm: make([]byte, 32<<10), dtcm: make([]byte, 16<<10), regs: map[uint32]uint32{}}
	// as the boot menu leaves it: DTCM at 0x03000000 (16 KB), ITCM 32 MB
	// (mirrored), both on, exceptions at 0xFFFF0000
	c.dtcmReg = 0x0300000a
	c.itcmReg = 0x00000020
	c.control = 0x00052078 | 1<<13
	c.update()
	return c
}

func (c *cp15) update() {
	c.dtcmBase = c.dtcmReg &^ 0xfff
	c.dtcmSize = 512 << (c.dtcmReg >> 1 & 0x1f)
	if c.dtcmSize < 4096 {
		c.dtcmSize = 4096
	}
	c.itcmEnd = 512 << (c.itcmReg >> 1 & 0x1f)
	c.dtcmOn = c.control&(1<<16) != 0 && c.control&(1<<17) == 0
	c.itcmOn = c.control&(1<<18) != 0 && c.control&(1<<19) == 0
	if c.m.arm9 != nil {
		if c.control&(1<<13) != 0 {
			c.m.arm9.VectorBase = 0xffff0000
		} else {
			c.m.arm9.VectorBase = 0
		}
	}
}

func key(op1, cn, cm, op2 uint32) uint32 { return op1<<12 | cn<<8 | cm<<4 | op2 }

func (c *cp15) MRC(op1, cn, cm, op2 uint32) uint32 {
	switch key(op1, cn, cm, op2) {
	case 0x000:
		return 0x41059461 // ARM946E-S
	case 0x001:
		return 0x0f0d2112 // cache type: 8 KB instruction, 4 KB data
	case 0x002:
		return 0x00140180 // TCM sizes: 32 KB ITCM, 16 KB DTCM
	case 0x100:
		return c.control
	case 0x910:
		return c.dtcmReg
	case 0x911:
		return c.itcmReg
	}
	return c.regs[key(op1, cn, cm, op2)]
}

func (c *cp15) MCR(op1, cn, cm, op2, v uint32) {
	switch k := key(op1, cn, cm, op2); k {
	case 0x100:
		c.control = c.control&^0x000ff085 | v&0x000ff085 | 0x78
		c.update()
	case 0x910:
		c.dtcmReg = v
		c.update()
	case 0x911:
		c.itcmReg = v
		c.update()
	case 0x704, 0x782: // wait for interrupt
		c.m.halt9()
	default:
		c.regs[k] = v
	}
}

// halt9 stops the ARM9 until an enabled interrupt is requested.
func (m *Machine) halt9() {
	if m.irq9.ie&m.irq9.ifl == 0 {
		m.arm9.Halted = true
	}
}
