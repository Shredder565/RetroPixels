package namco

// Galaga's 05xx starfield, worked out from MAME's pictures (the star pixels
// it shows, frame after frame, with the game's latches forced from Lua).
//
// A 16-bit shift register (shifted right, bit 15 fed with the parity of
// bits 0, 3, 5 and 10) steps once per pixel over the 256 middle columns of
// each line, so the stars are one long sequence laid out line after line.
// A pixel has a star when bits 12-15 are all set and bits 0, 3, 5 and 10
// clear; the other bits give its colour (2 bits each of red, green and blue,
// inverted) and which of four sets it belongs to. Latches $A003/$A004 pick
// two of the sets (they blink), $A000-$A002 the speed: moving the sequence's
// start each frame moves the stars down the (upright) screen or back up.
// $A005 turns the stars on.

const starPeriod = 65535

var starSeq = func() (t [starPeriod]uint16) {
	s := uint16(1)
	for i := range t {
		t[i] = s
		p := s & 0x0429
		p ^= p >> 8
		p ^= p >> 4
		p ^= p >> 2
		p ^= p >> 1
		s = s>>1 | (p&1)<<15
	}
	return
}()

// starSpeed: pixels a frame for latch values 0-7 ($A000 = bit 0).
var starSpeed = [8]int{-1, -2, -3, -4, 3, 2, 1, 0}

// starfield keeps where the sequence starts this frame.
type starfield struct {
	scroll int
}

// starStart is the sequence's position at power-on (lined up with MAME's
// cold boot).
const starStart = 38619

func (s *starfield) reset() { *s = starfield{scroll: starStart} }

// step moves the stars on before a frame is drawn, at the speed the game
// set during the frame.
func (s *starfield) step(b *board) {
	code := int(b.vlat[0] | b.vlat[1]<<1 | b.vlat[2]<<2)
	s.scroll = ((s.scroll-starSpeed[code])%starPeriod + starPeriod) % starPeriod
}

func (v *video) drawStars(b *board) {
	if b.vlat[5] == 0 {
		return
	}
	a3, a4 := uint16(b.vlat[3]), uint16(b.vlat[4])
	scr := b.screen
	for y := 0; y < scrH; y++ {
		n := (v.stars.scroll + 256*y) % starPeriod
		for x := 16; x < 272; x++ {
			s := starSeq[n]
			n++
			if n == starPeriod {
				n = 0
			}
			if s&0xf429 != 0xf000 {
				continue
			}
			// set: bit 11 picks the latch, bit 9 the value it must have
			if s>>11&1 == 0 && s>>9&1 != a3 || s>>11&1 != 0 && s>>9&1 != a4 {
				continue
			}
			c := ^s
			i := c>>6&3 | (c>>8&1|(c>>1&1)<<1)<<2 | (c>>2&1|(c>>4&1)<<1)<<4
			scr[y*scrW+x] = v.starPal[i]
		}
	}
}
