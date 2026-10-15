package gba

import "math"

// RetroPixels BIOS calls (the swi function of web/emu/gba.js, written from
// GBATEK's description of each call).

var sinTable = func() (t [256]int32) {
	for i := range t {
		t[i] = int32(jsRound(math.Sin(float64(i)*math.Pi/128) * 0x4000))
	}
	return
}()

// toInt32 is JavaScript's "| 0" on a number.
func toInt32(f float64) int32 {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return 0
	}
	f = math.Trunc(f)
	m := math.Mod(f, 4294967296)
	if m < 0 {
		m += 4294967296
	}
	return int32(uint32(m))
}

func (g *GBA) rd8(a uint32) uint32  { return g.read8raw(a) }
func (g *GBA) rd16(a uint32) uint32 { return g.read16raw(a &^ 1) }
func (g *GBA) rd32(a uint32) uint32 { return g.read32raw(a &^ 3) }
func (g *GBA) wr16(a, v uint32)     { g.write16raw(a&^1, v) }
func (g *GBA) wr32(a, v uint32)     { g.write32raw(a&^3, v) }

func (g *GBA) swi(n uint32) bool {
	c := g.cpu
	r := &c.R
	c.Cycles += 24
	g.biosLatch = 0xe3a02004
	// like the real BIOS, copy / decompression calls refuse to read from the BIOS area
	if ((n >= 0x0b && n <= 0x0c) || (n >= 0x10 && n <= 0x18)) && r[0]&0x0e000000 == 0 {
		return true
	}
	switch n {
	case 0x00: // SoftReset
		toRAM := g.rd8(0x03007ffa)
		for a := uint32(0x03007e00); a < 0x03008000; a += 4 {
			g.wr32(a, 0)
		}
		c.SwitchMode(0x13)
		r[13], r[14] = 0x03007fe0, 0
		c.SwitchMode(0x12)
		r[13], r[14] = 0x03007fa0, 0
		c.SwitchMode(0x1f)
		r[13] = 0x03007f00
		for k := 0; k < 13; k++ {
			r[k] = 0
		}
		c.T, c.I = 0, 0
		if toRAM != 0 {
			c.Branch(0x02000000)
		} else {
			c.Branch(0x08000000)
		}
	case 0x01: // RegisterRamReset
		f := r[0]
		if f&1 != 0 {
			g.ewram = [0x40000]byte{}
		}
		if f&2 != 0 {
			clear(g.iwram[:0x7e00])
		}
		if f&4 != 0 {
			g.pal = [0x400]byte{}
		}
		if f&8 != 0 {
			g.vram = [0x18000]byte{}
		}
		if f&16 != 0 {
			g.oam = [0x400]byte{}
		}
		if f&32 != 0 { // link port registers
			for a := 0x120; a < 0x130; a += 2 {
				g.setIO16(a, 0)
			}
			g.setIO16(0x134, 0x8000)
			g.setIO16(0x140, 0)
			for _, a := range []int{0x150, 0x152, 0x154, 0x156} {
				g.setIO16(a, 0)
			}
		}
		if f&64 != 0 { // sound registers (and the sound chip with them)
			g.ioWrite16(0x84, 0)
			for a := 0x60; a < 0xa8; a += 2 {
				if a != 0x84 && a != 0x88 {
					g.setIO16(a, 0)
				}
			}
			g.ioWrite16(0x88, 0x200)
		}
		if f&128 != 0 { // everything else: display, DMA, timers, keypad control, interrupts
			for a := 0; a < 0x60; a += 2 {
				g.setIO16(a, 0)
			}
			g.setIO16(0, 0x80)
			for _, a := range []int{0x20, 0x26, 0x30, 0x36} {
				g.setIO16(a, 0x100)
			}
			for k := 0; k < 2; k++ {
				g.ppu.writeRef(k, false)
				g.ppu.writeRef(k, true)
			}
			for a := 0xb0; a < 0xe0; a += 2 {
				g.ioWrite16(a, 0)
			}
			for a := 0x100; a < 0x110; a += 2 {
				g.ioWrite16(a, 0)
			}
			g.setIO16(0x132, 0)
			g.ioWrite16(0x200, 0)
			g.ioWrite16(0x202, 0xffff)
			g.ioWrite16(0x208, 0)
		}
	case 0x02: // Halt: sleep until any enabled interrupt
		c.Halted = true
	case 0x03: // Stop: deep sleep until keypad / cartridge / link interrupt
		c.Halted = true
		g.stopped = true
	case 0x04:
		g.intrWait(r[0]&1 != 0, int(r[1]&0x3fff))
	case 0x05: // VBlankIntrWait (leaves r0 = r1 = 1, like the real call)
		r[0], r[1] = 1, 1
		g.intrWait(true, 1)
	case 0x06, 0x07: // Div / DivArm
		num, den := int32(r[0]), int32(r[1])
		if n == 7 {
			num, den = den, num
		}
		if den == 0 {
			if num < 0 {
				r[0] = 0xffffffff
			} else {
				r[0] = 1
			}
			r[1] = uint32(num)
			r[3] = 1
			break
		}
		q := num / den
		r[0] = uint32(q)
		r[1] = uint32(num - q*den)
		if q < 0 {
			q = -q
		}
		r[3] = uint32(q)
		c.Cycles += 40
	case 0x08:
		r[0] = uint32(math.Floor(math.Sqrt(float64(r[0]))))
		c.Cycles += 30
	case 0x09:
		r[0] = uint32(arcTan(r[0]))
	case 0x0a: // ArcTan2(x, y)
		x, y := float64(int16(r[0])), float64(int16(r[1]))
		a := jsAtan2(y, x)
		if a < 0 {
			a += 2 * math.Pi
		}
		r[0] = uint32(int64(jsRound(a/(2*math.Pi)*0x10000)) & 0xffff)
	case 0x0b: // CpuSet
		src, dst := r[0], r[1]
		cnt, fill, word := r[2]&0x1fffff, r[2]&0x1000000 != 0, r[2]&0x4000000 != 0
		if word {
			src &^= 3
			dst &^= 3
			var v0 uint32
			if fill {
				v0 = g.rd32(src)
			}
			for k := uint32(0); k < cnt; k++ {
				if fill {
					g.wr32(dst, v0)
				} else {
					g.wr32(dst, g.rd32(src))
					src += 4
				}
				dst += 4
			}
		} else {
			src &^= 1
			dst &^= 1
			var v0 uint32
			if fill {
				v0 = g.rd16(src)
			}
			for k := uint32(0); k < cnt; k++ {
				if fill {
					g.wr16(dst, v0)
				} else {
					g.wr16(dst, g.rd16(src))
					src += 2
				}
				dst += 2
			}
		}
		c.Cycles += int64(cnt) * 4
	case 0x0c: // CpuFastSet
		src, dst := r[0]&^3, r[1]&^3
		cnt, fill := (r[2]&0x1fffff+7)&^7, r[2]&0x1000000 != 0
		var v0 uint32
		if fill {
			v0 = g.rd32(src)
		}
		for k := uint32(0); k < cnt; k++ {
			if fill {
				g.wr32(dst, v0)
			} else {
				g.wr32(dst, g.rd32(src))
				src += 4
			}
			dst += 4
		}
		c.Cycles += int64(cnt) * 2
	case 0x0d:
		r[0] = 0xbaae187f
	case 0x0e: // BgAffineSet
		src, dst := r[0], r[1]
		for k := int32(0); k < int32(r[2]); k++ {
			ox, oy := g.rd32(src), g.rd32(src+4)
			dx, dy := int64(int16(g.rd16(src+8))), int64(int16(g.rd16(src+10)))
			sx, sy := int64(int16(g.rd16(src+12))), int64(int16(g.rd16(src+14)))
			th := g.rd16(src+16) >> 8 & 0xff
			s, co := int64(sinTable[th]), int64(sinTable[(th+64)&255])
			pa, pb, pc, pd := (sx*co)>>14, -((sx * s) >> 14), (sy*s)>>14, (sy*co)>>14
			g.wr16(dst, uint32(pa))
			g.wr16(dst+2, uint32(pb))
			g.wr16(dst+4, uint32(pc))
			g.wr16(dst+6, uint32(pd))
			g.wr32(dst+8, ox-uint32(pa*dx+pb*dy))
			g.wr32(dst+12, oy-uint32(pc*dx+pd*dy))
			src += 20
			dst += 16
		}
	case 0x0f: // ObjAffineSet
		src, dst := r[0], r[1]
		step := r[3]
		for k := int32(0); k < int32(r[2]); k++ {
			sx, sy := int64(int16(g.rd16(src))), int64(int16(g.rd16(src+2)))
			th := g.rd16(src+4) >> 8 & 0xff
			s, co := int64(sinTable[th]), int64(sinTable[(th+64)&255])
			g.wr16(dst, uint32((sx*co)>>14))
			g.wr16(dst+step, uint32(-((sx * s) >> 14)))
			g.wr16(dst+step*2, uint32((sy*s)>>14))
			g.wr16(dst+step*3, uint32((sy*co)>>14))
			src += 8
			dst += step * 4
		}
	case 0x10: // BitUnPack
		src, dst, info := r[0], r[1], r[2]
		ln, sw, dw := g.rd16(info), g.rd8(info+2), g.rd8(info+3)
		offw := g.rd32(info + 4)
		off := offw & 0x7fffffff
		// (the web core sees a value read from the save area as positive)
		reg := ((info + 4) &^ 3) >> 24
		zero := offw>>31 != 0 && reg != 14 && reg != 15
		mask := uint32(0xffffffff)
		if dw != 32 {
			mask = uint32(1)<<(dw&31) - 1
		}
		smask := uint32(1)<<(sw&31) - 1
		var acc, bits uint32
		for k := uint32(0); k < ln && sw != 0; k++ {
			b := g.rd8(src + k)
			for s := uint32(0); s < 8; s += sw {
				v := b >> s & smask
				if v != 0 || zero {
					v += off
				}
				acc |= (v & mask) << bits
				bits += dw
				if bits >= 32 {
					g.wr32(dst, acc)
					dst += 4
					acc, bits = 0, 0
				}
			}
		}
	case 0x11, 0x12:
		g.writeOut(r[1], lz77(g, r[0]), n == 0x12)
	case 0x13: // writes 32 bits at a time
		out := huffman(g, r[0])
		at := func(i int) uint32 {
			if i < len(out) {
				return uint32(out[i])
			}
			return 0
		}
		for k := 0; k < len(out); k += 4 {
			g.wr32(r[1]+uint32(k), at(k)|at(k+1)<<8|at(k+2)<<16|at(k+3)<<24)
		}
		c.Cycles += int64(len(out)) * 3
	case 0x14, 0x15:
		g.writeOut(r[1], rle(g, r[0]), n == 0x15)
	case 0x16, 0x17, 0x18:
		src := r[0]
		size := g.rd32(src) >> 8
		out := make([]byte, size)
		if n == 0x18 {
			prev := uint32(0)
			for k := uint32(0); k+1 < size; k += 2 {
				prev = (prev + g.rd16(src+4+k)) & 0xffff
				out[k] = byte(prev)
				out[k+1] = byte(prev >> 8)
			}
		} else {
			prev := uint32(0)
			for k := uint32(0); k < size; k++ {
				prev = (prev + g.rd8(src+4+k)) & 0xff
				out[k] = byte(prev)
			}
		}
		g.writeOut(r[1], out, n != 0x16)
	case 0x19: // SoundBias: move the bias level to 0x200 (r0 != 0) or 0
		b := int(g.io16(0x88))
		lvl := 0
		if r[0] != 0 {
			lvl = 0x200
		}
		g.ioWrite16(0x88, b&0xc000|lvl)
		d := b&0x3ff - lvl
		if d < 0 {
			d = -d
		}
		c.Cycles += int64(d) * 8
	case 0x1f: // MidiKey2Freq
		freq := g.rd32(r[0] + 4)
		e := (180 - float64(int32(r[1])) - float64(int32(r[2]))/256) / 12
		r[0] = uint32(toInt32(math.Floor(float64(freq) / jsPow(2, e))))
	case 0x25: // MultiBoot: no link cable here, report failure
		r[0] = 1
	case 0x26: // HardReset: start the cartridge again from the top
		g.Reset()
	case 0x27: // CustomHalt: r2 bit 7 picks Stop over Halt
		g.ioWrite8(0x301, int(r[2]&0x80))
	}
	// 0x28 / 0x29 (SoundDriverVSyncOff / On), the BIOS sound driver
	// (0x1A-0x1E, 0x20-0x24) and GetJumpList: games bring their own, nothing to do
	return true
}

func (g *GBA) intrWait(discard bool, mask int) {
	c := g.cpu
	g.setIO16(0x208, 1) // IME on
	f := int(g.read16raw(0x03007ff8))
	if !g.waiting {
		g.waiting = true
		if discard {
			f &^= mask
			g.write16raw(0x03007ff8, uint32(f))
		}
	}
	if f&mask != 0 {
		g.write16raw(0x03007ff8, uint32(f&^mask))
		g.waiting = false
		g.updateIrq()
		return
	}
	// not yet: sleep, and run this call again after the next interrupt
	if c.T != 0 {
		c.Branch(c.PC - 2)
	} else {
		c.Branch(c.PC - 4)
	}
	c.Halted = true
	g.updateIrq()
}

// writeOut writes decompressed bytes; VRAM destinations take 16-bit writes.
func (g *GBA) writeOut(dst uint32, b []byte, vram16 bool) {
	if vram16 {
		for k := 0; k+1 < len(b); k += 2 {
			g.write16raw((dst+uint32(k))&^1, uint32(b[k])|uint32(b[k+1])<<8)
		}
	} else {
		for k := range b {
			g.write8raw(dst+uint32(k), uint32(b[k]))
		}
	}
	g.cpu.Cycles += int64(len(b)) * 3
}

// arcTan: r0 is a tangent in 1.14 fixed point (-1 to 1 is the useful range);
// the angle comes back with a full circle as 0x10000.
func arcTan(v uint32) int32 {
	x := float64(int16(v))
	return int32(math.Floor(jsAtan(x/0x4000) * 0x8000 / math.Pi))
}

func lz77(g *GBA, src uint32) []byte {
	size := (g.rd8(src) | g.rd8(src+1)<<8 | g.rd8(src+2)<<16 | g.rd8(src+3)<<24) >> 8
	out := make([]byte, size)
	p, o := src+4, uint32(0)
	for o < size {
		flags := g.rd8(p)
		p++
		for b := 7; b >= 0 && o < size; b-- {
			if flags&(1<<b) != 0 {
				b0, b1 := g.rd8(p), g.rd8(p+1)
				p += 2
				disp, ln := (b0&15)<<8|b1+1, b0>>4+3
				for k := uint32(0); k < ln && o < size; k, o = k+1, o+1 {
					if o >= disp {
						out[o] = out[o-disp]
					} else {
						out[o] = 0
					}
				}
			} else {
				out[o] = byte(g.rd8(p))
				o++
				p++
			}
		}
	}
	return out
}

func rle(g *GBA, src uint32) []byte {
	size := (g.rd8(src) | g.rd8(src+1)<<8 | g.rd8(src+2)<<16 | g.rd8(src+3)<<24) >> 8
	out := make([]byte, size)
	p, o := src+4, uint32(0)
	for o < size {
		f := g.rd8(p)
		p++
		if f&0x80 != 0 {
			ln, v := f&0x7f+3, byte(g.rd8(p))
			p++
			for k := uint32(0); k < ln && o < size; k++ {
				out[o] = v
				o++
			}
		} else {
			ln := f&0x7f + 1
			for k := uint32(0); k < ln && o < size; k++ {
				out[o] = byte(g.rd8(p))
				o++
				p++
			}
		}
	}
	return out
}

func huffman(g *GBA, src uint32) []byte {
	hdr := g.rd32(src)
	bitsPer, size := hdr&15, hdr>>8
	out := make([]byte, size)
	if bitsPer == 0 { // (the web core would never finish)
		return out
	}
	treeSize := (g.rd8(src+4) + 1) * 2
	root := src + 5
	p := src + 4 + treeSize
	o, acc, accBits := uint32(0), uint32(0), uint32(0)
	node, nodeVal := root, g.rd8(root)
	for o < size {
		word := g.rd32(p)
		p += 4
		for b := 31; b >= 0 && o < size; b-- {
			bit := word >> b & 1
			next := node&^1 + (nodeVal&0x3f)*2 + 2 + bit
			var isData bool
			if bit != 0 {
				isData = nodeVal&0x40 != 0
			} else {
				isData = nodeVal&0x80 != 0
			}
			if isData {
				acc |= g.rd8(next) << accBits
				accBits += bitsPer
				if accBits >= 32 {
					for k := 0; k < 4 && o < size; k++ {
						out[o] = byte(acc >> (k * 8))
						o++
					}
					acc, accBits = 0, 0
				}
				node, nodeVal = root, g.rd8(root)
			} else {
				node, nodeVal = next, g.rd8(next)
			}
		}
	}
	return out
}
