package nds

import (
	"math"

	"retropixels/cores/pixelds/arm"
)

// RetroPixels' DS BIOS: the BIOS calls (SWI) are answered here, in Go. Only
// the exception vectors, the interrupt dispatcher and the IntrWait loop are
// ARM code (bios_stub.go), because they run the game's own code.

type memBus interface {
	Read8(uint32) uint32
	Read16(uint32) uint32
	Read32(uint32) uint32
	Write8(uint32, uint32)
	Write16(uint32, uint32)
	Write32(uint32, uint32)
}

func (m *Machine) swi9(n uint32) bool { return m.swi(m.arm9, m.bus9, true, n) }
func (m *Machine) swi7(n uint32) bool { return m.swi(m.arm7, m.bus7, false, n) }

func (m *Machine) swi(c *arm.CPU, bus memBus, arm9 bool, n uint32) bool {
	r := &c.R
	c.Cycles += 16
	switch n {
	case 0x00: // SoftReset: not used by games
	case 0x03: // WaitByLoop
		c.Cycles += int64(r[0]) * 4
	case 0x04, 0x05: // IntrWait, VBlankIntrWait
		if n == 0x05 {
			r[0], r[1] = 1, 1
		}
		if arm9 {
			c.EnterSWI(0xffff0000 + arm9IntrWait)
		} else {
			c.EnterSWI(arm7IntrWait)
		}
	case 0x06: // Halt
		if arm9 {
			m.halt9()
		} else if m.irq7.ie&m.irq7.ifl == 0 {
			c.Halted = true
		}
	case 0x07: // Sleep (ARM7): treated as Halt
		if !arm9 && m.irq7.ie&m.irq7.ifl == 0 {
			c.Halted = true
		}
	case 0x08: // SoundBias (ARM7)
		if !arm9 {
			if r[0] != 0 {
				m.spu.bias = 0x200
			} else {
				m.spu.bias = 0
			}
		}
	case 0x09: // Div
		num, den := int32(r[0]), int32(r[1])
		if den == 0 {
			r[0], r[1], r[3] = 0, uint32(num), 0
			break
		}
		if num == math.MinInt32 && den == -1 {
			r[0], r[1], r[3] = uint32(num), 0, uint32(num)
			break
		}
		q := num / den
		r[0], r[1] = uint32(q), uint32(num%den)
		if q < 0 {
			q = -q
		}
		r[3] = uint32(q)
	case 0x0b: // CpuSet
		cpuSet(bus, r[0], r[1], r[2])
	case 0x0c: // CpuFastSet
		cpuFastSet(bus, r[0], r[1], r[2])
	case 0x0d: // Sqrt
		r[0] = uint32(math.Sqrt(float64(r[0])))
	case 0x0e: // GetCRC16
		crc := uint16(r[0])
		for i := uint32(0); i < r[2]; i++ {
			crc = crc16(crc, []byte{byte(bus.Read8(r[1] + i))})
		}
		r[0] = uint32(crc)
	case 0x0f: // IsDebugger
		r[0] = 0
	case 0x10: // BitUnPack
		bitUnPack(bus, r[0], r[1], r[2])
	case 0x11: // LZ77UnCompReadNormalWrite8bit
		writeOut(bus, r[1], lz77(bus, r[0]), false)
	case 0x12: // LZ77UnCompReadByCallbackWrite16bit (ARM9) / LZ77 Write16bit
		writeOut(bus, r[1], lz77(bus, r[0]), true)
	case 0x13: // HuffUnCompReadByCallback
		writeOut(bus, r[1], huffman(bus, r[0]), false)
	case 0x14: // RLUnCompReadNormalWrite8bit
		writeOut(bus, r[1], rle(bus, r[0]), false)
	case 0x15: // RLUnCompReadByCallbackWrite16bit
		writeOut(bus, r[1], rle(bus, r[0]), true)
	case 0x16: // Diff8bitUnFilterWrite8bit
		writeOut(bus, r[1], diff8(bus, r[0]), false)
	case 0x18: // Diff16bitUnFilterWrite16bit
		writeOut(bus, r[1], diff16(bus, r[0]), true)
	case 0x1a: // GetSineTable (ARM7)
		r[0] = uint32(int32(math.Round(math.Sin(float64(r[0]&63)*math.Pi/128) * 0x8000)))
	case 0x1b: // GetPitchTable (ARM7)
		r[0] = pitchTable(r[0])
	case 0x1c: // GetVolumeTable (ARM7)
		r[0] = volumeTable(r[0])
	case 0x1f: // CustomPost
		if arm9 {
			m.postflg9 |= 0
		}
	default:
		// other calls (and the DSi's) do nothing here
	}
	return true
}

func pitchTable(i uint32) uint32 {
	if i > 767 {
		i = 767
	}
	return uint32(math.Round(0x10000*math.Pow(2, float64(i)/768))) - 0x10000
}

// volumeTable: the SDK reads level i (0.1 dB steps from -72.3 dB) and adds
// the channel's divider itself: /2 below -6 dB, /4 below -12 dB, /16 below
// -24 dB, so the table's values are scaled up by those amounts.
func volumeTable(i uint32) uint32 {
	if i > 723 {
		i = 723
	}
	db := float64(int(i) - 723)
	scale := 1.0
	switch {
	case db < -240:
		scale = 16
	case db < -120:
		scale = 4
	case db < -60:
		scale = 2
	}
	v := math.Round(127 * math.Pow(10, db/200) * scale)
	if v > 127 {
		v = 127
	}
	return uint32(v)
}

func cpuSet(bus memBus, src, dst, ctl uint32) {
	n := ctl & 0x1fffff
	fill := ctl&(1<<24) != 0
	if ctl&(1<<26) != 0 {
		src &^= 3
		dst &^= 3
		v := bus.Read32(src)
		for i := uint32(0); i < n; i++ {
			if !fill {
				v = bus.Read32(src + 4*i)
			}
			bus.Write32(dst+4*i, v)
		}
		return
	}
	src &^= 1
	dst &^= 1
	v := bus.Read16(src)
	for i := uint32(0); i < n; i++ {
		if !fill {
			v = bus.Read16(src + 2*i)
		}
		bus.Write16(dst+2*i, v)
	}
}

func cpuFastSet(bus memBus, src, dst, ctl uint32) {
	n := (ctl&0x1fffff + 7) &^ 7
	fill := ctl&(1<<24) != 0
	src &^= 3
	dst &^= 3
	v := bus.Read32(src)
	for i := uint32(0); i < n; i++ {
		if !fill {
			v = bus.Read32(src + 4*i)
		}
		bus.Write32(dst+4*i, v)
	}
}

func bitUnPack(bus memBus, src, dst, info uint32) {
	length := bus.Read16(info)
	sw := bus.Read8(info + 2)
	dw := bus.Read8(info + 3)
	off := bus.Read32(info + 4)
	zeroToo := off&(1<<31) != 0
	off &= 0x7fffffff
	if sw == 0 || dw == 0 || sw > 8 || dw > 32 {
		return
	}
	var out uint32
	outBits := uint32(0)
	smask := uint32(1)<<sw - 1
	for i := uint32(0); i < length; i++ {
		b := bus.Read8(src + i)
		for bit := uint32(0); bit < 8; bit += sw {
			v := b >> bit & smask
			if v != 0 || zeroToo {
				v += off
			}
			out |= v << outBits
			outBits += dw
			if outBits >= 32 {
				bus.Write32(dst, out)
				dst += 4
				out, outBits = 0, 0
			}
		}
	}
}

func writeOut(bus memBus, dst uint32, data []byte, by16 bool) {
	if !by16 {
		for i, b := range data {
			bus.Write8(dst+uint32(i), uint32(b))
		}
		return
	}
	for i := 0; i+1 < len(data); i += 2 {
		bus.Write16(dst+uint32(i), uint32(data[i])|uint32(data[i+1])<<8)
	}
	if len(data)&1 != 0 {
		i := len(data) - 1
		bus.Write16(dst+uint32(i), uint32(data[i]))
	}
}

const maxUnpack = 4 << 20

func header(bus memBus, src uint32) uint32 {
	n := bus.Read32(src) >> 8
	if n > maxUnpack {
		n = maxUnpack
	}
	return n
}

func lz77(bus memBus, src uint32) []byte {
	size := header(bus, src)
	out := make([]byte, 0, size)
	p := src + 4
	for uint32(len(out)) < size {
		flags := bus.Read8(p)
		p++
		for i := 0; i < 8 && uint32(len(out)) < size; i++ {
			if flags&0x80 == 0 {
				out = append(out, byte(bus.Read8(p)))
				p++
			} else {
				b1, b2 := bus.Read8(p), bus.Read8(p+1)
				p += 2
				n := int(b1>>4) + 3
				disp := int(b1&15)<<8 | int(b2)
				for j := 0; j < n && uint32(len(out)) < size; j++ {
					k := len(out) - disp - 1
					if k < 0 {
						out = append(out, 0)
					} else {
						out = append(out, out[k])
					}
				}
			}
			flags <<= 1
		}
	}
	return out
}

func rle(bus memBus, src uint32) []byte {
	size := header(bus, src)
	out := make([]byte, 0, size)
	p := src + 4
	for uint32(len(out)) < size {
		f := bus.Read8(p)
		p++
		if f&0x80 != 0 {
			n := int(f&0x7f) + 3
			b := byte(bus.Read8(p))
			p++
			for j := 0; j < n; j++ {
				out = append(out, b)
			}
		} else {
			n := int(f&0x7f) + 1
			for j := 0; j < n; j++ {
				out = append(out, byte(bus.Read8(p)))
				p++
			}
		}
	}
	return out[:size]
}

func huffman(bus memBus, src uint32) []byte {
	h := bus.Read32(src)
	bitsPer := h & 15
	size := h >> 8
	if size > maxUnpack {
		size = maxUnpack
	}
	out := make([]byte, 0, size)
	if bitsPer != 4 && bitsPer != 8 {
		return make([]byte, size)
	}
	tree := src + 4
	treeSize := (bus.Read8(tree) + 1) * 2
	root := tree + 1
	p := tree + treeSize
	var acc uint32
	accBits := uint32(0)
	node := root
	for uint32(len(out)) < size {
		word := bus.Read32(p)
		p += 4
		for b := 0; b < 32 && uint32(len(out)) < size; b++ {
			dir := word >> 31 & 1
			word <<= 1
			v := bus.Read8(node)
			next := node&^1 + (v&0x3f)*2 + 2
			isLeaf := v>>(7-dir)&1 != 0
			node = next + dir
			if isLeaf {
				acc |= bus.Read8(node) << accBits
				accBits += bitsPer
				node = root
				if accBits == 8 {
					out = append(out, byte(acc))
					acc, accBits = 0, 0
				}
			}
		}
	}
	return out
}

func diff8(bus memBus, src uint32) []byte {
	size := header(bus, src)
	out := make([]byte, size)
	var v byte
	for i := uint32(0); i < size; i++ {
		v += byte(bus.Read8(src + 4 + i))
		out[i] = v
	}
	return out
}

func diff16(bus memBus, src uint32) []byte {
	size := header(bus, src) &^ 1
	out := make([]byte, size)
	var v uint16
	for i := uint32(0); i < size; i += 2 {
		v += uint16(bus.Read16(src + 4 + i))
		out[i], out[i+1] = byte(v), byte(v>>8)
	}
	return out
}
