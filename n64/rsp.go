package n64

import (
	"encoding/binary"
	"math/bits"
)

// RSP: the Reality Signal Processor — a MIPS-like scalar unit plus a vector
// unit with 32 registers of 8 x 16-bit lanes. Games upload their own
// microcode to it for graphics and audio.

// Reciprocal / inverse square root tables of the vector unit, generated from
// the formulas they're defined by.
var rcpROM, rsqROM [512]uint16

func init() {
	for i := 0; i < 512; i++ {
		rcpROM[i] = uint16((((uint64(1) << 34) / uint64(i+512)) + 1) >> 8)
		a := uint64(i + 512)
		if i&1 != 0 {
			a = uint64(i+512) >> 1
		} else {
			a = uint64(i + 512)
		}
		a = uint64((i + 512)) >> uint(i&1)
		b := uint64(1) << 17
		for a*(b+1)*(b+1) < uint64(1)<<44 {
			b++
		}
		rsqROM[i] = uint16(b >> 1)
	}
}

type RSP struct {
	m       *Machine
	r       [32]uint32
	pc      uint32
	next    uint32
	nDelay  bool
	delay   bool
	vr      [32][8]uint16
	acc     [8]int64 // 48-bit accumulators
	vcoLo   uint8
	vcoHi   uint8
	vccLo   uint8
	vccHi   uint8
	vce     uint8
	divIn   int32
	divOut  int32
	divLoad bool
	Cycles  uint64
}

func (r *RSP) reset() {
	m := r.m
	*r = RSP{m: m}
	r.next = 4
}

// setPC moves the RSP to a new address (from the CPU side).
func (r *RSP) setPC(v uint32) {
	r.pc = v & 0xffc
	r.next = (r.pc + 4) & 0xffc
	r.nDelay = false
}

func (r *RSP) dmem() []byte { return r.m.spmem[:0x1000] }

// ---------------------------------------------------------------------
// SP registers and DMA
// ---------------------------------------------------------------------

func (m *Machine) spRead(r uint32) uint32 {
	sp := &m.sp
	switch r {
	case 0:
		return sp.curMem
	case 1:
		return sp.curDram
	case 2, 3:
		return sp.length
	case 4:
		return sp.status
	case 5, 6:
		return 0
	case 7:
		v := sp.semaphore
		sp.semaphore = 1
		return v
	}
	return 0
}

func (m *Machine) spWrite(r uint32, v uint32) {
	sp := &m.sp
	switch r {
	case 0:
		sp.memAddr = v & 0x1ff8
	case 1:
		sp.dramAddr = v & 0xfffff8
	case 2:
		m.spDMA(v, true)
	case 3:
		m.spDMA(v, false)
	case 4:
		m.spStatusWrite(v)
	case 7:
		sp.semaphore = 0
	}
}

func (m *Machine) spStatusWrite(v uint32) {
	s := m.sp.status
	if v&1 != 0 && s&1 != 0 {
		// starting: let the RSP run before the CPU goes on
		s &^= 1
		m.rspTime = m.CPU.Cycles
		m.stop = true
	}
	if v&2 != 0 {
		s |= 1
	}
	if v&4 != 0 {
		s &^= 2
	}
	if v&8 != 0 {
		m.clear(intSP)
	}
	if v&16 != 0 {
		m.raise(intSP)
	}
	if v&32 != 0 {
		s &^= 32
	}
	if v&64 != 0 {
		s |= 32
	}
	if v&128 != 0 {
		s &^= 64
	}
	if v&256 != 0 {
		s |= 64
	}
	for i := uint32(0); i < 8; i++ {
		if v&(1<<(9+i*2)) != 0 {
			s &^= 1 << (7 + i)
		}
		if v&(1<<(10+i*2)) != 0 {
			s |= 1 << (7 + i)
		}
	}
	m.sp.status = s
}

// spDMA copies count rows of length bytes between RDRAM and DMEM/IMEM, skipping
// skip bytes in RDRAM between rows. The written address registers stay as
// they are for the next transfer; reads show where this one ended.
func (m *Machine) spDMA(v uint32, toSP bool) {
	sp := &m.sp
	length := ((v & 0xfff) | 7) + 1
	count := ((v >> 12) & 0xff) + 1
	skip := (v >> 20) & 0xfff
	mem := sp.memAddr & 0x1000
	ma := sp.memAddr & 0xff8
	da := sp.dramAddr & 0xfffff8
	for c := uint32(0); c < count; c++ {
		if c > 0 {
			da = (da + skip) & 0xfffff8
		}
		for i := uint32(0); i < length; i += 8 {
			sa := mem | ((ma + i) & 0xff8)
			d := (da + i) & 0xfffff8
			if d+8 > rdramSize {
				continue
			}
			if toSP {
				copy(m.spmem[sa:sa+8], m.rdram[d:d+8])
			} else {
				copy(m.rdram[d:d+8], m.spmem[sa:sa+8])
			}
		}
		ma = (ma + length) & 0xff8
		da = (da + length) & 0xfffff8
	}
	// the address counter carries into the DMEM/IMEM bit when it runs off
	// the end of a 4 KB bank
	sp.curMem = (sp.memAddr&0x1ff8 + count*length) & 0x1fff
	sp.curDram = da
	sp.length = skip<<20 | 0xff8
}

// ---------------------------------------------------------------------
// scalar unit
// ---------------------------------------------------------------------

func (r *RSP) imem32(pc uint32) uint32 {
	return binary.BigEndian.Uint32(r.m.spmem[0x1000+(pc&0xffc):])
}

func (r *RSP) ld8(a uint32) uint8 { return r.m.spmem[a&0xfff] }
func (r *RSP) ld16(a uint32) uint16 {
	d := r.m.spmem[:0x1000]
	return uint16(d[a&0xfff])<<8 | uint16(d[(a+1)&0xfff])
}
func (r *RSP) ld32(a uint32) uint32 {
	d := r.m.spmem[:0x1000]
	if a&3 == 0 && a&0xfff <= 0xffc {
		return binary.BigEndian.Uint32(d[a&0xfff:])
	}
	return uint32(d[a&0xfff])<<24 | uint32(d[(a+1)&0xfff])<<16 | uint32(d[(a+2)&0xfff])<<8 | uint32(d[(a+3)&0xfff])
}
func (r *RSP) st8(a uint32, v uint8) { r.m.spmem[a&0xfff] = v }
func (r *RSP) st16(a uint32, v uint16) {
	d := r.m.spmem[:0x1000]
	d[a&0xfff] = uint8(v >> 8)
	d[(a+1)&0xfff] = uint8(v)
}
func (r *RSP) st32(a uint32, v uint32) {
	d := r.m.spmem[:0x1000]
	d[a&0xfff] = uint8(v >> 24)
	d[(a+1)&0xfff] = uint8(v >> 16)
	d[(a+2)&0xfff] = uint8(v >> 8)
	d[(a+3)&0xfff] = uint8(v)
}

// run executes up to n cycles (one instruction per cycle).
func (r *RSP) run(n int) {
	m := r.m
	for i := 0; i < n; i++ {
		if m.sp.status&1 != 0 {
			return
		}
		pc := r.pc & 0xffc
		op := r.imem32(pc)
		r.delay = r.nDelay
		r.nDelay = false
		r.pc = r.next & 0xffc
		r.next = (r.pc + 4) & 0xffc
		r.exec(op, pc)
		r.r[0] = 0
		r.Cycles++
		if m.sp.status&32 != 0 && !r.nDelay { // single step
			m.sp.status |= 1
		}
	}
}

func (r *RSP) branch(cond bool, target uint32) {
	r.nDelay = true
	if cond {
		r.next = target & 0xffc
	}
}

func (r *RSP) exec(op uint32, pc uint32) {
	rs := (op >> 21) & 31
	rt := (op >> 16) & 31
	imm := uint32(int32(int16(op)))
	g := &r.r
	switch op >> 26 {
	case 0x00:
		rd := (op >> 11) & 31
		sa := (op >> 6) & 31
		switch op & 63 {
		case 0x00:
			g[rd] = g[rt] << sa
		case 0x02:
			g[rd] = g[rt] >> sa
		case 0x03:
			g[rd] = uint32(int32(g[rt]) >> sa)
		case 0x04:
			g[rd] = g[rt] << (g[rs] & 31)
		case 0x06:
			g[rd] = g[rt] >> (g[rs] & 31)
		case 0x07:
			g[rd] = uint32(int32(g[rt]) >> (g[rs] & 31))
		case 0x08:
			r.branch(true, g[rs])
		case 0x09:
			t := g[rs]
			g[rd] = (pc + 8) & 0xfff
			r.branch(true, t)
		case 0x0d: // BREAK
			st := r.m.sp.status | 3
			r.m.sp.status = st
			if st&64 != 0 {
				r.m.raise(intSP)
			}
		case 0x20, 0x21:
			g[rd] = g[rs] + g[rt]
		case 0x22, 0x23:
			g[rd] = g[rs] - g[rt]
		case 0x24:
			g[rd] = g[rs] & g[rt]
		case 0x25:
			g[rd] = g[rs] | g[rt]
		case 0x26:
			g[rd] = g[rs] ^ g[rt]
		case 0x27:
			g[rd] = ^(g[rs] | g[rt])
		case 0x2a:
			g[rd] = b2u(int32(g[rs]) < int32(g[rt]))
		case 0x2b:
			g[rd] = b2u(g[rs] < g[rt])
		}
	case 0x01:
		v := int32(g[rs])
		t := pc + 4 + imm<<2
		switch rt {
		case 0x00:
			r.branch(v < 0, t)
		case 0x01:
			r.branch(v >= 0, t)
		case 0x10:
			g[31] = (pc + 8) & 0xfff
			r.branch(v < 0, t)
		case 0x11:
			g[31] = (pc + 8) & 0xfff
			r.branch(v >= 0, t)
		}
	case 0x02:
		r.branch(true, (op&0x3ff)<<2)
	case 0x03:
		g[31] = (pc + 8) & 0xfff
		r.branch(true, (op&0x3ff)<<2)
	case 0x04:
		r.branch(g[rs] == g[rt], pc+4+imm<<2)
	case 0x05:
		r.branch(g[rs] != g[rt], pc+4+imm<<2)
	case 0x06:
		r.branch(int32(g[rs]) <= 0, pc+4+imm<<2)
	case 0x07:
		r.branch(int32(g[rs]) > 0, pc+4+imm<<2)
	case 0x08, 0x09:
		g[rt] = g[rs] + imm
	case 0x0a:
		g[rt] = b2u(int32(g[rs]) < int32(imm))
	case 0x0b:
		g[rt] = b2u(g[rs] < imm)
	case 0x0c:
		g[rt] = g[rs] & (op & 0xffff)
	case 0x0d:
		g[rt] = g[rs] | (op & 0xffff)
	case 0x0e:
		g[rt] = g[rs] ^ (op & 0xffff)
	case 0x0f:
		g[rt] = op << 16
	case 0x10: // COP0
		rd := (op >> 11) & 15
		switch rs {
		case 0x00:
			g[rt] = r.cop0Read(rd)
		case 0x04:
			r.cop0Write(rd, g[rt])
		}
	case 0x12:
		r.cop2(op)
	case 0x20:
		g[rt] = uint32(int32(int8(r.ld8(g[rs] + imm))))
	case 0x21:
		g[rt] = uint32(int32(int16(r.ld16(g[rs] + imm))))
	case 0x23, 0x27:
		g[rt] = r.ld32(g[rs] + imm)
	case 0x24:
		g[rt] = uint32(r.ld8(g[rs] + imm))
	case 0x25:
		g[rt] = uint32(r.ld16(g[rs] + imm))
	case 0x28:
		r.st8(g[rs]+imm, uint8(g[rt]))
	case 0x29:
		r.st16(g[rs]+imm, uint16(g[rt]))
	case 0x2b:
		r.st32(g[rs]+imm, g[rt])
	case 0x32:
		r.lwc2(op)
	case 0x3a:
		r.swc2(op)
	}
}

func b2u(b bool) uint32 {
	if b {
		return 1
	}
	return 0
}

func (r *RSP) cop0Read(rd uint32) uint32 {
	m := r.m
	if rd < 8 {
		return m.spRead(rd)
	}
	return m.dpRead(rd - 8)
}

func (r *RSP) cop0Write(rd uint32, v uint32) {
	m := r.m
	if rd < 8 {
		m.spWrite(rd, v)
		return
	}
	m.dpWrite(rd-8, v)
}

// ---------------------------------------------------------------------
// vector unit
// ---------------------------------------------------------------------

// element patterns for the "e" field
var elemSel = [16][8]int{
	{0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7},
	{0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
	{0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
	{0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
	{4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
}

func (r *RSP) vbyte(v uint32, i uint32) uint8 {
	w := r.vr[v][(i>>1)&7]
	if i&1 == 0 {
		return uint8(w >> 8)
	}
	return uint8(w)
}

func (r *RSP) setVbyte(v uint32, i uint32, b uint8) {
	p := &r.vr[v][(i>>1)&7]
	if i&1 == 0 {
		*p = (*p & 0x00ff) | uint16(b)<<8
	} else {
		*p = (*p & 0xff00) | uint16(b)
	}
}

func sclamp16(v int64) uint16 {
	if v > 32767 {
		return 0x7fff
	}
	if v < -32768 {
		return 0x8000
	}
	return uint16(int16(v))
}

func sext48(v int64) int64 { return (v << 16) >> 16 }

// signed clamp of accumulator bits 16..47
func accHiClamp(a int64) uint16 { return sclamp16(a >> 16) }

// accumulator low part, clamped by bits 16..47
func accLoClamp(a int64) uint16 {
	hi := a >> 16
	if hi < -32768 {
		return 0
	}
	if hi > 32767 {
		return 0xffff
	}
	return uint16(a)
}

func (r *RSP) cop2(op uint32) {
	rs := (op >> 21) & 31
	rt := (op >> 16) & 31
	rd := (op >> 11) & 31
	if op&(1<<25) == 0 {
		e := (op >> 7) & 15
		switch rs {
		case 0x00: // MFC2
			hi := r.vbyte(rd, e)
			lo := r.vbyte(rd, (e+1)&15)
			r.r[rt] = uint32(int32(int16(uint16(hi)<<8 | uint16(lo))))
		case 0x02: // CFC2
			var v uint16
			switch rd & 3 {
			case 0:
				v = uint16(r.vcoHi)<<8 | uint16(r.vcoLo)
			case 1:
				v = uint16(r.vccHi)<<8 | uint16(r.vccLo)
			default:
				v = uint16(r.vce)
			}
			r.r[rt] = uint32(int32(int16(v)))
		case 0x04: // MTC2
			v := uint16(r.r[rt])
			r.setVbyte(rd, e, uint8(v>>8))
			if e < 15 {
				r.setVbyte(rd, e+1, uint8(v))
			}
		case 0x06: // CTC2
			v := r.r[rt]
			switch rd & 3 {
			case 0:
				r.vcoHi, r.vcoLo = uint8(v>>8), uint8(v)
			case 1:
				r.vccHi, r.vccLo = uint8(v>>8), uint8(v)
			default:
				r.vce = uint8(v)
			}
		}
		return
	}
	// vector computational instruction
	e := (op >> 21) & 15
	vt := (op >> 16) & 31
	vs := (op >> 11) & 31
	vd := (op >> 6) & 31
	sel := &elemSel[e]
	var t [8]uint16
	for i := 0; i < 8; i++ {
		t[i] = r.vr[vt][sel[i]]
	}
	s := r.vr[vs]
	acc := &r.acc
	var res [8]uint16
	switch op & 63 {
	case 0x00, 0x08: // VMULF / VMACF
		for i := 0; i < 8; i++ {
			p := int64(int16(s[i])) * int64(int16(t[i])) * 2
			if op&63 == 0 {
				acc[i] = sext48(p + 0x8000)
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			res[i] = accHiClamp(acc[i])
		}
	case 0x01, 0x09: // VMULU / VMACU
		for i := 0; i < 8; i++ {
			p := int64(int16(s[i])) * int64(int16(t[i])) * 2
			if op&63 == 1 {
				acc[i] = sext48(p + 0x8000)
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			h := acc[i] >> 16
			if h < 0 {
				res[i] = 0
			} else if h > 32767 {
				res[i] = 0xffff
			} else {
				res[i] = uint16(h)
			}
		}
	case 0x02, 0x0a: // VRNDP / VRNDN
		for i := 0; i < 8; i++ {
			v := int64(int16(t[i]))
			if vs&1 != 0 {
				v <<= 16
			}
			if (op&63 == 2 && acc[i] >= 0) || (op&63 == 0x0a && acc[i] < 0) {
				acc[i] = sext48(acc[i] + v)
			}
			res[i] = accHiClamp(acc[i])
		}
	case 0x03, 0x0b: // VMULQ / VMACQ
		for i := 0; i < 8; i++ {
			if op&63 == 3 {
				p := int64(int16(s[i])) * int64(int16(t[i])) << 16
				if p < 0 {
					p += 31 << 16
				}
				acc[i] = sext48(p)
			} else {
				// nudge toward zero unless bit 21 is already set
				a := acc[i]
				if a&(1<<21) == 0 {
					if a < 0 {
						a += 32 << 16
					} else {
						a -= 32 << 16
					}
				}
				acc[i] = sext48(a)
			}
			res[i] = sclamp16(acc[i]>>17) &^ 15
		}
	case 0x04, 0x0c: // VMUDL / VMADL
		for i := 0; i < 8; i++ {
			p := int64(uint32(s[i]) * uint32(t[i]) >> 16)
			if op&63 == 4 {
				acc[i] = p
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			res[i] = accLoClamp(acc[i])
		}
	case 0x05, 0x0d: // VMUDM / VMADM
		for i := 0; i < 8; i++ {
			p := int64(int16(s[i])) * int64(t[i])
			if op&63 == 5 {
				acc[i] = sext48(p)
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			res[i] = accHiClamp(acc[i])
		}
	case 0x06, 0x0e: // VMUDN / VMADN
		for i := 0; i < 8; i++ {
			p := int64(s[i]) * int64(int16(t[i]))
			if op&63 == 6 {
				acc[i] = sext48(p)
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			res[i] = accLoClamp(acc[i])
		}
	case 0x07, 0x0f: // VMUDH / VMADH
		for i := 0; i < 8; i++ {
			p := int64(int16(s[i])) * int64(int16(t[i])) << 16
			if op&63 == 7 {
				acc[i] = sext48(p)
			} else {
				acc[i] = sext48(acc[i] + p)
			}
			res[i] = accHiClamp(acc[i])
		}
	case 0x10: // VADD
		for i := 0; i < 8; i++ {
			v := int64(int16(s[i])) + int64(int16(t[i])) + int64((r.vcoLo>>i)&1)
			setAccLo(acc, i, uint16(v))
			res[i] = sclamp16(v)
		}
		r.vcoLo, r.vcoHi = 0, 0
	case 0x11: // VSUB
		for i := 0; i < 8; i++ {
			v := int64(int16(s[i])) - int64(int16(t[i])) - int64((r.vcoLo>>i)&1)
			setAccLo(acc, i, uint16(v))
			res[i] = sclamp16(v)
		}
		r.vcoLo, r.vcoHi = 0, 0
	case 0x13: // VABS
		for i := 0; i < 8; i++ {
			sv, tv := int16(s[i]), int16(t[i])
			var v uint16
			switch {
			case sv < 0:
				if tv == -32768 {
					setAccLo(acc, i, 0x8000)
					res[i] = 0x7fff
					continue
				}
				v = uint16(-tv)
			case sv == 0:
				v = 0
			default:
				v = uint16(tv)
			}
			setAccLo(acc, i, v)
			res[i] = v
		}
	case 0x14: // VADDC
		r.vcoLo, r.vcoHi = 0, 0
		for i := 0; i < 8; i++ {
			v := uint32(s[i]) + uint32(t[i])
			if v > 0xffff {
				r.vcoLo |= 1 << i
			}
			setAccLo(acc, i, uint16(v))
			res[i] = uint16(v)
		}
	case 0x15: // VSUBC
		r.vcoLo, r.vcoHi = 0, 0
		for i := 0; i < 8; i++ {
			v := int32(s[i]) - int32(t[i])
			if v < 0 {
				r.vcoLo |= 1 << i
			}
			if v != 0 {
				r.vcoHi |= 1 << i
			}
			setAccLo(acc, i, uint16(v))
			res[i] = uint16(v)
		}
	case 0x1d: // VSAR
		for i := 0; i < 8; i++ {
			switch e {
			case 8:
				res[i] = uint16(acc[i] >> 32)
			case 9:
				res[i] = uint16(acc[i] >> 16)
			case 10:
				res[i] = uint16(acc[i])
			default:
				res[i] = 0
			}
		}
	case 0x20, 0x21, 0x22, 0x23: // VLT VEQ VNE VGE
		var lo uint8
		for i := 0; i < 8; i++ {
			sv, tv := int16(s[i]), int16(t[i])
			co := (r.vcoLo>>i)&1 != 0
			ne := (r.vcoHi>>i)&1 != 0
			var c bool
			switch op & 63 {
			case 0x20:
				c = sv < tv || (sv == tv && co && ne)
			case 0x21:
				c = sv == tv && !ne
			case 0x22:
				c = sv != tv || ne
			default:
				c = sv > tv || (sv == tv && !(co && ne))
			}
			v := t[i]
			if c {
				lo |= 1 << i
				v = s[i]
			}
			setAccLo(acc, i, v)
			res[i] = v
		}
		r.vccLo, r.vccHi = lo, 0
		r.vcoLo, r.vcoHi = 0, 0
	case 0x24: // VCL
		for i := 0; i < 8; i++ {
			sv, tv := uint32(s[i]), uint32(t[i])
			bit := uint8(1) << i
			le := r.vccLo&bit != 0
			ge := r.vccHi&bit != 0
			var v uint16
			if r.vcoLo&bit != 0 { // signs differed
				if r.vcoHi&bit == 0 {
					sum := sv + tv
					lz := sum&0xffff == 0
					uz := sum <= 0xffff
					if r.vce&bit != 0 {
						le = lz || uz
					} else {
						le = lz && uz
					}
				}
				if le {
					v = uint16(-int32(tv))
				} else {
					v = uint16(sv)
				}
			} else {
				if r.vcoHi&bit == 0 {
					ge = sv >= tv
				}
				if ge {
					v = uint16(tv)
				} else {
					v = uint16(sv)
				}
			}
			r.vccLo = setBit(r.vccLo, bit, le)
			r.vccHi = setBit(r.vccHi, bit, ge)
			setAccLo(acc, i, v)
			res[i] = v
		}
		r.vcoLo, r.vcoHi, r.vce = 0, 0, 0
	case 0x25: // VCH
		r.vcoLo, r.vcoHi, r.vccLo, r.vccHi, r.vce = 0, 0, 0, 0, 0
		for i := 0; i < 8; i++ {
			sv, tv := int32(int16(s[i])), int32(int16(t[i]))
			bit := uint8(1) << i
			var le, ge, vce, ne bool
			var v uint16
			if (sv ^ tv) < 0 {
				ge = tv < 0
				le = sv+tv <= 0
				vce = sv+tv == -1
				ne = sv+tv != 0 && !vce
				if le {
					v = uint16(-tv)
				} else {
					v = uint16(sv)
				}
				r.vcoLo |= bit
			} else {
				le = tv < 0
				ge = sv-tv >= 0
				ne = sv-tv != 0
				if ge {
					v = uint16(tv)
				} else {
					v = uint16(sv)
				}
			}
			r.vccLo = setBit(r.vccLo, bit, le)
			r.vccHi = setBit(r.vccHi, bit, ge)
			r.vce = setBit(r.vce, bit, vce)
			r.vcoHi = setBit(r.vcoHi, bit, ne)
			setAccLo(acc, i, v)
			res[i] = v
		}
	case 0x26: // VCR
		r.vcoLo, r.vcoHi, r.vccLo, r.vccHi, r.vce = 0, 0, 0, 0, 0
		for i := 0; i < 8; i++ {
			sv, tv := int32(int16(s[i])), int32(int16(t[i]))
			bit := uint8(1) << i
			var le, ge bool
			var v uint16
			if (sv ^ tv) < 0 {
				ge = tv < 0
				le = sv+tv+1 <= 0
				if le {
					v = uint16(^tv)
				} else {
					v = uint16(sv)
				}
			} else {
				le = tv < 0
				ge = sv-tv >= 0
				if ge {
					v = uint16(tv)
				} else {
					v = uint16(sv)
				}
			}
			r.vccLo = setBit(r.vccLo, bit, le)
			r.vccHi = setBit(r.vccHi, bit, ge)
			setAccLo(acc, i, v)
			res[i] = v
		}
	case 0x27: // VMRG
		for i := 0; i < 8; i++ {
			v := t[i]
			if (r.vccLo>>i)&1 != 0 {
				v = s[i]
			}
			setAccLo(acc, i, v)
			res[i] = v
		}
		r.vcoLo, r.vcoHi = 0, 0
	case 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d: // VAND VNAND VOR VNOR VXOR VNXOR
		for i := 0; i < 8; i++ {
			var v uint16
			switch op & 63 {
			case 0x28:
				v = s[i] & t[i]
			case 0x29:
				v = ^(s[i] & t[i])
			case 0x2a:
				v = s[i] | t[i]
			case 0x2b:
				v = ^(s[i] | t[i])
			case 0x2c:
				v = s[i] ^ t[i]
			default:
				v = ^(s[i] ^ t[i])
			}
			setAccLo(acc, i, v)
			res[i] = v
		}
	case 0x30, 0x31, 0x32, 0x34, 0x35, 0x36: // VRCP VRCPL VRCPH VRSQ VRSQL VRSQH
		de := vs & 7
		in := t[e&7]
		for i := 0; i < 8; i++ {
			setAccLo(acc, i, t[i])
		}
		switch op & 63 {
		case 0x32, 0x36: // high half in, previous high result out
			r.divIn = int32(in) << 16
			r.divLoad = true
			r.vr[vd][de] = uint16(r.divOut >> 16)
			return
		}
		var input int32
		if (op&63 == 0x31 || op&63 == 0x35) && r.divLoad {
			input = r.divIn | int32(in)
		} else {
			input = int32(int16(in))
		}
		r.divLoad = false
		var out int32
		if op&63 <= 0x31 {
			out = rcp32(input, op&63 == 0x30 || !r.divLoadUsed(op))
		} else {
			out = rsq32(input)
		}
		r.divOut = out
		r.vr[vd][de] = uint16(out)
		return
	case 0x33: // VMOV
		de := vs & 7
		for i := 0; i < 8; i++ {
			setAccLo(acc, i, t[i])
		}
		// the source lane is the destination lane after the element pattern
		r.vr[vd][de] = t[de]
		return
	case 0x37, 0x3f: // VNOP / VNULL
		return
	default:
		// reserved opcodes: the adder still runs into the accumulator's
		// low part, the result register gets zero
		for i := 0; i < 8; i++ {
			setAccLo(acc, i, s[i]+t[i])
			res[i] = 0
		}
	}
	r.vr[vd] = res
}

func (r *RSP) divLoadUsed(op uint32) bool { return false }

func setBit(v uint8, bit uint8, on bool) uint8 {
	if on {
		return v | bit
	}
	return v &^ bit
}

func setAccLo(acc *[8]int64, i int, v uint16) {
	acc[i] = sext48((acc[i] &^ 0xffff) | int64(v))
}

func rcp32(input int32, _ bool) int32 {
	if input == 0 {
		return 0x7fffffff
	}
	if input == -32768 {
		return -65536
	}
	x := input
	if x < 0 {
		x = -x
	}
	shift := bits.LeadingZeros32(uint32(x))
	idx := (uint32(x) << uint(shift) & 0x7fc00000) >> 22
	res := int32(((0x10000 | uint32(rcpROM[idx])) << 14) >> uint(31-shift))
	if input < 0 {
		res = ^res
	}
	return res
}

func rsq32(input int32) int32 {
	if input == 0 {
		return 0x7fffffff
	}
	if input == -32768 {
		return -65536
	}
	x := input
	if x < 0 {
		x = -x
	}
	shift := bits.LeadingZeros32(uint32(x))
	idx := (uint32(x) << uint(shift) & 0x7fc00000) >> 22
	idx = ((idx | 0x200) & 0x3fe) | uint32(shift&1)
	res := int32(((0x10000 | uint32(rsqROM[idx&511])) << 14) >> uint((31-shift)>>1))
	if input < 0 {
		res = ^res
	}
	return res
}

// ---------------------------------------------------------------------
// vector loads and stores
// ---------------------------------------------------------------------

func (r *RSP) lwc2(op uint32) {
	base := r.r[(op>>21)&31]
	vt := (op >> 16) & 31
	kind := (op >> 11) & 31
	e := (op >> 7) & 15
	off := uint32(int32(op<<25) >> 25)
	d := r.m.spmem[:0x1000]
	switch kind {
	case 0, 1, 2, 3: // LBV LSV LLV LDV
		n := uint32(1) << kind
		a := base + off*n
		for i := uint32(0); i < n && e+i < 16; i++ {
			r.setVbyte(vt, e+i, d[(a+i)&0xfff])
		}
	case 4: // LQV
		a := base + off*16
		end := (a &^ 15) + 16
		for i := uint32(0); a+i < end && e+i < 16; i++ {
			r.setVbyte(vt, e+i, d[(a+i)&0xfff])
		}
	case 5: // LRV
		a := base + off*16
		start := a &^ 15
		n := a & 15
		for i := uint32(0); i < n; i++ {
			idx := 16 - n + i + e
			if idx < 16 {
				r.setVbyte(vt, idx, d[(start+i)&0xfff])
			}
		}
	case 6, 7: // LPV LUV
		a := base + off*8
		start := a &^ 7
		o := (a & 7) - e
		sh := uint(8)
		if kind == 7 {
			sh = 7
		}
		for i := uint32(0); i < 8; i++ {
			r.vr[vt][i] = uint16(d[(start+((o+i)&15))&0xfff]) << sh
		}
	case 8: // LHV
		a := base + off*16
		start := a &^ 7
		o := (a & 7) - e
		for i := uint32(0); i < 8; i++ {
			r.vr[vt][i] = uint16(d[(start+((o+i*2)&15))&0xfff]) << 7
		}
	case 9: // LFV
		a := base + off*16
		start := a &^ 7
		o := (a & 7) - e
		var tmp [8]uint16
		for i := uint32(0); i < 4; i++ {
			tmp[i] = uint16(d[(start+((o+i*4)&15))&0xfff]) << 7
			tmp[i+4] = uint16(d[(start+((o+i*4+8)&15))&0xfff]) << 7
		}
		for i := e; i < e+8 && i < 16; i++ {
			b := uint8(tmp[(i>>1)&7] >> 8)
			if i&1 != 0 {
				b = uint8(tmp[(i>>1)&7])
			}
			r.setVbyte(vt, i, b)
		}
	case 11: // LTV
		a := base + off*16
		vbase := vt &^ 7
		start := a &^ 7
		for i := uint32(0); i < 8; i++ {
			reg := vbase + ((i + e/2) & 7)
			for b := uint32(0); b < 2; b++ {
				byteIdx := (i*2 + b)
				r.setVbyte(reg, byteIdx, d[(start+((a&8)+byteIdx+e)&15)&0xfff])
			}
		}
	}
}

func (r *RSP) swc2(op uint32) {
	base := r.r[(op>>21)&31]
	vt := (op >> 16) & 31
	kind := (op >> 11) & 31
	e := (op >> 7) & 15
	off := uint32(int32(op<<25) >> 25)
	d := r.m.spmem[:0x1000]
	switch kind {
	case 0, 1, 2, 3: // SBV SSV SLV SDV
		n := uint32(1) << kind
		a := base + off*n
		for i := uint32(0); i < n; i++ {
			d[(a+i)&0xfff] = r.vbyte(vt, (e+i)&15)
		}
	case 4: // SQV
		a := base + off*16
		end := (a &^ 15) + 16
		for i := uint32(0); a+i < end; i++ {
			d[(a+i)&0xfff] = r.vbyte(vt, (e+i)&15)
		}
	case 5: // SRV
		a := base + off*16
		start := a &^ 15
		n := a & 15
		for i := uint32(0); i < n; i++ {
			d[(start+i)&0xfff] = r.vbyte(vt, (16-n+e+i)&15)
		}
	case 6: // SPV
		a := base + off*8
		for i := uint32(0); i < 8; i++ {
			idx := (e + i) & 15
			var b uint8
			if idx < 8 {
				b = uint8(r.vr[vt][idx] >> 8)
			} else {
				b = uint8(r.vr[vt][idx&7] >> 7)
			}
			d[(a+i)&0xfff] = b
		}
	case 7: // SUV
		a := base + off*8
		for i := uint32(0); i < 8; i++ {
			idx := (e + i) & 15
			var b uint8
			if idx < 8 {
				b = uint8(r.vr[vt][idx] >> 7)
			} else {
				b = uint8(r.vr[vt][idx&7] >> 8)
			}
			d[(a+i)&0xfff] = b
		}
	case 8: // SHV
		a := base + off*16
		start := a &^ 7
		o := a & 7
		for i := uint32(0); i < 8; i++ {
			idx := (e + i*2) & 15
			hi := r.vbyte(vt, idx)
			lo := r.vbyte(vt, (idx+1)&15)
			v := uint8((uint16(hi)<<8 | uint16(lo)) >> 7)
			d[(start+((o+i*2)&15))&0xfff] = v
		}
	case 9: // SFV
		a := base + off*16
		start := a &^ 7
		o := a & 7
		var lanes [4]uint32
		switch e {
		case 0, 15:
			lanes = [4]uint32{0, 1, 2, 3}
		case 1:
			lanes = [4]uint32{6, 7, 4, 5}
		case 4:
			lanes = [4]uint32{1, 2, 3, 0}
		case 5:
			lanes = [4]uint32{7, 4, 5, 6}
		case 8:
			lanes = [4]uint32{4, 5, 6, 7}
		case 11:
			lanes = [4]uint32{3, 0, 1, 2}
		case 12:
			lanes = [4]uint32{5, 6, 7, 4}
		default:
			for i := uint32(0); i < 4; i++ {
				d[(start+((o+i*4)&15))&0xfff] = 0
			}
			return
		}
		for i := uint32(0); i < 4; i++ {
			d[(start+((o+i*4)&15))&0xfff] = uint8(r.vr[vt][lanes[i]] >> 7)
		}
	case 10: // SWV
		a := base + off*16
		start := a &^ 7
		o := a & 7
		for i := uint32(0); i < 16; i++ {
			d[(start+((o+i)&15))&0xfff] = r.vbyte(vt, (e+i)&15)
		}
	case 11: // STV
		a := base + off*16
		vbase := vt &^ 7
		start := a &^ 7
		o := a & 7
		for i := uint32(0); i < 8; i++ {
			reg := vbase + ((i + e/2) & 7)
			for b := uint32(0); b < 2; b++ {
				d[(start+((o+i*2+b)&15))&0xfff] = r.vbyte(reg, i*2+b)
			}
		}
	}
}
