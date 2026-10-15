package snes

import (
	"encoding/binary"
	"math"
	"time"
)

// SPC7110 (web/emu/snes_spc7110.js): graphics decompressor, data ROM port,
// multiply / divide unit, data ROM windows, battery RAM, and the Epson
// RTC-4513 clock of Far East of Eden Zero.

var spcEvolution = [53][4]uint8{
	{0x5a, 1, 1, 1}, {0x25, 6, 2, 0}, {0x11, 8, 3, 0}, {0x08, 10, 4, 0}, {0x03, 12, 5, 0}, {0x01, 15, 5, 0},
	{0x5a, 7, 7, 1}, {0x3f, 19, 8, 0}, {0x2c, 21, 9, 0}, {0x20, 22, 10, 0}, {0x17, 23, 11, 0}, {0x11, 25, 12, 0}, {0x0c, 26, 13, 0}, {0x09, 28, 14, 0},
	{0x07, 29, 15, 0}, {0x05, 31, 16, 0}, {0x04, 32, 17, 0}, {0x03, 34, 18, 0}, {0x02, 35, 5, 0},
	{0x5a, 20, 20, 1}, {0x48, 39, 21, 0}, {0x3a, 40, 22, 0}, {0x2e, 42, 23, 0}, {0x26, 44, 24, 0}, {0x1f, 45, 25, 0}, {0x19, 46, 26, 0}, {0x15, 25, 27, 0},
	{0x11, 26, 28, 0}, {0x0e, 26, 29, 0}, {0x0b, 27, 30, 0}, {0x09, 28, 31, 0}, {0x08, 29, 32, 0}, {0x07, 30, 33, 0}, {0x05, 31, 34, 0}, {0x04, 33, 35, 0},
	{0x04, 33, 36, 0}, {0x03, 34, 37, 0}, {0x02, 35, 38, 0}, {0x02, 36, 5, 0},
	{0x58, 39, 40, 1}, {0x4d, 47, 41, 0}, {0x43, 48, 42, 0}, {0x3b, 49, 43, 0}, {0x34, 50, 44, 0}, {0x2e, 51, 45, 0}, {0x29, 44, 46, 0}, {0x25, 45, 24, 0},
	{0x56, 47, 48, 1}, {0x4f, 47, 49, 0}, {0x47, 48, 50, 0}, {0x41, 49, 51, 0}, {0x3c, 50, 52, 0}, {0x37, 51, 43, 0},
}

func spcShape(a, b, c int) int {
	if a == b {
		if b == c {
			return 0
		}
		return 1
	}
	if b == c {
		return 2
	}
	if a == c {
		return 3
	}
	return 4
}

func spcCtxMode2(k, path, ref int) int {
	switch k {
	case 0:
		return 0
	case 1:
		return 1 + path&1
	}
	p0 := (path >> (k - 1)) & 1
	if k == 2 {
		if p0 != 0 {
			return 13 + path&1
		}
		if path&1 != 0 {
			return 8 + ref
		}
		return 3 + ref
	}
	p1, last := (path>>1)&1, path&1
	if p0 == 0 {
		if p1 != 0 {
			return 25 + last
		}
		return 15 + 2*ref + last
	}
	return 27 + 2*p1 + last
}

// spcBump moves v to the front of a most-recently-used list.
func spcBump(list []uint8, v int) {
	i := -1
	for k, x := range list {
		if int(x) == v {
			i = k
			break
		}
	}
	if i > 0 {
		copy(list[1:i+1], list[:i])
	}
	list[0] = uint8(v)
}

func spcPlanes(bits uint32, bpp int, out []uint8) []uint8 {
	var pl [4]uint8
	for i := 0; i < 8; i++ {
		v := (bits >> ((7 - i) * bpp)) & (1<<bpp - 1)
		for k := 0; k < bpp; k++ {
			pl[k] |= uint8((v>>k)&1) << (7 - i)
		}
	}
	return append(out, pl[:bpp]...)
}

type spcDec struct {
	readByte func(a int) int
	idx, inv [32]uint8
	queue    [32]uint8 // decoded bytes waiting: queue[qpos:qlen]
	qpos     int
	qlen     int
	mode     int
	addr     int
	ready    bool

	code, inByte, inLeft, rng int
	out, out1                 uint32
	order                     []uint8
}

func (d *spcDec) start(mode, addr, skip int) {
	d.mode = mode
	if mode > 2 {
		d.mode = 0
	}
	d.addr = addr
	d.idx, d.inv = [32]uint8{}, [32]uint8{}
	d.qpos, d.qlen = 0, 0
	d.code = d.nextIn()
	d.inByte = d.nextIn()
	d.inLeft, d.rng = 8, 0xff
	d.out, d.out1 = 0, 0
	if d.mode == 2 {
		d.order = []uint8{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
	} else {
		d.order = []uint8{0, 1, 2, 3}
	}
	d.ready = true
	for i := 0; i < skip; i++ {
		d.next()
	}
}

func (d *spcDec) nextIn() int {
	v := d.readByte(d.addr)
	d.addr = (d.addr + 1) & 0xffffff
	return v
}

func (d *spcDec) decide(con int) int {
	e := &spcEvolution[d.idx[con]]
	prob := int(e[0])
	split := d.rng - prob
	lps := 0
	if d.code > split {
		lps = 1
		d.code -= split + 1
		d.rng = prob - 1
	} else {
		d.rng = split
	}
	renorm := false
	for d.rng < 0x7f {
		renorm = true
		d.rng = d.rng<<1 | 1
		d.code = (d.code<<1 | d.inByte>>7) & 0xff
		d.inByte = (d.inByte << 1) & 0xff
		d.inLeft--
		if d.inLeft == 0 {
			d.inByte = d.nextIn()
			d.inLeft = 8
		}
	}
	if lps != 0 {
		d.inv[con] ^= e[3]
		d.idx[con] = e[1]
	} else if renorm {
		d.idx[con] = e[2]
	}
	return lps
}

func (d *spcDec) bitIn(con int) int {
	inv := int(d.inv[con])
	return d.decide(con) ^ inv
}

func (d *spcDec) next() uint8 {
	if !d.ready {
		return 0
	}
	if d.qpos == d.qlen {
		d.qpos, d.qlen = 0, 0
		d.fill()
	}
	v := d.queue[d.qpos]
	d.qpos++
	return v
}

func (d *spcDec) pixelOrder(a, b, c int, r []uint8) []uint8 {
	spcBump(d.order, a)
	r = append(r[:0], d.order...)
	spcBump(r, c)
	spcBump(r, b)
	spcBump(r, a)
	return r
}

func (d *spcDec) fill() {
	q := d.queue[:0]
	var rbuf [16]uint8
	switch d.mode {
	case 0:
		for half := 0; half < 2; half++ {
			node := 0
			for k := 0; k < 4; k++ {
				b := d.bitIn(node + half*15)
				d.out = d.out<<1 | (uint32(b) ^ (d.out>>15)&1)
				node = 2*node + 1 + b
			}
		}
		q = append(q, uint8(d.out))
	case 1:
		for px := 0; px < 8; px++ {
			a, b, c := int(d.out&3), int(d.out>>14&3), int(d.out>>16&3)
			r := d.pixelOrder(a, b, c, rbuf[:0])
			sh := spcShape(a, b, c)
			d0 := d.bitIn(sh)
			d1 := d.bitIn(5 + 2*sh + d0)
			d.out = d.out<<2 | uint32(r[d0<<1|d1])
		}
		q = spcPlanes(d.out, 2, q)
	default:
		var hi []uint8
		for row := 0; row < 8; row++ {
			for px := 0; px < 8; px++ {
				a, b, c := int(d.out&15), int(d.out>>28&15), int(d.out1&15)
				r := d.pixelOrder(a, b, c, rbuf[:0])
				sh := spcShape(a, b, c)
				path := 0
				for k := 0; k < 4; k++ {
					path = path<<1 | d.bitIn(spcCtxMode2(k, path, sh))
				}
				d.out1 = d.out1<<4 | d.out>>28
				d.out = d.out<<4 | uint32(r[path])
			}
			var pl [4]uint8
			spcPlanes(d.out, 4, pl[:0])
			q = append(q, pl[0], pl[1])
			hi = append(hi, pl[2], pl[3])
		}
		q = append(q, hi...)
	}
	d.qlen = len(q)
}

func (d *spcDec) save(s *sio) {
	s.Ints(&d.mode, &d.addr, &d.code, &d.inByte, &d.inLeft, &d.rng)
	s.Bool(&d.ready)
	s.Bytes(d.idx[:])
	s.Bytes(d.inv[:])
	o1, o2 := int(d.out), int(d.out1)
	s.Ints(&o1, &o2)
	d.out, d.out1 = uint32(o1), uint32(o2)
	s.Ints(&d.qpos, &d.qlen)
	s.Bytes(d.queue[:])
	ord := make([]int, len(d.order))
	for i, v := range d.order {
		ord[i] = int(v)
	}
	s.IntVar(&ord)
	d.order = d.order[:0]
	for _, v := range ord {
		d.order = append(d.order, uint8(v))
	}
}

// ---------------------------------------------------------------------------
// Epson RTC-4513

type rtc4513 struct {
	now    func() float64
	offset int
	ctl    [3]int
	ce     int
	st     int
	mode   int
	index  int
	regs   [16]uint8
	dirty  bool
}

func newRTC4513(now func() float64) *rtc4513 {
	r := &rtc4513{now: now, ctl: [3]int{0, 0, 4}}
	r.reset()
	return r
}

func (r *rtc4513) reset() {
	r.ce, r.st, r.mode, r.index = 0, 0, 0, 0
	r.regs = [16]uint8{}
	r.dirty = false
}

func (r *rtc4513) seconds() int { return int(math.Floor(r.now()/1000)) + r.offset }

func (r *rtc4513) latch() {
	d := time.Unix(int64(r.seconds()), 0).UTC()
	g := &r.regs
	sec, mn, hour, day, mon, yr := d.Second(), d.Minute(), d.Hour(), d.Day(), int(d.Month()), d.Year()%100
	h, pm := hour, 0
	if r.ctl[2]&4 == 0 {
		if hour >= 12 {
			pm = 4
		}
		h = hour % 12
	}
	vals := [13]int{sec % 10, sec / 10, mn % 10, mn / 10, h % 10, h/10 | pm, day % 10, day / 10, mon % 10, mon / 10, yr % 10, yr / 10, int(d.Weekday())}
	for i, v := range vals {
		g[i] = uint8(v)
	}
	g[13], g[14], g[15] = uint8(r.ctl[0]), uint8(r.ctl[1]), uint8(r.ctl[2])
}

func (r *rtc4513) commit() {
	g := &r.regs
	r.ctl = [3]int{int(g[13] & 15), int(g[14] & 15), int(g[15] & 15)}
	hour := int(g[4]) + int(g[5]&3)*10
	if r.ctl[2]&4 == 0 {
		hour = hour % 12
		if g[5]&4 != 0 {
			hour += 12
		}
	}
	yr := int(g[10]) + int(g[11])*10
	if yr >= 90 {
		yr += 1900
	} else {
		yr += 2000
	}
	mon := max(0, int(g[8])+int(g[9]&1)*10-1)
	day := int(g[6]) + int(g[7]&3)*10
	if day == 0 {
		day = 1
	}
	t := time.Date(yr, time.Month(mon+1), day, hour, int(g[2])+int(g[3]&7)*10, int(g[0])+int(g[1]&7)*10, 0, time.UTC)
	r.offset = int(t.Unix()) - int(math.Floor(r.now()/1000))
}

func (r *rtc4513) setCE(v uint8) {
	ce := int(v & 1)
	if ce != 0 && r.ce == 0 {
		r.st = 1
		r.dirty = false
	}
	if ce == 0 && r.ce != 0 {
		if r.dirty {
			r.commit()
		}
		r.st = 0
	}
	r.ce = ce
}

func (r *rtc4513) write(v uint8) {
	v &= 15
	if r.ce == 0 {
		return
	}
	switch {
	case r.st == 1:
		if v == 0x03 || v == 0x0c {
			r.mode = int(v)
			r.st = 2
		}
	case r.st == 2:
		r.index = int(v)
		r.st = 3
		r.latch()
	case r.st == 3 && r.mode == 0x03:
		r.regs[r.index] = v
		r.index = (r.index + 1) & 15
		r.dirty = true
	}
}

func (r *rtc4513) read() int {
	if r.ce == 0 || r.st != 3 || r.mode != 0x0c {
		return 0
	}
	v := r.regs[r.index]
	r.index = (r.index + 1) & 15
	return int(v)
}

func (r *rtc4513) bytes() []byte {
	o := make([]byte, 8)
	binary.LittleEndian.PutUint32(o, uint32(int32(r.offset)))
	o[4] = 0x52
	o[5], o[6], o[7] = uint8(r.ctl[0]), uint8(r.ctl[1]), uint8(r.ctl[2])
	return o
}

func (r *rtc4513) fromBytes(b []byte) {
	if len(b) >= 8 && b[4] == 0x52 {
		r.offset = int(int32(binary.LittleEndian.Uint32(b)))
		r.ctl = [3]int{int(b[5]), int(b[6]), int(b[7])}
	}
}

func (r *rtc4513) save(s *sio) {
	s.Ints(&r.offset, &r.ctl[0], &r.ctl[1], &r.ctl[2], &r.ce, &r.st, &r.mode, &r.index)
	s.Bytes(r.regs[:])
	s.Bool(&r.dirty)
}

// ---------------------------------------------------------------------------
// the chip

type spc7110 struct {
	s                *SNES
	c                *cart
	dataSize         int
	dec              spcDec
	rtc              *rtc4513
	r                [0x50]uint8
	portSet          int
	latch14, latch15 bool
}

func newSPC7110(s *SNES, o *chipOpts) coproc {
	c := &spc7110{s: s, c: s.cart}
	c.dataSize = max(1, len(c.c.rom)-0x100000)
	c.dec.readByte = c.dataByte
	if c.c.chip == 0xf9 {
		c.rtc = newRTC4513(s.now)
		if n := c.c.sramSize; o != nil && len(o.sram) >= n+8 {
			c.rtc.fromBytes(o.sram[n : n+8])
		}
	}
	s.claim(0x00, 0x3f, 0x4000, 0x4fff, true)
	s.claim(0x00, 0x3f, 0x6000, 0xffff, true)
	s.claim(0x40, 0x4f, 0x0000, 0xffff, false)
	s.claim(0x50, 0x50, 0x0000, 0xffff, false)
	s.claim(0xc0, 0xff, 0x0000, 0xffff, false)
	c.reset()
	return c
}

func (c *spc7110) reset() {
	c.r = [0x50]uint8{}
	c.r[0x31], c.r[0x32], c.r[0x33] = 0, 1, 2
	c.portSet = 0
	c.latch14, c.latch15 = false, false
	c.dec.ready = false
	if c.rtc != nil {
		c.rtc.reset()
	}
}

func (c *spc7110) dataByte(a int) int {
	if i := 0x100000 + a%c.dataSize; i < len(c.c.rom) {
		return int(c.c.rom[i])
	}
	return 0
}

func (c *spc7110) progByte(a int) int {
	if i := a & 0xfffff; i < len(c.c.rom) {
		return int(c.c.rom[i])
	}
	return 0
}

func (c *spc7110) ptr() int { return int(c.r[0x11]) | int(c.r[0x12])<<8 | int(c.r[0x13])<<16 }
func (c *spc7110) setPtr(v int) {
	c.r[0x11], c.r[0x12], c.r[0x13] = uint8(v), uint8(v>>8), uint8(v>>16)
}
func (c *spc7110) adj() int     { return int(c.r[0x14]) | int(c.r[0x15])<<8 }
func (c *spc7110) setAdj(v int) { c.r[0x14], c.r[0x15] = uint8(v), uint8(v>>8) }
func (c *spc7110) stepVal() int { return int(c.r[0x16]) | int(c.r[0x17])<<8 }
func (c *spc7110) sramOn() bool { return c.c.sramSize != 0 && c.r[0x30]&0x80 != 0 }
func (c *spc7110) sramAt(bank, off int) int {
	return ((bank&0x1f)<<13 | (off - 0x6000)) & c.c.sramMask
}

func (c *spc7110) read(bank, off int) int {
	b := bank & 0x7f
	if bank >= 0xd0 {
		return c.dataByte(int(c.r[0x31+(bank-0xd0)>>4]&7)<<20 | (bank&0x0f)<<16 | off)
	}
	if bank >= 0xc0 || (b >= 0x40 && b < 0x50) {
		return c.progByte((bank&0x0f)<<16 | off)
	}
	if b == 0x50 {
		return int(c.decRead())
	}
	if b >= 0x40 {
		return -1
	}
	if off >= 0x8000 {
		return c.progByte((bank&0x0f)<<16 | off)
	}
	if off >= 0x6000 {
		if !c.sramOn() {
			return 0
		}
		return int(c.c.sram[c.sramAt(bank, off)])
	}
	if off >= 0x4800 && off < 0x4850 {
		return c.readReg(off - 0x4800)
	}
	return -1
}

func (c *spc7110) write(bank, off int, v uint8) bool {
	b := bank & 0x7f
	if bank >= 0xc0 || (b >= 0x40 && b <= 0x50) {
		return true
	}
	if b >= 0x40 {
		return false
	}
	if off >= 0x8000 {
		return true
	}
	if off >= 0x6000 {
		if c.sramOn() {
			c.c.sram[c.sramAt(bank, off)] = v
		}
		return true
	}
	if off >= 0x4800 && off < 0x4850 {
		c.writeReg(off-0x4800, v)
		return true
	}
	return false
}

func (c *spc7110) decRead() uint8 {
	n := (int(c.r[0x09]) | int(c.r[0x0a])<<8) - 1
	c.r[0x09], c.r[0x0a] = uint8(n), uint8(n>>8)
	return c.dec.next()
}

func (c *spc7110) decStart() {
	table := int(c.r[0x01]) | int(c.r[0x02])<<8 | int(c.r[0x03])<<16
	ent := table + int(c.r[0x04])<<2
	mode := c.dataByte(ent)
	addr := c.dataByte(ent+1)<<16 | c.dataByte(ent+2)<<8 | c.dataByte(ent+3)
	c.dec.start(mode, addr, (int(c.r[0x05])|int(c.r[0x06])<<8)<<min(2, mode))
	c.r[0x0c] = 0x80
}

func (c *spc7110) portRead() int {
	if c.portSet != 7 {
		return 0
	}
	m, addr := c.r[0x18], c.ptr()
	adj := c.adj()
	if m&8 != 0 {
		adj = s16(adj)
	}
	a := addr
	if m&2 != 0 {
		a = addr + adj
		c.setAdj(adj + 1)
	}
	v := c.dataByte(a & 0xffffff)
	if m&2 == 0 {
		inc := 1
		if m&1 != 0 {
			inc = c.stepVal()
		}
		if m&4 != 0 {
			inc = s16(inc)
		}
		if m&0x10 == 0 {
			c.setPtr(addr + inc)
		} else {
			c.setAdj(adj + inc)
		}
	}
	return v
}

func (c *spc7110) portReadAdjusted() int {
	if c.portSet != 7 {
		return 0
	}
	m, addr := c.r[0x18], c.ptr()
	adj := c.adj()
	if m&8 != 0 {
		adj = s16(adj)
	}
	v := c.dataByte((addr + adj) & 0xffffff)
	if m&0x60 == 0x60 {
		if m&0x10 == 0 {
			c.setPtr(addr + adj)
		} else {
			c.setAdj(adj + adj)
		}
	}
	return v
}

func (c *spc7110) adjustWritten() {
	m := c.r[0x18]
	if !(c.latch14 && c.latch15) || m&2 == 0 || m&0x10 != 0 {
		return
	}
	switch m & 0x60 {
	case 0x20:
		inc := c.adj() & 0xff
		if m&8 != 0 {
			inc = s8(inc)
		}
		c.setPtr(c.ptr() + inc)
	case 0x40:
		inc := c.adj()
		if m&8 != 0 {
			inc = s16(inc)
		}
		c.setPtr(c.ptr() + inc)
	}
}

func (c *spc7110) readReg(i int) int {
	switch i {
	case 0x00:
		return int(c.decRead())
	case 0x0c:
		v := c.r[0x0c]
		c.r[0x0c] &= 0x7f
		return int(v)
	case 0x10:
		return c.portRead()
	case 0x1a:
		return c.portReadAdjusted()
	case 0x2f:
		return 0
	case 0x41:
		if c.rtc != nil {
			return c.rtc.read()
		}
		return 0
	case 0x42:
		return 0x80
	}
	return int(c.r[i])
}

func (c *spc7110) writeReg(i int, v uint8) {
	c.r[i] = v
	switch i {
	case 0x06:
		c.decStart()
	case 0x11:
		c.portSet |= 1
	case 0x12:
		c.portSet |= 2
	case 0x13:
		c.portSet |= 4
	case 0x14:
		c.latch14 = true
		c.adjustWritten()
	case 0x15:
		c.latch15 = true
		c.adjustWritten()
	case 0x18:
		c.latch14, c.latch15 = false, false
	case 0x25:
		c.multiply()
	case 0x27:
		c.divide()
	case 0x2e:
		c.r[0x2e] = v & 1
	case 0x40:
		if c.rtc != nil {
			c.rtc.setCE(v)
		}
	case 0x41:
		if c.rtc != nil {
			c.rtc.write(v)
		}
	}
}

func (c *spc7110) multiply() {
	r := &c.r
	a, b := int(r[0x20])|int(r[0x21])<<8, int(r[0x24])|int(r[0x25])<<8
	if r[0x2e]&1 != 0 {
		a, b = s16(a), s16(b)
	}
	p := uint32(a * b)
	r[0x28], r[0x29], r[0x2a], r[0x2b] = uint8(p), uint8(p>>8), uint8(p>>16), uint8(p>>24)
}

func (c *spc7110) divide() {
	r := &c.r
	n := int(uint32(r[0x20]) | uint32(r[0x21])<<8 | uint32(r[0x22])<<16 | uint32(r[0x23])<<24)
	d := int(r[0x26]) | int(r[0x27])<<8
	if r[0x2e]&1 != 0 {
		n, d = int(int32(n)), s16(d)
	}
	var q, m int
	if d == 0 {
		q, m = 0, n&0xffff
	} else {
		q = n / d
		m = n - q*d
	}
	uq := uint32(q)
	m &= 0xffff
	r[0x28], r[0x29], r[0x2a], r[0x2b] = uint8(uq), uint8(uq>>8), uint8(uq>>16), uint8(uq>>24)
	r[0x2c], r[0x2d] = uint8(m), uint8(m>>8)
}

func (c *spc7110) getSRAM() []byte {
	k := c.c
	var s []byte
	if k.sramSize != 0 {
		s = append([]byte(nil), k.sram[:k.sramSize]...)
	}
	if c.rtc == nil {
		return s
	}
	return append(append([]byte{}, s...), c.rtc.bytes()...)
}

func (c *spc7110) state(s *sio) {
	s.Bytes(c.r[:])
	s.Int(&c.portSet)
	s.Bools(&c.latch14, &c.latch15)
	c.dec.save(s)
	if c.rtc != nil {
		c.rtc.save(s)
	}
}
