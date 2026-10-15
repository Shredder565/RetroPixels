package main

import (
	"encoding/binary"
	"errors"
	"math"

	"retropixels/core"
)

// testCore is a tiny stand-in emulator for testing the shell: a moving test
// pattern, a box that the controls move, a tone, a battery save that counts
// presses of A, save states and rumble. Test builds (-tags fakecore) offer it
// for files ending in .rptest; the unit tests use it directly.
type testCore struct {
	rate          int
	frame         uint32
	x, y          int32
	input         [2]uint32
	prevA         bool
	phase         float64
	screen        []uint32
	audio         []float32
	sram          []byte
	analog        [2][4]float32
	width, height int
}

var testButtons = []string{"A", "B", "Select", "Start", "Up", "Down", "Left", "Right"}

func newTestCore(rom []byte, o core.Options) (core.Core, error) {
	if len(rom) == 0 {
		return nil, errors.New("empty test file")
	}
	c := &testCore{rate: o.SampleRate, x: 120, y: 100, width: 256, height: 240, sram: make([]byte, 16)}
	if len(o.SRAM) == len(c.sram) {
		copy(c.sram, o.SRAM)
	}
	if c.rate <= 0 {
		c.rate = 48000
	}
	c.screen = make([]uint32, c.width*c.height)
	c.draw()
	return c, nil
}

func (c *testCore) Info() core.Info {
	return core.Info{System: "fake", Buttons: testButtons, Players: 2, Aspect: 4.0 / 3}
}
func (c *testCore) FPS() float64     { return 60 }
func (c *testCore) Width() int       { return c.width }
func (c *testCore) Height() int      { return c.height }
func (c *testCore) Screen() []uint32 { return c.screen }
func (c *testCore) SetInput(p int, bits uint32) {
	if p < 2 {
		c.input[p] = bits
	}
}
func (c *testCore) SetAnalog(p, axis int, v float32) {
	if p < 2 && axis < 4 {
		c.analog[p][axis] = v
	}
}
func (c *testCore) Rumble() float32 {
	if c.input[0]&2 != 0 { // B
		return 1
	}
	return 0
}

func (c *testCore) RunFrame() {
	c.frame++
	in := c.input[0]
	if in&(1<<4) != 0 {
		c.y -= 2
	}
	if in&(1<<5) != 0 {
		c.y += 2
	}
	if in&(1<<6) != 0 {
		c.x -= 2
	}
	if in&(1<<7) != 0 {
		c.x += 2
	}
	if a := math.Abs(float64(c.analog[0][0])); a > 0.1 {
		c.x = int32((c.analog[0][0] + 1) / 2 * float32(c.width-16))
	}
	c.x = max(0, min(c.x, int32(c.width-16)))
	c.y = max(0, min(c.y, int32(c.height-16)))
	a := in&1 != 0
	if a && !c.prevA {
		n := binary.LittleEndian.Uint32(c.sram)
		binary.LittleEndian.PutUint32(c.sram, n+1)
	}
	c.prevA = a
	c.draw()
	// one frame of a 440 Hz tone (880 Hz while A is held), mono
	n := c.rate / 60
	freq := 440.0
	if a {
		freq = 880
	}
	for i := 0; i < n; i++ {
		c.audio = append(c.audio, float32(0.2*math.Sin(c.phase)))
		c.phase += 2 * math.Pi * freq / float64(c.rate)
	}
	c.phase = math.Mod(c.phase, 2*math.Pi)
}

func (c *testCore) draw() {
	w, h := c.width, c.height
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			// colour bars scrolling sideways, a checkerboard band at the bottom
			bar := ((x + int(c.frame)) / 32) % 8
			col := uint32(0)
			if bar&1 != 0 {
				col |= 0x0000ff
			}
			if bar&2 != 0 {
				col |= 0x00ff00
			}
			if bar&4 != 0 {
				col |= 0xff0000
			}
			if y >= h-32 {
				col = 0x202020
				if (x/8+y/8)%2 == 0 {
					col = 0xe0e0e0
				}
			}
			c.screen[y*w+x] = col
		}
	}
	// a white frame round the edge, to check nothing is cut off
	for x := 0; x < w; x++ {
		c.screen[x], c.screen[(h-1)*w+x] = 0xffffff, 0xffffff
	}
	for y := 0; y < h; y++ {
		c.screen[y*w], c.screen[y*w+w-1] = 0xffffff, 0xffffff
	}
	for y := c.y; y < c.y+16; y++ {
		for x := c.x; x < c.x+16; x++ {
			c.screen[int(y)*w+int(x)] = 0xffffff
		}
	}
}

func (c *testCore) TakeAudio() []float32 {
	a := c.audio
	c.audio = nil
	return a
}

func (c *testCore) Reset() { c.frame, c.x, c.y = 0, 120, 100; c.draw() }

func (c *testCore) SaveState() ([]byte, error) {
	b := make([]byte, 20)
	binary.LittleEndian.PutUint32(b, c.frame)
	binary.LittleEndian.PutUint32(b[4:], uint32(c.x))
	binary.LittleEndian.PutUint32(b[8:], uint32(c.y))
	binary.LittleEndian.PutUint64(b[12:], math.Float64bits(c.phase))
	return b, nil
}

func (c *testCore) LoadState(b []byte) error {
	if len(b) != 20 {
		return errors.New("bad test state")
	}
	c.frame = binary.LittleEndian.Uint32(b)
	c.x = int32(binary.LittleEndian.Uint32(b[4:]))
	c.y = int32(binary.LittleEndian.Uint32(b[8:]))
	c.phase = math.Float64frombits(binary.LittleEndian.Uint64(b[12:]))
	c.draw()
	return nil
}

func (c *testCore) SRAM() []byte { return c.sram }
