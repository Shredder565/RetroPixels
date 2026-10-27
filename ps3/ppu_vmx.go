package ps3

// The PPU's vector unit (AltiVec/VMX). A Vec holds 16 bytes in memory
// order: byte 0 is the top byte of word 0. Floating point runs in Java mode
// off (denormals are flushed to zero), as the PS3 sets VSCR[NJ].

import (
	"math"
	"math/bits"
)

func (v Vec) b(i int) uint8 { return uint8(v[i>>2] >> (24 - 8*(i&3))) }
func (v *Vec) setB(i int, x uint8) {
	sh := 24 - 8*(i&3)
	v[i>>2] = v[i>>2]&^(0xff<<sh) | uint32(x)<<sh
}
func (v Vec) h(i int) uint16 { return uint16(v[i>>1] >> (16 - 16*(i&1))) }
func (v *Vec) setH(i int, x uint16) {
	sh := 16 - 16*(i&1)
	v[i>>1] = v[i>>1]&^(0xffff<<sh) | uint32(x)<<sh
}
func (v Vec) f(i int) float32        { return math.Float32frombits(v[i]) }
func (v *Vec) setF(i int, x float32) { v[i] = math.Float32bits(x) }

func (v Vec) bytes() (o [16]byte) {
	for i := 0; i < 4; i++ {
		o[4*i] = byte(v[i] >> 24)
		o[4*i+1] = byte(v[i] >> 16)
		o[4*i+2] = byte(v[i] >> 8)
		o[4*i+3] = byte(v[i])
	}
	return
}

func vecFromBytes(b []byte) (v Vec) {
	for i := 0; i < 4; i++ {
		v[i] = uint32(b[4*i])<<24 | uint32(b[4*i+1])<<16 | uint32(b[4*i+2])<<8 | uint32(b[4*i+3])
	}
	return
}

func (c *PPU) readVec(ea uint32) Vec {
	ea &^= 15
	return Vec{c.Mem.Read32(ea), c.Mem.Read32(ea + 4), c.Mem.Read32(ea + 8), c.Mem.Read32(ea + 12)}
}

func (c *PPU) writeVec(ea uint32, v Vec) {
	ea &^= 15
	c.Mem.Write32(ea, v[0])
	c.Mem.Write32(ea+4, v[1])
	c.Mem.Write32(ea+8, v[2])
	c.Mem.Write32(ea+12, v[3])
}

// execVMXMem runs the vector loads and stores (primary opcode 31).
func (c *PPU) execVMXMem(i uint32, xo uint32) bool {
	d := rD(i)
	switch xo {
	case 6, 38: // lvsl, lvsr
		sh := uint8(c.eaX(i) & 15)
		var v Vec
		for k := 0; k < 16; k++ {
			if xo == 6 {
				v.setB(k, sh+uint8(k))
			} else {
				v.setB(k, 16-sh+uint8(k))
			}
		}
		c.VR[d] = v
	case 7, 39, 71, 103, 359: // lvebx lvehx lvewx lvx lvxl
		c.VR[d] = c.readVec(c.eaX(i))
	case 135: // stvebx
		ea := c.eaX(i)
		c.Mem.Write8(ea, c.VR[d].b(int(ea&15)))
	case 167: // stvehx
		ea := c.eaX(i) &^ 1
		c.Mem.Write16(ea, c.VR[d].h(int(ea&15)>>1))
	case 199: // stvewx
		ea := c.eaX(i) &^ 3
		c.Mem.Write32(ea, c.VR[d][(ea&15)>>2])
	case 231, 487: // stvx stvxl
		c.writeVec(c.eaX(i), c.VR[d])
	case 519, 775: // lvlx lvlxl
		ea := c.eaX(i)
		sh := int(ea & 15)
		src := c.readVec(ea).bytes()
		var o [16]byte
		copy(o[:], src[sh:])
		c.VR[d] = vecFromBytes(o[:])
	case 551, 807: // lvrx lvrxl
		ea := c.eaX(i)
		sh := int(ea & 15)
		var o [16]byte
		if sh != 0 {
			src := c.readVec(ea).bytes()
			copy(o[16-sh:], src[:sh])
		}
		c.VR[d] = vecFromBytes(o[:])
	case 647, 903: // stvlx stvlxl
		ea := c.eaX(i)
		sh := int(ea & 15)
		v := c.VR[d].bytes()
		for k := 0; k < 16-sh; k++ {
			c.Mem.Write8(ea+uint32(k), v[k])
		}
	case 679, 935: // stvrx stvrxl
		ea := c.eaX(i)
		sh := int(ea & 15)
		v := c.VR[d].bytes()
		base := ea &^ 15
		for k := 0; k < sh; k++ {
			c.Mem.Write8(base+uint32(k), v[16-sh+k])
		}
	default:
		return false
	}
	return true
}

// flush: denormals to (signed) zero
func flush(f float32) float32 {
	b := math.Float32bits(f)
	if b&0x7f800000 == 0 {
		return math.Float32frombits(b & 0x80000000)
	}
	return f
}

func sat8s(v int32) (int8, bool) {
	if v > 127 {
		return 127, true
	}
	if v < -128 {
		return -128, true
	}
	return int8(v), false
}
func sat8u(v int32) (uint8, bool) {
	if v > 255 {
		return 255, true
	}
	if v < 0 {
		return 0, true
	}
	return uint8(v), false
}
func sat16s(v int64) (int16, bool) {
	if v > 32767 {
		return 32767, true
	}
	if v < -32768 {
		return -32768, true
	}
	return int16(v), false
}
func sat16u(v int64) (uint16, bool) {
	if v > 65535 {
		return 65535, true
	}
	if v < 0 {
		return 0, true
	}
	return uint16(v), false
}
func sat32s(v int64) (int32, bool) {
	if v > math.MaxInt32 {
		return math.MaxInt32, true
	}
	if v < math.MinInt32 {
		return math.MinInt32, true
	}
	return int32(v), false
}
func sat32u(v int64) (uint32, bool) {
	if v > math.MaxUint32 {
		return math.MaxUint32, true
	}
	if v < 0 {
		return 0, true
	}
	return uint32(v), false
}

func (c *PPU) sat(s bool) {
	if s {
		c.VSCR |= 1
	}
}

func (c *PPU) execVMX(i uint32) {
	d, a, b, cc := rD(i), rA(i), rB(i), rC(i)
	va, vb, vc := c.VR[a], c.VR[b], c.VR[cc]
	var r Vec
	// VA form
	if x := i & 0x3f; x >= 32 {
		switch x {
		case 32: // vmhaddshs
			for k := 0; k < 8; k++ {
				p := int64(int16(va.h(k)))*int64(int16(vb.h(k)))>>15 + int64(int16(vc.h(k)))
				s, o := sat16s(p)
				c.sat(o)
				r.setH(k, uint16(s))
			}
		case 33: // vmhraddshs
			for k := 0; k < 8; k++ {
				p := (int64(int16(va.h(k)))*int64(int16(vb.h(k)))+0x4000)>>15 + int64(int16(vc.h(k)))
				s, o := sat16s(p)
				c.sat(o)
				r.setH(k, uint16(s))
			}
		case 34: // vmladduhm
			for k := 0; k < 8; k++ {
				r.setH(k, va.h(k)*vb.h(k)+vc.h(k))
			}
		case 36: // vmsumubm
			for k := 0; k < 4; k++ {
				s := vc[k]
				for j := 0; j < 4; j++ {
					s += uint32(va.b(4*k+j)) * uint32(vb.b(4*k+j))
				}
				r[k] = s
			}
		case 37: // vmsummbm
			for k := 0; k < 4; k++ {
				s := int32(vc[k])
				for j := 0; j < 4; j++ {
					s += int32(int8(va.b(4*k+j))) * int32(vb.b(4*k+j))
				}
				r[k] = uint32(s)
			}
		case 38, 39: // vmsumuhm, vmsumuhs
			for k := 0; k < 4; k++ {
				s := int64(vc[k])
				for j := 0; j < 2; j++ {
					s += int64(va.h(2*k+j)) * int64(vb.h(2*k+j))
				}
				if x == 39 {
					v, o := sat32u(s)
					c.sat(o)
					r[k] = v
				} else {
					r[k] = uint32(s)
				}
			}
		case 40, 41: // vmsumshm, vmsumshs
			for k := 0; k < 4; k++ {
				s := int64(int32(vc[k]))
				for j := 0; j < 2; j++ {
					s += int64(int16(va.h(2*k+j))) * int64(int16(vb.h(2*k+j)))
				}
				if x == 41 {
					v, o := sat32s(s)
					c.sat(o)
					r[k] = uint32(v)
				} else {
					r[k] = uint32(s)
				}
			}
		case 42: // vsel
			for k := 0; k < 4; k++ {
				r[k] = va[k]&^vc[k] | vb[k]&vc[k]
			}
		case 43: // vperm
			ab := va.bytes()
			bb := vb.bytes()
			for k := 0; k < 16; k++ {
				idx := vc.b(k) & 31
				if idx < 16 {
					r.setB(k, ab[idx])
				} else {
					r.setB(k, bb[idx-16])
				}
			}
		case 44: // vsldoi
			sh := int(i >> 6 & 15)
			ab := va.bytes()
			bb := vb.bytes()
			var all [32]byte
			copy(all[:], ab[:])
			copy(all[16:], bb[:])
			r = vecFromBytes(all[sh : sh+16])
		case 46: // vmaddfp: a*c + b
			for k := 0; k < 4; k++ {
				r.setF(k, flush(float32(math.FMA(float64(flush(va.f(k))), float64(flush(vc.f(k))), float64(flush(vb.f(k)))))))
			}
		case 47: // vnmsubfp: -(a*c - b)
			for k := 0; k < 4; k++ {
				r.setF(k, flush(float32(-math.FMA(float64(flush(va.f(k))), float64(flush(vc.f(k))), -float64(flush(vb.f(k)))))))
			}
		default:
			c.fail(i, "unknown vector instruction")
			return
		}
		c.VR[d] = r
		return
	}
	// compares (VC form)
	switch i & 0x3ff {
	case 6, 70, 134, 198, 454, 518, 582, 646, 710, 774, 838, 902, 966:
		c.vcmp(i, va, vb)
		return
	}
	switch i & 0x7ff {
	// --- integer add/sub
	case 0: // vaddubm
		for k := 0; k < 16; k++ {
			r.setB(k, va.b(k)+vb.b(k))
		}
	case 64: // vadduhm
		for k := 0; k < 8; k++ {
			r.setH(k, va.h(k)+vb.h(k))
		}
	case 128: // vadduwm
		for k := 0; k < 4; k++ {
			r[k] = va[k] + vb[k]
		}
	case 384: // vaddcuw
		for k := 0; k < 4; k++ {
			_, cy := bits.Add32(va[k], vb[k], 0)
			r[k] = cy
		}
	case 512: // vaddubs
		for k := 0; k < 16; k++ {
			s, o := sat8u(int32(va.b(k)) + int32(vb.b(k)))
			c.sat(o)
			r.setB(k, s)
		}
	case 576: // vadduhs
		for k := 0; k < 8; k++ {
			s, o := sat16u(int64(va.h(k)) + int64(vb.h(k)))
			c.sat(o)
			r.setH(k, s)
		}
	case 640: // vadduws
		for k := 0; k < 4; k++ {
			s, o := sat32u(int64(va[k]) + int64(vb[k]))
			c.sat(o)
			r[k] = s
		}
	case 768: // vaddsbs
		for k := 0; k < 16; k++ {
			s, o := sat8s(int32(int8(va.b(k))) + int32(int8(vb.b(k))))
			c.sat(o)
			r.setB(k, uint8(s))
		}
	case 832: // vaddshs
		for k := 0; k < 8; k++ {
			s, o := sat16s(int64(int16(va.h(k))) + int64(int16(vb.h(k))))
			c.sat(o)
			r.setH(k, uint16(s))
		}
	case 896: // vaddsws
		for k := 0; k < 4; k++ {
			s, o := sat32s(int64(int32(va[k])) + int64(int32(vb[k])))
			c.sat(o)
			r[k] = uint32(s)
		}
	case 1024: // vsububm
		for k := 0; k < 16; k++ {
			r.setB(k, va.b(k)-vb.b(k))
		}
	case 1088: // vsubuhm
		for k := 0; k < 8; k++ {
			r.setH(k, va.h(k)-vb.h(k))
		}
	case 1152: // vsubuwm
		for k := 0; k < 4; k++ {
			r[k] = va[k] - vb[k]
		}
	case 1408: // vsubcuw
		for k := 0; k < 4; k++ {
			if va[k] >= vb[k] {
				r[k] = 1
			}
		}
	case 1536: // vsububs
		for k := 0; k < 16; k++ {
			s, o := sat8u(int32(va.b(k)) - int32(vb.b(k)))
			c.sat(o)
			r.setB(k, s)
		}
	case 1600: // vsubuhs
		for k := 0; k < 8; k++ {
			s, o := sat16u(int64(va.h(k)) - int64(vb.h(k)))
			c.sat(o)
			r.setH(k, s)
		}
	case 1664: // vsubuws
		for k := 0; k < 4; k++ {
			s, o := sat32u(int64(va[k]) - int64(vb[k]))
			c.sat(o)
			r[k] = s
		}
	case 1792: // vsubsbs
		for k := 0; k < 16; k++ {
			s, o := sat8s(int32(int8(va.b(k))) - int32(int8(vb.b(k))))
			c.sat(o)
			r.setB(k, uint8(s))
		}
	case 1856: // vsubshs
		for k := 0; k < 8; k++ {
			s, o := sat16s(int64(int16(va.h(k))) - int64(int16(vb.h(k))))
			c.sat(o)
			r.setH(k, uint16(s))
		}
	case 1920: // vsubsws
		for k := 0; k < 4; k++ {
			s, o := sat32s(int64(int32(va[k])) - int64(int32(vb[k])))
			c.sat(o)
			r[k] = uint32(s)
		}
	// --- max/min/avg
	case 2: // vmaxub
		for k := 0; k < 16; k++ {
			r.setB(k, max(va.b(k), vb.b(k)))
		}
	case 66: // vmaxuh
		for k := 0; k < 8; k++ {
			r.setH(k, max(va.h(k), vb.h(k)))
		}
	case 130: // vmaxuw
		for k := 0; k < 4; k++ {
			r[k] = max(va[k], vb[k])
		}
	case 258: // vmaxsb
		for k := 0; k < 16; k++ {
			r.setB(k, uint8(max(int8(va.b(k)), int8(vb.b(k)))))
		}
	case 322: // vmaxsh
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(max(int16(va.h(k)), int16(vb.h(k)))))
		}
	case 386: // vmaxsw
		for k := 0; k < 4; k++ {
			r[k] = uint32(max(int32(va[k]), int32(vb[k])))
		}
	case 514: // vminub
		for k := 0; k < 16; k++ {
			r.setB(k, min(va.b(k), vb.b(k)))
		}
	case 578: // vminuh
		for k := 0; k < 8; k++ {
			r.setH(k, min(va.h(k), vb.h(k)))
		}
	case 642: // vminuw
		for k := 0; k < 4; k++ {
			r[k] = min(va[k], vb[k])
		}
	case 770: // vminsb
		for k := 0; k < 16; k++ {
			r.setB(k, uint8(min(int8(va.b(k)), int8(vb.b(k)))))
		}
	case 834: // vminsh
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(min(int16(va.h(k)), int16(vb.h(k)))))
		}
	case 898: // vminsw
		for k := 0; k < 4; k++ {
			r[k] = uint32(min(int32(va[k]), int32(vb[k])))
		}
	case 1026: // vavgub
		for k := 0; k < 16; k++ {
			r.setB(k, uint8((uint32(va.b(k))+uint32(vb.b(k))+1)>>1))
		}
	case 1090: // vavguh
		for k := 0; k < 8; k++ {
			r.setH(k, uint16((uint32(va.h(k))+uint32(vb.h(k))+1)>>1))
		}
	case 1154: // vavguw
		for k := 0; k < 4; k++ {
			r[k] = uint32((uint64(va[k]) + uint64(vb[k]) + 1) >> 1)
		}
	case 1282: // vavgsb
		for k := 0; k < 16; k++ {
			r.setB(k, uint8((int32(int8(va.b(k)))+int32(int8(vb.b(k)))+1)>>1))
		}
	case 1346: // vavgsh
		for k := 0; k < 8; k++ {
			r.setH(k, uint16((int32(int16(va.h(k)))+int32(int16(vb.h(k)))+1)>>1))
		}
	case 1410: // vavgsw
		for k := 0; k < 4; k++ {
			r[k] = uint32((int64(int32(va[k])) + int64(int32(vb[k])) + 1) >> 1)
		}
	// --- logic
	case 1028:
		for k := 0; k < 4; k++ {
			r[k] = va[k] & vb[k]
		}
	case 1092:
		for k := 0; k < 4; k++ {
			r[k] = va[k] &^ vb[k]
		}
	case 1156:
		for k := 0; k < 4; k++ {
			r[k] = va[k] | vb[k]
		}
	case 1220:
		for k := 0; k < 4; k++ {
			r[k] = va[k] ^ vb[k]
		}
	case 1284:
		for k := 0; k < 4; k++ {
			r[k] = ^(va[k] | vb[k])
		}
	// --- shifts and rotates
	case 4: // vrlb
		for k := 0; k < 16; k++ {
			r.setB(k, bits.RotateLeft8(va.b(k), int(vb.b(k)&7)))
		}
	case 68: // vrlh
		for k := 0; k < 8; k++ {
			r.setH(k, bits.RotateLeft16(va.h(k), int(vb.h(k)&15)))
		}
	case 132: // vrlw
		for k := 0; k < 4; k++ {
			r[k] = bits.RotateLeft32(va[k], int(vb[k]&31))
		}
	case 260: // vslb
		for k := 0; k < 16; k++ {
			r.setB(k, va.b(k)<<(vb.b(k)&7))
		}
	case 324: // vslh
		for k := 0; k < 8; k++ {
			r.setH(k, va.h(k)<<(vb.h(k)&15))
		}
	case 388: // vslw
		for k := 0; k < 4; k++ {
			r[k] = va[k] << (vb[k] & 31)
		}
	case 516: // vsrb
		for k := 0; k < 16; k++ {
			r.setB(k, va.b(k)>>(vb.b(k)&7))
		}
	case 580: // vsrh
		for k := 0; k < 8; k++ {
			r.setH(k, va.h(k)>>(vb.h(k)&15))
		}
	case 644: // vsrw
		for k := 0; k < 4; k++ {
			r[k] = va[k] >> (vb[k] & 31)
		}
	case 772: // vsrab
		for k := 0; k < 16; k++ {
			r.setB(k, uint8(int8(va.b(k))>>(vb.b(k)&7)))
		}
	case 836: // vsrah
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(int16(va.h(k))>>(vb.h(k)&15)))
		}
	case 900: // vsraw
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(va[k]) >> (vb[k] & 31))
		}
	case 452, 708: // vsl, vsr (whole register by bits)
		sh := uint(vb.b(15) & 7)
		hi := uint64(va[0])<<32 | uint64(va[1])
		lo := uint64(va[2])<<32 | uint64(va[3])
		if sh != 0 {
			if i&0x7ff == 452 {
				hi = hi<<sh | lo>>(64-sh)
				lo <<= sh
			} else {
				lo = lo>>sh | hi<<(64-sh)
				hi >>= sh
			}
		}
		r = Vec{uint32(hi >> 32), uint32(hi), uint32(lo >> 32), uint32(lo)}
	case 1036, 1100: // vslo, vsro (by octets)
		sh := int(vb.b(15) >> 3 & 15)
		ab := va.bytes()
		var o [16]byte
		if i&0x7ff == 1036 {
			copy(o[:], ab[sh:])
		} else {
			copy(o[sh:], ab[:16-sh])
		}
		r = vecFromBytes(o[:])
	// --- merges, splats, packs, unpacks
	case 12: // vmrghb
		for k := 0; k < 8; k++ {
			r.setB(2*k, va.b(k))
			r.setB(2*k+1, vb.b(k))
		}
	case 76: // vmrghh
		for k := 0; k < 4; k++ {
			r.setH(2*k, va.h(k))
			r.setH(2*k+1, vb.h(k))
		}
	case 140: // vmrghw
		r = Vec{va[0], vb[0], va[1], vb[1]}
	case 268: // vmrglb
		for k := 0; k < 8; k++ {
			r.setB(2*k, va.b(k+8))
			r.setB(2*k+1, vb.b(k+8))
		}
	case 332: // vmrglh
		for k := 0; k < 4; k++ {
			r.setH(2*k, va.h(k+4))
			r.setH(2*k+1, vb.h(k+4))
		}
	case 396: // vmrglw
		r = Vec{va[2], vb[2], va[3], vb[3]}
	case 524: // vspltb
		x := vb.b(int(a & 15))
		for k := 0; k < 16; k++ {
			r.setB(k, x)
		}
	case 588: // vsplth
		x := vb.h(int(a & 7))
		for k := 0; k < 8; k++ {
			r.setH(k, x)
		}
	case 652: // vspltw
		x := vb[a&3]
		r = Vec{x, x, x, x}
	case 780: // vspltisb
		x := uint8(int8(a<<3) >> 3)
		for k := 0; k < 16; k++ {
			r.setB(k, x)
		}
	case 844: // vspltish
		x := uint16(int16(int8(a<<3) >> 3))
		for k := 0; k < 8; k++ {
			r.setH(k, x)
		}
	case 908: // vspltisw
		x := uint32(int32(int8(a<<3) >> 3))
		r = Vec{x, x, x, x}
	case 14: // vpkuhum
		for k := 0; k < 8; k++ {
			r.setB(k, uint8(va.h(k)))
			r.setB(k+8, uint8(vb.h(k)))
		}
	case 78: // vpkuwum
		for k := 0; k < 4; k++ {
			r.setH(k, uint16(va[k]))
			r.setH(k+4, uint16(vb[k]))
		}
	case 142: // vpkuhus
		for k := 0; k < 8; k++ {
			x, o1 := sat8u(int32(min(va.h(k), 255)) + 0)
			y, o2 := sat8u(int32(min(vb.h(k), 255)))
			c.sat(o1 || o2 || va.h(k) > 255 || vb.h(k) > 255)
			r.setB(k, x)
			r.setB(k+8, y)
		}
	case 206: // vpkuwus
		for k := 0; k < 4; k++ {
			x, o1 := sat16u(int64(va[k]))
			y, o2 := sat16u(int64(vb[k]))
			c.sat(o1 || o2)
			r.setH(k, x)
			r.setH(k+4, y)
		}
	case 270: // vpkshus
		for k := 0; k < 8; k++ {
			x, o1 := sat8u(int32(int16(va.h(k))))
			y, o2 := sat8u(int32(int16(vb.h(k))))
			c.sat(o1 || o2)
			r.setB(k, x)
			r.setB(k+8, y)
		}
	case 334: // vpkswus
		for k := 0; k < 4; k++ {
			x, o1 := sat16u(int64(int32(va[k])))
			y, o2 := sat16u(int64(int32(vb[k])))
			c.sat(o1 || o2)
			r.setH(k, x)
			r.setH(k+4, y)
		}
	case 398: // vpkshss
		for k := 0; k < 8; k++ {
			x, o1 := sat8s(int32(int16(va.h(k))))
			y, o2 := sat8s(int32(int16(vb.h(k))))
			c.sat(o1 || o2)
			r.setB(k, uint8(x))
			r.setB(k+8, uint8(y))
		}
	case 462: // vpkswss
		for k := 0; k < 4; k++ {
			x, o1 := sat16s(int64(int32(va[k])))
			y, o2 := sat16s(int64(int32(vb[k])))
			c.sat(o1 || o2)
			r.setH(k, uint16(x))
			r.setH(k+4, uint16(y))
		}
	case 782: // vpkpx
		px := func(w uint32) uint16 {
			return uint16(w>>24&1)<<15 | uint16(w>>19&0x1f)<<10 | uint16(w>>11&0x1f)<<5 | uint16(w>>3&0x1f)
		}
		for k := 0; k < 4; k++ {
			r.setH(k, px(va[k]))
			r.setH(k+4, px(vb[k]))
		}
	case 526: // vupkhsb
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(int16(int8(vb.b(k)))))
		}
	case 654: // vupklsb
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(int16(int8(vb.b(k+8)))))
		}
	case 590: // vupkhsh
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(vb.h(k))))
		}
	case 718: // vupklsh
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(vb.h(k + 4))))
		}
	case 846, 974: // vupkhpx, vupklpx
		off := 0
		if i&0x7ff == 974 {
			off = 4
		}
		for k := 0; k < 4; k++ {
			p := vb.h(k + off)
			a8 := uint32(0)
			if p&0x8000 != 0 {
				a8 = 0xff
			}
			r[k] = a8<<24 | uint32(p>>10&0x1f)<<16 | uint32(p>>5&0x1f)<<8 | uint32(p&0x1f)
		}
	// --- multiplies
	case 8, 520: // vmuloub, vmuleub
		o := 1
		if i&0x7ff == 520 {
			o = 0
		}
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(va.b(2*k+o))*uint16(vb.b(2*k+o)))
		}
	case 264, 776: // vmulosb, vmulesb
		o := 1
		if i&0x7ff == 776 {
			o = 0
		}
		for k := 0; k < 8; k++ {
			r.setH(k, uint16(int16(int8(va.b(2*k+o)))*int16(int8(vb.b(2*k+o)))))
		}
	case 72, 584: // vmulouh, vmuleuh
		o := 1
		if i&0x7ff == 584 {
			o = 0
		}
		for k := 0; k < 4; k++ {
			r[k] = uint32(va.h(2*k+o)) * uint32(vb.h(2*k+o))
		}
	case 328, 840: // vmulosh, vmulesh
		o := 1
		if i&0x7ff == 840 {
			o = 0
		}
		for k := 0; k < 4; k++ {
			r[k] = uint32(int32(int16(va.h(2*k+o))) * int32(int16(vb.h(2*k+o))))
		}
	// --- sums
	case 1544: // vsum4ubs
		for k := 0; k < 4; k++ {
			s := int64(vb[k])
			for j := 0; j < 4; j++ {
				s += int64(va.b(4*k + j))
			}
			v, o := sat32u(s)
			c.sat(o)
			r[k] = v
		}
	case 1800: // vsum4sbs
		for k := 0; k < 4; k++ {
			s := int64(int32(vb[k]))
			for j := 0; j < 4; j++ {
				s += int64(int8(va.b(4*k + j)))
			}
			v, o := sat32s(s)
			c.sat(o)
			r[k] = uint32(v)
		}
	case 1608: // vsum4shs
		for k := 0; k < 4; k++ {
			s := int64(int32(vb[k])) + int64(int16(va.h(2*k))) + int64(int16(va.h(2*k+1)))
			v, o := sat32s(s)
			c.sat(o)
			r[k] = uint32(v)
		}
	case 1672: // vsum2sws
		for k := 0; k < 2; k++ {
			s := int64(int32(vb[2*k+1])) + int64(int32(va[2*k])) + int64(int32(va[2*k+1]))
			v, o := sat32s(s)
			c.sat(o)
			r[2*k+1] = uint32(v)
		}
	case 1928: // vsumsws
		s := int64(int32(vb[3]))
		for k := 0; k < 4; k++ {
			s += int64(int32(va[k]))
		}
		v, o := sat32s(s)
		c.sat(o)
		r[3] = uint32(v)
	// --- floating point
	case 10: // vaddfp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(flush(va.f(k))+flush(vb.f(k))))
		}
	case 74: // vsubfp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(flush(va.f(k))-flush(vb.f(k))))
		}
	case 1034: // vmaxfp
		for k := 0; k < 4; k++ {
			x, y := flush(va.f(k)), flush(vb.f(k))
			if x != x {
				r.setF(k, x)
			} else if y != y {
				r.setF(k, y)
			} else if x > y || (x == y && !math.Signbit(float64(x))) {
				r.setF(k, x)
			} else {
				r.setF(k, y)
			}
		}
	case 1098: // vminfp
		for k := 0; k < 4; k++ {
			x, y := flush(va.f(k)), flush(vb.f(k))
			if x != x {
				r.setF(k, x)
			} else if y != y {
				r.setF(k, y)
			} else if x < y || (x == y && math.Signbit(float64(x))) {
				r.setF(k, x)
			} else {
				r.setF(k, y)
			}
		}
	case 266: // vrefp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(1/flush(vb.f(k))))
		}
	case 330: // vrsqrtefp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(float32(1/math.Sqrt(float64(flush(vb.f(k)))))))
		}
	case 394: // vexptefp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(float32(math.Exp2(float64(flush(vb.f(k)))))))
		}
	case 458: // vlogefp
		for k := 0; k < 4; k++ {
			r.setF(k, flush(float32(math.Log2(float64(flush(vb.f(k)))))))
		}
	case 522, 586, 650, 714: // vrfin vrfiz vrfip vrfim
		for k := 0; k < 4; k++ {
			x := float64(flush(vb.f(k)))
			switch i & 0x7ff {
			case 522:
				x = math.RoundToEven(x)
			case 586:
				x = math.Trunc(x)
			case 650:
				x = math.Ceil(x)
			default:
				x = math.Floor(x)
			}
			r.setF(k, float32(x))
		}
	case 778: // vcfux
		for k := 0; k < 4; k++ {
			r.setF(k, float32(float64(vb[k])/float64(uint64(1)<<a)))
		}
	case 842: // vcfsx
		for k := 0; k < 4; k++ {
			r.setF(k, float32(float64(int32(vb[k]))/float64(uint64(1)<<a)))
		}
	case 906: // vctuxs
		for k := 0; k < 4; k++ {
			x := float64(flush(vb.f(k))) * float64(uint64(1)<<a)
			switch {
			case x != x:
				r[k] = 0
				c.sat(true)
			case x >= 4294967295:
				r[k] = 0xffffffff
				c.sat(true)
			case x < 0:
				r[k] = 0
				c.sat(x <= -1)
			default:
				r[k] = uint32(x)
			}
		}
	case 970: // vctsxs
		for k := 0; k < 4; k++ {
			x := float64(flush(vb.f(k))) * float64(uint64(1)<<a)
			switch {
			case x != x:
				r[k] = 0
				c.sat(true)
			case x >= 2147483647:
				r[k] = 0x7fffffff
				c.sat(true)
			case x <= -2147483648:
				r[k] = 0x80000000
				c.sat(x < -2147483648)
			default:
				r[k] = uint32(int32(x))
			}
		}
	case 1540: // mfvscr
		r = Vec{0, 0, 0, c.VSCR}
	case 1604: // mtvscr
		c.VSCR = vb[3] & 0x10001
		return
	default:
		c.fail(i, "unknown vector instruction")
		return
	}
	c.VR[d] = r
}

func (c *PPU) vcmp(i uint32, va, vb Vec) {
	var r Vec
	xo := i & 0x3ff
	bfp := xo == 966
	switch xo {
	case 6:
		for k := 0; k < 16; k++ {
			if va.b(k) == vb.b(k) {
				r.setB(k, 0xff)
			}
		}
	case 70:
		for k := 0; k < 8; k++ {
			if va.h(k) == vb.h(k) {
				r.setH(k, 0xffff)
			}
		}
	case 134:
		for k := 0; k < 4; k++ {
			if va[k] == vb[k] {
				r[k] = ^uint32(0)
			}
		}
	case 198, 454, 710: // vcmpeqfp, vcmpgefp, vcmpgtfp
		for k := 0; k < 4; k++ {
			x, y := flush(va.f(k)), flush(vb.f(k))
			var t bool
			switch xo {
			case 198:
				t = x == y
			case 454:
				t = x >= y
			default:
				t = x > y
			}
			if t {
				r[k] = ^uint32(0)
			}
		}
	case 518:
		for k := 0; k < 16; k++ {
			if va.b(k) > vb.b(k) {
				r.setB(k, 0xff)
			}
		}
	case 582:
		for k := 0; k < 8; k++ {
			if va.h(k) > vb.h(k) {
				r.setH(k, 0xffff)
			}
		}
	case 646:
		for k := 0; k < 4; k++ {
			if va[k] > vb[k] {
				r[k] = ^uint32(0)
			}
		}
	case 774:
		for k := 0; k < 16; k++ {
			if int8(va.b(k)) > int8(vb.b(k)) {
				r.setB(k, 0xff)
			}
		}
	case 838:
		for k := 0; k < 8; k++ {
			if int16(va.h(k)) > int16(vb.h(k)) {
				r.setH(k, 0xffff)
			}
		}
	case 902:
		for k := 0; k < 4; k++ {
			if int32(va[k]) > int32(vb[k]) {
				r[k] = ^uint32(0)
			}
		}
	case 966: // vcmpbfp
		for k := 0; k < 4; k++ {
			x, y := flush(va.f(k)), flush(vb.f(k))
			var w uint32
			if !(x <= y) {
				w |= 0x80000000
			}
			if !(x >= -y) {
				w |= 0x40000000
			}
			r[k] = w
		}
	}
	c.VR[rD(i)] = r
	if i&0x400 != 0 {
		all := r[0]&r[1]&r[2]&r[3] == ^uint32(0)
		none := r[0]|r[1]|r[2]|r[3] == 0
		var f uint32
		if bfp {
			if none {
				f = 2
			}
		} else {
			if all {
				f |= 8
			}
			if none {
				f |= 2
			}
		}
		c.setCRField(6, f)
	}
}
