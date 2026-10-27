package ps3

// The SPU: a 128-bit SIMD processor with 128 registers and 256 KB of local
// store. This is an interpreter of its instruction set; the MFC (DMA),
// mailboxes, signals and events are channels, run in spu_mfc.go.

import (
	"encoding/binary"
	"math"
	"math/bits"
	"os"
)

const lsSize = 0x40000
const lsMask = lsSize - 1

// SPUCore is one SPU's state.
type SPUCore struct {
	GPR  [128]Vec
	PC   uint32
	LS   []byte
	SRR0 uint32

	Ran     uint64
	stop    bool
	Err     error
	stalled bool // waiting on a channel (retry the same instruction)

	jit   *spuJIT // the recompiler (made on the first Run), or nil
	noJIT bool    // interpreter only

	// channel handlers (set by the owner)
	RdCh    func(c *SPUCore, ch uint32) (uint32, bool) // false = wait
	WrCh    func(c *SPUCore, ch uint32, v uint32) bool // false = wait
	ChCount func(c *SPUCore, ch uint32) uint32
	Stop    func(c *SPUCore, code uint32) bool // false = wait; true = carry on (the handler may stop the core)
	Halt    func(c *SPUCore)
}

func newSPUCore() *SPUCore { return &SPUCore{LS: make([]byte, lsSize)} }

func (c *SPUCore) ls32(a uint32) uint32 {
	a &= lsMask &^ 3
	return uint32(c.LS[a])<<24 | uint32(c.LS[a+1])<<16 | uint32(c.LS[a+2])<<8 | uint32(c.LS[a+3])
}

func (c *SPUCore) setLS32(a, v uint32) {
	a &= lsMask &^ 3
	c.LS[a], c.LS[a+1], c.LS[a+2], c.LS[a+3] = byte(v>>24), byte(v>>16), byte(v>>8), byte(v)
	c.lsWrote(a, 4)
}

// lsWrote tells the recompiler local store changed.
func (c *SPUCore) lsWrote(a uint32, n int) {
	if c.jit != nil {
		c.jit.lsWrote(a, n)
	}
}

func (c *SPUCore) lq(a uint32) Vec {
	a &= lsMask &^ 15
	return Vec{c.ls32(a), c.ls32(a + 4), c.ls32(a + 8), c.ls32(a + 12)}
}

func (c *SPUCore) stq(a uint32, v Vec) {
	a &= lsMask &^ 15
	binary.BigEndian.PutUint32(c.LS[a:], v[0])
	binary.BigEndian.PutUint32(c.LS[a+4:], v[1])
	binary.BigEndian.PutUint32(c.LS[a+8:], v[2])
	binary.BigEndian.PutUint32(c.LS[a+12:], v[3])
	c.lsWrote(a, 16)
}

func (c *SPUCore) fail(msg string, w uint32) {
	if c.Err == nil {
		c.Err = &CPUError{PC: c.PC, Inst: w, Msg: "SPU: " + msg}
	}
	c.stop = true
}

// Run runs up to n instructions (less when the SPU stops or waits on a channel).
func (c *SPUCore) Run(n int) int {
	c.stop = false
	if c.jit == nil && !c.noJIT {
		c.jit = newSPUJIT()
		c.noJIT = c.jit == nil
	}
	if c.jit != nil && spuHist == nil {
		return c.jit.run(c, n)
	}
	ran := 0
	for ran < n && !c.stop {
		pc := c.PC & lsMask &^ 3
		w := uint32(c.LS[pc])<<24 | uint32(c.LS[pc+1])<<16 | uint32(c.LS[pc+2])<<8 | uint32(c.LS[pc+3])
		c.PC = pc + 4
		c.stalled = false
		if spuHist != nil {
			spuHist[pc]++
		}
		c.exec(w)
		if c.stalled {
			c.PC = pc
			c.stop = true
			break
		}
		ran++
	}
	c.Ran += uint64(ran)
	return ran
}

// spuHist (PS3_SPUHIST=1): instructions run at each local store address
// (debugging).
var spuHist = func() []uint64 {
	if os.Getenv("PS3_SPUHIST") == "" {
		return nil
	}
	return make([]uint64, lsSize)
}()

func i7(w uint32) int32    { return int32(w<<11) >> 25 }
func i10(w uint32) int32   { return int32(w<<8) >> 22 }
func i16(w uint32) int32   { return int32(w<<9) >> 16 }
func u16f(w uint32) uint32 { return w >> 7 & 0xffff }
func i18(w uint32) uint32  { return w >> 7 & 0x3ffff }

func splat(v uint32) Vec { return Vec{v, v, v, v} }

func (v Vec) withB(i int, x uint8) Vec {
	sh := 24 - 8*(i&3)
	v[i>>2] = v[i>>2]&^(0xff<<sh) | uint32(x)<<sh
	return v
}

func (v Vec) withH(i int, x uint16) Vec {
	sh := 16 - 16*(i&1)
	v[i>>1] = v[i>>1]&^(0xffff<<sh) | uint32(x)<<sh
	return v
}

func (v Vec) d(i int) uint64 { return uint64(v[2*i])<<32 | uint64(v[2*i+1]) }
func (v Vec) withD(i int, x uint64) Vec {
	v[2*i] = uint32(x >> 32)
	v[2*i+1] = uint32(x)
	return v
}

// quad as two uint64 (hi = bytes 0-7)
func (v Vec) hl() (uint64, uint64) { return v.d(0), v.d(1) }
func fromHL(hi, lo uint64) Vec {
	return Vec{uint32(hi >> 32), uint32(hi), uint32(lo >> 32), uint32(lo)}
}

func shlQuadBits(v Vec, n uint) Vec {
	hi, lo := v.hl()
	if n == 0 {
		return v
	}
	if n >= 128 {
		return Vec{}
	}
	if n >= 64 {
		return fromHL(lo<<(n-64), 0)
	}
	return fromHL(hi<<n|lo>>(64-n), lo<<n)
}

func shrQuadBits(v Vec, n uint) Vec {
	hi, lo := v.hl()
	if n == 0 {
		return v
	}
	if n >= 128 {
		return Vec{}
	}
	if n >= 64 {
		return fromHL(0, hi>>(n-64))
	}
	return fromHL(hi>>n, lo>>n|hi<<(64-n))
}

func rotQuadBits(v Vec, n uint) Vec {
	n &= 127
	if n == 0 {
		return v
	}
	a := shlQuadBits(v, n)
	b := shrQuadBits(v, 128-n)
	return Vec{a[0] | b[0], a[1] | b[1], a[2] | b[2], a[3] | b[3]}
}

// spuF: SPU single precision (no denormals: they're zero)
func spuF(x uint32) float32 {
	if x&0x7f800000 == 0 {
		return math.Float32frombits(x & 0x80000000)
	}
	return math.Float32frombits(x)
}

func spuFOut(f float32) uint32 {
	b := math.Float32bits(f)
	if b&0x7f800000 == 0 {
		return b & 0x80000000
	}
	return b
}

func (c *SPUCore) exec(w uint32) {
	rT := w & 0x7f
	rA := w >> 7 & 0x7f
	rB := w >> 14 & 0x7f
	g := &c.GPR
	// RRR forms (4-bit opcodes)
	switch w >> 28 {
	case 0x8, 0xb, 0xc, 0xd, 0xe, 0xf:
		t := w >> 21 & 0x7f
		rC := w & 0x7f
		a, b, cc := g[rA], g[rB], g[rC]
		var r Vec
		switch w >> 28 {
		case 0x8: // selb
			for k := 0; k < 4; k++ {
				r[k] = cc[k]&b[k] | ^cc[k]&a[k]
			}
		case 0xb: // shufb
			for k := 0; k < 16; k++ {
				s := cc.b(k)
				var x uint8
				switch {
				case s&0xc0 == 0x80:
					x = 0
				case s&0xe0 == 0xc0:
					x = 0xff
				case s&0xe0 == 0xe0:
					x = 0x80
				default:
					idx := s & 0x1f
					if idx < 16 {
						x = a.b(int(idx))
					} else {
						x = b.b(int(idx - 16))
					}
				}
				r = r.withB(k, x)
			}
		case 0xc: // mpya
			for k := 0; k < 4; k++ {
				r[k] = uint32(int32(int16(a[k]))*int32(int16(b[k]))) + cc[k]
			}
		case 0xd: // fnms: c - a*b
			for k := 0; k < 4; k++ {
				r[k] = spuFOut(float32(float64(spuF(cc[k])) - float64(spuF(a[k]))*float64(spuF(b[k]))))
			}
		case 0xe: // fma: a*b + c
			for k := 0; k < 4; k++ {
				r[k] = spuFOut(float32(float64(spuF(a[k]))*float64(spuF(b[k])) + float64(spuF(cc[k]))))
			}
		case 0xf: // fms: a*b - c
			for k := 0; k < 4; k++ {
				r[k] = spuFOut(float32(float64(spuF(a[k]))*float64(spuF(b[k])) - float64(spuF(cc[k]))))
			}
		}
		g[t] = r
		return
	}
	// RI18 (7-bit)
	switch w >> 25 {
	case 0x21: // ila
		g[rT] = splat(i18(w))
		return
	case 0x08, 0x09: // hbra, hbrr
		return
	}
	// RI10 (8-bit)
	if c.exec8(w, rT, rA) {
		return
	}
	// RI16 (9-bit)
	if c.exec9(w, rT) {
		return
	}
	// RI8 (10-bit)
	switch w >> 22 {
	case 0x1d8, 0x1d9, 0x1da, 0x1db:
		c.execConv(w, rT, rA)
		return
	}
	c.exec11(w, rT, rA, rB)
}

func (c *SPUCore) exec8(w, rT, rA uint32) bool {
	g := &c.GPR
	a := g[rA]
	im := i10(w)
	var r Vec
	switch w >> 24 {
	case 0x34: // lqd
		g[rT] = c.lq(a[0] + uint32(im)<<4)
		return true
	case 0x24: // stqd
		c.stq(a[0]+uint32(im)<<4, g[rT])
		return true
	case 0x1c: // ai
		for k := 0; k < 4; k++ {
			r[k] = a[k] + uint32(im)
		}
	case 0x1d: // ahi
		for k := 0; k < 8; k++ {
			r = r.withH(k, a.h(k)+uint16(im))
		}
	case 0x0c: // sfi
		for k := 0; k < 4; k++ {
			r[k] = uint32(im) - a[k]
		}
	case 0x0d: // sfhi
		for k := 0; k < 8; k++ {
			r = r.withH(k, uint16(im)-a.h(k))
		}
	case 0x14: // andi
		for k := 0; k < 4; k++ {
			r[k] = a[k] & uint32(im)
		}
	case 0x15: // andhi
		m := uint32(uint16(im)) * 0x10001
		for k := 0; k < 4; k++ {
			r[k] = a[k] & m
		}
	case 0x16: // andbi
		m := uint32(uint8(im)) * 0x01010101
		for k := 0; k < 4; k++ {
			r[k] = a[k] & m
		}
	case 0x04: // ori
		for k := 0; k < 4; k++ {
			r[k] = a[k] | uint32(im)
		}
	case 0x05: // orhi
		m := uint32(uint16(im)) * 0x10001
		for k := 0; k < 4; k++ {
			r[k] = a[k] | m
		}
	case 0x06: // orbi
		m := uint32(uint8(im)) * 0x01010101
		for k := 0; k < 4; k++ {
			r[k] = a[k] | m
		}
	case 0x44: // xori
		for k := 0; k < 4; k++ {
			r[k] = a[k] ^ uint32(im)
		}
	case 0x45: // xorhi
		m := uint32(uint16(im)) * 0x10001
		for k := 0; k < 4; k++ {
			r[k] = a[k] ^ m
		}
	case 0x46: // xorbi
		m := uint32(uint8(im)) * 0x01010101
		for k := 0; k < 4; k++ {
			r[k] = a[k] ^ m
		}
	case 0x74: // mpyi
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(a[k])) * im)
		}
	case 0x75: // mpyui
		for k := 0; k < 4; k++ {
			r[k] = (a[k] & 0xffff) * (uint32(im) & 0xffff)
		}
	case 0x7c, 0x4c, 0x5c: // ceqi cgti clgti
		for k := 0; k < 4; k++ {
			var t bool
			switch w >> 24 {
			case 0x7c:
				t = a[k] == uint32(im)
			case 0x4c:
				t = int32(a[k]) > im
			default:
				t = a[k] > uint32(im)
			}
			if t {
				r[k] = ^uint32(0)
			}
		}
	case 0x7d, 0x4d, 0x5d: // ceqhi cgthi clgthi
		for k := 0; k < 8; k++ {
			x := a.h(k)
			var t bool
			switch w >> 24 {
			case 0x7d:
				t = x == uint16(im)
			case 0x4d:
				t = int16(x) > int16(im)
			default:
				t = x > uint16(im)
			}
			if t {
				r = r.withH(k, 0xffff)
			}
		}
	case 0x7e, 0x4e, 0x5e: // ceqbi cgtbi clgtbi
		for k := 0; k < 16; k++ {
			x := a.b(k)
			var t bool
			switch w >> 24 {
			case 0x7e:
				t = x == uint8(im)
			case 0x4e:
				t = int8(x) > int8(im)
			default:
				t = x > uint8(im)
			}
			if t {
				r = r.withB(k, 0xff)
			}
		}
	case 0x7f, 0x4f, 0x5f: // heqi hgti hlgti
		var t bool
		switch w >> 24 {
		case 0x7f:
			t = a[0] == uint32(im)
		case 0x4f:
			t = int32(a[0]) > im
		default:
			t = a[0] > uint32(im)
		}
		if t {
			c.halt(w)
		}
		return true
	default:
		return false
	}
	g[rT] = r
	return true
}

func (c *SPUCore) halt(w uint32) {
	if c.Halt != nil {
		c.Halt(c)
	}
	c.fail("halt", w)
}

func (c *SPUCore) exec9(w, rT uint32) bool {
	g := &c.GPR
	im := i16(w)
	pc := c.PC - 4
	switch w >> 23 {
	case 0x061: // lqa
		g[rT] = c.lq(uint32(im) << 2)
	case 0x067: // lqr
		g[rT] = c.lq(pc + uint32(im)<<2)
	case 0x041: // stqa
		c.stq(uint32(im)<<2, g[rT])
	case 0x047: // stqr
		c.stq(pc+uint32(im)<<2, g[rT])
	case 0x083: // ilh
		g[rT] = splat(u16f(w) * 0x10001)
	case 0x082: // ilhu
		g[rT] = splat(u16f(w) << 16)
	case 0x081: // il
		g[rT] = splat(uint32(im))
	case 0x0c1: // iohl
		for k := 0; k < 4; k++ {
			g[rT][k] |= u16f(w)
		}
	case 0x065: // fsmbi
		m := u16f(w)
		var r Vec
		for k := 0; k < 16; k++ {
			if m>>(15-k)&1 != 0 {
				r = r.withB(k, 0xff)
			}
		}
		g[rT] = r
	case 0x064: // br
		c.PC = (pc + uint32(im)<<2) & lsMask
	case 0x060: // bra
		c.PC = uint32(im) << 2 & lsMask
	case 0x066: // brsl
		g[rT] = Vec{(pc + 4) & lsMask, 0, 0, 0}
		c.PC = (pc + uint32(im)<<2) & lsMask
	case 0x062: // brasl
		g[rT] = Vec{(pc + 4) & lsMask, 0, 0, 0}
		c.PC = uint32(im) << 2 & lsMask
	case 0x042: // brnz
		if g[rT][0] != 0 {
			c.PC = (pc + uint32(im)<<2) & lsMask
		}
	case 0x040: // brz
		if g[rT][0] == 0 {
			c.PC = (pc + uint32(im)<<2) & lsMask
		}
	case 0x046: // brhnz
		if g[rT][0]&0xffff != 0 {
			c.PC = (pc + uint32(im)<<2) & lsMask
		}
	case 0x044: // brhz
		if g[rT][0]&0xffff == 0 {
			c.PC = (pc + uint32(im)<<2) & lsMask
		}
	default:
		return false
	}
	return true
}

func (c *SPUCore) execConv(w, rT, rA uint32) {
	i8 := w >> 14 & 0xff
	a := c.GPR[rA]
	var r Vec
	switch w >> 22 {
	case 0x1da: // csflt
		s := math.Ldexp(1, -int(155-int(i8)))
		for k := 0; k < 4; k++ {
			r[k] = spuFOut(float32(float64(int32(a[k])) * s))
		}
	case 0x1db: // cuflt
		s := math.Ldexp(1, -int(155-int(i8)))
		for k := 0; k < 4; k++ {
			r[k] = spuFOut(float32(float64(a[k]) * s))
		}
	case 0x1d8: // cflts
		s := math.Ldexp(1, int(173-int(i8)))
		for k := 0; k < 4; k++ {
			x := float64(spuF(a[k])) * s
			switch {
			case x != x:
				r[k] = 0
			case x >= 2147483647:
				r[k] = 0x7fffffff
			case x <= -2147483648:
				r[k] = 0x80000000
			default:
				r[k] = uint32(int32(x))
			}
		}
	case 0x1d9: // cfltu
		s := math.Ldexp(1, int(173-int(i8)))
		for k := 0; k < 4; k++ {
			x := float64(spuF(a[k])) * s
			switch {
			case x != x || x <= 0:
				r[k] = 0
			case x >= 4294967295:
				r[k] = 0xffffffff
			default:
				r[k] = uint32(x)
			}
		}
	}
	c.GPR[rT] = r
}

func (c *SPUCore) exec11(w, rT, rA, rB uint32) {
	g := &c.GPR
	a, b := g[rA], g[rB]
	var r Vec
	op := w >> 21
	switch op {
	case 0x000: // stop
		if c.Stop == nil || !c.Stop(c, w&0x3fff) {
			if c.Stop == nil {
				c.fail("stop", w)
			} else {
				c.stalled = true
			}
		}
		return
	case 0x140: // stopd
		c.fail("stopd", w)
		return
	case 0x001, 0x201, 0x002, 0x003, 0x1ac: // lnop nop sync dsync hbr
		return
	case 0x00c: // mfspr
		g[rT] = Vec{}
		return
	case 0x10c: // mtspr
		return
	case 0x00d: // rdch
		v, ok := c.RdCh(c, rA)
		if !ok {
			c.stalled = true
			return
		}
		g[rT] = Vec{v, 0, 0, 0}
		return
	case 0x00f: // rchcnt
		v := c.ChCount(c, rA)
		if c.stalled { // a polling loop with nothing to read: wait instead
			return
		}
		g[rT] = Vec{v, 0, 0, 0}
		return
	case 0x10d: // wrch
		if !c.WrCh(c, rA, g[rT][0]) {
			c.stalled = true
		}
		return
	case 0x1c4: // lqx
		g[rT] = c.lq(a[0] + b[0])
		return
	case 0x144: // stqx
		c.stq(a[0]+b[0], g[rT])
		return
	case 0x1f4, 0x1f5, 0x1f6, 0x1f7, 0x1d4, 0x1d5, 0x1d6, 0x1d7: // cbd chd cwd cdd cbx chx cwx cdx
		var t uint32
		if op >= 0x1f4 {
			t = a[0] + uint32(i7(w))
		} else {
			t = a[0] + b[0]
		}
		r = Vec{0x10111213, 0x14151617, 0x18191a1b, 0x1c1d1e1f}
		switch op & 3 {
		case 0:
			r = r.withB(int(t&15), 0x03)
		case 1:
			r = r.withH(int(t&14)/2, 0x0203)
		case 2:
			r[t&12/4] = 0x00010203
		case 3:
			r = r.withD(int(t&8)/8, 0x0001020304050607)
		}
	case 0x0c0: // a
		for k := 0; k < 4; k++ {
			r[k] = a[k] + b[k]
		}
	case 0x0c8: // ah
		for k := 0; k < 8; k++ {
			r = r.withH(k, a.h(k)+b.h(k))
		}
	case 0x040: // sf: b - a
		for k := 0; k < 4; k++ {
			r[k] = b[k] - a[k]
		}
	case 0x048: // sfh
		for k := 0; k < 8; k++ {
			r = r.withH(k, b.h(k)-a.h(k))
		}
	case 0x340: // addx
		t := g[rT]
		for k := 0; k < 4; k++ {
			r[k] = a[k] + b[k] + t[k]&1
		}
	case 0x341: // sfx
		t := g[rT]
		for k := 0; k < 4; k++ {
			r[k] = b[k] + ^a[k] + t[k]&1
		}
	case 0x0c2: // cg
		for k := 0; k < 4; k++ {
			_, cy := bits.Add32(a[k], b[k], 0)
			r[k] = cy
		}
	case 0x342: // cgx
		t := g[rT]
		for k := 0; k < 4; k++ {
			_, cy := bits.Add32(a[k], b[k], t[k]&1)
			r[k] = cy
		}
	case 0x042: // bg
		for k := 0; k < 4; k++ {
			if b[k] >= a[k] {
				r[k] = 1
			}
		}
	case 0x343: // bgx
		t := g[rT]
		for k := 0; k < 4; k++ {
			if t[k]&1 != 0 {
				if b[k] >= a[k] {
					r[k] = 1
				}
			} else if b[k] > a[k] {
				r[k] = 1
			}
		}
	case 0x3c4: // mpy
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(a[k])) * int32(int16(b[k])))
		}
	case 0x3cc: // mpyu
		for k := 0; k < 4; k++ {
			r[k] = (a[k] & 0xffff) * (b[k] & 0xffff)
		}
	case 0x3c5: // mpyh
		for k := 0; k < 4; k++ {
			r[k] = ((a[k] >> 16) * (b[k] & 0xffff)) << 16
		}
	case 0x3c7: // mpys
		for k := 0; k < 4; k++ {
			r[k] = uint32((int32(int16(a[k])) * int32(int16(b[k]))) >> 16)
		}
	case 0x3c6: // mpyhh
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(a[k]>>16)) * int32(int16(b[k]>>16)))
		}
	case 0x346: // mpyhha
		t := g[rT]
		for k := 0; k < 4; k++ {
			r[k] = t[k] + uint32(int32(int16(a[k]>>16))*int32(int16(b[k]>>16)))
		}
	case 0x3ce: // mpyhhu
		for k := 0; k < 4; k++ {
			r[k] = (a[k] >> 16) * (b[k] >> 16)
		}
	case 0x34e: // mpyhhau
		t := g[rT]
		for k := 0; k < 4; k++ {
			r[k] = t[k] + (a[k]>>16)*(b[k]>>16)
		}
	case 0x2a5: // clz
		for k := 0; k < 4; k++ {
			r[k] = uint32(bits.LeadingZeros32(a[k]))
		}
	case 0x2b4: // cntb
		for k := 0; k < 16; k++ {
			r = r.withB(k, uint8(bits.OnesCount8(a.b(k))))
		}
	case 0x1b6: // fsmb
		m := a[0] & 0xffff
		for k := 0; k < 16; k++ {
			if m>>(15-k)&1 != 0 {
				r = r.withB(k, 0xff)
			}
		}
	case 0x1b5: // fsmh
		m := a[0] & 0xff
		for k := 0; k < 8; k++ {
			if m>>(7-k)&1 != 0 {
				r = r.withH(k, 0xffff)
			}
		}
	case 0x1b4: // fsm
		m := a[0] & 0xf
		for k := 0; k < 4; k++ {
			if m>>(3-k)&1 != 0 {
				r[k] = ^uint32(0)
			}
		}
	case 0x1b2: // gbb
		var m uint32
		for k := 0; k < 16; k++ {
			m = m<<1 | uint32(a.b(k)&1)
		}
		r[0] = m
	case 0x1b1: // gbh
		var m uint32
		for k := 0; k < 8; k++ {
			m = m<<1 | uint32(a.h(k)&1)
		}
		r[0] = m
	case 0x1b0: // gb
		var m uint32
		for k := 0; k < 4; k++ {
			m = m<<1 | a[k]&1
		}
		r[0] = m
	case 0x0d3: // avgb
		for k := 0; k < 16; k++ {
			r = r.withB(k, uint8((uint32(a.b(k))+uint32(b.b(k))+1)>>1))
		}
	case 0x053: // absdb
		for k := 0; k < 16; k++ {
			x, y := a.b(k), b.b(k)
			if x > y {
				r = r.withB(k, x-y)
			} else {
				r = r.withB(k, y-x)
			}
		}
	case 0x253: // sumb
		for k := 0; k < 4; k++ {
			var sa, sb uint32
			for j := 0; j < 4; j++ {
				sa += uint32(a.b(4*k + j))
				sb += uint32(b.b(4*k + j))
			}
			r[k] = sb<<16 | sa
		}
	case 0x2b6: // xsbh
		for k := 0; k < 8; k++ {
			r = r.withH(k, uint16(int16(int8(a.h(k)))))
		}
	case 0x2ae: // xshw
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(a[k])))
		}
	case 0x2a6: // xswd
		for k := 0; k < 2; k++ {
			r = r.withD(k, uint64(int64(int32(a[2*k+1]))))
		}
	case 0x0c1:
		for k := 0; k < 4; k++ {
			r[k] = a[k] & b[k]
		}
	case 0x2c1:
		for k := 0; k < 4; k++ {
			r[k] = a[k] &^ b[k]
		}
	case 0x041:
		for k := 0; k < 4; k++ {
			r[k] = a[k] | b[k]
		}
	case 0x2c9:
		for k := 0; k < 4; k++ {
			r[k] = a[k] | ^b[k]
		}
	case 0x241:
		for k := 0; k < 4; k++ {
			r[k] = a[k] ^ b[k]
		}
	case 0x0c9:
		for k := 0; k < 4; k++ {
			r[k] = ^(a[k] & b[k])
		}
	case 0x049:
		for k := 0; k < 4; k++ {
			r[k] = ^(a[k] | b[k])
		}
	case 0x249:
		for k := 0; k < 4; k++ {
			r[k] = ^(a[k] ^ b[k])
		}
	case 0x1f0: // orx
		r[0] = a[0] | a[1] | a[2] | a[3]
	// --- shifts and rotates
	case 0x05f, 0x07f: // shlh, shlhi
		for k := 0; k < 8; k++ {
			n := uint32(i7(w))
			if op == 0x05f {
				n = uint32(b.h(k))
			}
			n &= 0x1f
			if n < 16 {
				r = r.withH(k, a.h(k)<<n)
			}
		}
	case 0x05b, 0x07b: // shl, shli
		for k := 0; k < 4; k++ {
			n := uint32(i7(w))
			if op == 0x05b {
				n = b[k]
			}
			n &= 0x3f
			if n < 32 {
				r[k] = a[k] << n
			}
		}
	case 0x1db: // shlqbi
		r = shlQuadBits(a, uint(b[0]&7))
	case 0x1fb: // shlqbii
		r = shlQuadBits(a, uint(uint32(i7(w))&7))
	case 0x1df: // shlqby
		r = shlQuadBits(a, uint(b[0]&0x1f)*8)
	case 0x1ff: // shlqbyi
		r = shlQuadBits(a, uint(uint32(i7(w))&0x1f)*8)
	case 0x1cf: // shlqbybi
		r = shlQuadBits(a, uint(b[0]>>3&0x1f)*8)
	case 0x05c, 0x07c: // roth, rothi
		for k := 0; k < 8; k++ {
			n := uint32(i7(w))
			if op == 0x05c {
				n = uint32(b.h(k))
			}
			r = r.withH(k, bits.RotateLeft16(a.h(k), int(n&15)))
		}
	case 0x058, 0x078: // rot, roti
		for k := 0; k < 4; k++ {
			n := uint32(i7(w))
			if op == 0x058 {
				n = b[k]
			}
			r[k] = bits.RotateLeft32(a[k], int(n&31))
		}
	case 0x1dc: // rotqby
		r = rotQuadBits(a, uint(b[0]&15)*8)
	case 0x1fc: // rotqbyi
		r = rotQuadBits(a, uint(uint32(i7(w))&15)*8)
	case 0x1cc: // rotqbybi
		r = rotQuadBits(a, uint(b[0]>>3&15)*8)
	case 0x1d8: // rotqbi
		r = rotQuadBits(a, uint(b[0]&7))
	case 0x1f8: // rotqbii
		r = rotQuadBits(a, uint(uint32(i7(w))&7))
	case 0x05d, 0x07d: // rothm, rothmi
		for k := 0; k < 8; k++ {
			n := uint32(i7(w))
			if op == 0x05d {
				n = uint32(b.h(k))
			}
			n = -n & 0x1f
			if n < 16 {
				r = r.withH(k, a.h(k)>>n)
			}
		}
	case 0x059, 0x079: // rotm, rotmi
		for k := 0; k < 4; k++ {
			n := uint32(i7(w))
			if op == 0x059 {
				n = b[k]
			}
			n = -n & 0x3f
			if n < 32 {
				r[k] = a[k] >> n
			}
		}
	case 0x1dd: // rotqmby
		r = shrQuadBits(a, uint(-b[0]&0x1f)*8)
	case 0x1fd: // rotqmbyi
		r = shrQuadBits(a, uint(-uint32(i7(w))&0x1f)*8)
	case 0x1cd: // rotqmbybi
		r = shrQuadBits(a, uint(-(b[0]>>3)&0x1f)*8)
	case 0x1d9: // rotqmbi
		r = shrQuadBits(a, uint(-b[0]&7))
	case 0x1f9: // rotqmbii
		r = shrQuadBits(a, uint(-uint32(i7(w))&7))
	case 0x05e, 0x07e: // rotmah, rotmahi
		for k := 0; k < 8; k++ {
			n := uint32(i7(w))
			if op == 0x05e {
				n = uint32(b.h(k))
			}
			n = -n & 0x1f
			if n > 15 {
				n = 15
			}
			r = r.withH(k, uint16(int16(a.h(k))>>n))
		}
	case 0x05a, 0x07a: // rotma, rotmai
		for k := 0; k < 4; k++ {
			n := uint32(i7(w))
			if op == 0x05a {
				n = b[k]
			}
			n = -n & 0x3f
			if n > 31 {
				n = 31
			}
			r[k] = uint32(int32(a[k]) >> n)
		}
	// --- compares
	case 0x3c0, 0x240, 0x2c0: // ceq cgt clgt
		for k := 0; k < 4; k++ {
			var t bool
			switch op {
			case 0x3c0:
				t = a[k] == b[k]
			case 0x240:
				t = int32(a[k]) > int32(b[k])
			default:
				t = a[k] > b[k]
			}
			if t {
				r[k] = ^uint32(0)
			}
		}
	case 0x3c8, 0x248, 0x2c8: // ceqh cgth clgth
		for k := 0; k < 8; k++ {
			x, y := a.h(k), b.h(k)
			var t bool
			switch op {
			case 0x3c8:
				t = x == y
			case 0x248:
				t = int16(x) > int16(y)
			default:
				t = x > y
			}
			if t {
				r = r.withH(k, 0xffff)
			}
		}
	case 0x3d0, 0x250, 0x2d0: // ceqb cgtb clgtb
		for k := 0; k < 16; k++ {
			x, y := a.b(k), b.b(k)
			var t bool
			switch op {
			case 0x3d0:
				t = x == y
			case 0x250:
				t = int8(x) > int8(y)
			default:
				t = x > y
			}
			if t {
				r = r.withB(k, 0xff)
			}
		}
	case 0x3d8, 0x258, 0x2d8: // heq hgt hlgt
		var t bool
		switch op {
		case 0x3d8:
			t = a[0] == b[0]
		case 0x258:
			t = int32(a[0]) > int32(b[0])
		default:
			t = a[0] > b[0]
		}
		if t {
			c.halt(w)
		}
		return
	// --- branches
	case 0x1a8: // bi
		c.PC = a[0] & lsMask &^ 3
		return
	case 0x1a9: // bisl
		g[rT] = Vec{c.PC & lsMask, 0, 0, 0}
		c.PC = a[0] & lsMask &^ 3
		return
	case 0x1aa: // iret
		c.PC = c.SRR0 & lsMask &^ 3
		return
	case 0x1ab: // bisled: no interrupts here
		g[rT] = Vec{c.PC & lsMask, 0, 0, 0}
		return
	case 0x128: // biz
		if g[rT][0] == 0 {
			c.PC = a[0] & lsMask &^ 3
		}
		return
	case 0x129: // binz
		if g[rT][0] != 0 {
			c.PC = a[0] & lsMask &^ 3
		}
		return
	case 0x12a: // bihz
		if g[rT][0]&0xffff == 0 {
			c.PC = a[0] & lsMask &^ 3
		}
		return
	case 0x12b: // bihnz
		if g[rT][0]&0xffff != 0 {
			c.PC = a[0] & lsMask &^ 3
		}
		return
	// --- single precision
	case 0x2c4, 0x2c5, 0x2c6: // fa fs fm
		for k := 0; k < 4; k++ {
			x, y := float64(spuF(a[k])), float64(spuF(b[k]))
			var v float64
			switch op {
			case 0x2c4:
				v = x + y
			case 0x2c5:
				v = x - y
			default:
				v = x * y
			}
			r[k] = spuFOut(float32(v))
		}
	case 0x1b8: // frest
		for k := 0; k < 4; k++ {
			r[k] = spuFOut(1 / spuF(a[k]))
		}
	case 0x1b9: // frsqest
		for k := 0; k < 4; k++ {
			r[k] = spuFOut(float32(1 / math.Sqrt(math.Abs(float64(spuF(a[k]))))))
		}
	case 0x3d4: // fi: the estimate is already exact
		r = b
	case 0x3c2, 0x2c2, 0x3ca, 0x2ca: // fceq fcgt fcmeq fcmgt
		for k := 0; k < 4; k++ {
			x, y := spuF(a[k]), spuF(b[k])
			if op == 0x3ca || op == 0x2ca {
				x, y = float32(math.Abs(float64(x))), float32(math.Abs(float64(y)))
			}
			var t bool
			if op == 0x3c2 || op == 0x3ca {
				t = x == y
			} else {
				t = x > y
			}
			if t {
				r[k] = ^uint32(0)
			}
		}
	case 0x3b9: // frds
		for k := 0; k < 2; k++ {
			r[2*k] = math.Float32bits(float32(math.Float64frombits(a.d(k))))
		}
	case 0x3b8: // fesd
		for k := 0; k < 2; k++ {
			r = r.withD(k, math.Float64bits(float64(math.Float32frombits(a[2*k]))))
		}
	case 0x398: // fscrrd
	case 0x3ba: // fscrwr
		return
	// --- double precision
	case 0x2cc, 0x2cd, 0x2ce: // dfa dfs dfm
		for k := 0; k < 2; k++ {
			x, y := math.Float64frombits(a.d(k)), math.Float64frombits(b.d(k))
			var v float64
			switch op {
			case 0x2cc:
				v = x + y
			case 0x2cd:
				v = x - y
			default:
				v = x * y
			}
			r = r.withD(k, math.Float64bits(v))
		}
	case 0x35c, 0x35d, 0x35e, 0x35f: // dfma dfms dfnms dfnma
		t := g[rT]
		for k := 0; k < 2; k++ {
			x, y, z := math.Float64frombits(a.d(k)), math.Float64frombits(b.d(k)), math.Float64frombits(t.d(k))
			var v float64
			switch op {
			case 0x35c:
				v = math.FMA(x, y, z)
			case 0x35d:
				v = math.FMA(x, y, -z)
			case 0x35e:
				v = -math.FMA(x, y, -z)
			default:
				v = -math.FMA(x, y, z)
			}
			r = r.withD(k, math.Float64bits(v))
		}
	case 0x3c3, 0x2c3, 0x3cb, 0x2cb: // dfceq dfcgt dfcmeq dfcmgt
		for k := 0; k < 2; k++ {
			x, y := math.Float64frombits(a.d(k)), math.Float64frombits(b.d(k))
			if op == 0x3cb || op == 0x2cb {
				x, y = math.Abs(x), math.Abs(y)
			}
			var t bool
			if op == 0x3c3 || op == 0x3cb {
				t = x == y
			} else {
				t = x > y
			}
			if t {
				r = r.withD(k, ^uint64(0))
			}
		}
	case 0x3bf: // dftsv
		im := uint32(i7(w)) & 0x7f
		for k := 0; k < 2; k++ {
			x := math.Float64frombits(a.d(k))
			bits := a.d(k)
			neg := bits>>63 != 0
			var t bool
			switch {
			case math.IsNaN(x):
				t = im&0x40 != 0
			case math.IsInf(x, 0):
				t = (!neg && im&0x20 != 0) || (neg && im&0x10 != 0)
			case x == 0:
				t = (!neg && im&0x08 != 0) || (neg && im&0x04 != 0)
			case bits&0x7ff0000000000000 == 0:
				t = (!neg && im&0x02 != 0) || (neg && im&0x01 != 0)
			}
			if t {
				r = r.withD(k, ^uint64(0))
			}
		}
	default:
		c.fail("unknown instruction", w)
		return
	}
	g[rT] = r
}
