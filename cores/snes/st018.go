package snes

import (
	"encoding/binary"
	"fmt"

	"retropixels/cores/arm7"
)

// ST018 (web/emu/snes_st018.js): a small ARM computer on the cartridge,
// running the program the player supplies, on RetroPixels' ARM core
// (cores/arm7). One ARM cycle per SNES master clock, one cycle per memory
// access; the ARM catches up whenever the SNES touches its registers.

const (
	st018Prog = 0x20000
	st018Data = 0x8000
	st018RAM  = 0x4000
	st018FW   = st018Prog + st018Data
)

func st018NeedFirmware(msg string) *FirmwareError {
	return &FirmwareError{Chip: "ST018", File: "st018.rom", Size: st018FW, Msg: msg}
}

func st018Firmware(c *cart, o *chipOpts) ([]byte, error) {
	if o != nil {
		if fw := o.firmware["ST018"]; len(fw) > 0 {
			if len(fw) != st018FW {
				return nil, st018NeedFirmware(fmt.Sprintf("The ST018 chip program should be 160 KB (%d bytes), but the chosen file is %d bytes. Choose the right file to continue.", st018FW, len(fw)))
			}
			return fw, nil
		}
	}
	declared := int(int32(uint32(0x400) << (c.hdr(0x17) & 31))) // (as JavaScript's 0x400 << n)
	if len(c.rom) == declared+st018FW {
		tail := append([]byte(nil), c.rom[declared:]...)
		c.rom = c.rom[:declared:declared]
		c.size = declared
		return tail, nil
	}
	return nil, st018NeedFirmware("This game needs the ST018 chip's program (st018.rom, 160 KB). Choose the file to continue.")
}

type st018 struct {
	s    *SNES
	prog []uint32
	data []uint32
	ram  [st018RAM]uint8
	arm  *arm7.CPU

	held                                         bool
	clock                                        int
	toSnes, toSnesFull, toArm, toArmFull, signal int

	// the waiting-loop detector
	ver      int32
	pollPC   int64
	pollVer  int32
	pollRegs [17]uint32
	idle     bool
}

func newST018(s *SNES, o *chipOpts) (coproc, error) {
	fw, err := st018Firmware(s.cart, o)
	if err != nil {
		return nil, err
	}
	c := &st018{s: s, prog: make([]uint32, st018Prog>>2), data: make([]uint32, st018Data>>2)}
	for i := range c.prog {
		c.prog[i] = binary.LittleEndian.Uint32(fw[i*4:])
	}
	for i := range c.data {
		c.data[i] = binary.LittleEndian.Uint32(fw[st018Prog+i*4:])
	}
	c.arm = arm7.New(c)
	s.claim(0x00, 0x3f, 0x3000, 0x3fff, true)
	c.reset()
	return c, nil
}

func (c *st018) status() int {
	v := c.toSnesFull | c.signal<<2 | c.toArmFull<<3
	if !c.held {
		v |= 0x80
	}
	return v
}

func (c *st018) resetArm() {
	c.ram = [st018RAM]uint8{}
	c.toSnes, c.toSnesFull, c.toArm, c.toArmFull, c.signal = 0, 0, 0, 0, 0
	c.arm.Reset()
	c.arm.Cycles = 0
	c.changed()
}

func (c *st018) reset() {
	c.held = false
	c.clock = c.s.clock
	c.resetArm()
}

func (c *st018) changed() {
	c.ver++
	c.pollPC = -1
}

// ---- the ARM's bus ----

func (c *st018) rd32(a uint32) uint32 {
	c.arm.Cycles++
	switch a >> 29 {
	case 0:
		return c.prog[(a>>2)&0x7fff]
	case 2:
		return uint32(c.io(a))
	case 5:
		return c.data[(a>>2)&0x1fff]
	case 7:
		k := a & 0x3ffc
		return uint32(c.ram[k]) | uint32(c.ram[k+1])<<8 | uint32(c.ram[k+2])<<16 | uint32(c.ram[k+3])<<24
	}
	return 0
}

func (c *st018) Read32(a uint32) uint32  { return c.rd32(a) }
func (c *st018) Fetch32(a uint32) uint32 { return c.rd32(a) }
func (c *st018) Read16(a uint32) uint32  { return c.rd32(a) >> ((a & 2) * 8) & 0xffff }
func (c *st018) Fetch16(a uint32) uint32 { return c.rd32(a) >> ((a & 2) * 8) & 0xffff }
func (c *st018) Read8(a uint32) uint32 {
	switch a >> 29 {
	case 7:
		c.arm.Cycles++
		return uint32(c.ram[a&0x3fff])
	case 2:
		c.arm.Cycles++
		return uint32(c.io(a))
	}
	return c.rd32(a) >> ((a & 3) * 8) & 0xff
}

func (c *st018) wr(a, v uint32, size int) {
	c.arm.Cycles++
	switch a >> 29 {
	case 7:
		r := &c.ram
		switch size {
		case 1:
			r[a&0x3fff] = uint8(v)
		case 2:
			k := a & 0x3ffe
			r[k], r[k+1] = uint8(v), uint8(v>>8)
		default:
			k := a & 0x3ffc
			r[k], r[k+1], r[k+2], r[k+3] = uint8(v), uint8(v>>8), uint8(v>>16), uint8(v>>24)
		}
		c.changed()
	case 2:
		c.ioWrite(a, int(v&0xff))
	}
}

func (c *st018) Write8(a, v uint32)  { c.wr(a, v, 1) }
func (c *st018) Write16(a, v uint32) { c.wr(a, v, 2) }
func (c *st018) Write32(a, v uint32) { c.wr(a, v, 4) }

func (c *st018) io(a uint32) int {
	switch a & 0x3c {
	case 0x10:
		if c.toArmFull != 0 {
			c.toArmFull = 0
			c.changed()
		}
		return c.toArm
	case 0x20:
		c.pollCheck()
		return c.status()
	}
	return 0
}

func (c *st018) ioWrite(a uint32, v int) {
	switch a & 0x3c {
	case 0x00:
		c.toSnes, c.toSnesFull = v, 1
	case 0x10:
		c.signal = 1
	default:
		return
	}
	c.changed()
}

// pollCheck: the ARM reads its status port with its whole state the same as
// at the last such read and nothing changed since: it is in a waiting loop
// that spins until the SNES acts, so skip ahead to the SNES's time.
func (c *st018) pollCheck() {
	a := c.arm
	cpsr := a.CPSR()
	if c.pollPC == int64(a.PC) && c.pollVer == c.ver && c.pollRegs[16] == cpsr {
		same := true
		for k := 0; k < 16; k++ {
			if c.pollRegs[k] != a.R[k] {
				same = false
				break
			}
		}
		if same {
			c.idle = true
			return
		}
	}
	c.pollPC, c.pollVer = int64(a.PC), c.ver
	copy(c.pollRegs[:16], a.R[:])
	c.pollRegs[16] = cpsr
}

// sync runs the ARM up to the SNES's current time.
func (c *st018) sync() {
	target := c.s.clock
	if c.held {
		c.clock = target
		return
	}
	a := c.arm
	c.idle = false
	for c.clock < target {
		a.Cycles = 0
		a.Step()
		if a.Cycles != 0 {
			c.clock += int(a.Cycles)
		} else {
			c.clock++
		}
		if c.idle {
			c.idle = false
			if c.clock < target {
				c.clock = target
			}
		}
	}
}

// ---- SNES side ----

func (c *st018) read(bank, off int) int {
	if off < 0x3800 || off >= 0x3900 {
		return -1
	}
	c.sync()
	switch off {
	case 0x3800:
		if c.toSnesFull != 0 {
			c.toSnesFull = 0
			c.changed()
		}
		return c.toSnes
	case 0x3802:
		if c.signal != 0 {
			c.signal = 0
			c.changed()
		}
		return 0
	case 0x3804:
		return c.status()
	}
	return -1
}

func (c *st018) write(bank, off int, v uint8) bool {
	if off < 0x3800 || off >= 0x3900 {
		return false
	}
	c.sync()
	switch off {
	case 0x3802:
		c.toArm, c.toArmFull = int(v), 1
		c.changed()
	case 0x3804:
		hold := v&1 == 1
		if hold && !c.held {
			c.resetArm()
		}
		c.held = hold
		c.changed()
	}
	return true
}

func (c *st018) state(s *sio) {
	var b []byte
	if s.save {
		b = c.arm.AppendState(nil)
	} else {
		b = make([]byte, arm7.StateSize)
	}
	s.Bytes(b)
	if !s.save && s.err == nil {
		if err := c.arm.LoadStateBytes(b); err != nil {
			s.err = errState
		}
	}
	s.Bytes(c.ram[:])
	s.Int(&c.clock)
	s.Bool(&c.held)
	s.Ints(&c.toSnes, &c.toSnesFull, &c.toArm, &c.toArmFull, &c.signal)
	// (the waiting-loop detector too, so a restored game runs on identically)
	ver, pollVer, pollPC := int(c.ver), int(c.pollVer), int(c.pollPC)
	s.Ints(&ver, &pollVer, &pollPC)
	c.ver, c.pollVer, c.pollPC = int32(ver), int32(pollVer), int64(pollPC)
	for i := range c.pollRegs {
		v := int(c.pollRegs[i])
		s.Int(&v)
		c.pollRegs[i] = uint32(v)
	}
}
