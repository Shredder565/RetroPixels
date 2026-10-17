// Package upd7759 is the NEC uPD7759 ADPCM speech chip.
//
// The chip plays 4-bit ADPCM samples. In stand-alone (master) mode it reads
// them from its own ROM: the CPU writes a sample number to the data port and
// pulses ST. In slave mode (the Sega System 16 sound boards) there is no ROM
// on the chip: it asks for each byte with its DRQ pin and the sound CPU,
// usually from an NMI handler, writes the bytes to the data port. The byte
// stream is the same as the ROM's: the "last sample" byte, a dummy byte, the
// two address bytes, another dummy byte, then the sample's blocks.
//
// Blocks start with a header byte: 0x00 ends the sample, 0x01-0x3f is
// silence (n*256 steps), 0x40-0x7f is 256 nibbles at rate (n&0x3f)+1,
// 0x80-0xbf is (next byte + 1) nibbles at rate (n&0x3f)+1, and 0xc0-0xff
// repeats the following blocks (n&7)+1 times (stand-alone mode only).
package upd7759

import "retropixels/cores/arcade"

var adjust = [16]int{-1, -1, 0, 0, 1, 2, 2, 3, -1, -1, 0, 0, 1, 2, 2, 3}

const (
	stIdle = iota
	stCmd
	stCount // unused; kept so the state numbers stay as they were
	stNibbles
	stSilence
)

// UPD7759 is one chip.
type UPD7759 struct {
	clock float64
	rom   []byte
	Acc   float64 // sum of the outputs since the board last took them
	AccN  int
	Out   float64 // the current output, -1..1

	st, rst, data, state, pos, left, period, wait int
	sample, step, nib, repeat, repeatPos, clk     int

	// slave mode
	slave  bool
	OnDRQ  func(on bool) // called when DRQ changes (slave mode)
	drq    bool
	have   bool // a byte written by the CPU waits in data
	skip   int  // header bytes still to drop before the first block
	cur    int  // the byte whose nibbles are being played
	inByte bool // cur holds a byte whose low nibble is still to play
}

// New makes the chip in stand-alone mode with its sample ROM (clock 0 = 640 kHz).
func New(clock float64, rom []byte) *UPD7759 {
	if clock == 0 {
		clock = 640000
	}
	if rom == nil {
		rom = make([]byte, 16)
	}
	u := &UPD7759{clock: clock, rom: rom}
	u.Reset()
	return u
}

// NewSlave makes the chip in slave mode (no ROM; the CPU feeds it on DRQ).
func NewSlave(clock float64) *UPD7759 {
	u := New(clock, nil)
	u.slave = true
	return u
}

// Clock is the chip's input clock in Hz.
func (u *UPD7759) Clock() float64 { return u.clock }

func (u *UPD7759) Reset() {
	u.st, u.rst, u.data = 0, 1, 0
	u.state, u.pos, u.left, u.period, u.wait = stIdle, 0, 0, 1, 0
	u.sample, u.step, u.nib, u.repeat, u.repeatPos = 0, 0, 0, 0, 0
	u.Out, u.clk = 0, 0
	u.have, u.skip, u.cur, u.inByte = false, 0, 0, false
	u.setDRQ(false)
}

func (u *UPD7759) rd(a int) int { return int(u.rom[a%len(u.rom)]) }

func (u *UPD7759) setDRQ(on bool) {
	if on != u.drq {
		u.drq = on
		if u.OnDRQ != nil {
			u.OnDRQ(on)
		}
	}
}

// DRQ is the data request pin (slave mode).
func (u *UPD7759) DRQ() bool { return u.drq }

// Port is the 8-bit data port: the sample number in stand-alone mode. In
// slave mode a write while the chip is idle starts a sample (the board's
// write strobe pulses ST), and a write while it plays answers DRQ.
func (u *UPD7759) Port(v uint8) {
	u.data = int(v)
	if !u.slave {
		return
	}
	if u.rst == 0 {
		return
	}
	if u.state == stIdle {
		// the first byte is the sample number; then the five header bytes
		u.state, u.skip, u.have, u.wait = stCmd, 5, false, 0
		u.sample, u.step, u.inByte = 0, 0, false
		u.setDRQ(true)
		return
	}
	u.have = true
	u.setDRQ(false)
}

// ResetLine is the RESET pin (false = held in reset).
func (u *UPD7759) ResetLine(v bool) {
	u.rst = 0
	if v {
		u.rst = 1
	}
	if u.rst == 0 {
		u.state, u.sample, u.Out = stIdle, 0, 0
		u.have, u.inByte = false, false
		u.setDRQ(false)
	}
}

// Start is the ST pin (stand-alone mode).
func (u *UPD7759) Start(v bool) {
	if !u.slave && v && u.st == 0 && u.rst != 0 && u.state == stIdle {
		u.begin(u.data)
	}
	u.st = 0
	if v {
		u.st = 1
	}
}

// Busy is the BUSY pin (true while a sample plays).
func (u *UPD7759) Busy() bool { return u.state != stIdle }

func (u *UPD7759) begin(n int) {
	if n > u.rd(0) {
		return
	}
	at := 5 + n*2
	u.pos = (u.rd(at)<<8|u.rd(at+1))*2 + 1
	u.sample, u.step, u.repeat = 0, 0, 0
	u.state, u.wait = stCmd, 0
}

// header acts on a block header byte.
func (u *UPD7759) header(c int) {
	if c == 0 {
		u.state, u.sample = stIdle, 0
		return
	}
	if c < 0x40 {
		u.state, u.wait, u.sample, u.step = stSilence, c*256, 0, 0
		return
	}
	if c < 0xc0 {
		u.period = (c & 0x3f) + 1
		if c < 0x80 {
			u.left = 256
		} else if u.slave {
			u.left = -1 // the count byte comes next
		} else {
			u.left = u.rd(u.pos) + 1
			u.pos++
		}
		u.state, u.nib, u.wait = stNibbles, 0, 0
		return
	}
	u.repeat, u.repeatPos = (c&7)+1, u.pos
}

func (u *UPD7759) command() {
	c := u.rd(u.pos)
	u.pos++
	u.header(c)
}

func (u *UPD7759) endBlock() {
	if u.repeat > 1 {
		u.repeat--
		u.pos = u.repeatPos
	} else {
		u.repeat = 0
	}
	u.state = stCmd
}

// decode plays one nibble.
func (u *UPD7759) decode(n int) {
	s := u.sample + stepTable[u.step*16+n]
	if s > 127 {
		s = 127
	} else if s < -128 {
		s = -128
	}
	u.sample = s
	u.step += adjust[n]
	if u.step < 0 {
		u.step = 0
	} else if u.step > 15 {
		u.step = 15
	}
	u.wait = u.period - 1
}

// Tick is one native step (clock / 4).
func (u *UPD7759) Tick() {
	if u.slave {
		u.tickSlave()
		return
	}
	if u.state == stIdle || u.rst == 0 {
		u.Out = 0
		return
	}
	if u.wait > 0 {
		u.wait--
		u.Out = float64(u.sample) / 128
		return
	}
	switch u.state {
	case stCmd:
		u.command()
	case stSilence:
		u.endBlock()
	case stNibbles:
		b := u.rd(u.pos)
		n := b >> 4
		if u.nib != 0 {
			n = b & 15
			u.pos++
		}
		u.nib ^= 1
		u.decode(n)
		if u.left--; u.left <= 0 {
			if u.nib != 0 {
				u.pos++
				u.nib = 0
			}
			u.endBlock()
		}
	}
	u.Out = float64(u.sample) / 128
}

// take returns the byte the CPU wrote, or false (and asks for one) if none.
func (u *UPD7759) take() (int, bool) {
	if u.have {
		u.have = false
		return u.data, true
	}
	u.setDRQ(true)
	return 0, false
}

func (u *UPD7759) tickSlave() {
	if u.state == stIdle || u.rst == 0 {
		u.Out = 0
		return
	}
	u.Out = float64(u.sample) / 128
	if u.wait > 0 {
		u.wait--
		return
	}
	switch u.state {
	case stCmd:
		c, ok := u.take()
		if !ok {
			return
		}
		if u.skip > 0 {
			u.skip--
			u.setDRQ(true)
			return
		}
		u.header(c)
		if u.state != stIdle {
			u.setDRQ(true)
		}
	case stSilence:
		u.state = stCmd
	case stNibbles:
		if u.left < 0 {
			c, ok := u.take()
			if !ok {
				return
			}
			u.left = c + 1
			u.setDRQ(true)
			return
		}
		if !u.inByte {
			c, ok := u.take()
			if !ok {
				return
			}
			u.cur, u.inByte = c, true
			u.decode(c >> 4)
		} else {
			u.inByte = false
			u.decode(u.cur & 15)
		}
		u.left--
		if u.left <= 0 {
			u.inByte = false
			u.state = stCmd
		}
		if !u.inByte {
			u.setDRQ(true)
		}
	}
	u.Out = float64(u.sample) / 128
}

// Clocks runs n input clocks, summing into Acc / AccN.
func (u *UPD7759) Clocks(n int) {
	u.clk += n
	for u.clk >= 4 {
		u.clk -= 4
		u.Tick()
		u.Acc += u.Out
		u.AccN++
	}
}

func (u *UPD7759) ints() []*int {
	return []*int{&u.st, &u.rst, &u.data, &u.state, &u.pos, &u.left, &u.period, &u.wait, &u.sample, &u.step, &u.nib,
		&u.repeat, &u.repeatPos, &u.clk}
}

// AppendState writes the chip's state (slave-mode chips add their own fields).
func (u *UPD7759) AppendState(w *arcade.StateW) {
	for _, p := range u.ints() {
		w.Int(*p)
	}
	w.F64(u.Out)
	w.F64(u.Acc)
	w.Int(u.AccN)
	if u.slave {
		w.Bool(u.drq)
		w.Bool(u.have)
		w.Int(u.skip)
		w.Int(u.cur)
		w.Bool(u.inByte)
	}
}

// LoadState reads what AppendState wrote. OnDRQ is not called: the board
// keeps its own copy of the line in its state.
func (u *UPD7759) LoadState(r *arcade.StateR) {
	for _, p := range u.ints() {
		*p = r.Int()
	}
	u.Out = r.F64()
	u.Acc = r.F64()
	u.AccN = r.Int()
	if u.slave {
		u.drq = r.Bool()
		u.have = r.Bool()
		u.skip = r.Int()
		u.cur = r.Int()
		u.inByte = r.Bool()
	}
}
