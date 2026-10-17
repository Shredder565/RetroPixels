package atari

import (
	"retropixels/cores/arcade"
)

// earom is the ER2055: 64 bytes of electrically alterable ROM that keep the
// high scores and the bookkeeping with the power off. The game latches an
// address and a byte by writing to the chip's address range, then drives
// its control lines:
//
//	bit 3     chip select
//	bits 2,1  the mode: neither is read, both is erase, one of them is write
//	          (Centipede and Asteroids Deluxe wire them the other way round)
//	bit 0     clock (a read takes the byte on the rising edge)
//
// The real chip needs the erase and write held for milliseconds; the games
// do that, and here each happens at once.
type earom struct {
	mem  [64]uint8
	addr int
	data uint8
	out  uint8
	ctl  uint8
}

// latch is a write to the address range: the address and the byte to write.
func (e *earom) latch(a int, v uint8) {
	e.addr = a & 63
	e.data = v
}

// control is a write to the control lines.
func (e *earom) control(v uint8) {
	old := e.ctl
	e.ctl = v
	if v&8 == 0 {
		return
	}
	switch v & 6 {
	case 0: // read: clocked into the output latch
		if v&1 != 0 && old&1 == 0 {
			e.out = e.mem[e.addr]
		}
	case 6: // erase
		e.mem[e.addr] = 0
	default: // write
		e.mem[e.addr] = e.data
	}
}

func (e *earom) read() uint8 { return e.out }

func (e *earom) save(w *arcade.StateW) {
	w.Bytes(e.mem[:])
	w.Int(e.addr)
	w.U8(e.data)
	w.U8(e.out)
	w.U8(e.ctl)
}

func (e *earom) load(r *arcade.StateR) {
	r.Bytes(e.mem[:])
	e.addr = r.Int() & 63
	e.data = r.U8()
	e.out = r.U8()
	e.ctl = r.U8()
}
