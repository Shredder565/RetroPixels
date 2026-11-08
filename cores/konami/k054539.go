package konami

import (
	"math"

	"retropixels/cores/arcade"
)

// K054539 is Konami's 8-channel PCM / DPCM sound chip (X-Men and the later
// 68000 boards): 8-bit and 16-bit PCM and 4-bit DPCM channels with looping,
// panning and a reverb buffer. It makes one stereo sample every 384 clocks
// (48 kHz from the usual 18.432 MHz crystal).
type K054539 struct {
	Regs     [0x230]uint8
	rom      []byte
	reverb   [0x4000]int16
	posLatch [8][3]uint8
	ch       [8]k054539Chan
	revPos   int
	curPtr   int
	romAddr  int

	voltab [256]float64
	pantab [15]float64
}

type k054539Chan struct {
	pos       int
	pfrac     int
	val, pval int
}

// NewK054539 makes the chip with its sample ROM.
func NewK054539(rom []byte) *K054539 {
	k := &K054539{rom: rom}
	if len(k.rom) == 0 {
		k.rom = make([]byte, 1)
	}
	for i := range k.voltab {
		k.voltab[i] = math.Pow(10, (-36*float64(i)/0x40)/20) / 4
	}
	for i := range k.pantab {
		k.pantab[i] = math.Sqrt(float64(i)) / math.Sqrt(0xe)
	}
	k.Reset()
	return k
}

// Reset is the chip's power-on state.
func (k *K054539) Reset() {
	k.Regs = [0x230]uint8{}
	k.posLatch = [8][3]uint8{}
	k.reverb = [0x4000]int16{}
	k.ch = [8]k054539Chan{}
	k.revPos, k.curPtr, k.romAddr = 0, 0, 0
}

func (k *K054539) regUpdate() bool { return k.Regs[0x22f]&0x80 == 0 }

func (k *K054539) keyOn(c int) {
	if k.regUpdate() {
		k.Regs[0x22c] |= 1 << c
	}
}

func (k *K054539) keyOff(c int) {
	if k.regUpdate() {
		k.Regs[0x22c] &^= 1 << c
	}
}

func (k *K054539) romByte(a int) int {
	return int(k.rom[uint32(a)&0xffffff%uint32(len(k.rom))])
}

func (k *K054539) ramByte(a int) uint8 {
	w := uint16(k.reverb[a>>1])
	if a&1 != 0 {
		return uint8(w >> 8)
	}
	return uint8(w)
}

func (k *K054539) setRAMByte(a int, v uint8) {
	w := uint16(k.reverb[a>>1])
	if a&1 != 0 {
		w = w&0x00ff | uint16(v)<<8
	} else {
		w = w&0xff00 | uint16(v)
	}
	k.reverb[a>>1] = int16(w)
}

// Write is a write to register r (0x000-0x22f).
func (k *K054539) Write(r int, v uint8) {
	if r < 0 || r >= len(k.Regs) {
		return
	}
	latch := k.Regs[0x22f]&1 != 0 // the chip updates channel positions at key-on
	if latch && r < 0x100 {
		if o := (r & 0x1f) - 0xc; o >= 0 && o <= 2 {
			k.posLatch[r>>5][o] = v
			return
		}
	} else {
		switch r {
		case 0x214:
			for c := 0; c < 8; c++ {
				if v&(1<<c) == 0 {
					continue
				}
				if latch {
					copy(k.Regs[c<<5+0xc:c<<5+0xf], k.posLatch[c][:])
				}
				k.keyOn(c)
			}
		case 0x215:
			for c := 0; c < 8; c++ {
				if v&(1<<c) != 0 {
					k.keyOff(c)
				}
			}
		case 0x22d:
			if k.romAddr == 0x80 {
				k.setRAMByte((k.curPtr&0x3fff)|(k.curPtr&0x10000)>>2, v)
			}
			k.curPtr = (k.curPtr + 1) & 0x1ffff
		case 0x22e:
			k.romAddr, k.curPtr = int(v), 0
		}
	}
	k.Regs[r] = v
}

// Read is a read of register r.
func (k *K054539) Read(r int) uint8 {
	if r < 0 || r >= len(k.Regs) {
		return 0
	}
	if r == 0x22d {
		if k.Regs[0x22f]&0x10 == 0 {
			return 0
		}
		var v uint8
		if k.romAddr == 0x80 {
			v = k.ramByte((k.curPtr & 0x3fff) | (k.curPtr&0x10000)>>2)
		} else {
			v = uint8(k.romByte(0x20000*k.romAddr + k.curPtr))
		}
		k.curPtr = (k.curPtr + 1) & 0x1ffff
		return v
	}
	return k.Regs[r]
}

var k054539DPCM = [16]int{
	0 * 0x100, 1 * 0x100, 2 * 0x100, 4 * 0x100, 8 * 0x100, 16 * 0x100, 32 * 0x100, 64 * 0x100,
	0 * 0x100, -64 * 0x100, -32 * 0x100, -16 * 0x100, -8 * 0x100, -4 * 0x100, -2 * 0x100, -1 * 0x100,
}

// Sample makes the next stereo sample (each side about -1..1).
func (k *K054539) Sample() (l, r float64) {
	if k.Regs[0x22f]&1 == 0 {
		return 0, 0
	}
	const volCap = 1.80
	lval := float64(k.reverb[k.revPos])
	rval := lval
	k.reverb[k.revPos] = 0
	for c := 0; c < 8; c++ {
		if k.Regs[0x22c]&(1<<c) == 0 {
			continue
		}
		b1 := k.Regs[0x20*c : 0x20*c+0x20]
		b2 := k.Regs[0x200+2*c : 0x200+2*c+2]
		ch := &k.ch[c]
		delta := int(b1[0]) | int(b1[1])<<8 | int(b1[2])<<16
		vol := int(b1[3])
		bval := min(vol+int(b1[4]), 255)
		pan := int(b1[5])
		switch {
		case pan >= 0x81 && pan <= 0x8f:
			pan -= 0x81
		case pan >= 0x11 && pan <= 0x1f:
			pan -= 0x11
		default:
			pan = 0x18 - 0x11
		}
		lvol := min(k.voltab[vol]*k.pantab[pan], volCap)
		rvol := min(k.voltab[vol]*k.pantab[0xe-pan], volCap)
		rbvol := min(k.voltab[bval]/2, volCap)
		rdelta := (int(b1[6]) | int(b1[7])<<8) >> 3
		rdelta = (rdelta + k.revPos) & 0x3fff
		curPos := int(b1[0xc]) | int(b1[0xd])<<8 | int(b1[0xe])<<16
		fdelta, pdelta := -0x10000, 1
		if b2[0]&0x20 != 0 {
			delta, fdelta, pdelta = -delta, 0x10000, -1
		}
		var pfrac, val, pval int
		if curPos != ch.pos {
			ch.pos = curPos
		} else {
			pfrac, val, pval = ch.pfrac, ch.val, ch.pval
		}
		loopAt := int(b1[8]) | int(b1[9])<<8 | int(b1[0xa])<<16
		switch b2[0] & 0xc {
		case 0x0: // 8-bit PCM
			pfrac += delta
			for pfrac&^0xffff != 0 {
				pfrac += fdelta
				curPos += pdelta
				pval = val
				val = int(int16(uint16(k.romByte(curPos) << 8)))
				if val == -0x8000 && b2[1]&1 != 0 {
					curPos = loopAt
					val = int(int16(uint16(k.romByte(curPos) << 8)))
				}
				if val == -0x8000 {
					k.keyOff(c)
					val = 0
					break
				}
			}
		case 0x4: // 16-bit PCM, low byte first
			pdelta <<= 1
			pfrac += delta
			for pfrac&^0xffff != 0 {
				pfrac += fdelta
				curPos += pdelta
				pval = val
				val = int(int16(uint16(k.romByte(curPos) | k.romByte(curPos+1)<<8)))
				if val == -0x8000 && b2[1]&1 != 0 {
					curPos = loopAt
					val = int(int16(uint16(k.romByte(curPos) | k.romByte(curPos+1)<<8)))
				}
				if val == -0x8000 {
					k.keyOff(c)
					val = 0
					break
				}
			}
		case 0x8: // 4-bit DPCM
			curPos <<= 1
			pfrac <<= 1
			if pfrac&0x10000 != 0 {
				pfrac &= 0xffff
				curPos |= 1
			}
			pfrac += delta
			for pfrac&^0xffff != 0 {
				pfrac += fdelta
				curPos += pdelta
				pval = val
				val = k.romByte(curPos >> 1)
				if val == 0x88 && b2[1]&1 != 0 {
					curPos = loopAt << 1
					val = k.romByte(curPos >> 1)
				}
				if val == 0x88 {
					k.keyOff(c)
					val = 0
					break
				}
				if curPos&1 != 0 {
					val >>= 4
				} else {
					val &= 15
				}
				val = max(-32768, min(32767, pval+k054539DPCM[val]))
			}
			pfrac >>= 1
			if curPos&1 != 0 {
				pfrac |= 0x8000
			}
			curPos >>= 1
		}
		lval += float64(val) * lvol
		rval += float64(val) * rvol
		ri := (rdelta + k.revPos) & 0x1fff
		k.reverb[ri] += int16(int32(float64(val) * rbvol))
		ch.pos, ch.pfrac, ch.pval, ch.val = curPos, pfrac, pval, val
		if k.regUpdate() {
			b1[0xc], b1[0xd], b1[0xe] = uint8(curPos), uint8(curPos>>8), uint8(curPos>>16)
		}
	}
	k.revPos = (k.revPos + 1) & 0x1fff
	return lval / 32768, rval / 32768
}

// AppendState / LoadState: everything the chip remembers.
func (k *K054539) AppendState(w *arcade.StateW) {
	w.Bytes(k.Regs[:])
	for i := range k.reverb {
		w.U16(uint16(k.reverb[i]))
	}
	for c := range k.posLatch {
		w.Bytes(k.posLatch[c][:])
	}
	for c := range k.ch {
		w.Ints([]int{k.ch[c].pos, k.ch[c].pfrac, k.ch[c].val, k.ch[c].pval})
	}
	w.Ints([]int{k.revPos, k.curPtr, k.romAddr})
}

func (k *K054539) LoadState(r *arcade.StateR) {
	r.Bytes(k.Regs[:])
	for i := range k.reverb {
		k.reverb[i] = int16(r.U16())
	}
	for c := range k.posLatch {
		r.Bytes(k.posLatch[c][:])
	}
	v := make([]int, 4)
	for c := range k.ch {
		r.Ints(v)
		k.ch[c] = k054539Chan{pos: v[0], pfrac: v[1], val: v[2], pval: v[3]}
	}
	m := make([]int, 3)
	r.Ints(m)
	k.revPos, k.curPtr, k.romAddr = m[0], m[1], m[2]
}
