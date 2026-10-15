package snes

import (
	"math"

	"retropixels/cores/w65816"
)

// The SA-1 cartridge chip (web/emu/snes_sa1.js): a second 65C816 at half the
// master clock with 2 KB of I-RAM, the BW-RAM (also seen as a packed 2/4-bit
// bitmap), the Super MMC ROM bank switcher, an arithmetic unit, a
// variable-length bit reader, DMA with character conversion, and an H/V timer.

const (
	two40 = 1 << 40
	two39 = 1 << 39
	never = math.MaxInt
)

type sa1 struct {
	snes   *SNES
	cart   *cart
	bw     []byte
	ownBw  bool
	bwMask int
	iram   [0x800]byte
	brf    [16]byte
	cpu    *w65816.CPU

	clock, mdr                int
	ccnt, sie, crv, cnv, civ  int
	mmc                       [4]int
	bmaps, sbwe, bwpa, siwp   int
	scnt, cie, snv, siv       int
	tmc, hcnt, vcnt           int
	bmap, cbwe, ciwp          int
	dcnt, cdma, sda, dda, dtc int
	bbf                       int
	mcnt, ma, mb              int
	mr                        int // 40-bit sum, signed
	of                        int
	vbd, vda, vbit            int
	sfrIrq, sfrDma            bool
	cfrIrq, cfrTimer, cfrDma  bool
	cfrNmi                    bool
	cc1                       bool
	cc2Line                   int
	timerBase, hLatch, vLatch int
	running                   bool
	snesIrq                   bool
}

type sa1Bus struct{ c *sa1 }

func (b sa1Bus) Read(a uint32) uint8     { return b.c.read(int(a)) }
func (b sa1Bus) Write(a uint32, v uint8) { b.c.write(int(a), v) }
func (b sa1Bus) Idle()                   { b.c.clock += 2 }

func newSA1(s *SNES) *sa1 {
	c := &sa1{snes: s, cart: s.cart}
	if s.cart.sramSize != 0 {
		c.bw = s.cart.sram
	} else {
		c.bw = make([]byte, 0x2000)
		for i := range c.bw {
			c.bw[i] = 0xff
		}
		c.ownBw = true
	}
	c.bwMask = len(c.bw) - 1
	c.cpu = w65816.New(sa1Bus{c})
	s.cart.sa1 = c
	c.reset()
	return c
}

func (c *sa1) reset() {
	c.iram = [0x800]byte{}
	c.brf = [16]byte{}
	c.clock = c.snes.clock
	c.mdr = 0
	c.ccnt = 0x20
	c.sie, c.crv, c.cnv, c.civ = 0, 0, 0, 0
	c.mmc = [4]int{0, 1, 2, 3}
	c.bmaps, c.sbwe, c.bwpa, c.siwp = 0, 0, 0x0f, 0
	c.scnt, c.cie, c.snv, c.siv = 0, 0, 0, 0
	c.tmc, c.hcnt, c.vcnt = 0, 0, 0
	c.bmap, c.cbwe, c.ciwp = 0, 0, 0
	c.dcnt, c.cdma, c.sda, c.dda, c.dtc = 0, 0, 0, 0, 0
	c.bbf = 0
	c.mcnt, c.ma, c.mb, c.mr, c.of = 0, 0, 0, 0, 0
	c.vbd, c.vda, c.vbit = 0, 0, 0
	c.sfrIrq, c.sfrDma = false, false
	c.cfrIrq, c.cfrTimer, c.cfrDma, c.cfrNmi = false, false, false, false
	c.cc1, c.cc2Line = false, 0
	c.timerBase, c.hLatch, c.vLatch = c.clock, 0, 0
	c.running = false
	c.snesIrq = false
	c.cpu.NMIPending, c.cpu.IRQLine = false, false
}

// ---------- ROM through the Super MMC ----------

func (c *sa1) romLo(bank, off int) int {
	g := (bank>>6)&2 | (bank>>5)&1
	r := c.mmc[g]
	block := g
	if r&0x80 != 0 {
		block = r & 7
	}
	return c.cart.romAt(block<<20 | (bank&0x1f)<<15 | off&0x7fff)
}

func (c *sa1) romHi(bank, off int) int {
	block := c.mmc[(bank>>4)&3] & 7
	return c.cart.romAt(block<<20 | (bank&0x0f)<<16 | off)
}

func (c *sa1) romByte(a int) int {
	bank, off := (a>>16)&0xff, a&0xffff
	if bank >= 0xc0 {
		return c.romHi(bank, off)
	}
	if bank&0x40 == 0 && off >= 0x8000 {
		return c.romLo(bank, off)
	}
	return 0
}

// ---------- BW-RAM ----------

func (c *sa1) bwWritable(i int) bool {
	return c.sbwe&0x80 != 0 || c.cbwe&0x80 != 0 || i >= 0x100<<(c.bwpa&0x0f)
}

func (c *sa1) bwWrite(i, v int) {
	i &= c.bwMask
	if !c.bwWritable(i) {
		return
	}
	c.bw[i] = byte(v)
}

func (c *sa1) bitmapRead(p int) int {
	if c.bbf&0x80 != 0 {
		s := (p & 3) << 1
		return int(c.bw[(p>>2)&c.bwMask]) >> s & 3
	}
	s := (p & 1) << 2
	return int(c.bw[(p>>1)&c.bwMask]) >> s & 15
}

func (c *sa1) bitmapWrite(p, v int) {
	var i, m, s int
	if c.bbf&0x80 != 0 {
		i, s, m = (p>>2)&c.bwMask, (p&3)<<1, 3
	} else {
		i, s, m = (p>>1)&c.bwMask, (p&1)<<2, 15
	}
	c.bwWrite(i, int(c.bw[i])&^(m<<s)|(v&m)<<s)
}

// ---------- the SA-1 CPU's bus ----------

func (c *sa1) read(a int) uint8 {
	bank, off := a>>16, a&0xffff
	v := -1
	if bank&0x40 == 0 {
		if off >= 0x8000 {
			c.clock += 2
			if bank == 0 && off >= 0xffea {
				switch off {
				case 0xfffc:
					v = c.crv & 0xff
				case 0xfffd:
					v = c.crv >> 8
				case 0xffea, 0xfffa:
					v = c.cnv & 0xff
				case 0xffeb, 0xfffb:
					v = c.cnv >> 8
				case 0xffee, 0xfffe:
					v = c.civ & 0xff
				case 0xffef, 0xffff:
					v = c.civ >> 8
				}
			}
			if v < 0 {
				v = c.romLo(bank, off)
			}
		} else if off < 0x0800 {
			c.clock += 2
			v = int(c.iram[off])
		} else if off >= 0x3000 && off < 0x3800 {
			c.clock += 2
			v = int(c.iram[off&0x7ff])
		} else if off >= 0x2200 && off < 0x2400 {
			c.clock += 2
			v = c.ioRead(off, false)
		} else if off >= 0x6000 {
			c.clock += 4
			if c.bmap&0x80 != 0 {
				v = c.bitmapRead((c.bmap&0x7f)<<13 | off&0x1fff)
			} else {
				v = int(c.bw[((c.bmap&0x1f)<<13|off&0x1fff)&c.bwMask])
			}
		} else {
			c.clock += 2
			v = c.mdr
		}
	} else if bank >= 0xc0 {
		c.clock += 2
		v = c.romHi(bank, off)
	} else if bank < 0x50 {
		c.clock += 4
		v = int(c.bw[((bank&0x0f)<<16|off)&c.bwMask])
	} else if bank >= 0x60 && bank < 0x70 {
		c.clock += 4
		v = c.bitmapRead((bank&0x0f)<<16 | off)
	} else {
		c.clock += 2
		v = c.mdr
	}
	c.mdr = v
	return uint8(v)
}

func (c *sa1) write(a int, v8 uint8) {
	bank, off := a>>16, a&0xffff
	v := int(v8)
	c.mdr = v
	if bank&0x40 == 0 {
		if off >= 0x8000 {
			c.clock += 2
			return
		}
		if off < 0x0800 || (off >= 0x3000 && off < 0x3800) {
			c.clock += 2
			if c.ciwp&(1<<((off>>8)&7)) != 0 {
				c.iram[off&0x7ff] = v8
			}
			return
		}
		if off >= 0x2200 && off < 0x2400 {
			c.clock += 2
			c.ioWrite(off, v)
			return
		}
		if off >= 0x6000 {
			c.clock += 4
			if c.bmap&0x80 != 0 {
				c.bitmapWrite((c.bmap&0x7f)<<13|off&0x1fff, v)
			} else {
				c.bwWrite((c.bmap&0x1f)<<13|off&0x1fff, v)
			}
			return
		}
		c.clock += 2
		return
	}
	if bank >= 0x40 && bank < 0x50 {
		c.clock += 4
		c.bwWrite((bank&0x0f)<<16|off, v)
		return
	}
	if bank >= 0x60 && bank < 0x70 {
		c.clock += 4
		c.bitmapWrite((bank&0x0f)<<16|off, v)
		return
	}
	c.clock += 2
}

// ---------- the SNES CPU's view of the cartridge ----------

func (c *sa1) snesRead(bank, off int) int {
	if bank&0x40 == 0 {
		if off >= 0x8000 {
			if bank == 0 && off >= 0xffea && off <= 0xffef {
				if c.scnt&0x10 != 0 && off == 0xffea {
					return c.snv & 0xff
				}
				if c.scnt&0x10 != 0 && off == 0xffeb {
					return c.snv >> 8
				}
				if c.scnt&0x40 != 0 && off == 0xffee {
					return c.siv & 0xff
				}
				if c.scnt&0x40 != 0 && off == 0xffef {
					return c.siv >> 8
				}
			}
			return c.romLo(bank, off)
		}
		if off >= 0x6000 {
			c.catchUp(c.snes.clock)
			return c.snesBw((c.bmaps&0x1f)<<13 | off&0x1fff)
		}
		return -1
	}
	if bank >= 0xc0 {
		return c.romHi(bank, off)
	}
	if bank < 0x50 {
		c.catchUp(c.snes.clock)
		return c.snesBw((bank&0x0f)<<16 | off)
	}
	return -1
}

func (c *sa1) snesBw(i int) int {
	if c.cc1 {
		if v := c.cc1Byte(i); v >= 0 {
			return v
		}
	}
	return int(c.bw[i&c.bwMask])
}

func (c *sa1) snesWrite(bank, off int, v uint8) {
	if bank&0x40 == 0 {
		if off >= 0x6000 && off < 0x8000 {
			c.catchUp(c.snes.clock)
			c.bwWrite((c.bmaps&0x1f)<<13|off&0x1fff, int(v))
		}
		return
	}
	if bank < 0x50 {
		c.catchUp(c.snes.clock)
		c.bwWrite((bank&0x0f)<<16|off, int(v))
	}
}

// snesIO: $2200-$3FFF in banks 00-3F / 80-BF on the SNES side.
func (c *sa1) snesIO(off int, write bool, v uint8) uint8 {
	if off >= 0x2400 && (off < 0x3000 || off >= 0x3800) {
		if write {
			return 0
		}
		return c.snes.mdr
	}
	c.catchUp(c.snes.clock)
	if off >= 0x3000 {
		if !write {
			return c.iram[off&0x7ff]
		}
		if c.siwp&(1<<((off>>8)&7)) != 0 {
			c.iram[off&0x7ff] = v
		}
		return 0
	}
	if write {
		c.ioWrite(off, int(v))
		return 0
	}
	return uint8(c.ioRead(off, true))
}

// ---------- interrupt lines ----------

func (c *sa1) updateIrqs() {
	c.cpu.IRQLine = (c.cfrIrq && c.cie&0x80 != 0) || (c.cfrTimer && c.cie&0x40 != 0) || (c.cfrDma && c.cie&0x20 != 0)
	s := (c.sfrIrq && c.sie&0x80 != 0) || (c.sfrDma && c.sie&0x20 != 0)
	if s != c.snesIrq {
		c.snesIrq = s
		c.snes.setIrqLine()
	}
}

// ---------- registers ----------

func (c *sa1) ioRead(off int, fromSnes bool) int {
	switch off {
	case 0x2300:
		return b2i(c.sfrIrq)<<7 | c.scnt&0x40 | b2i(c.sfrDma)<<5 | c.scnt&0x10 | c.scnt&0x0f
	case 0x2301:
		return b2i(c.cfrIrq)<<7 | b2i(c.cfrTimer)<<6 | b2i(c.cfrDma)<<5 | b2i(c.cfrNmi)<<4 | c.ccnt&0x0f
	case 0x2302:
		c.latchTimer()
		return c.hLatch & 0xff
	case 0x2303:
		return c.hLatch >> 8
	case 0x2304:
		return c.vLatch & 0xff
	case 0x2305:
		return c.vLatch >> 8
	case 0x2306, 0x2307, 0x2308, 0x2309, 0x230a:
		u := c.mr
		if u < 0 {
			u += two40
		}
		return u >> ((off - 0x2306) * 8) & 0xff
	case 0x230b:
		if c.of != 0 {
			return 0x80
		}
		return 0
	case 0x230c:
		return c.vdp() & 0xff
	case 0x230d:
		v := c.vdp() >> 8
		if c.vbd&0x80 != 0 {
			c.vAdvance()
		}
		return v
	case 0x230e:
		return 0x23
	}
	if fromSnes {
		return int(c.snes.mdr)
	}
	return c.mdr
}

func (c *sa1) ioWrite(off, v int) {
	switch off {
	case 0x2200:
		was := c.ccnt
		c.ccnt = v
		if v&0x80 != 0 {
			c.cfrIrq = true
		}
		if v&0x10 != 0 {
			c.cfrNmi = true
			if c.cie&0x10 != 0 {
				c.cpu.NMIPending = true
			}
		}
		if was&0x20 != 0 && v&0x20 == 0 {
			c.clock = max(c.clock, c.snes.clock)
			c.running = true
			c.cpu.Reset()
			c.timerBase = c.clock
		}
		c.updateIrqs()
		return
	case 0x2201:
		c.sie = v
		c.updateIrqs()
		return
	case 0x2202:
		if v&0x80 != 0 {
			c.sfrIrq = false
		}
		if v&0x20 != 0 {
			c.sfrDma = false
		}
		c.updateIrqs()
		return
	case 0x2203:
		c.crv = c.crv&0xff00 | v
		return
	case 0x2204:
		c.crv = c.crv&0xff | v<<8
		return
	case 0x2205:
		c.cnv = c.cnv&0xff00 | v
		return
	case 0x2206:
		c.cnv = c.cnv&0xff | v<<8
		return
	case 0x2207:
		c.civ = c.civ&0xff00 | v
		return
	case 0x2208:
		c.civ = c.civ&0xff | v<<8
		return
	case 0x2220, 0x2221, 0x2222, 0x2223:
		c.mmc[off-0x2220] = v
		return
	case 0x2224:
		c.bmaps = v
		return
	case 0x2226:
		c.sbwe = v
		return
	case 0x2228:
		c.bwpa = v
		return
	case 0x2229:
		c.siwp = v
		return
	case 0x2209:
		c.scnt = v
		if v&0x80 != 0 {
			c.sfrIrq = true
		}
		c.updateIrqs()
		return
	case 0x220a:
		was := c.cie
		c.cie = v
		if was&0x10 == 0 && v&0x10 != 0 && c.cfrNmi {
			c.cpu.NMIPending = true
		}
		c.updateIrqs()
		return
	case 0x220b:
		if v&0x80 != 0 {
			c.cfrIrq = false
		}
		if v&0x40 != 0 {
			c.cfrTimer = false
		}
		if v&0x20 != 0 {
			c.cfrDma = false
		}
		if v&0x10 != 0 {
			c.cfrNmi = false
		}
		c.updateIrqs()
		return
	case 0x220c:
		c.snv = c.snv&0xff00 | v
		return
	case 0x220d:
		c.snv = c.snv&0xff | v<<8
		return
	case 0x220e:
		c.siv = c.siv&0xff00 | v
		return
	case 0x220f:
		c.siv = c.siv&0xff | v<<8
		return
	case 0x2210:
		c.tmc = v
		return
	case 0x2211:
		c.timerBase = c.clock
		return
	case 0x2212:
		c.hcnt = c.hcnt&0x100 | v
		return
	case 0x2213:
		c.hcnt = c.hcnt&0xff | (v&1)<<8
		return
	case 0x2214:
		c.vcnt = c.vcnt&0x100 | v
		return
	case 0x2215:
		c.vcnt = c.vcnt&0xff | (v&1)<<8
		return
	case 0x2225:
		c.bmap = v
		return
	case 0x2227:
		c.cbwe = v
		return
	case 0x222a:
		c.ciwp = v
		return
	case 0x2230:
		c.dcnt = v
		c.cc2Line = 0
		if v&0x80 == 0 {
			c.cc1 = false
		}
		return
	case 0x2231:
		c.cdma = v
		if v&0x80 != 0 {
			c.cc1 = false
		}
		return
	case 0x2232:
		c.sda = c.sda&0xffff00 | v
		return
	case 0x2233:
		c.sda = c.sda&0xff00ff | v<<8
		return
	case 0x2234:
		c.sda = c.sda&0x00ffff | v<<16
		return
	case 0x2235:
		c.dda = c.dda&0xffff00 | v
		c.cc2Line = 0
		return
	case 0x2236:
		c.dda = c.dda&0xff00ff | v<<8
		if c.dcnt&0x80 != 0 {
			if c.dcnt&0x20 != 0 {
				if c.dcnt&0x10 != 0 {
					c.startCc1()
				}
			} else if c.dcnt&0x04 == 0 {
				c.normalDma()
			}
		}
		return
	case 0x2237:
		c.dda = c.dda&0x00ffff | v<<16
		if c.dcnt&0xa0 == 0x80 && c.dcnt&0x04 != 0 {
			c.normalDma()
		}
		return
	case 0x2238:
		c.dtc = c.dtc&0xff00 | v
		return
	case 0x2239:
		c.dtc = c.dtc&0xff | v<<8
		return
	case 0x223f:
		c.bbf = v
		return
	case 0x2250:
		c.mcnt = v
		if v&2 != 0 {
			c.mr, c.of = 0, 0
		}
		return
	case 0x2251:
		c.ma = c.ma&0xff00 | v
		return
	case 0x2252:
		c.ma = c.ma&0xff | v<<8
		return
	case 0x2253:
		c.mb = c.mb&0xff00 | v
		return
	case 0x2254:
		c.mb = c.mb&0xff | v<<8
		c.arith()
		return
	case 0x2258:
		c.vbd = v
		if v&0x80 == 0 {
			c.vAdvance()
		}
		return
	case 0x2259:
		c.vda = c.vda&0xffff00 | v
		return
	case 0x225a:
		c.vda = c.vda&0xff00ff | v<<8
		return
	case 0x225b:
		c.vda = c.vda&0x00ffff | v<<16
		c.vbit = 0
		return
	}
	if off >= 0x2240 && off <= 0x224f {
		c.brf[off&15] = byte(v)
		if off&7 == 7 && c.dcnt&0xb0 == 0xa0 {
			c.cc2Row(off & 8)
		}
	}
}

// ---------- arithmetic ----------

func (c *sa1) arith() {
	a := s16(c.ma)
	if c.mcnt&2 != 0 {
		b := s16(c.mb)
		s := c.mr + a*b
		if s >= two39 || s < -two39 {
			c.of = 1
			if s >= two39 {
				s -= two40
			} else {
				s += two40
			}
		}
		c.mr = s
		c.mb = 0
	} else if c.mcnt&1 != 0 {
		b := c.mb
		q, r := 0, 0
		if b != 0 {
			q = floorDiv(a, b)
			r = a - q*b
		}
		c.mr = q&0xffff + (r&0xffff)*65536
		c.ma, c.mb = 0, 0
	} else {
		c.mr = a * s16(c.mb)
		c.mb = 0
	}
}

// ---------- bit reader ----------

func (c *sa1) vdp() int {
	a := c.vda
	d := c.romByte(a) | c.romByte((a+1)&0xffffff)<<8 | c.romByte((a+2)&0xffffff)<<16
	return d >> c.vbit & 0xffff
}

func (c *sa1) vAdvance() {
	n := c.vbd & 15
	if n == 0 {
		n = 16
	}
	c.vbit += n
	c.vda = (c.vda + c.vbit>>3) & 0xffffff
	c.vbit &= 7
}

// ---------- DMA ----------

func (c *sa1) normalDma() {
	src, toBw := c.dcnt&3, c.dcnt&4 != 0
	s, d, n := c.sda, c.dda, c.dtc
	for i := 0; i < n; i++ {
		var v int
		switch src {
		case 0:
			v = c.romByte(s)
		case 1:
			v = int(c.bw[s&c.bwMask])
		default:
			v = int(c.iram[s&0x7ff])
		}
		if toBw {
			c.bwWrite(d&0x3ffff, v)
		} else {
			c.iram[d&0x7ff] = byte(v)
		}
		s, d = (s+1)&0xffffff, (d+1)&0xffffff
	}
	if src == 1 || toBw {
		c.clock += n * 4
	} else {
		c.clock += n * 2
	}
	c.cfrDma = true
	c.updateIrqs()
}

func (c *sa1) tileInfo() (bpp, w, tileBytes int) {
	switch c.cdma & 3 {
	case 0:
		bpp = 8
	case 1:
		bpp = 4
	default:
		bpp = 2
	}
	w = 1 << min(5, (c.cdma>>2)&7)
	return bpp, w, bpp * 8
}

func (c *sa1) startCc1() {
	c.cc1 = true
	c.sfrDma = true
	c.updateIrqs()
}

func (c *sa1) cc1Byte(i int) int {
	base := c.sda & 0x3ffff
	rel := i&c.bwMask - base&c.bwMask
	if rel < 0 {
		return -1
	}
	bpp, w, tileBytes := c.tileInfo()
	t, b := rel/tileBytes, rel%tileBytes
	tx, ty := t%w, t/w
	row, plane := (b>>1)&7, (b>>4)<<1|b&1
	lineAddr := base + (ty*8+row)*(w*bpp) + tx*bpp
	out := 0
	for x := 0; x < 8; x++ {
		bitPos := x * bpp
		by := int(c.bw[(lineAddr+bitPos>>3)&c.bwMask])
		px := by >> (bitPos & 7) & (1<<bpp - 1)
		out |= (px >> plane & 1) << (7 - x)
	}
	c.iram[(c.dda&0x7ff+(t&1)*tileBytes+b)&0x7ff] = byte(out)
	return out
}

func (c *sa1) cc2Row(half int) {
	bpp, _, tileBytes := c.tileInfo()
	line := c.cc2Line & 15
	base := c.dda&0x7ff + (line>>3)*tileBytes + (line&7)*2
	mask := 1<<bpp - 1
	for p := 0; p < bpp; p++ {
		by := 0
		for x := 0; x < 8; x++ {
			by |= ((int(c.brf[half+x]) & mask) >> p & 1) << (7 - x)
		}
		c.iram[(base+(p>>1)*16+p&1)&0x7ff] = byte(by)
	}
	c.cc2Line = (c.cc2Line + 1) & 15
	c.clock += 2 * bpp
}

// ---------- H/V timer ----------

func (c *sa1) timerGeom() (L, F int) {
	if c.tmc&0x80 != 0 {
		return 512, 512 * 512
	}
	return 341, 341 * c.snes.lines
}

func (c *sa1) timerTarget() (t, P int) {
	en := c.tmc & 3
	if en == 0 {
		return 0, 0
	}
	L, F := c.timerGeom()
	switch en {
	case 1:
		return c.hcnt, L
	case 2:
		return c.vcnt * L, F
	}
	return c.vcnt*L + c.hcnt, F
}

func (c *sa1) latchTimer() {
	L, F := c.timerGeom()
	d := floorDiv(c.clock-c.timerBase, 4) % F
	c.hLatch, c.vLatch = d%L, floorDiv(d, L)
}

func (c *sa1) checkTimer(c0, c1 int) {
	t, P := c.timerTarget()
	if P == 0 {
		return
	}
	d0, d1 := floorDiv(c0-c.timerBase, 4), floorDiv(c1-c.timerBase, 4)
	if floorDiv(d1-t, P) > floorDiv(d0-t, P) {
		c.cfrTimer = true
		c.updateIrqs()
	}
}

func (c *sa1) nextTimerClock(from int) int {
	t, P := c.timerTarget()
	if P == 0 {
		return never
	}
	d := floorDiv(from-c.timerBase, 4)
	k := floorDiv(d-t, P) + 1
	return c.timerBase + (t+k*P)*4
}

// ---------- running ----------

func (c *sa1) catchUp(target int) {
	if c.clock >= target {
		return
	}
	if c.ccnt&0x60 != 0 {
		c.checkTimer(c.clock, target)
		c.clock = target
		return
	}
	cpu, timer := c.cpu, c.tmc&3 != 0
	for c.clock < target {
		if cpu.Waiting() && !cpu.IRQLine && !cpu.NMIPending {
			next := target
			if timer {
				next = min(target, c.nextTimerClock(c.clock))
			}
			c0 := c.clock
			c.clock = max(next, c0+2)
			if timer {
				c.checkTimer(c0, c.clock)
			}
			continue
		}
		c0 := c.clock
		cpu.Step()
		if timer {
			c.checkTimer(c0, c.clock)
		}
	}
}

// ---------- save states ----------

func (c *sa1) state(s *sio) {
	cs := c.cpu.GetState()
	s.IntSlice(cs[:])
	if !s.save {
		c.cpu.SetState(cs)
	}
	s.Bytes(c.iram[:])
	s.Bytes(c.brf[:])
	if c.ownBw {
		s.Bytes(c.bw)
	}
	s.IntSlice(c.mmc[:])
	s.Ints(&c.clock, &c.mdr, &c.ccnt, &c.sie, &c.crv, &c.cnv, &c.civ, &c.bmaps, &c.sbwe, &c.bwpa, &c.siwp,
		&c.scnt, &c.cie, &c.snv, &c.siv, &c.tmc, &c.hcnt, &c.vcnt, &c.bmap, &c.cbwe, &c.ciwp,
		&c.dcnt, &c.cdma, &c.sda, &c.dda, &c.dtc, &c.bbf, &c.mcnt, &c.ma, &c.mb, &c.mr, &c.of,
		&c.vbd, &c.vda, &c.vbit)
	s.Bools(&c.sfrIrq, &c.sfrDma, &c.cfrIrq, &c.cfrTimer, &c.cfrDma, &c.cfrNmi, &c.cc1)
	s.Ints(&c.cc2Line, &c.timerBase, &c.hLatch, &c.vLatch)
	s.Bool(&c.running)
	if !s.save {
		c.snesIrq = (c.sfrIrq && c.sie&0x80 != 0) || (c.sfrDma && c.sie&0x20 != 0)
	}
}
