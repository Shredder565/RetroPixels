package capcom

import (
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// ramBoard is a board whose main-CPU memory a test can read when it draws.
type ramBoard interface {
	peek(a uint16) uint8
	setRenderHook(f func())
}

func (b *b1942) peek(a uint16) uint8    { return b.read(a) }
func (b *b1942) setRenderHook(f func()) { b.onRender = f }

// playInputs: coin at 121-126 and start at 201-206 of every 900 frames, as
// lua/simple.lua presses them in MAME.
func playInputs(frame int, idx map[string]int) uint32 {
	var bits uint32
	if m := (frame - 1) % 900; m >= 120 && m < 126 {
		bits |= 1 << idx["Coin"]
	}
	if m := (frame - 1) % 900; m >= 200 && m < 206 {
		bits |= 1 << idx["Start (1P)"]
	}
	return bits
}

// TestRAMCompare (RP_RAMCMP=set:dumpdir) plays the set as simple.lua plays
// it in MAME and compares the RAM at each dumped frame.
func TestRAMCompare(t *testing.T) {
	spec := os.Getenv("RP_RAMCMP")
	if spec == "" {
		t.Skip("RP_RAMCMP not set")
	}
	p := strings.Split(spec, ":")
	c := openReal(t, p[0])
	dumps, _ := filepath.Glob(filepath.Join(p[1], "dump_*.txt"))
	sort.Strings(dumps)
	want := map[int]*mameDump{}
	last := 0
	for _, dp := range dumps {
		d, err := readDump(dp)
		if err != nil {
			t.Fatal(err)
		}
		n := strings.TrimSuffix(strings.TrimPrefix(filepath.Base(dp), "dump_"), ".txt")
		d.frame, _ = strconv.Atoi(n)
		want[d.frame] = d
		last = max(last, d.frame)
	}
	rb := c.Board().(ramBoard)
	frame, bad := 0, 0
	rb.setRenderHook(func() {
		d := want[frame]
		if d == nil {
			return
		}
		for a, v := range d.ranges {
			n, first := 0, -1
			for i, x := range v {
				if got := rb.peek(uint16(a + i)); got != x {
					if n < 12 && os.Getenv("RP_RAMDETAIL") != "" {
						t.Logf("  %04x: native %02x mame %02x", a+i, got, x)
					}
					n++
					if first < 0 {
						first = a + i
					}
				}
			}
			if n > 0 {
				bad++
				t.Logf("frame %d: range %04x: %d bytes differ (first %04x)", frame, a, n, first)
			}
		}
	})
	idx := map[string]int{}
	for i, b := range c.Info().Buttons {
		idx[b] = i
	}
	for frame = 1; frame <= last; frame++ {
		c.SetInput(0, playInputs(frame, idx))
		c.RunFrame()
	}
	t.Logf("%d ranges differ", bad)
}
