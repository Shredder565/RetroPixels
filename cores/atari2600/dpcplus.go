package atari2600

// Atari 2600 DPC+ cartridges (Harmony / Melody), a port of atari2600_dpcplus.js.
//
// DPC+ is the successor of Pitfall II's DPC chip, run in software by the ARM
// processor on a Harmony or Melody cartridge. The 32K file holds the ARM
// driver (3K), six 4K banks for the 6507, 4K of display data and a 1K table
// of note frequencies. The driver's work for the 6507 (data fetchers, random
// numbers, music, copy / fill functions) is done here directly; the game's
// own ARM code (CALLFUNCTION 254 / 255) runs on the ARM7TDMI core
// (cores/arm7), in the same memory layout the cartridge has:
//   0x00000000  the cartridge's 32K flash (the game file)
//   0x40000000  8K of RAM: driver copy, display data at 0x40000C00,
//               frequency table at 0x40001C00 (the custom code's variables and
//               stack live in the top part of it)

import (
	"errors"
	"math"

	"retropixels/cores/arm7"
)

const (
	dpcpCPUHz      = float64(clock) / 3
	dpcpRandomInit = 0x2b435044 // "DPC+"
	dpcpRandomTaps = 0x10adab1e
	dpcpReturnAddr = 0x0fffff00 // where the ARM code returns to when it's done
	dpcpARMLimit   = 20000000   // instructions; a runaway routine is stopped after this many
)

// ErrARMRunaway is the panic value RunFrame raises when a DPC+ game's ARM code
// doesn't finish (the web core throws the same message).
var ErrARMRunaway = errors.New("This game's ARM code didn't finish (DPC+).")

type dpcPlusCart struct {
	flash []byte // the 32K file (never written)
	ram8k [8192]uint8
	bank  int
	ptr   [8]uint16 // data fetcher pointers (12 bits)
	fptr  [8]uint16 // fractional fetcher pointers
	ffrac [8]uint8
	finc  [8]uint8
	tops  [8]uint8
	bots  [8]uint8

	random      uint32
	fast        bool
	ldaNext     bool
	params      [8]uint8
	nParams     int
	wave        [3]uint8
	freq        [3]uint32
	phase       [3]uint32
	musicCycles float64

	arm *arm7.CPU
}

func newDPCPlusCart(rom []byte) *dpcPlusCart {
	// Some files are missing the padding at the end; make room for all 32K.
	flash := make([]byte, max(32768, len(rom)))
	copy(flash, rom)
	c := &dpcPlusCart{flash: flash[:32768], bank: 5, random: dpcpRandomInit}
	copy(c.ram8k[0:], c.flash[0:0x0c00])
	copy(c.ram8k[0x0c00:], c.flash[0x6c00:0x7c00])
	copy(c.ram8k[0x1c00:], c.flash[0x7c00:0x8000])
	return c
}

func (c *dpcPlusCart) kind() string { return "DPC+" }
func (c *dpcPlusCart) sram() []byte { return nil }

// ---- 6507 side ----

func (c *dpcPlusCart) read(a int, m *Atari) uint8 {
	off := a & 0xfff
	var v uint8
	if off < 0x28 {
		v = c.reg(off, m)
	} else {
		if off >= 0xff6 && off <= 0xffb {
			c.bank = off - 0xff6
		}
		v = c.flash[0x0c00+c.bank*4096+off]
	}
	// Fast fetch: the operand of LDA #imm (if below $28) reads that register instead.
	if c.ldaNext {
		c.ldaNext = false
		if v < 0x28 && off >= 0x28 {
			v = c.reg(int(v), m)
		}
	} else if c.fast && m != nil && m.isOp && v == 0xa9 {
		c.ldaNext = true
	}
	return v
}

func (c *dpcPlusCart) write(a int, v uint8, m *Atari) {
	off := a & 0xfff
	if off >= 0x28 && off < 0x80 {
		c.regWrite(off, v, m)
	} else if off >= 0xff6 && off <= 0xffb {
		c.bank = off - 0xff6
	}
}

func (c *dpcPlusCart) disp(i uint16) uint8 { return c.ram8k[0x0c00+int(i&0xfff)] }

func (c *dpcPlusCart) inWindow(i int) bool {
	p := uint8(c.ptr[i])
	return p-c.tops[i]-1 < c.bots[i]-c.tops[i]-1
}

func (c *dpcPlusCart) reg(r int, m *Atari) uint8 {
	i := r & 7
	switch r >> 3 {
	case 0:
		switch r {
		case 0:
			c.clockRandom()
			return uint8(c.random)
		case 1:
			c.unclockRandom()
			return uint8(c.random)
		case 2:
			return uint8(c.random >> 8)
		case 3:
			return uint8(c.random >> 16)
		case 4:
			return uint8(c.random >> 24)
		case 5:
			return c.amplitude(m)
		}
		return 0
	case 1:
		v := c.disp(c.ptr[i])
		c.ptr[i] = (c.ptr[i] + 1) & 0xfff
		return v
	case 2:
		var v uint8
		if c.inWindow(i) {
			v = c.disp(c.ptr[i])
		}
		c.ptr[i] = (c.ptr[i] + 1) & 0xfff
		return v
	case 3:
		v := c.disp(c.fptr[i])
		f := int(c.ffrac[i]) + int(c.finc[i])
		c.ffrac[i] = uint8(f)
		if f > 0xff {
			c.fptr[i] = (c.fptr[i] + 1) & 0xfff
		}
		return v
	case 4:
		if i < 4 && c.inWindow(i) {
			return 0xff
		}
		return 0
	}
	return 0
}

func (c *dpcPlusCart) regWrite(r int, v uint8, m *Atari) {
	i := r & 7
	switch r >> 3 {
	case 5: // $28 DFxFRACLOW
		c.fptr[i] = c.fptr[i]&0xf00 | uint16(v)
		c.ffrac[i] = 0
	case 6: // $30 DFxFRACHI
		c.fptr[i] = c.fptr[i]&0x0ff | uint16(v&15)<<8
		c.ffrac[i] = 0
	case 7: // $38 DFxFRACINC
		c.finc[i] = v
		c.ffrac[i] = 0
	case 8: // $40 DFxTOP
		c.tops[i] = v
	case 9: // $48 DFxBOT
		c.bots[i] = v
	case 10: // $50 DFxLOW
		c.ptr[i] = c.ptr[i]&0xf00 | uint16(v)
	case 11:
		switch r {
		case 0x58: // FASTFETCH
			c.fast = v == 0
		case 0x59: // PARAMETER
			if c.nParams < 8 {
				c.params[c.nParams] = v
				c.nParams++
			}
		case 0x5a: // CALLFUNCTION
			c.callFunction(v, m)
		case 0x5d, 0x5e, 0x5f: // WAVEFORMx
			c.syncMusic(m)
			c.wave[r-0x5d] = v & 0x7f
		}
	case 12: // $60 DFxPUSH
		c.ptr[i] = (c.ptr[i] - 1) & 0xfff
		c.ram8k[0x0c00+int(c.ptr[i])] = v
	case 13: // $68 DFxHI
		c.ptr[i] = c.ptr[i]&0x0ff | uint16(v&15)<<8
	case 14:
		switch {
		case r == 0x70: // RRESET
			c.random = dpcpRandomInit
		case r <= 0x74: // RWRITEx
			sh := uint(r-0x71) * 8
			c.random = c.random&^(0xff<<sh) | uint32(v)<<sh
		case r <= 0x77: // NOTEx
			c.syncMusic(m)
			c.freq[r-0x75] = c.ram32(0x1c00 + int(v)*4)
		}
	case 15: // $78 DFxWRITE
		c.ram8k[0x0c00+int(c.ptr[i])] = v
		c.ptr[i] = (c.ptr[i] + 1) & 0xfff
	}
}

func (c *dpcPlusCart) ram32(o int) uint32 {
	o &= 0x1ffc
	r := &c.ram8k
	return uint32(r[o]) | uint32(r[o+1])<<8 | uint32(r[o+2])<<16 | uint32(r[o+3])<<24
}

// 32-bit random number generator, steppable both ways.
func (c *dpcPlusCart) clockRandom() {
	r := c.random
	c.random = (r>>11 | r<<21)
	if r&0x400 != 0 {
		c.random ^= dpcpRandomTaps
	}
}

func (c *dpcPlusCart) unclockRandom() {
	r := c.random
	if r&0x80000000 != 0 {
		r ^= dpcpRandomTaps
	}
	c.random = r<<11 | r>>21
}

// ---- music: three 32-step waveforms in display data, stepped at 20 kHz ----

func (c *dpcPlusCart) syncMusic(m *Atari) {
	if m == nil {
		return
	}
	steps := int64(math.Floor((float64(m.cycles) - c.musicCycles) * 20000 / dpcpCPUHz))
	if steps <= 0 {
		return
	}
	c.musicCycles += float64(steps) * dpcpCPUHz / 20000
	for v := 0; v < 3; v++ {
		c.phase[v] += c.freq[v] * uint32(steps)
	}
}

func (c *dpcPlusCart) amplitude(m *Atari) uint8 {
	c.syncMusic(m)
	s := 0
	for v := 0; v < 3; v++ {
		s += int(c.ram8k[0x0c00+((int(c.wave[v])<<5)+int(c.phase[v]>>27))&0xfff])
	}
	return uint8(s)
}

// ---- functions ----

func (c *dpcPlusCart) callFunction(fn uint8, m *Atari) {
	p := &c.params
	switch fn {
	case 1: // copy from the 6507 banks into display data at fetcher p[2]'s pointer
		src := 0x0c00 + (int(p[1])<<8 | int(p[0]))
		dst := int(c.ptr[p[2]&7])
		for k := 0; k < int(p[3]); k++ {
			c.ram8k[0x0c00+(dst+k)&0xfff] = c.flash[(src+k)&0x7fff]
		}
	case 2: // fill display data at fetcher p[2]'s pointer with p[0]
		dst := int(c.ptr[p[2]&7])
		for k := 0; k < int(p[3]); k++ {
			c.ram8k[0x0c00+(dst+k)&0xfff] = p[0]
		}
	case 254, 255:
		c.runARM()
	}
	c.nParams = 0
}

// ---- the game's own ARM code ----

// armBus is the ARM's view of the cartridge.
type armBus struct{ c *dpcPlusCart }

func (b armBus) Read8(a uint32) uint32 {
	if a < 0x8000 {
		return uint32(b.c.flash[a])
	}
	if a >= 0x40000000 && a < 0x40002000 {
		return uint32(b.c.ram8k[a-0x40000000])
	}
	return 0
}
func (b armBus) Read16(a uint32) uint32 {
	a &^= 1
	return b.Read8(a) | b.Read8(a+1)<<8
}
func (b armBus) Read32(a uint32) uint32 {
	a &^= 3
	return b.Read8(a) | b.Read8(a+1)<<8 | b.Read8(a+2)<<16 | b.Read8(a+3)<<24
}
func (b armBus) Fetch16(a uint32) uint32 { return b.Read16(a) }
func (b armBus) Fetch32(a uint32) uint32 { return b.Read32(a) }
func (b armBus) write8(a uint32, v uint8) {
	if a >= 0x40000000 && a < 0x40002000 {
		b.c.ram8k[a-0x40000000] = v
	}
}
func (b armBus) Write8(a, v uint32) { b.write8(a, uint8(v)) }
func (b armBus) Write16(a, v uint32) {
	a &^= 1
	b.write8(a, uint8(v))
	b.write8(a+1, uint8(v>>8))
}
func (b armBus) Write32(a, v uint32) {
	a &^= 3
	b.write8(a, uint8(v))
	b.write8(a+1, uint8(v>>8))
	b.write8(a+2, uint8(v>>16))
	b.write8(a+3, uint8(v>>24))
}

func (c *dpcPlusCart) runARM() {
	if c.arm == nil {
		c.arm = arm7.New(armBus{c})
	}
	a := c.arm
	a.Reset()
	a.R[13] = 0x40001fdc         // the driver's stack for the custom code
	a.R[14] = dpcpReturnAddr | 1 // returning lands here (in Thumb state)
	a.I, a.F = 1, 1
	a.Branch(0x0c00) // custom code entry (ARM state)
	for n := 0; n < dpcpARMLimit; n++ {
		if a.PC&^1 == dpcpReturnAddr {
			return
		}
		a.Step()
	}
	panic(ErrARMRunaway)
}

// The ARM's registers are not part of the state: they are set up afresh for every call.
func (c *dpcPlusCart) save(w *stateW) {
	w.bytes(c.ram8k[:])
	w.int(c.bank)
	w.u16s(c.ptr[:])
	w.u16s(c.fptr[:])
	w.bytes(c.ffrac[:])
	w.bytes(c.finc[:])
	w.bytes(c.tops[:])
	w.bytes(c.bots[:])
	w.u32(c.random)
	w.bool(c.fast)
	w.bool(c.ldaNext)
	w.bytes(c.params[:])
	w.int(c.nParams)
	w.bytes(c.wave[:])
	w.u32s(c.freq[:])
	w.u32s(c.phase[:])
	w.f64(c.musicCycles)
}

func (c *dpcPlusCart) load(r *stateR) {
	r.bytesInto(c.ram8k[:])
	c.bank = r.int()
	r.u16sInto(c.ptr[:])
	r.u16sInto(c.fptr[:])
	r.bytesInto(c.ffrac[:])
	r.bytesInto(c.finc[:])
	r.bytesInto(c.tops[:])
	r.bytesInto(c.bots[:])
	c.random = r.u32()
	c.fast = r.bool()
	c.ldaNext = r.bool()
	r.bytesInto(c.params[:])
	c.nParams = r.int()
	r.bytesInto(c.wave[:])
	r.u32sInto(c.freq[:])
	r.u32sInto(c.phase[:])
	c.musicCycles = r.f64()
}

func (c *dpcPlusCart) clone() cart {
	n := *c
	n.arm = nil
	return &n
}
