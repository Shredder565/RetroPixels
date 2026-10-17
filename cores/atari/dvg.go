package atari

import "math"

// The Digital Vector Generator (Asteroids, Asteroids Deluxe, Lunar Lander).
//
// The DVG reads 16-bit words (low byte first) from its own address space:
// word w is the processor's byte address $4000+2w (vector RAM, then the
// vector ROM). The processor starts it at word 0 and it runs until a HALT.
// Positions are 10 bits each way, 0-1023, with y going up the screen.
//
//	VCTR  op 0-9   y (10 bits + sign), then brightness, x (10 bits + sign):
//	               a line, its length scaled by 2^(op+global scale-9)
//	LABS  op $A    y position, then the global scale and x position
//	HALT  op $B    stop
//	JSRL  op $C    call the 12-bit word address (a four-deep stack)
//	RTSL  op $D    return
//	JMPL  op $E    jump
//	SVEC  op $F    a short vector in one word: 2-bit x and y, brightness,
//	               and a 2-bit scale (2-5) in bits 11 and 3
//
// The board's state-machine PROM (034602-01) only sequences these steps, so
// this is written from the instructions' documented behaviour and the PROM
// isn't needed. Its pictures were checked line by line against MAME's run on
// the same vector RAM.

// dvgLine is one line the beam draws (in DVG units), or a dot when its ends
// are the same.
type dvgLine struct {
	x0, y0, x1, y1 float32
	z              uint8 // brightness 1-15
}

// dvgRun runs the vector list from word 0 and returns the lines drawn and
// roughly how many processor cycles the drawing takes.
// read gets a word of the DVG's address space.
func dvgRun(read func(w int) uint16, lines []dvgLine) ([]dvgLine, int) {
	var stack [4]int
	sp := 0
	pc := 0
	x, y := 0.0, 0.0
	gscale := 0
	cycles := 0
	lines = lines[:0]
	for steps := 0; steps < 20000; steps++ { // a list that never halts stops here
		w0 := read(pc)
		pc = (pc + 1) & 0xfff
		op := int(w0 >> 12)
		cycles += 4
		switch op {
		case 0xa: // LABS
			w1 := read(pc)
			pc = (pc + 1) & 0xfff
			y = float64(w0 & 0x3ff)
			x = float64(w1 & 0x3ff)
			gscale = int(w1 >> 12)
		case 0xb: // HALT
			return lines, cycles
		case 0xc: // JSRL
			stack[sp&3] = pc
			sp++
			pc = int(w0 & 0xfff)
		case 0xd: // RTSL
			sp--
			pc = stack[sp&3]
		case 0xe: // JMPL
			pc = int(w0 & 0xfff)
		default: // VCTR or SVEC
			var dx, dy, z, scale int
			if op == 0xf {
				dx = int(w0&3) << 8
				dy = int(w0&0x300) >> 0
				if w0&4 != 0 {
					dx = -dx
				}
				if w0&0x400 != 0 {
					dy = -dy
				}
				z = int(w0>>4) & 15
				scale = 2 + int(w0>>2&2|w0>>11&1)
			} else {
				w1 := read(pc)
				pc = (pc + 1) & 0xfff
				dy = int(w0 & 0x3ff)
				if w0&0x400 != 0 {
					dy = -dy
				}
				dx = int(w1 & 0x3ff)
				if w1&0x400 != 0 {
					dx = -dx
				}
				z = int(w1 >> 12)
				scale = op
			}
			t := (scale + gscale) & 15
			if t > 9 { // the scale's high values count as negative
				t -= 16
			}
			f := math.Ldexp(1, t-9)
			nx, ny := x+float64(dx)*f, y+float64(dy)*f
			if z != 0 {
				lines = append(lines, dvgLine{float32(x), float32(y), float32(nx), float32(ny), uint8(z)})
			}
			x, y = nx, ny
			// the beam's timer runs 2^t steps
			if t > 0 {
				cycles += 1 << t >> 1
			}
		}
	}
	return lines, cycles
}
