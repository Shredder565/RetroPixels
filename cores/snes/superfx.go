package snes

// The Super FX (GSU-1 / GSU-2) cartridge coprocessor (web/emu/snes_superfx.js):
// a 16-bit RISC CPU with prefix opcodes, a two-stage pipeline, a 512-byte
// instruction cache, ROM / RAM buffers and a bitplane pixel plotter. The SNES
// reaches it through registers at $3000-$34FF. Timing is in master clocks.

const (
	gfZ    = 0x02
	gfCY   = 0x04
	gfS    = 0x08
	gfOV   = 0x10
	gfGO   = 0x20
	gfR    = 0x40
	gfALT1 = 0x100
	gfALT2 = 0x200
	gfB    = 0x1000
	gfIRQ  = 0x8000
)

// what the SNES sees in place of ROM while the Super FX owns it
var gsuRomStandin = [16]int{0x00, 0x01, 0x00, 0x01, 0x04, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x01, 0x00, 0x01, 0x0c, 0x01}

type superFX struct {
	snes              *SNES
	rom               []byte
	romMask           int
	ram               []byte
	ramMask           int
	battery           bool
	batterySize       int
	version           int
	r                 [16]int
	cache             [512]byte
	valid             [32]bool
	pcColor           [2][8]byte
	pcX, pcY, pcMask  [2]int
	z, cy, s, ov      bool
	goFlag, irq       bool
	alt               int
	bflag             bool
	sreg, dreg        int
	pbr, rombr, rambr int
	cbr, scbr, scmr   int
	cfgr, clsr, bramr int
	colr, por         int
	pipe              int
	r15w              bool
	romBuf, romReady  int
	ramReady, ramAddr int
	clock             int
}

func newSuperFX(c *cart, s *SNES, savedRam []byte) *superFX {
	g := &superFX{snes: s}
	rs := 1
	for rs < c.size {
		rs <<= 1
	}
	rs = max(rs, 0x8000)
	g.rom = make([]byte, rs)
	for i := range g.rom {
		if i < c.size {
			g.rom[i] = c.rom[i]
		} else {
			g.rom[i] = byte(c.romAt(i))
		}
	}
	g.romMask = rs - 1
	h, r := c.headerAt, c.rom
	ram := 0x8000
	if h+0x1a < len(r) && h-3 >= 0 && r[h+0x1a] == 0x33 && r[h-3] > 0 && r[h-3] <= 8 {
		ram = 1024 << r[h-3]
	}
	ram = min(0x20000, max(0x8000, ram))
	g.ram = make([]byte, ram)
	g.ramMask = ram - 1
	low := c.chip & 15
	g.battery = low == 5 || low == 6 || low == 0xa
	if g.battery {
		sz := c.sramSize
		if sz == 0 {
			sz = ram
		}
		g.batterySize = min(ram, sz)
		if len(savedRam) > 0 {
			copy(g.ram, savedRam[:min(len(savedRam), ram)])
		}
	}
	g.version = 4
	if c.chip == 0x13 {
		g.version = 1
	}
	g.reset()
	return g
}

func (g *superFX) reset() {
	g.r = [16]int{}
	g.cache = [512]byte{}
	g.valid = [32]bool{}
	g.z, g.cy, g.s, g.ov = false, false, false, false
	g.goFlag, g.irq = false, false
	g.alt, g.bflag, g.sreg, g.dreg = 0, false, 0, 0
	g.pbr, g.rombr, g.rambr, g.cbr = 0, 0, 0, 0
	g.scbr, g.scmr, g.cfgr, g.clsr, g.bramr = 0, 0, 0, 0, 0
	g.colr, g.por = 0, 0
	g.pipe, g.r15w = 0x01, false
	g.romBuf, g.romReady, g.ramReady, g.ramAddr = 0, 0, 0, 0
	g.pcMask = [2]int{}
	g.clock = g.snes.clock
}

func (g *superFX) spd() int {
	if g.clsr&1 != 0 {
		return 1
	}
	return 2
}

func (g *superFX) mem() int {
	if g.clsr&1 != 0 {
		return 5
	}
	return 6
}

func (g *superFX) busRead(bank, a int) int {
	if bank < 0x40 {
		return int(g.rom[((bank&0x3f)<<15|a&0x7fff)&g.romMask])
	}
	if bank < 0x60 {
		return int(g.rom[((bank&0x1f)<<16|a)&g.romMask])
	}
	if bank >= 0x70 && bank < 0x74 {
		return int(g.ram[((bank&3)<<16|a)&g.ramMask])
	}
	return 0
}

func (g *superFX) ramRead(a int) int { return int(g.ram[(g.rambr<<16|a)&g.ramMask]) }
func (g *superFX) ramWrite(a, v int) { g.ram[(g.rambr<<16|a)&g.ramMask] = byte(v) }
func (g *superFX) ramWait() {
	if g.clock < g.ramReady {
		g.clock = g.ramReady
	}
}

func (g *superFX) loadWord(a int) int {
	g.ramWait()
	g.ramAddr = a
	g.clock += g.mem() + g.spd()
	return g.ramRead(a) | g.ramRead(a^1)<<8
}

func (g *superFX) loadByte(a int) int {
	g.ramWait()
	g.ramAddr = a
	g.clock += g.mem()
	return g.ramRead(a)
}

func (g *superFX) storeWord(a, v int) {
	g.ramWait()
	g.ramAddr = a
	g.ramWrite(a, v&0xff)
	g.ramWrite(a^1, v>>8&0xff)
	g.ramReady = g.clock + g.mem() + g.spd()
}

func (g *superFX) storeByte(a, v int) {
	g.ramWait()
	g.ramAddr = a
	g.ramWrite(a, v&0xff)
	g.ramReady = g.clock + g.mem()
}

func (g *superFX) fetch(a int) int {
	off := (a - g.cbr) & 0xffff
	if off < 512 {
		line := off >> 4
		if !g.valid[line] {
			base := (g.cbr + line<<4) & 0xffff
			for i := 0; i < 16; i++ {
				g.cache[line<<4|i] = byte(g.busRead(g.pbr, (base+i)&0xffff))
			}
			g.valid[line] = true
			g.clock += g.mem() * (16 - off&15)
		}
		g.clock += g.spd()
		return int(g.cache[off])
	}
	g.clock += g.mem()
	return g.busRead(g.pbr, a)
}

func (g *superFX) imm() int {
	v := g.pipe
	g.r[15] = (g.r[15] + 1) & 0xffff
	g.pipe = g.fetch(g.r[15])
	return v
}

func (g *superFX) flushCache() { g.valid = [32]bool{} }

func (g *superFX) setReg(n, v int) {
	v &= 0xffff
	g.r[n] = v
	if n == 14 {
		g.romLoad()
	} else if n == 15 {
		g.r15w = true
	}
}

func (g *superFX) romLoad() {
	g.romBuf = g.busRead(g.rombr, g.r[14])
	g.romReady = g.clock + g.mem()
}

func (g *superFX) romWait() int {
	if g.clock < g.romReady {
		g.clock = g.romReady
	}
	return g.romBuf
}

func (g *superFX) setSZ(v int) {
	g.s = v&0x8000 != 0
	g.z = v&0xffff == 0
}

// ---------- plotting ----------

func (g *superFX) bpp() int {
	switch g.scmr & 3 {
	case 0:
		return 2
	case 3:
		return 8
	}
	return 4
}

func (g *superFX) pixelAddr(x, y, bpp int) int {
	ht := (g.scmr>>2)&1 | (g.scmr>>4)&2
	if g.por&0x10 != 0 {
		ht = 3
	}
	var tile int
	cx, cy := x>>3, y>>3
	switch ht {
	case 0:
		tile = cx*16 + cy
	case 1:
		tile = cx*20 + cy
	case 2:
		tile = cx*24 + cy
	default:
		tile = (y&0x80)<<2 | (x&0x80)<<1 | (y&0x78)<<1 | (x&0x78)>>3
	}
	return g.scbr<<10 + tile*(bpp<<3) + (y&7)<<1
}

func (g *superFX) writePixelRow(k int) {
	mask := g.pcMask[k]
	if mask == 0 {
		return
	}
	bpp := g.bpp()
	base := g.pixelAddr(g.pcX[k]<<3, g.pcY[k], bpp)
	col := &g.pcColor[k]
	g.ramWait()
	for p := 0; p < bpp; p++ {
		a := (base + (p>>1)<<4 + p&1) & g.ramMask
		bits := 0
		for i := 0; i < 8; i++ {
			if (col[i]>>p)&1 != 0 {
				bits |= 0x80 >> i
			}
		}
		v := bits
		if mask != 0xff {
			v = int(g.ram[a])&^mask | bits&mask
		}
		g.ram[a] = byte(v)
	}
	if mask == 0xff {
		g.ramReady = g.clock + g.mem()*bpp
	} else {
		g.ramReady = g.clock + g.mem()*bpp*2
	}
	g.pcMask[k] = 0
}

func (g *superFX) pushPrimary() {
	if g.pcMask[0] == 0 {
		return
	}
	g.writePixelRow(1)
	g.pcColor[1] = g.pcColor[0]
	g.pcX[1], g.pcY[1], g.pcMask[1] = g.pcX[0], g.pcY[0], g.pcMask[0]
	g.pcMask[0] = 0
}

func (g *superFX) plot() {
	x, y, bpp := g.r[1]&0xff, g.r[2]&0xff, g.bpp()
	c := g.colr
	if bpp != 8 && g.por&2 != 0 && (x^y)&1 != 0 {
		c >>= 4
	}
	switch bpp {
	case 2:
		c &= 3
	case 4:
		c &= 15
	default:
		c &= 0xff
	}
	draw := true
	if g.por&1 == 0 {
		if bpp == 8 && g.por&8 != 0 {
			draw = c&15 != 0
		} else {
			draw = c != 0
		}
	}
	if draw {
		if g.pcMask[0] != 0 && (g.pcX[0] != x>>3 || g.pcY[0] != y) {
			g.pushPrimary()
		}
		g.pcX[0], g.pcY[0] = x>>3, y
		g.pcColor[0][x&7] = byte(c)
		g.pcMask[0] |= 0x80 >> (x & 7)
		if g.pcMask[0] == 0xff {
			g.pushPrimary()
		}
	}
	g.r[1] = (g.r[1] + 1) & 0xffff
}

func (g *superFX) rpix() int {
	g.pushPrimary()
	g.writePixelRow(1)
	x, y, bpp := g.r[1]&0xff, g.r[2]&0xff, g.bpp()
	base, sh := g.pixelAddr(x, y, bpp), 7-(x&7)
	g.ramWait()
	c := 0
	for p := 0; p < bpp; p++ {
		c |= (int(g.ram[(base+(p>>1)<<4+p&1)&g.ramMask]) >> sh & 1) << p
	}
	g.clock += g.mem() * bpp
	return c
}

func (g *superFX) setColor(v int) {
	v &= 0xff
	if g.por&4 != 0 {
		v = v&0xf0 | v>>4
	}
	if g.por&8 != 0 {
		v = g.colr&0xf0 | v&0x0f
	}
	g.colr = v
}

// ---------- running ----------

func (g *superFX) start() {
	g.goFlag = true
	g.pipe, g.r15w = 0x01, true
	if g.clock < g.snes.clock {
		g.clock = g.snes.clock
	}
}

func (g *superFX) stop() {
	g.goFlag = false
	g.irq = true
	g.snes.updateIrq()
}

func (g *superFX) run(target int) {
	if !g.goFlag {
		if g.clock < target {
			g.clock = target
		}
		return
	}
	for g.goFlag && g.clock < target {
		g.step()
	}
	if !g.goFlag && g.clock < target {
		g.clock = target
	}
}

func (g *superFX) sync() { g.run(g.snes.clock) }

func (g *superFX) step() {
	op := g.pipe
	if g.r15w {
		g.r15w = false
	} else {
		g.r[15] = (g.r[15] + 1) & 0xffff
	}
	g.pipe = g.fetch(g.r[15])
	g.exec(op)
}

func (g *superFX) done() { g.alt, g.bflag, g.sreg, g.dreg = 0, false, 0, 0 }

func (g *superFX) branch(cond bool) {
	e := s8(g.imm())
	if cond {
		g.setReg(15, g.r[15]+e)
	}
	g.done()
}

func (g *superFX) exec(op int) {
	r := &g.r
	n, alt := op&15, g.alt
	S, D := r[g.sreg], g.dreg
	switch op >> 4 {
	case 0x0:
		switch op {
		case 0x00: // STOP
			g.done()
			g.stop()
		case 0x01: // NOP
			g.done()
		case 0x02: // CACHE
			b := r[15] & 0xfff0
			if b != g.cbr {
				g.cbr = b
				g.flushCache()
			}
			g.done()
		case 0x03: // LSR
			v := S >> 1
			g.cy = S&1 != 0
			g.setSZ(v)
			g.setReg(D, v)
			g.done()
		case 0x04: // ROL
			v := (S<<1 | b2i(g.cy)) & 0xffff
			g.cy = S&0x8000 != 0
			g.setSZ(v)
			g.setReg(D, v)
			g.done()
		case 0x05:
			g.branch(true)
		case 0x06:
			g.branch(g.s == g.ov)
		case 0x07:
			g.branch(g.s != g.ov)
		case 0x08:
			g.branch(!g.z)
		case 0x09:
			g.branch(g.z)
		case 0x0a:
			g.branch(!g.s)
		case 0x0b:
			g.branch(g.s)
		case 0x0c:
			g.branch(!g.cy)
		case 0x0d:
			g.branch(g.cy)
		case 0x0e:
			g.branch(!g.ov)
		case 0x0f:
			g.branch(g.ov)
		}
	case 0x1: // TO / MOVE
		if g.bflag {
			g.setReg(n, S)
			g.done()
		} else {
			g.dreg = n
		}
	case 0x2: // WITH
		g.sreg, g.dreg, g.bflag = n, n, true
	case 0x3:
		switch {
		case n < 12: // STW / STB
			if alt&1 != 0 {
				g.storeByte(r[n], S)
			} else {
				g.storeWord(r[n], S)
			}
			g.done()
		case n == 12: // LOOP
			v := (r[12] - 1) & 0xffff
			r[12] = v
			g.setSZ(v)
			if v != 0 {
				g.setReg(15, r[13])
			}
			g.done()
		case n == 13:
			g.alt = 1
		case n == 14:
			g.alt = 2
		default:
			g.alt = 3
		}
	case 0x4:
		if n < 12 { // LDW / LDB
			var v int
			if alt&1 != 0 {
				v = g.loadByte(r[n])
			} else {
				v = g.loadWord(r[n])
			}
			g.setReg(D, v)
			g.done()
			return
		}
		switch n {
		case 12: // PLOT / RPIX
			if alt&1 != 0 {
				v := g.rpix()
				g.setSZ(v)
				g.setReg(D, v)
			} else {
				g.plot()
			}
		case 13: // SWAP
			v := (S>>8 | S<<8) & 0xffff
			g.setSZ(v)
			g.setReg(D, v)
		case 14: // COLOR / CMODE
			if alt&1 != 0 {
				g.por = S & 0x1f
			} else {
				g.setColor(S)
			}
		default: // NOT
			v := ^S & 0xffff
			g.setSZ(v)
			g.setReg(D, v)
		}
		g.done()
	case 0x5: // ADD / ADC
		o := r[n]
		if alt&2 != 0 {
			o = n
		}
		sum := S + o
		if alt&1 != 0 && g.cy {
			sum++
		}
		v := sum & 0xffff
		g.ov = ^(S^o)&(o^v)&0x8000 != 0
		g.cy = sum > 0xffff
		g.setSZ(v)
		g.setReg(D, v)
		g.done()
	case 0x6: // SUB / SBC / CMP
		o := r[n]
		if alt == 2 {
			o = n
		}
		dif := S - o
		if alt == 1 && !g.cy {
			dif--
		}
		v := dif & 0xffff
		g.ov = (S^o)&(S^v)&0x8000 != 0
		g.cy = dif >= 0
		g.setSZ(v)
		if alt != 3 {
			g.setReg(D, v)
		}
		g.done()
	case 0x7:
		if n == 0 { // MERGE
			v := r[7]&0xff00 | r[8]>>8
			g.s, g.ov, g.cy, g.z = v&0x8080 != 0, v&0xc0c0 != 0, v&0xe0e0 != 0, v&0xf0f0 != 0
			g.setReg(D, v)
			g.done()
			return
		}
		o := r[n]
		if alt&2 != 0 {
			o = n
		}
		v := S & o
		if alt&1 != 0 {
			v = S &^ o & 0xffff
		}
		g.setSZ(v)
		g.setReg(D, v)
		g.done()
	case 0x8: // MULT / UMULT
		o := r[n]
		if alt&2 != 0 {
			o = n
		}
		var v int
		if alt&1 != 0 {
			v = (S & 0xff) * (o & 0xff) & 0xffff
		} else {
			v = s8(S) * s8(o) & 0xffff
		}
		if g.cfgr&0x20 == 0 {
			g.clock += g.spd()
		}
		g.setSZ(v)
		g.setReg(D, v)
		g.done()
	case 0x9:
		switch n {
		case 0: // SBK
			g.storeWord(g.ramAddr, S)
		case 1, 2, 3, 4: // LINK
			g.setReg(11, r[15]+n)
		case 5: // SEX
			v := s8(S) & 0xffff
			g.setSZ(v)
			g.setReg(D, v)
		case 6: // ASR / DIV2
			v := s16(S) >> 1 & 0xffff
			if alt&1 != 0 && S == 0xffff {
				v = 0
			}
			g.cy = S&1 != 0
			g.setSZ(v)
			g.setReg(D, v)
		case 7: // ROR
			v := S >> 1
			if g.cy {
				v |= 0x8000
			}
			g.cy = S&1 != 0
			g.setSZ(v)
			g.setReg(D, v)
		case 14: // LOB
			v := S & 0xff
			g.s, g.z = v&0x80 != 0, v == 0
			g.setReg(D, v)
		case 15: // FMULT / LMULT
			p := s16(S) * s16(r[6])
			hi := (p >> 16) & 0xffff
			if g.cfgr&0x20 != 0 {
				g.clock += g.spd() * 3
			} else {
				g.clock += g.spd() * 7
			}
			if alt&1 != 0 {
				r[4] = p & 0xffff
			}
			g.cy = (p>>15)&1 != 0
			g.setSZ(hi)
			g.setReg(D, hi)
		default: // JMP / LJMP
			if alt&1 != 0 {
				g.pbr = r[n] & 0x7f
				g.setReg(15, S)
				g.cbr = S & 0xfff0
				g.flushCache()
			} else {
				g.setReg(15, r[n])
			}
		}
		g.done()
	case 0xa: // IBT / LMS / SMS
		k := g.imm()
		switch alt {
		case 0:
			g.setReg(n, s8(k))
		case 2:
			g.storeWord(k<<1, r[n])
		default:
			g.setReg(n, g.loadWord(k<<1))
		}
		g.done()
	case 0xb: // FROM / MOVES
		if g.bflag {
			v := r[n]
			g.ov = v&0x80 != 0
			g.setSZ(v)
			g.setReg(D, v)
			g.done()
		} else {
			g.sreg = n
		}
	case 0xc:
		if n == 0 { // HIB
			v := S >> 8
			g.s, g.z = v&0x80 != 0, v == 0
			g.setReg(D, v)
			g.done()
			return
		}
		o := r[n]
		if alt&2 != 0 {
			o = n
		}
		v := S | o
		if alt&1 != 0 {
			v = S ^ o
		}
		g.setSZ(v)
		g.setReg(D, v)
		g.done()
	case 0xd:
		if n < 15 { // INC
			v := (r[n] + 1) & 0xffff
			g.setSZ(v)
			g.setReg(n, v)
			g.done()
			return
		}
		switch alt {
		case 3: // ROMB
			g.romWait()
			g.rombr = S & 0x7f
		case 2: // RAMB
			g.ramWait()
			g.rambr = S & 1
		default: // GETC
			g.setColor(g.romWait())
		}
		g.done()
	case 0xe:
		if n < 15 { // DEC
			v := (r[n] - 1) & 0xffff
			g.setSZ(v)
			g.setReg(n, v)
			g.done()
			return
		}
		b := g.romWait() // GETB / GETBH / GETBL / GETBS
		var v int
		switch alt {
		case 0:
			v = b
		case 1:
			v = S&0xff | b<<8
		case 2:
			v = S&0xff00 | b
		default:
			v = s8(b) & 0xffff
		}
		g.setReg(D, v)
		g.done()
	default: // IWT / LM / SM
		lo := g.imm()
		hi := g.imm()
		k := lo | hi<<8
		switch alt {
		case 0:
			g.setReg(n, k)
		case 2:
			g.storeWord(k, r[n])
		default:
			g.setReg(n, g.loadWord(k))
		}
		g.done()
	}
}

// ---------- SNES side ----------

func (g *superFX) sfr() int {
	v := 0
	if g.z {
		v |= gfZ
	}
	if g.cy {
		v |= gfCY
	}
	if g.s {
		v |= gfS
	}
	if g.ov {
		v |= gfOV
	}
	if g.goFlag {
		v |= gfGO
	}
	if g.clock < g.romReady {
		v |= gfR
	}
	if g.alt&1 != 0 {
		v |= gfALT1
	}
	if g.alt&2 != 0 {
		v |= gfALT2
	}
	if g.bflag {
		v |= gfB
	}
	if g.irq {
		v |= gfIRQ
	}
	return v
}

func (g *superFX) irqOut() bool { return g.irq && g.cfgr&0x80 == 0 }

func (g *superFX) regRead(off int) uint8 {
	g.sync()
	if off >= 0x3100 && off < 0x3300 {
		return g.cache[off-0x3100]
	}
	if off < 0x3020 {
		v := g.r[(off>>1)&15]
		if off&1 != 0 {
			return uint8(v >> 8)
		}
		return uint8(v)
	}
	switch off {
	case 0x3030:
		return uint8(g.sfr())
	case 0x3031:
		v := g.sfr() >> 8
		if g.irq {
			g.irq = false
			g.snes.updateIrq()
		}
		return uint8(v)
	case 0x3033:
		return uint8(g.bramr)
	case 0x3034:
		return uint8(g.pbr)
	case 0x3036:
		return uint8(g.rombr)
	case 0x3037:
		return uint8(g.cfgr)
	case 0x3038:
		return uint8(g.scbr)
	case 0x3039:
		return uint8(g.clsr)
	case 0x303a:
		return uint8(g.scmr)
	case 0x303b:
		return uint8(g.version)
	case 0x303c:
		return uint8(g.rambr)
	case 0x303e:
		return uint8(g.cbr)
	case 0x303f:
		return uint8(g.cbr >> 8)
	}
	return 0
}

func (g *superFX) regWrite(off int, v8 uint8) {
	g.sync()
	v := int(v8)
	if off >= 0x3100 && off < 0x3300 {
		i := off - 0x3100
		g.cache[i] = v8
		if i&15 == 15 {
			g.valid[i>>4] = true
		}
		return
	}
	if off < 0x3020 {
		k := (off >> 1) & 15
		if off&1 != 0 {
			g.r[k] = g.r[k]&0xff | v<<8
			if k == 14 {
				g.romLoad()
			}
			if k == 15 {
				g.start()
			}
		} else {
			g.r[k] = g.r[k]&0xff00 | v
		}
		return
	}
	switch off {
	case 0x3030:
		wasGo := g.goFlag
		g.z, g.cy, g.s, g.ov = v&gfZ != 0, v&gfCY != 0, v&gfS != 0, v&gfOV != 0
		if v&gfGO == 0 {
			g.goFlag = false
			if wasGo || g.cbr != 0 {
				g.cbr = 0
				g.flushCache()
			}
		} else if !wasGo {
			g.start()
		}
	case 0x3031:
		g.alt = v & 3
		g.bflag = v&0x10 != 0
		g.irq = v&0x80 != 0
		g.snes.updateIrq()
	case 0x3033:
		g.bramr = v & 1
	case 0x3034:
		g.pbr = v & 0x7f
	case 0x3037:
		g.cfgr = v
		g.snes.updateIrq()
	case 0x3038:
		g.scbr = v
	case 0x3039:
		g.clsr = v & 1
	case 0x303a:
		g.scmr = v
	}
}

func (g *superFX) romOwned() bool { return g.goFlag && g.scmr&0x10 != 0 }
func (g *superFX) ramOwned() bool { return g.goFlag && g.scmr&0x08 != 0 }

// snesRead: the SNES bus outside $3000-$34FF; -1 for open bus.
func (g *superFX) snesRead(bank, off int) int {
	b := bank & 0x7f
	if b < 0x40 {
		if off >= 0x8000 {
			if g.romOwned() {
				g.sync()
				if g.romOwned() {
					return gsuRomStandin[off&15]
				}
			}
			return int(g.rom[(b<<15|off&0x7fff)&g.romMask])
		}
		if off >= 0x6000 {
			if g.ramOwned() {
				return -1
			}
			return int(g.ram[(off-0x6000)&g.ramMask])
		}
		return -1
	}
	if b < 0x60 {
		if g.romOwned() {
			g.sync()
			if g.romOwned() {
				return gsuRomStandin[off&15]
			}
		}
		return int(g.rom[((b&0x1f)<<16|off)&g.romMask])
	}
	if b >= 0x70 && b < 0x74 {
		if g.ramOwned() {
			return -1
		}
		return int(g.ram[((b&3)<<16|off)&g.ramMask])
	}
	return -1
}

func (g *superFX) snesWrite(bank, off int, v uint8) {
	b := bank & 0x7f
	i := -1
	if b < 0x40 {
		if off >= 0x6000 && off < 0x8000 {
			i = (off - 0x6000) & g.ramMask
		}
	} else if b >= 0x70 && b < 0x74 {
		i = ((b&3)<<16 | off) & g.ramMask
	}
	if i < 0 || g.ramOwned() {
		return
	}
	g.ram[i] = v
}

func (g *superFX) getSRAM() []byte {
	if !g.battery {
		return nil
	}
	return append([]byte(nil), g.ram[:g.batterySize]...)
}

func (g *superFX) state(s *sio) {
	s.IntSlice(g.r[:])
	s.Bytes(g.cache[:])
	for i := range g.valid {
		s.Bool(&g.valid[i])
	}
	s.Bytes(g.ram)
	s.Bytes(g.pcColor[0][:])
	s.Bytes(g.pcColor[1][:])
	s.IntSlice(g.pcX[:])
	s.IntSlice(g.pcY[:])
	s.IntSlice(g.pcMask[:])
	s.Bools(&g.z, &g.cy, &g.s, &g.ov, &g.goFlag, &g.irq, &g.bflag, &g.r15w)
	s.Ints(&g.alt, &g.sreg, &g.dreg, &g.pbr, &g.rombr, &g.rambr, &g.cbr, &g.scbr, &g.scmr, &g.cfgr, &g.clsr, &g.bramr,
		&g.colr, &g.por, &g.pipe, &g.romBuf, &g.romReady, &g.ramReady, &g.ramAddr, &g.clock)
}
