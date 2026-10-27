package model3

import (
	"encoding/binary"
	"fmt"
	"os"
)

// The tile generator: four 64x64 layers of 8x8 tiles (A, A', B, B') drawn in
// front of or behind the 3D picture, with a 32768-colour palette.
//
// Its memory (1 MB of tiles and tables, then 128 KB of palette) is little
// endian: the processor sees it as a big-endian byte array, and the tile
// generator reads 32-bit little-endian words from it. Its registers likewise
// hold the byte-reversed value of what the processor writes.

const (
	tgVRAMSize = 0x120000
	tgPalBase  = 0x100000
	screenW    = 496
	screenH    = 384
)

type tileGen struct {
	vram [tgVRAMSize]byte
	regs [64]uint32

	pal    [2][32768]uint32 // per layer pair: colour with the offset applied (0 = transparent), 0xAARRGGBB
	offR   [2]int32
	offG   [2]int32
	offB   [2]int32
	bottom [screenW * screenH]uint32 // layers below the 3D picture, by line
	top    [screenW * screenH]uint32 // layers above it
}

func (t *tileGen) word(byteAddr uint32) uint32 {
	return binary.LittleEndian.Uint32(t.vram[byteAddr&^3:])
}

func (t *tileGen) reset() {
	t.vram = [tgVRAMSize]byte{}
	t.regs = [64]uint32{}
	t.offR, t.offG, t.offB = [2]int32{}, [2]int32{}, [2]int32{}
	for p := 0; p < 2; p++ {
		for i := range t.pal[p] {
			t.pal[p][i] = 0
		}
	}
	t.clear()
}

func (t *tileGen) clear() {
	clear(t.bottom[:])
	clear(t.top[:])
}

// palette entry i after a write
func (t *tileGen) updatePal(i uint32) {
	w := binary.LittleEndian.Uint32(t.vram[tgPalBase+i*4:])
	for p := 0; p < 2; p++ {
		t.pal[p][i] = t.colour(p, w)
	}
}

func (t *tileGen) colour(p int, w uint32) uint32 {
	if w&0x8000 != 0 {
		return 0 // transparent
	}
	r := int32((w&0x1F)*255/31) + t.offR[p]
	g := int32((w>>5&0x1F)*255/31) + t.offG[p]
	b := int32((w>>10&0x1F)*255/31) + t.offB[p]
	clamp := func(v int32) uint32 {
		if v < 0 {
			return 0
		}
		if v > 255 {
			return 255
		}
		return uint32(v)
	}
	return 0xFF000000 | clamp(r)<<16 | clamp(g)<<8 | clamp(b)
}

func (t *tileGen) recomputePal(p int) {
	for i := uint32(0); i < 32768; i++ {
		w := binary.LittleEndian.Uint32(t.vram[tgPalBase+i*4:])
		t.pal[p][i] = t.colour(p, w)
	}
}

// writes from the processor (addresses within the VRAM, big-endian values)

func (t *tileGen) write8(a uint32, v uint8) {
	t.vram[a] = v
	if a >= tgPalBase {
		t.updatePal((a - tgPalBase) >> 2)
	}
}

func (t *tileGen) write16(a uint32, v uint16) {
	binary.BigEndian.PutUint16(t.vram[a:], v)
	if a >= tgPalBase {
		t.updatePal((a - tgPalBase) >> 2)
	}
}

func (t *tileGen) write32(a uint32, v uint32) {
	binary.BigEndian.PutUint32(t.vram[a:], v)
	if a >= tgPalBase {
		t.updatePal((a - tgPalBase) >> 2)
	}
}

// writeReg: a register write (v already byte-reversed to the chip's view).
// It returns true for an IRQ acknowledge (reg 0x10), with the bits in v&0xFF.
func (t *tileGen) writeReg(reg uint32, v uint32) {
	reg &= 0xFC
	switch reg {
	case 0x40, 0x44:
		p := int(reg-0x40) / 4
		if t.regs[reg/4] != v {
			t.offB[p] = int32(int8(v >> 16))
			t.offG[p] = int32(int8(v >> 8))
			t.offR[p] = int32(int8(v))
			t.regs[reg/4] = v
			t.recomputePal(p)
		}
	}
	t.regs[reg/4] = v
}

var debugLayerMask = func() uint32 {
	var m uint32
	fmt.Sscanf(os.Getenv("M3_TGOFF"), "%x", &m)
	return m
}()

func (t *tileGen) enabled(l int) bool {
	return t.regs[0x60/4+l]&0x80000000 != 0 && debugLayerMask&(1<<l) == 0
}
func (t *tileGen) above3D(l int) bool { return t.regs[0x20/4]>>(8+l)&1 != 0 }
func (t *tileGen) is4Bit(l int) bool  { return t.regs[0x20/4]>>(12+l)&1 != 0 }

// drawLine draws one line of the four layers into the bottom and top
// buffers (B and B' first, then A and A' over them).
func (t *tileGen) drawLine(line int) {
	for pair := 1; pair >= 0; pair-- {
		prim, alt := pair*2, pair*2+1
		has := [2]bool{t.enabled(prim), t.enabled(alt)}
		if !has[0] && !has[1] {
			continue
		}
		dst := [2][]uint32{t.surface(prim)[line*screenW : (line+1)*screenW], t.surface(alt)[line*screenW : (line+1)*screenW]}
		maskWord := t.word(0xF7000 + uint32(line)*4)
		var lineMask uint32
		if prim < 2 {
			lineMask = maskWord >> 16
		} else {
			lineMask = maskWord & 0xFFFF
		}
		var sx, sy [2]int
		for k, l := range [2]int{prim, alt} {
			r := t.regs[0x60/4+l]
			sy[k] = int(r >> 16 & 0x1FF)
			if r&0x8000 != 0 {
				w := t.word(0xF6000 + uint32(l)*0x400 + uint32(line/2)*4)
				sx[k] = int(w >> ((1 - uint(line%2)) * 16) & 0xFFFF)
			} else {
				sx[k] = int(r & 0x3FF)
			}
		}
		pixMask := func(x int) int {
			if lineMask&(1<<(15-uint(x/32))) != 0 {
				return 1
			}
			return 0
		}
		for x := 0; x < screenW; {
			layer := (pixMask(x) + 1) & 1 // 0 = primary
			maskSwitch := pixMask(x) != pixMask(x+15)
			if !has[layer] {
				x += 32 - (x & 15)
				continue
			}
			l := prim + layer
			tx := x
			if maskSwitch {
				tx = x - 16
			}
			xi := ((tx + sx[layer]) / 16) & 0x1F
			yi := ((line + sy[layer]) / 8) & 0x3F
			pairNum := uint32(yi*32 + xi)
			hStart := (x + sx[layer]) & 15
			hEnd := 16
			if maskSwitch {
				hEnd = 16 - (x & 15)
			}
			vFine := uint32((line + sy[layer]) & 7)
			data := t.word(0xF8000 + uint32(l)*0x2000 + pairNum*4)
			pal := &t.pal[l/2]
			out := dst[layer]
			if t.is4Bit(l) {
				p0 := ((data>>16&0x3FFF)<<1 | data>>31&1) * 32
				p1 := ((data&0x3FFF)<<1 | data>>15&1) * 32
				pi := [2]uint32{data >> 16 & 0x7FF0, data & 0x7FF0}
				pat := [2]uint32{t.word(p0 + vFine*4), t.word(p1 + vFine*4)}
				for i := hStart; i < hEnd; i++ {
					if x >= screenW {
						break
					}
					p := pat[i/8] >> ((7 - uint(i%8)) * 4) & 0xF
					if c := pal[pi[i/8]|p]; c != 0 {
						out[x] = c
					}
					x++
				}
			} else {
				p0 := (data >> 16 & 0x3FFF) * 64
				p1 := (data & 0x3FFF) * 64
				pi := [2]uint32{data >> 16 & 0x7F00, data & 0x7F00}
				pat := [4]uint32{t.word(p0 + vFine*8), t.word(p0 + vFine*8 + 4), t.word(p1 + vFine*8), t.word(p1 + vFine*8 + 4)}
				for i := hStart; i < hEnd; i++ {
					if x >= screenW {
						break
					}
					p := pat[i/4] >> ((3 - uint(i%4)) * 8) & 0xFF
					if c := pal[pi[i/8]|p]; c != 0 {
						out[x] = c
					}
					x++
				}
			}
		}
	}
}

func (t *tileGen) surface(l int) []uint32 {
	if t.above3D(l) {
		return t.top[:]
	}
	return t.bottom[:]
}
