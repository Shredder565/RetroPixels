// Package capcom has Capcom's early Z80 arcade boards: 1942 (1984) and 1943:
// The Battle of Midway (1987, also 1943 Kai). Both are vertical shooters on
// a sideways monitor: a main Z80 runs the game, a second Z80 the sound
// (two AY-3-8910s on 1942, two YM2203s on 1943), and the picture is a
// character layer over scrolling backgrounds and 16x16 sprites, coloured
// through lookup PROMs into a 256-colour palette held in three 4-bit PROMs.
//
// The boards draw the picture as the hardware scans it (256x224, before the
// framework turns it upright).
package capcom

import (
	"retropixels/cores/arcade"
	"retropixels/cores/z80"
)

// Both boards: a 6 MHz pixel clock, 384 pixels a line, 262 lines a frame;
// lines 16-239 are shown.
const (
	pixelHz    = 6000000
	lineClocks = 384
	lines      = 262
	firstLine  = 16  // the first row of the 256-line picture shown
	vblankLine = 246 // the frame is drawn at the start of the vertical blank
	vblankEnd  = 22  // and the blank ends here
	scrW       = 256
	scrH       = 224
	fps        = float64(pixelHz) / (lineClocks * lines)
	soundSteps = 3 // sound samples made per line
)

// gfxLayout says where each bit of a tile is in its ROM, in bits from the
// tile's start (bit 0 is the top bit of the first byte); planes go from the
// highest bit of the pixel value to the lowest.
type gfxLayout struct {
	w, h   int
	planes []int
	xs, ys []int
	step   int // bits from one tile to the next
}

// decode turns count tiles into one byte per pixel (w*h per tile).
func (l *gfxLayout) decode(rom []byte, count int) []uint8 {
	out := make([]uint8, count*l.w*l.h)
	bit := func(o int) uint8 {
		if o>>3 >= len(rom) {
			return 0
		}
		return rom[o>>3] >> (7 - o&7) & 1
	}
	for t := 0; t < count; t++ {
		base := t * l.step
		d := out[t*l.w*l.h:]
		for y := 0; y < l.h; y++ {
			for x := 0; x < l.w; x++ {
				var v uint8
				for _, p := range l.planes {
					v = v<<1 | bit(base+p+l.ys[y]+l.xs[x])
				}
				d[y*l.w+x] = v
			}
		}
	}
	return out
}

// seq is n numbers from a, step apart.
func seq(a, step, n int) []int {
	s := make([]int, n)
	for i := range s {
		s[i] = a + i*step
	}
	return s
}

// The 8x8 characters: two planes in the two nibbles of each byte, four
// pixels a byte, two bytes a line.
func charLayout() *gfxLayout {
	return &gfxLayout{w: 8, h: 8, planes: []int{4, 0}, xs: []int{0, 1, 2, 3, 8, 9, 10, 11}, ys: seq(0, 16, 8), step: 128}
}

// The 16x16 sprites: like the characters (16 bits a line, two planes in each
// byte's nibbles), with the other two planes in the second half of the ROMs
// and the right half of the sprite 32 bytes on.
func spriteLayout(romBytes int) *gfxLayout {
	half := romBytes * 4
	xs := append([]int{0, 1, 2, 3, 8, 9, 10, 11}, seq(256, 1, 4)...)
	xs = append(xs, seq(264, 1, 4)...)
	return &gfxLayout{w: 16, h: 16, planes: []int{half + 4, half, 4, 0}, xs: xs, ys: seq(0, 16, 16), step: 512}
}

// promColour turns three 4-bit PROM values into a pixel: each bit drives a
// resistor (weights about 1, 2, 4, 8 to full scale 255).
func promColour(r, g, b uint8) uint32 {
	c := func(v uint8) int {
		w := [4]int{14, 31, 67, 143}
		s := 0
		for i := 0; i < 4; i++ {
			if v>>i&1 != 0 {
				s += w[i]
			}
		}
		return s
	}
	return arcade.RGB(c(r), c(g), c(b))
}

// palette makes the 256 colours from the red, green and blue PROMs.
func palette(r, g, b []byte) [256]uint32 {
	var p [256]uint32
	for i := range p {
		p[i] = promColour(r[i]&15, g[i]&15, b[i]&15)
	}
	return p
}

// irqCPU is a Z80 whose interrupt line is held until the interrupt is taken
// (the board's bus forwards the acknowledge to Ack). In interrupt mode
// 0, the processor runs the instruction on the data bus: these boards put an
// RST there (vector), which the z80 package doesn't model, so the board moves
// the processor to the RST's address after the interrupt is taken.
type irqCPU struct {
	cpu    *z80.CPU
	vector uint8 // the RST opcode on the bus (0xff = RST 38)
	taken  bool
}

func (c *irqCPU) Ack() {
	c.cpu.IRQ = false
	c.taken = true
}

// assert raises the interrupt line with an RST opcode on the data bus.
func (c *irqCPU) assert(vector uint8) {
	c.vector = vector
	c.cpu.IRQ = true
}

// step runs one instruction (or takes an interrupt); returns clocks.
func (c *irqCPU) step() int {
	t := c.cpu.Step()
	if c.taken {
		c.taken = false
		if r := c.cpu.Regs(); r.IM == 0 {
			// the z80 package went to 0x38; an RST n goes to n*8
			r.PC = uint16(c.vector & 0x38)
			r.WZ = r.PC
			c.cpu.SetRegs(r)
			t = 13
		}
	}
	return t
}

// run runs the processor until it has used up its clocks for a slice;
// over is the clocks it ran past the last slice's end.
func (c *irqCPU) run(clocks int, over *int) {
	n := *over
	for n < clocks {
		n += c.step()
	}
	*over = n - clocks
}

// padBits: active-low bits for the buttons held (bit -> button name).
func padBits(p arcade.Pad, names ...string) uint8 {
	v := uint8(0xff)
	for i, n := range names {
		if n != "" && p[n] {
			v &^= 1 << i
		}
	}
	return v
}

func pad(p []arcade.Pad, i int) arcade.Pad {
	if i < len(p) && p[i] != nil {
		return p[i]
	}
	return arcade.Pad{}
}

// clampOver keeps a saved slice overrun in range.
func clampOver(v int) int { return max(0, min(v, 64)) }
