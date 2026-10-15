package nes

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
)

// TestCompareJS runs the web NES core (web/emu/nes.js in Node) and this port on
// the same ROMs with the same scripted inputs and compares every frame's
// picture and every audio sample.
//
// It needs ROMs, which are not in the repository: set NES_COMPARE_LIST to a
// file listing one ROM path per line (optionally followed by a frame count).
func TestCompareJS(t *testing.T) {
	list := os.Getenv("NES_COMPARE_LIST")
	if list == "" {
		t.Skip("NES_COMPARE_LIST not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	f, err := os.Open(list)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	emu, _ := filepath.Abs("../../web/emu")
	runner, _ := filepath.Abs("testdata/nesrun.js")
	tmp := t.TempDir()
	sc := bufio.NewScanner(f)
	i := 0
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		frames := 3000
		if k := strings.LastIndexByte(line, ' '); k > 0 {
			if n, err := strconv.Atoi(line[k+1:]); err == nil {
				frames = n
				line = strings.TrimSpace(line[:k])
			}
		}
		rom := line
		out := filepath.Join(tmp, fmt.Sprintf("%d.bin", i))
		i++
		t.Run(filepath.Base(rom), func(t *testing.T) {
			t.Parallel()
			compareROM(t, node, runner, emu, rom, out, frames)
		})
	}
}

// scriptInput is the input script shared with nesrun.js.
type scriptInput struct{ seed, p1, p2 uint32 }

func (s *scriptInput) next(f int) (uint32, uint32) {
	s.seed ^= s.seed << 13
	s.seed ^= s.seed >> 17
	s.seed ^= s.seed << 5
	r := s.seed
	if f%8 == 0 {
		s.p1 = r & 0xf7
		if (r>>8)&15 == 0 {
			s.p1 |= 8
		}
		s.p2 = (r >> 16) & 0xff
	}
	if f < 120 {
		s.p1 = 0
		if f >= 60 && f < 70 {
			s.p1 = 8
		}
	}
	return s.p1, s.p2
}

func screenHash(s []uint32) uint32 {
	h := uint32(2166136261)
	for _, c := range s {
		h = (h ^ c) * 16777619
	}
	return h
}

func compareROM(t *testing.T, node, runner, emu, rom, out string, frames int) {
	cmd := exec.Command(node, "--max-old-space-size=4096", runner, emu, rom, strconv.Itoa(frames), out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("node: %v\n%s", err, b)
	}
	js, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(rom)
	if err != nil {
		t.Fatal(err)
	}
	c, err := New(data, core.Options{SampleRate: 48000})
	if strings.HasPrefix(string(js), "ERR ") {
		if err == nil {
			t.Fatalf("JS refused the ROM (%s) but Go opened it", js[4:])
		}
		if err.Error() != string(js[4:]) {
			t.Fatalf("error differs: js %q go %q", js[4:], err)
		}
		t.Logf("both refuse: %v", err)
		return
	}
	if err != nil {
		t.Fatalf("Go refused the ROM: %v", err)
	}
	n := c.(*NES)
	in := &scriptInput{seed: 0x1234567}
	pos := 0
	maxDiff, samples := 0.0, 0
	for f := 0; f < frames; f++ {
		p1, p2 := in.next(f)
		n.SetInput(0, p1)
		n.SetInput(1, p2)
		if f == frames/2 {
			n.Reset()
		}
		n.RunFrame()
		a := n.TakeAudio()
		wantHash := binary.LittleEndian.Uint32(js[pos:])
		cnt := int(binary.LittleEndian.Uint32(js[pos+4:]))
		pos += 8
		if got := screenHash(n.Screen()); got != wantHash {
			dump := out + ".frame"
			exec.Command(node, runner, emu, rom, strconv.Itoa(f+1), out+".x", strconv.Itoa(f), dump).Run()
			jsPix, _ := os.ReadFile(dump)
			diff, first := 0, -1
			for i, c := range n.Screen() {
				if 4*i+4 <= len(jsPix) && binary.LittleEndian.Uint32(jsPix[4*i:]) != c {
					if first < 0 {
						first = i
					}
					diff++
				}
			}
			t.Fatalf("frame %d: picture differs (%d pixels, first at x=%d y=%d)", f, diff, first%256, first/256)
		}
		if cnt != len(a) {
			t.Fatalf("frame %d: %d audio samples, JS made %d", f, len(a), cnt)
		}
		for i := 0; i < cnt; i++ {
			w := math.Float32frombits(binary.LittleEndian.Uint32(js[pos+4*i:]))
			d := math.Abs(float64(w) - float64(a[i]))
			if d > maxDiff {
				maxDiff = d
			}
			if d >= 1e-4 {
				t.Fatalf("frame %d sample %d: js %v go %v", f, i, w, a[i])
			}
		}
		samples += cnt
		pos += 4 * cnt
	}
	sl := binary.LittleEndian.Uint32(js[pos:])
	pos += 4
	sram := n.SRAM()
	if sl == 0xffffffff {
		if sram != nil {
			t.Fatalf("Go has SRAM, JS has none")
		}
	} else if string(sram) != string(js[pos:pos+int(sl)]) {
		t.Fatalf("SRAM differs")
	}
	t.Logf("%d frames identical, %d samples (max diff %.3g), mapper %d", frames, samples, maxDiff, n.cart.mapper)
}
