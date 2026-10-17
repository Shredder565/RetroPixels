package namco

import "retropixels/cores/arcade"

// Namco's custom I/O chips, as the games see them through the 06xx.
//
// The 06xx's control register ($7100) picks the chips (bits 0-3: Galaga has
// the 51xx at 0 and the 54xx at 3, Dig Dug the 51xx at 0 and the 53xx at 1),
// the direction (bit 4 set = read) and how often it interrupts the main
// processor (bits 5-7: every 64<<n cycles, as MAME's 06xx does; 0 = never).
// The game's NMI handler moves one byte through $7000 per interrupt and
// writes $10 when it is done. Reading $7100 gives the control byte back.
//
// The 51xx, 53xx and 54xx are Fujitsu MB88xx microcontrollers whose programs
// aren't in the sets, so this is what they do from the outside, worked out
// from what the games send and what they check in the answers:
//
// 51xx commands: 1 = coinage (four bytes follow: coins and credits for slot
// 1, then slot 2; only the low 4 bits count, Dig Dug sends character codes;
// 0 coins = free play), 2 = credit mode (and let the start buttons take
// credits again), 3 = joysticks as they are, 4 = joysticks as directions
// 0-7 (8 = centred), 5 = switch mode. Reads in switch mode give the buttons
// byte, then the joysticks byte. Reads in credit mode give the credits (BCD;
// $A0 in free play), then each player's joystick in bits 0-3 with the fire
// button in bit 4 (0 for one read when it is pressed) and bit 5 (0 while it
// is held). A start button takes one credit (two for 2 players) and then the
// starts are off until the game sends 2 again (both games do when a game is
// over), so starting doesn't eat credits during play.
//
// 53xx (Dig Dug): each read gives switch bank A, then bank B.
//
// 54xx (Galaga): 1x/2x play noise A/B, 3x/4x set A's/B's four parameters,
// 5x plays C, 6x sets C's five parameters, 7x sets C's volume.

// io06xx is the bus controller.
type io06xx struct {
	ctl     uint8
	nextNMI int    // main processor cycle of the next NMI
	idx     [4]int // bytes read from each chip since it was picked
}

func (x *io06xx) reset() { *x = io06xx{ctl: 0x10} }

func (x *io06xx) active() bool { return x.ctl&0xe0 != 0 }
func (x *io06xx) period() int  { return 64 << (x.ctl >> 5) }

func (x *io06xx) writeCtl(v uint8, cyc int) {
	x.ctl = v
	x.idx = [4]int{}
	x.nextNMI = cyc + x.period()
}

func (x *io06xx) readData(b *board) uint8 {
	if x.ctl&0x10 == 0 {
		return 0xff
	}
	v := uint8(0xff)
	for c := 0; c < 4; c++ {
		if x.ctl&(1<<c) != 0 {
			v &= b.chipRead(c, x.idx[c])
			x.idx[c]++
		}
	}
	return v
}

func (x *io06xx) writeData(b *board, v uint8) {
	if x.ctl&0x10 != 0 {
		return
	}
	for c := 0; c < 4; c++ {
		if x.ctl&(1<<c) != 0 {
			b.chipWrite(c, v)
		}
	}
}

func (b *board) chipRead(c, i int) uint8 {
	switch {
	case c == 0:
		return b.n51.read(b, i)
	case c == 1 && b.kind == digdug:
		a, bb := b.dsw()
		if i&1 == 0 {
			return a
		}
		return bb
	}
	return 0xff
}

func (b *board) chipWrite(c int, v uint8) {
	switch {
	case c == 0:
		b.n51.write(v)
	case c == 3 && b.kind == galaga:
		b.soundTo(b.cyc[0])
		b.n54.write(v)
	}
}

// ---------------------------------------------------------------- 51xx

type n51xx struct {
	credit    bool // credit mode (else switch mode)
	remap     bool
	startsOn  bool
	cmd, args int
	argv      [4]uint8
	coinage   [4]int // coins, credits for slot 1; the same for slot 2
	credits   int
	coins     [2]int
	lastIn1   uint8
	fireHeld  [2]bool
}

func (n *n51xx) reset() {
	*n = n51xx{coinage: [4]int{1, 1, 1, 1}, lastIn1: 0xff}
}

func (n *n51xx) write(v uint8) {
	if n.args > 0 {
		n.argv[4-n.args] = v
		n.args--
		if n.args == 0 && n.cmd == 1 {
			for i := range n.coinage {
				n.coinage[i] = int(n.argv[i] & 15)
			}
		}
		return
	}
	n.cmd = int(v)
	switch v {
	case 1:
		n.args = 4
	case 2:
		n.credit, n.startsOn = true, true
	case 3:
		n.remap = false
	case 4:
		n.remap = true
	case 5:
		n.credit = false
	}
}

// directions: an active-low up/right/down/left nibble as 0 (up) to 7
// (up-left) clockwise, 8 = centred.
var directions = func() (t [16]uint8) {
	for v := range t {
		p := ^v & 15
		u, r, d, l := p&1 != 0, p&2 != 0, p&4 != 0, p&8 != 0
		if u && d {
			u, d = false, false
		}
		if l && r {
			l, r = false, false
		}
		c := uint8(8)
		switch {
		case u && r:
			c = 1
		case d && r:
			c = 3
		case d && l:
			c = 5
		case u && l:
			c = 7
		case u:
			c = 0
		case r:
			c = 2
		case d:
			c = 4
		case l:
			c = 6
		}
		t[v] = c
	}
	return
}()

func (n *n51xx) read(b *board, i int) uint8 {
	in0, in1 := b.in0(), b.in1()
	if !n.credit {
		switch i {
		case 0:
			return in1
		case 1:
			return in0
		}
		return 0xff
	}
	if i == 0 {
		n.poll(in1)
		if n.credits >= 100 {
			return 0xa0
		}
		return uint8(n.credits/10<<4 | n.credits%10)
	}
	if i > 2 {
		return 0xff
	}
	p := i - 1
	j := in0 >> (4 * p) & 15
	if n.remap {
		j = directions[j]
	}
	held := in1>>p&1 == 0
	v := 0xc0 | j
	if !held || n.fireHeld[p] {
		v |= 0x10 // not just pressed
	}
	if !held {
		v |= 0x20
	}
	n.fireHeld[p] = held
	return v
}

// poll counts coins and start buttons (in credit mode, once per reading).
func (n *n51xx) poll(in1 uint8) {
	pressed := ^in1 & n.lastIn1
	n.lastIn1 = in1
	free := n.coinage[0] == 0
	if free {
		n.credits = 100
	}
	for s := 0; s < 2; s++ {
		c, k := n.coinage[s*2], n.coinage[s*2+1]
		if pressed&(0x10<<s) != 0 && c > 0 {
			n.coins[s]++
			if n.coins[s] >= c {
				n.coins[s] = 0
				n.credits += k
			}
		}
	}
	if pressed&0x40 != 0 && !free {
		n.credits++ // service coin
	}
	if !free && n.credits > 99 {
		n.credits = 99
	}
	if n.startsOn {
		switch {
		case pressed&0x04 != 0 && n.credits >= 1:
			n.credits--
			n.startsOn = false
		case pressed&0x08 != 0 && n.credits >= 2:
			n.credits -= 2
			n.startsOn = false
		}
	}
}

func (n *n51xx) save(w *arcade.StateW) {
	for _, f := range []bool{n.credit, n.remap, n.startsOn, n.fireHeld[0], n.fireHeld[1]} {
		w.Bool(f)
	}
	w.Ints([]int{n.cmd, n.args, n.credits, n.coins[0], n.coins[1]})
	w.Ints(n.coinage[:])
	w.Bytes(n.argv[:])
	w.U8(n.lastIn1)
}

func (n *n51xx) load(r *arcade.StateR) {
	n.credit, n.remap, n.startsOn, n.fireHeld[0], n.fireHeld[1] = r.Bool(), r.Bool(), r.Bool(), r.Bool(), r.Bool()
	v := make([]int, 5)
	r.Ints(v)
	n.cmd, n.args, n.credits, n.coins[0], n.coins[1] = v[0], v[1], v[2], v[3], v[4]
	r.Ints(n.coinage[:])
	r.Bytes(n.argv[:])
	n.lastIn1 = r.U8()
}
