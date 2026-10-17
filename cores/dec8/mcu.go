package dec8

// The i8751 microcontroller, simulated from what the game sends and expects
// (the user's set has no dump of it).
//
// The game writes a 16-bit question to 3860 (high) / 3861 (low) and reads
// the answer from 3840 (high) / 3860 (low). At power-on it asks 00AA and
// waits for 0655, then 021A and waits for 06E5 (021A again before each
// scene). 04xx and 05xx are answered with themselves (the game's hidden
// test at FDB3 uses them).
//
// The MCU also watches the coin slots: a coin raises the main processor's
// IRQ (while bit 0 of 3840 is on; the game turns the bit off and on again to
// take it) with the high byte saying who it is for: bit 7 player 1, bit 6
// player 2, bit 4 both (the service coin). The game's interrupt handler at
// 89F2 plays the coin sound and joins that player (or adds to their
// energy). The real MCU's coinage settings and coin counters are not
// simulated: a coin is one credit.

import "retropixels/cores/arcade"

type mcu struct {
	q       uint16
	hi, lo  uint8
	on      bool  // bit 0 of 3840: its interrupt allowed
	irq     bool  // the main processor's IRQ line
	pending uint8 // coins not yet told
	held    uint8 // coin buttons held last frame
}

func (m *mcu) reset() { *m = mcu{hi: 0xff, lo: 0xff} }

// ask takes a byte of the main processor's question; the high byte
// (written last) makes the answer.
func (m *mcu) ask(v uint8, high bool) {
	if !high {
		m.q = m.q&0xff00 | uint16(v)
		return
	}
	m.q = m.q&0xff | uint16(v)<<8
	switch q := m.q; {
	case q == 0x00aa:
		m.hi, m.lo = 0x06, 0x55
	case q == 0x021a:
		m.hi, m.lo = 0x06, 0xe5
	case q>>8 == 4 || q>>8 == 5:
		m.hi, m.lo = uint8(q>>8), uint8(q)
	default:
		m.hi, m.lo = 0x06, 0x00
	}
}

// enable is bit 0 of 3840: off clears the interrupt; on lets a waiting coin
// through.
func (m *mcu) enable(on bool) {
	m.on = on
	if !on {
		m.irq = false
		return
	}
	m.tell()
}

func (m *mcu) tell() {
	if m.on && !m.irq && m.pending != 0 {
		m.hi, m.lo = m.pending, 0
		m.pending = 0
		m.irq = true
	}
}

// coins reads the coin buttons once a frame: a press is a coin.
func (m *mcu) coins(p1, p2 arcade.Pad) {
	var now uint8
	if p1["Coin"] {
		now |= 0x80
	}
	if p2["Coin"] || p1["Coin 2"] {
		now |= 0x40
	}
	if p1["Service"] || p2["Service"] {
		now |= 0x10
	}
	m.pending |= now &^ m.held
	m.held = now
	m.tell()
}

func (m *mcu) save(w *arcade.StateW) {
	w.U16(m.q)
	w.U8(m.hi)
	w.U8(m.lo)
	w.Bool(m.on)
	w.Bool(m.irq)
	w.U8(m.pending)
	w.U8(m.held)
}

func (m *mcu) load(r *arcade.StateR) {
	m.q = r.U16()
	m.hi = r.U8()
	m.lo = r.U8()
	m.on = r.Bool()
	m.irq = r.Bool()
	m.pending = r.U8()
	m.held = r.U8()
}
