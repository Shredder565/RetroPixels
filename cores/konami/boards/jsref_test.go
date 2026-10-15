package boards

// Compares the Go boards with the web ones (web/emu, run in Node) frame by
// frame: every pixel of every frame, every sound sample and the EEPROM.
//
//	RP_KONAMI_ROMS=/path/to/sets go test ./cores/konami/boards -run MatchesJS -v
//
// RP_KONAMI_ROMS is a directory of .zip ROM sets (or one .zip); RP_KONAMI_FRAMES
// sets the frames per game (default 3000). A game that ends with an EEPROM
// is run again from it. Skipped without node or sets (no ROM sets are kept
// in the repository).

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"hash/fnv"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
	"retropixels/cores/arcade"
)

type frameInput struct {
	pads  [4]uint32
	reset bool
}

// script: a deterministic, game-like run: coins and starts for every player
// early on (and now and then later), random moves and buttons, and the
// player's Restart button once half way.
func script(seed uint64, frames int, buttons []string) []frameInput {
	s := seed | 1
	rnd := func(n int) int {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return int(s % uint64(n))
	}
	bitOf := func(name string) uint32 {
		for i, n := range buttons {
			if n == name {
				return 1 << i
			}
		}
		return 0
	}
	var play []uint32
	for _, n := range []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3"} {
		if b := bitOf(n); b != 0 {
			play = append(play, b)
		}
	}
	coin, start, start2 := bitOf("Coin"), bitOf("Start (1P)"), bitOf("Start (2P)")
	out := make([]frameInput, frames)
	var pads [4]uint32
	for f := range out {
		if f%15 == 0 {
			for p := range pads {
				v := uint32(0)
				for k := rnd(4); k > 0; k-- {
					v |= play[rnd(len(play))]
				}
				pads[p] = v
			}
		}
		in := frameInput{pads: pads}
		for p := 0; p < 4; p++ {
			at := 300 + p*40 // after the start-up checks
			if f%1500 >= at && f%1500 < at+4 {
				in.pads[p] |= coin
			}
			if f%1500 >= at+20 && f%1500 < at+24 {
				in.pads[p] |= start
			}
		}
		if f%1100 >= 900 && f%1100 < 904 {
			in.pads[0] |= start2
		}
		in.reset = f == frames/2 && frames >= 200
		out[f] = in
	}
	return out
}

func writeScript(t *testing.T, ins []frameInput) string {
	b := make([]byte, 0, len(ins)*20)
	for _, in := range ins {
		for _, v := range in.pads {
			b = binary.LittleEndian.AppendUint32(b, v)
		}
		r := uint32(0)
		if in.reset {
			r = 1
		}
		b = binary.LittleEndian.AppendUint32(b, r)
	}
	p := filepath.Join(t.TempDir(), "inputs.bin")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func romSets(t *testing.T) []string {
	dir := os.Getenv("RP_KONAMI_ROMS")
	if dir == "" {
		t.Skip("RP_KONAMI_ROMS not set")
	}
	st, err := os.Stat(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !st.IsDir() {
		return []string{dir}
	}
	ents, _ := os.ReadDir(dir)
	var out []string
	for _, e := range ents {
		if strings.HasSuffix(strings.ToLower(e.Name()), ".zip") {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

type jsFrame struct {
	err    string
	sram   []byte
	end    bool
	w, h   int
	pixels []uint32
	audio  []float32
}

func readJSFrame(r *bufio.Reader) (*jsFrame, error) {
	var hdr [4]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return nil, err
	}
	f := &jsFrame{}
	u32 := func() uint32 {
		var b [4]byte
		io.ReadFull(r, b[:])
		return binary.LittleEndian.Uint32(b[:])
	}
	switch binary.LittleEndian.Uint32(hdr[:]) {
	case 1:
		msg := make([]byte, u32())
		io.ReadFull(r, msg)
		f.err = string(msg)
		return f, nil
	case 2:
		f.end = true
		if n := u32(); n != 0xffffffff {
			f.sram = make([]byte, n)
			io.ReadFull(r, f.sram)
		}
		return f, nil
	}
	f.w, f.h = int(u32()), int(u32())
	buf := make([]byte, f.w*f.h*4)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	f.pixels = make([]uint32, f.w*f.h)
	for i := range f.pixels {
		f.pixels[i] = binary.LittleEndian.Uint32(buf[i*4:])
	}
	n := int(u32())
	ab := make([]byte, n*4)
	if _, err := io.ReadFull(r, ab); err != nil {
		return nil, err
	}
	f.audio = make([]float32, n)
	for i := range f.audio {
		f.audio[i] = math.Float32frombits(binary.LittleEndian.Uint32(ab[i*4:]))
	}
	return f, nil
}

func hashPixels(p []uint32) uint64 {
	h := fnv.New64a()
	b := make([]byte, 4)
	for _, v := range p {
		binary.LittleEndian.PutUint32(b, v)
		h.Write(b)
	}
	return h.Sum64()
}

type compareResult struct {
	frames, samples, distinct int
	maxDiff, peak             float64
	sram                      []byte
	desc                      string
}

func openGo(t *testing.T, zipPath string, sram []byte) *arcade.Core {
	data, err := os.ReadFile(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	name := strings.TrimSuffix(filepath.Base(zipPath), filepath.Ext(zipPath))
	c, err := arcade.New(data, core.Options{SampleRate: 48000, Name: name, SRAM: sram})
	if err != nil {
		t.Fatalf("Go core refused the set: %v", err)
	}
	return c.(*arcade.Core)
}

// compareRun runs one set in both cores and fails on the first difference.
func compareRun(t *testing.T, node, zipPath string, sram []byte, frames int, seed uint64) *compareResult {
	emu, _ := filepath.Abs("../../../web/emu")
	ref, _ := filepath.Abs("testdata/jsref.js")
	c := openGo(t, zipPath, sram)
	ins := script(seed, frames, c.Info().Buttons)
	args := []string{ref, emu, zipPath, writeScript(t, ins), strconv.Itoa(frames), "48000"}
	if sram != nil {
		p := filepath.Join(t.TempDir(), "save.sav")
		os.WriteFile(p, sram, 0o644)
		args = append(args, p)
	}
	cmd := exec.Command(node, args...)
	cmd.Stderr = os.Stderr
	stdout, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Wait()
	defer stdout.Close()
	r := bufio.NewReaderSize(stdout, 1<<20)
	res := &compareResult{}
	{
		k := make([]byte, 8)
		io.ReadFull(r, k)
		desc := make([]byte, binary.LittleEndian.Uint32(k[4:]))
		io.ReadFull(r, desc)
		if binary.LittleEndian.Uint32(k) != 0 {
			t.Fatalf("the JS core refused the set: %s", desc)
		}
		res.desc = fmt.Sprintf("%s %d %dx%d", c.GameID(), c.Players(), c.Width(), c.Height())
		if res.desc != string(desc) {
			t.Fatalf("game: go %q, js %q", res.desc, desc)
		}
	}
	distinct := map[uint64]bool{}
	for i := 0; i < frames; i++ {
		in := ins[i]
		if in.reset {
			c.Reset()
		}
		for p, v := range in.pads {
			c.SetInput(p, v)
		}
		c.RunFrame()
		f, err := readJSFrame(r)
		if err != nil {
			t.Fatalf("frame %d: reading JS output: %v", i, err)
		}
		if f.err != "" {
			t.Fatalf("frame %d: JS error %q", i, f.err)
		}
		if f.w != c.Width() || f.h != c.Height() {
			t.Fatalf("frame %d: size %dx%d, JS %dx%d", i, c.Width(), c.Height(), f.w, f.h)
		}
		scr := c.Screen()
		hp := hashPixels(scr)
		distinct[hp] = true
		if hp != hashPixels(f.pixels) {
			n, first := 0, -1
			for k := range scr {
				if scr[k] != f.pixels[k] {
					if first < 0 {
						first = k
					}
					n++
				}
			}
			t.Fatalf("frame %d: %d pixels differ, first at x=%d y=%d: go %06x js %06x", i, n, first%f.w, first/f.w, scr[first], f.pixels[first])
		}
		aud := c.TakeAudio()
		if len(aud) != len(f.audio) {
			t.Fatalf("frame %d: %d samples, JS %d", i, len(aud), len(f.audio))
		}
		for k := range aud {
			res.peak = max(res.peak, math.Abs(float64(aud[k])))
			if d := math.Abs(float64(aud[k]) - float64(f.audio[k])); d > res.maxDiff {
				res.maxDiff = d
			}
		}
		if res.maxDiff >= 1e-4 {
			t.Fatalf("frame %d: sound differs by %g", i, res.maxDiff)
		}
		res.samples += len(aud)
	}
	end, err := readJSFrame(r)
	if err != nil || !end.end {
		t.Fatalf("reading the JS battery save: %v", err)
	}
	gs := c.SRAM()
	if string(gs) != string(end.sram) || (gs == nil) != (end.sram == nil) {
		t.Fatalf("EEPROM differs: go %x, js %x", gs, end.sram)
	}
	res.frames, res.distinct, res.sram = frames, len(distinct), gs
	return res
}

func TestMatchesJS(t *testing.T) {
	frames := 3000
	if s := os.Getenv("RP_KONAMI_FRAMES"); s != "" {
		frames, _ = strconv.Atoi(s)
	}
	sets := romSets(t)
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	for _, zp := range sets {
		name := filepath.Base(zp)
		t.Run(name, func(t *testing.T) {
			h := fnv.New64a()
			h.Write([]byte(name))
			res := compareRun(t, node, zp, nil, frames, h.Sum64())
			t.Logf("%s (%s): %d frames identical (%d different pictures), %d samples (peak %.3f), max sample diff %g, EEPROM %d bytes",
				name, res.desc, res.frames, res.distinct, res.samples, res.peak, res.maxDiff, len(res.sram))
			if res.sram != nil {
				n := frames / 3
				res2 := compareRun(t, node, zp, res.sram, n, h.Sum64()+1)
				t.Logf("  ...and %d more frames identical started from that EEPROM (%d samples, max diff %g)", n, res2.samples, res2.maxDiff)
			}
		})
	}
}
