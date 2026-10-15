package genesis

// Compares the Go core with the web core (web/emu, run in Node) frame by
// frame: every pixel of every frame, every sound sample and the battery save.
//
//	RP_MD_ROMS=/path/to/roms go test ./cores/genesis -run MatchesJS -v
//
// RP_MD_ROMS is a directory of .md / .gen / .smd / .bin files (or one file);
// RP_MD_FRAMES sets the frames per game (default 3000). A game that ends with
// a battery save is run again from that save. Skipped without node or ROMs
// (no game files are kept in the repository). TestCartsMatchJS runs the
// generated cartridge test programs (carttest_test.go) the same way.

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
)

type frameInput struct {
	pads  [4]uint32
	reset bool
}

// script makes a deterministic, game-like run of inputs: pad moves and
// buttons on all four pads (Start now and then to start games, A/B/C to get
// through menus), Mode held at power-on for some games, and the player's
// Restart button once half way.
func script(seed uint64, frames int) []frameInput {
	s := seed | 1
	rnd := func(n int) int {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return int(s % uint64(n))
	}
	modeAtBoot := seed%5 == 0
	out := make([]frameInput, frames)
	var pads [4]uint32
	for f := range out {
		if f%20 == 0 {
			for p := range pads {
				v := uint32(0)
				switch rnd(4) {
				case 0:
				case 1:
					v |= bUp << rnd(4)
				default:
					v |= bUp<<rnd(4) | bUp<<rnd(4)
				}
				for k := 0; k < 3; k++ {
					if rnd(3) == 0 {
						v |= 1 << rnd(8)
					}
				}
				v &^= bStart | bMode
				pads[p] = v
			}
		}
		in := frameInput{pads: pads}
		if f%300 >= 60 && f%300 < 66 || f%300 >= 150 && f%300 < 153 { // start games, get past title screens
			in.pads[0] |= bStart
		}
		if f%400 >= 200 && f%400 < 204 {
			in.pads[0] |= bA | bC
		}
		if f%700 >= 500 && f%700 < 503 {
			in.pads[1] |= bStart
		}
		if f < 4 && modeAtBoot {
			in.pads[0] |= bMode
		}
		if f%900 >= 800 && f%900 < 806 {
			in.pads[0] |= bMode
		}
		in.reset = f == frames/2 && frames >= 200
		out[f] = in
	}
	return out
}

func applyInput(c core.Core, in frameInput) {
	if in.reset {
		c.Reset()
	}
	for p, v := range in.pads {
		c.SetInput(p, v)
	}
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

func romFiles(t *testing.T) []string {
	dir := os.Getenv("RP_MD_ROMS")
	if dir == "" {
		t.Skip("RP_MD_ROMS not set")
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
		n := strings.ToLower(e.Name())
		for _, x := range []string{".md", ".gen", ".smd", ".bin"} {
			if strings.HasSuffix(n, x) {
				out = append(out, filepath.Join(dir, e.Name()))
			}
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

func runFrameSafe(c core.Core) (msg string) {
	defer func() {
		if r := recover(); r != nil {
			msg = fmt.Sprint(r)
		}
	}()
	c.RunFrame()
	return ""
}

// describe: the cartridge as jsref.js describes it.
func describe(g *Genesis) string {
	c := g.cart
	ee := "noee"
	if c.ee != nil {
		ee = fmt.Sprintf("ee%d/%d", len(c.ee.mem), c.ee.mode)
	}
	mapper := c.mapper
	if mapper == "" {
		mapper = "none"
	}
	b := func(v bool) int {
		if v {
			return 1
		}
		return 0
	}
	return fmt.Sprintf("%s %s %s jcart=%d three=%d sram=%x-%x", g.region, mapper, ee, b(c.jcart), b(c.threeButton), c.sramStart, c.sramEnd)
}

type compareResult struct {
	frames, samples, distinct int
	maxDiff                   float64
	sram                      []byte
	desc                      string
	refused                   string
}

// compareRun runs one game in both cores and fails on the first difference.
func compareRun(t *testing.T, node, romPath string, sram []byte, frames int, seed uint64) *compareResult {
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/jsref.js")
	rom, err := os.ReadFile(romPath)
	if err != nil {
		t.Fatal(err)
	}
	ins := script(seed, frames)
	args := []string{ref, emu, romPath, writeScript(t, ins), strconv.Itoa(frames), "48000"}
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

	c, goErr := New(rom, core.Options{SampleRate: 48000, Name: filepath.Base(romPath), SRAM: sram})
	res := &compareResult{}
	{
		k := make([]byte, 8)
		io.ReadFull(r, k)
		desc := make([]byte, binary.LittleEndian.Uint32(k[4:]))
		io.ReadFull(r, desc)
		if binary.LittleEndian.Uint32(k) != 0 {
			if goErr == nil {
				t.Fatalf("the JS core refused the game (%s) but the Go core accepted it", desc)
			}
			if goErr.Error() != string(desc) {
				t.Fatalf("refusals differ: go %q, js %q", goErr, desc)
			}
			res.refused = goErr.Error()
			return res
		}
		if goErr != nil {
			t.Fatalf("Go core refused the game: %v", goErr)
		}
		res.desc = describe(c.(*Genesis))
		if res.desc != string(desc) {
			t.Fatalf("cartridge: go %q, js %q", res.desc, desc)
		}
	}
	distinct := map[uint64]bool{}
	for i := 0; i < frames; i++ {
		applyInput(c, ins[i])
		goMsg := runFrameSafe(c)
		f, err := readJSFrame(r)
		if err != nil {
			t.Fatalf("frame %d: reading JS output: %v", i, err)
		}
		if f.err != "" || goMsg != "" {
			t.Fatalf("frame %d: errors: go %q, js %q", i, goMsg, f.err)
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
			d := math.Abs(float64(aud[k]) - float64(f.audio[k]))
			if d > res.maxDiff {
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
		t.Fatalf("battery save differs: go %d bytes, js %d bytes", len(gs), len(end.sram))
	}
	res.frames, res.distinct, res.sram = frames, len(distinct), gs
	return res
}

func compareGames(t *testing.T, roms []string, frames int) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	totalFrames, totalSamples := 0, 0
	for _, romPath := range roms {
		name := filepath.Base(romPath)
		t.Run(name, func(t *testing.T) {
			h := fnv.New64a()
			h.Write([]byte(name))
			res := compareRun(t, node, romPath, nil, frames, h.Sum64())
			if res.refused != "" {
				t.Logf("%s: both cores refuse it: %s", name, res.refused)
				return
			}
			totalFrames += res.frames
			totalSamples += res.samples
			t.Logf("%s (%s): %d frames identical (%d different pictures), %d samples, max sample diff %g, battery save %d bytes",
				name, res.desc, res.frames, res.distinct, res.samples, res.maxDiff, len(res.sram))
			if res.sram != nil {
				n := frames / 3
				res2 := compareRun(t, node, romPath, res.sram, n, h.Sum64()+1)
				totalFrames += res2.frames
				totalSamples += res2.samples
				t.Logf("  ...and %d more frames identical started from that battery save", n)
			}
		})
	}
	t.Logf("total: %d frames, %d samples compared", totalFrames, totalSamples)
}

func TestMatchesJS(t *testing.T) {
	frames := 3000
	if s := os.Getenv("RP_MD_FRAMES"); s != "" {
		frames, _ = strconv.Atoi(s)
	}
	compareGames(t, romFiles(t), frames)
}
