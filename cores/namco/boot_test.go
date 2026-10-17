package namco

import (
	"encoding/binary"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/cores/arcade"
)

// mamePlay gives the controls the reference script (mame-ref/tools/play.lua,
// seed 1) holds during each frame: it sets them at the end of a frame, so
// they count from the next one.
type mamePlay struct {
	frame int
	s     int64
	held  [7]bool // up, down, left, right, buttons 1-3
}

func (m *mamePlay) rnd(n int64) int64 {
	m.s = (m.s*1103515245 + 12345) % 2147483648
	return m.s % n
}

func (m *mamePlay) next() arcade.Pad {
	f := m.frame // the frame whose end set this frame's controls
	m.frame++
	p := arcade.Pad{}
	if f%900 >= 120 && f%900 < 126 {
		p["Coin"] = true
	}
	if f%900 >= 200 && f%900 < 206 {
		p["Start (1P)"] = true
	}
	if f > 260 && f%20 == 0 {
		for k := range m.held {
			m.held[k] = m.rnd(3) == 0
		}
		if m.held[0] {
			m.held[1] = false
		}
		if m.held[2] {
			m.held[3] = false
		}
		m.held[4] = m.rnd(2) == 1
	}
	for k, n := range []string{"Up", "Down", "Left", "Right", "Fire"} {
		if m.held[k] {
			p[n] = true
		}
	}
	return p
}

// TestColdBoot runs the user's set from power-on with MAME's input script
// (coin at frame 120, start at 200, nothing else before frame 260) and
// compares each frame that has a MAME picture in $NAMCO_WORK/ref/<dir>/
// (RP_BOOT=galaga:galagao,...; RP_OFF shifts the frame numbers).
func TestColdBoot(t *testing.T) {
	e := os.Getenv("RP_BOOT")
	if e == "" {
		t.Skip("set RP_BOOT")
	}
	off, _ := strconv.Atoi(os.Getenv("RP_OFF"))
	for _, x := range strings.Split(e, ",") {
		set, dir, _ := strings.Cut(x, ":")
		pngs, _ := filepath.Glob(filepath.Join(work, "ref", dir, "*.png"))
		want := map[int]string{}
		last := 0
		for _, p := range pngs {
			n, err := strconv.Atoi(strings.TrimSuffix(filepath.Base(p), ".png"))
			if err == nil {
				want[n] = p
				last = max(last, n)
			}
		}
		c := openReal(t, set)
		b := c.Board().(*board)
		mp := mamePlay{s: 1}
		var audio []float32
		if os.Getenv("RP_WAV") != "" {
			defer func(set string) { writeWAV(t, fmt.Sprintf("%s/compare/%s.wav", work, set), audio) }(set)
		}
		for i := 1; i <= last+off; i++ {
			b.SetInput([]arcade.Pad{mp.next(), {}})
			c.RunFrame()
			audio = append(audio, c.TakeAudio()...)
			path, ok := want[i-off]
			if !ok {
				continue
			}
			m, _, _, err := readPix(path)
			if err != nil {
				continue
			}
			got := upright(b)
			diff := 0
			for k := range got {
				if got[k] != m[k] {
					diff++
				}
			}
			t.Logf("%s frame %d: %d pixels differ", set, i-off, diff)
			if diff > 0 && b.kind == galaga && b.vlat[5] != 0 && os.Getenv("RP_ALIGN") != "" {
				raw := make([]uint32, scrW*scrH)
				arcade.RotateInto(m, scrH, scrW, 270, raw)
				t.Logf("  MAME star scroll %d, ours %d", alignStars(b, raw), b.vid.stars.scroll)
				if d, err := os.ReadFile(strings.TrimSuffix(path, ".png") + ".bin"); err == nil && len(d) >= 0x2008 {
					t.Logf("  latches MAME %v ours %v; 92A0 MAME %02x ours %02x", d[0x2000:0x2008], b.vlat, d[0x12a0], b.ram[1][0x2a0])
				}
				b.vid.render(b)
			}
			if os.Getenv("RP_BOOTPNG") != "" {
				side := make([]uint32, scrH*2*scrW)
				for y := 0; y < scrW; y++ {
					copy(side[y*scrH*2:], got[y*scrH:(y+1)*scrH])
					copy(side[y*scrH*2+scrH:], m[y*scrH:(y+1)*scrH])
				}
				writePix(t, side, scrH*2, scrW, fmt.Sprintf("%s/compare/boot-%s-%05d.png", work, set, i-off))
			}
		}
	}
}

// writeWAV saves mono samples at 48 kHz as 16-bit PCM.
func writeWAV(t *testing.T, path string, s []float32) {
	d := make([]byte, 44+2*len(s))
	copy(d, "RIFF")
	binary.LittleEndian.PutUint32(d[4:], uint32(36+2*len(s)))
	copy(d[8:], "WAVEfmt ")
	binary.LittleEndian.PutUint32(d[16:], 16)
	binary.LittleEndian.PutUint16(d[20:], 1)
	binary.LittleEndian.PutUint16(d[22:], 1)
	binary.LittleEndian.PutUint32(d[24:], 48000)
	binary.LittleEndian.PutUint32(d[28:], 96000)
	binary.LittleEndian.PutUint16(d[32:], 2)
	binary.LittleEndian.PutUint16(d[34:], 16)
	copy(d[36:], "data")
	binary.LittleEndian.PutUint32(d[40:], uint32(2*len(s)))
	for i, v := range s {
		binary.LittleEndian.PutUint16(d[44+2*i:], uint16(int16(max(-1, min(1, v))*32767)))
	}
	if err := os.WriteFile(path, d, 0o644); err != nil {
		t.Log(err)
	}
}
