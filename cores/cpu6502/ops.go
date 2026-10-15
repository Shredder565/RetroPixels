package cpu6502

type mode uint8

const (
	mImm mode = iota
	mZp
	mZpx
	mZpy
	mAbs
	mAbx
	mAby
	mIzx
	mIzy
)

type opFunc func(c *CPU)

var ops [256]opFunc

func (c *CPU) exec(op uint8) { ops[op](c) }

func init() {
	t := &ops
	read := func(op int, m mode, fn func(*CPU, uint8)) {
		t[op] = func(c *CPU) { fn(c, c.readMode(m)) }
	}
	store := func(op int, m mode, fn func(*CPU) uint8) {
		t[op] = func(c *CPU) { ea := c.addrMode(m); c.wr(ea, fn(c)) }
	}
	modify := func(op int, m mode, fn func(*CPU, uint8) uint8) {
		t[op] = func(c *CPU) { c.rmw(c.addrMode(m), fn) }
	}
	implied := func(op int, fn func(*CPU)) {
		t[op] = func(c *CPU) { c.rd(c.PC); fn(c) }
	}

	modesR := []struct {
		m   mode
		off int
	}{{mImm, 0x09}, {mZp, 0x05}, {mZpx, 0x15}, {mAbs, 0x0d}, {mAbx, 0x1d}, {mAby, 0x19}, {mIzx, 0x01}, {mIzy, 0x11}}
	group1 := []struct {
		base int
		fn   func(*CPU, uint8)
	}{
		{0x00, func(c *CPU, v uint8) { c.A = c.setZN(c.A | v) }}, // ORA
		{0x20, func(c *CPU, v uint8) { c.A = c.setZN(c.A & v) }}, // AND
		{0x40, func(c *CPU, v uint8) { c.A = c.setZN(c.A ^ v) }}, // EOR
		{0x60, func(c *CPU, v uint8) { c.adc(v) }},               // ADC
		{0xa0, func(c *CPU, v uint8) { c.A = c.setZN(v) }},       // LDA
		{0xc0, func(c *CPU, v uint8) { c.cmp(c.A, v) }},          // CMP
		{0xe0, func(c *CPU, v uint8) { c.sbc(v) }},               // SBC
	}
	for _, g := range group1 {
		for _, m := range modesR {
			read(g.base+m.off, m.m, g.fn)
		}
	}
	for _, m := range modesR {
		if m.m != mImm {
			store(0x80+m.off, m.m, func(c *CPU) uint8 { return c.A }) // STA
		}
	}

	// shifts / inc / dec
	shifts := []struct {
		base int
		fn   func(*CPU, uint8) uint8
	}{{0x00, (*CPU).asl}, {0x20, (*CPU).rol}, {0x40, (*CPU).lsr}, {0x60, (*CPU).ror}}
	for _, s := range shifts {
		fn := s.fn
		implied(s.base+0x0a, func(c *CPU) { c.A = fn(c, c.A) })
		modify(s.base+0x06, mZp, fn)
		modify(s.base+0x16, mZpx, fn)
		modify(s.base+0x0e, mAbs, fn)
		modify(s.base+0x1e, mAbx, fn)
	}
	inc := func(c *CPU, v uint8) uint8 { return c.setZN(v + 1) }
	dec := func(c *CPU, v uint8) uint8 { return c.setZN(v - 1) }
	modify(0xe6, mZp, inc)
	modify(0xf6, mZpx, inc)
	modify(0xee, mAbs, inc)
	modify(0xfe, mAbx, inc)
	modify(0xc6, mZp, dec)
	modify(0xd6, mZpx, dec)
	modify(0xce, mAbs, dec)
	modify(0xde, mAbx, dec)

	// loads / stores for X and Y
	ldx := func(c *CPU, v uint8) { c.X = c.setZN(v) }
	ldy := func(c *CPU, v uint8) { c.Y = c.setZN(v) }
	read(0xa2, mImm, ldx)
	read(0xa6, mZp, ldx)
	read(0xb6, mZpy, ldx)
	read(0xae, mAbs, ldx)
	read(0xbe, mAby, ldx)
	read(0xa0, mImm, ldy)
	read(0xa4, mZp, ldy)
	read(0xb4, mZpx, ldy)
	read(0xac, mAbs, ldy)
	read(0xbc, mAbx, ldy)
	getX := func(c *CPU) uint8 { return c.X }
	getY := func(c *CPU) uint8 { return c.Y }
	store(0x86, mZp, getX)
	store(0x96, mZpy, getX)
	store(0x8e, mAbs, getX)
	store(0x84, mZp, getY)
	store(0x94, mZpx, getY)
	store(0x8c, mAbs, getY)
	cpx := func(c *CPU, v uint8) { c.cmp(c.X, v) }
	cpy := func(c *CPU, v uint8) { c.cmp(c.Y, v) }
	read(0xe0, mImm, cpx)
	read(0xe4, mZp, cpx)
	read(0xec, mAbs, cpx)
	read(0xc0, mImm, cpy)
	read(0xc4, mZp, cpy)
	read(0xcc, mAbs, cpy)
	bit := func(c *CPU, v uint8) {
		c.P &^= FlagZ | FlagV | FlagN
		if c.A&v == 0 {
			c.P |= FlagZ
		}
		c.P |= v & (FlagV | FlagN)
	}
	read(0x24, mZp, bit)
	read(0x2c, mAbs, bit)

	// transfers, flags, inc/dec registers
	implied(0xaa, func(c *CPU) { c.X = c.setZN(c.A) })
	implied(0xa8, func(c *CPU) { c.Y = c.setZN(c.A) })
	implied(0x8a, func(c *CPU) { c.A = c.setZN(c.X) })
	implied(0x98, func(c *CPU) { c.A = c.setZN(c.Y) })
	implied(0xba, func(c *CPU) { c.X = c.setZN(c.S) })
	implied(0x9a, func(c *CPU) { c.S = c.X })
	implied(0xe8, func(c *CPU) { c.X = c.setZN(c.X + 1) })
	implied(0xc8, func(c *CPU) { c.Y = c.setZN(c.Y + 1) })
	implied(0xca, func(c *CPU) { c.X = c.setZN(c.X - 1) })
	implied(0x88, func(c *CPU) { c.Y = c.setZN(c.Y - 1) })
	implied(0x18, func(c *CPU) { c.P &^= FlagC })
	implied(0x38, func(c *CPU) { c.P |= FlagC })
	implied(0x58, func(c *CPU) { c.P &^= FlagI })
	implied(0x78, func(c *CPU) { c.P |= FlagI })
	implied(0xb8, func(c *CPU) { c.P &^= FlagV })
	implied(0xd8, func(c *CPU) { c.P &^= FlagD })
	implied(0xf8, func(c *CPU) { c.P |= FlagD })
	implied(0xea, func(c *CPU) {})

	// stack
	t[0x48] = func(c *CPU) { c.rd(c.PC); c.push(c.A) }
	t[0x08] = func(c *CPU) { c.rd(c.PC); c.push(c.P | FlagB | FlagU) }
	t[0x68] = func(c *CPU) { c.rd(c.PC); c.rd(0x100 | uint16(c.S)); c.A = c.setZN(c.pull()) }
	t[0x28] = func(c *CPU) { c.rd(c.PC); c.rd(0x100 | uint16(c.S)); c.P = c.pull()&^FlagB | FlagU }

	// jumps & calls
	t[0x4c] = func(c *CPU) { c.PC = c.fetch16() }
	t[0x6c] = func(c *CPU) { // indirect, with the page-wrap quirk
		p := c.fetch16()
		lo := uint16(c.rd(p))
		c.PC = lo | uint16(c.rd(p&0xff00|(p+1)&0xff))<<8
	}
	t[0x20] = func(c *CPU) {
		lo := uint16(c.fetch())
		c.rd(0x100 | uint16(c.S))
		c.push(uint8(c.PC >> 8))
		c.push(uint8(c.PC))
		c.PC = lo | uint16(c.fetch())<<8
	}
	t[0x60] = func(c *CPU) {
		c.rd(c.PC)
		c.rd(0x100 | uint16(c.S))
		lo := uint16(c.pull())
		c.PC = lo | uint16(c.pull())<<8
		c.rd(c.PC)
		c.PC++
	}
	t[0x40] = func(c *CPU) {
		c.rd(c.PC)
		c.rd(0x100 | uint16(c.S))
		c.P = c.pull()&^FlagB | FlagU
		lo := uint16(c.pull())
		c.PC = lo | uint16(c.pull())<<8
	}
	t[0x00] = func(c *CPU) { c.fetch(); c.interrupt(0xfffe, true) }

	// branches
	t[0x10] = func(c *CPU) { c.branch(c.P&FlagN == 0) }
	t[0x30] = func(c *CPU) { c.branch(c.P&FlagN != 0) }
	t[0x50] = func(c *CPU) { c.branch(c.P&FlagV == 0) }
	t[0x70] = func(c *CPU) { c.branch(c.P&FlagV != 0) }
	t[0x90] = func(c *CPU) { c.branch(c.P&FlagC == 0) }
	t[0xb0] = func(c *CPU) { c.branch(c.P&FlagC != 0) }
	t[0xd0] = func(c *CPU) { c.branch(c.P&FlagZ == 0) }
	t[0xf0] = func(c *CPU) { c.branch(c.P&FlagZ != 0) }

	// ---- unofficial opcodes (several games rely on these) ----
	nop := func(c *CPU, v uint8) {}
	for _, op := range []int{0x1a, 0x3a, 0x5a, 0x7a, 0xda, 0xfa} {
		implied(op, func(c *CPU) {})
	}
	for _, op := range []int{0x80, 0x82, 0x89, 0xc2, 0xe2} {
		read(op, mImm, nop)
	}
	for _, op := range []int{0x04, 0x44, 0x64} {
		read(op, mZp, nop)
	}
	for _, op := range []int{0x14, 0x34, 0x54, 0x74, 0xd4, 0xf4} {
		read(op, mZpx, nop)
	}
	read(0x0c, mAbs, nop)
	for _, op := range []int{0x1c, 0x3c, 0x5c, 0x7c, 0xdc, 0xfc} {
		read(op, mAbx, nop)
	}
	lax := func(c *CPU, v uint8) { c.X = c.setZN(v); c.A = c.X }
	read(0xa7, mZp, lax)
	read(0xb7, mZpy, lax)
	read(0xaf, mAbs, lax)
	read(0xbf, mAby, lax)
	read(0xa3, mIzx, lax)
	read(0xb3, mIzy, lax)
	read(0xab, mImm, func(c *CPU, v uint8) { c.X = c.setZN(v); c.A = c.X }) // (A | $FF) & v
	sax := func(c *CPU) uint8 { return c.A & c.X }
	store(0x87, mZp, sax)
	store(0x97, mZpy, sax)
	store(0x8f, mAbs, sax)
	store(0x83, mIzx, sax)
	read(0xeb, mImm, func(c *CPU, v uint8) { c.sbc(v) })
	combo := func(base int, rmwFn func(*CPU, uint8) uint8, after func(*CPU, uint8)) {
		f := func(c *CPU, v uint8) uint8 { r := rmwFn(c, v); after(c, r); return r }
		modify(base+0x07, mZp, f)
		modify(base+0x17, mZpx, f)
		modify(base+0x0f, mAbs, f)
		modify(base+0x1f, mAbx, f)
		modify(base+0x1b, mAby, f)
		modify(base+0x03, mIzx, f)
		modify(base+0x13, mIzy, f)
	}
	combo(0x00, (*CPU).asl, func(c *CPU, r uint8) { c.A = c.setZN(c.A | r) })                          // SLO
	combo(0x20, (*CPU).rol, func(c *CPU, r uint8) { c.A = c.setZN(c.A & r) })                          // RLA
	combo(0x40, (*CPU).lsr, func(c *CPU, r uint8) { c.A = c.setZN(c.A ^ r) })                          // SRE
	combo(0x60, (*CPU).ror, func(c *CPU, r uint8) { c.adc(r) })                                        // RRA
	combo(0xc0, func(c *CPU, v uint8) uint8 { return v - 1 }, func(c *CPU, r uint8) { c.cmp(c.A, r) }) // DCP
	combo(0xe0, func(c *CPU, v uint8) uint8 { return v + 1 }, func(c *CPU, r uint8) { c.sbc(r) })      // ISC
	anc := func(c *CPU, v uint8) { c.A = c.setZN(c.A & v); c.P = c.P&^FlagC | c.A>>7 }
	read(0x0b, mImm, anc)
	read(0x2b, mImm, anc)
	read(0x4b, mImm, func(c *CPU, v uint8) { c.A = c.lsr(c.A & v) }) // ALR
	read(0x6b, mImm, func(c *CPU, v uint8) {                         // ARR
		r := (c.A&v)>>1 | (c.P&FlagC)<<7
		c.A = c.setZN(r)
		c.P = c.P&^(FlagC|FlagV) | (r>>6)&1
		if ((r>>6)^(r>>5))&1 != 0 {
			c.P |= FlagV
		}
	})
	read(0xcb, mImm, func(c *CPU, v uint8) { // AXS
		ax := c.A & c.X
		c.P &^= FlagC
		if ax >= v {
			c.P |= FlagC
		}
		c.X = c.setZN(ax - v)
	})
	read(0xbb, mAby, func(c *CPU, v uint8) { c.S = c.setZN(v & c.S); c.A, c.X = c.S, c.S }) // LAS
	// Unstable stores: high-byte AND quirks.
	t[0x9c] = func(c *CPU) { c.shStore(c.X, c.Y) }     // SHY
	t[0x9e] = func(c *CPU) { c.shStore(c.Y, c.X) }     // SHX
	t[0x9f] = func(c *CPU) { c.shStore(c.Y, c.A&c.X) } // SHA abs,Y
	t[0x9b] = func(c *CPU) {                           // TAS
		base := c.fetch16()
		ea := base + uint16(c.Y)
		c.rd(base&0xff00 | ea&0xff)
		c.S = c.A & c.X
		c.wr(ea, c.S&uint8(base>>8+1))
	}
	t[0x93] = func(c *CPU) { // SHA (zp),Y
		z := c.fetch()
		lo := uint16(c.rd(uint16(z)))
		base := lo | uint16(c.rd(uint16(z+1)))<<8
		ea := base + uint16(c.Y)
		c.rd(base&0xff00 | ea&0xff)
		c.wr(ea, c.A&c.X&uint8(base>>8+1))
	}
	read(0x8b, mImm, func(c *CPU, v uint8) { c.A = c.setZN((c.A | 0xee) & c.X & v) }) // XAA
	// JAM opcodes stop the processor.
	for _, op := range []int{0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xb2, 0xd2, 0xf2} {
		t[op] = func(c *CPU) { c.Jammed = true }
	}
	for i := range t {
		if t[i] == nil {
			t[i] = func(c *CPU) {}
		}
	}
}
