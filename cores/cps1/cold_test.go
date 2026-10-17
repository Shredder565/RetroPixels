package cps1

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"retropixels/cores/arcade"
)

// TestMAMECold plays from power on with the same inputs as a MAME run
// (CPS1_COLD: its dumps) and compares every frame that has a dump: the
// picture (MAME's picture at frame n+1 is the one drawn at the end of frame
// n) and the work and graphics RAM at the same moment. With CPS1_OUT it
// also writes the side-by-side pictures that differ and the sound.
// CPS1_NOFM / CPS1_NOOKI mute a chip (for measuring the mix).
func TestMAMECold(t *testing.T) {
	dir := os.Getenv("CPS1_COLD")
	if dir == "" {
		t.Skip("CPS1_COLD not set")
	}
	if os.Getenv("CPS1_NOFM") != "" {
		old := fmLevel
		fmLevel = 0
		defer func() { fmLevel = old }()
	}
	if os.Getenv("CPS1_NOOKI") != "" {
		old := okiLevel
		okiLevel = 0
		defer func() { okiLevel = old }()
	}
	c := loadReal(t)
	b := c.Board().(*board)
	out := os.Getenv("CPS1_OUT")
	// MAME's script sets the inputs after frame f, so they count from
	// frame f+1 here
	presses := []press{{"Coin", 1001, 6}, {"Start (1P)", 1101, 6}, {"Button 1", 1201, 300}, {"Left", 1301, 60}}
	last := 1800
	if os.Getenv("CPS1_LONG") != "" {
		// the long run's pattern from frame 1200 (inputs2.lua)
		last = 5400
		presses = presses[:2]
		for f := 1200; f < last; f++ {
			if f%20 == 0 {
				presses = append(presses, press{"Button 1", f + 1, 10})
			}
			if f%240 == 0 {
				presses = append(presses, press{"Left", f + 1, 120}, press{"Right", f + 121, 120})
			}
			if f%400 == 0 {
				presses = append(presses, press{"Up", f + 1, 200})
			}
		}
	}
	var audio []float32
	frames, bad := 0, 0
	for f := 1; f <= last; f++ {
		runRange(c, f, presses)
		audio = append(audio, c.TakeAudio()...)
		if r, err := readDump(filepath.Join(dir, fmt.Sprintf("f%05d.bin", f))); err == nil {
			nw, ng := 0, 0
			for i := range r.wram {
				if r.wram[i] != b.wram[i] {
					nw++
				}
			}
			for i := range r.gfx {
				if r.gfx[i] != b.vid.gfxram[i] {
					ng++
				}
			}
			if nw+ng > 0 {
				t.Logf("frame %d: work RAM %d bytes differ, graphics RAM %d", f, nw, ng)
			}
		}
		d, err := readDump(filepath.Join(dir, fmt.Sprintf("f%05d.bin", f+1)))
		if err != nil {
			continue
		}
		diff := 0
		for i, px := range b.vid.screen {
			if px != d.pix[i] {
				diff++
			}
		}
		frames++
		if diff > 0 {
			bad++
			t.Logf("frame %d: %d pixels differ", f, diff)
		}
		if out != "" && (diff > 0 || os.Getenv("CPS1_SAVEALL") != "") {
			side := make([]uint32, ScreenW*2*ScreenH)
			for y := 0; y < ScreenH; y++ {
				copy(side[y*ScreenW*2:], b.vid.screen[y*ScreenW:(y+1)*ScreenW])
				copy(side[y*ScreenW*2+ScreenW:], d.pix[y*ScreenW:(y+1)*ScreenW])
			}
			savePNG(filepath.Join(out, fmt.Sprintf("cold%05d.png", f)), side, ScreenW*2, ScreenH)
		}
	}
	t.Logf("%d frames compared, %d differ", frames, bad)
	if out != "" {
		f, _ := os.Create(filepath.Join(out, "cold.raw"))
		binary.Write(f, binary.LittleEndian, audio)
		f.Close()
	}
}

// runRange runs frame f with the presses held.
func runRange(c *arcade.Core, f int, presses []press) {
	v := uint32(0)
	for _, p := range presses {
		if f >= p.from && f < p.from+p.n {
			for i, b := range c.Info().Buttons {
				if b == p.btn {
					v |= 1 << i
				}
			}
		}
	}
	c.SetInput(0, v)
	c.RunFrame()
}
