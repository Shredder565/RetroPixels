package atari2600

// Compares the Go core with the web core (web/emu, run in Node) frame by
// frame: every pixel of every frame and every sound sample.
//
//	RP_A2600_ROMS=/path/to/roms go test ./cores/atari2600 -run MatchesJS -v
//
// RP_A2600_ROMS is a directory of .bin / .a26 files (or one file);
// RP_A2600_FRAMES sets the frames per game (default 3000). Skipped without
// node or ROMs (no game files are kept in the repository).

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

// frameInput is one frame of a scripted run.
type frameInput struct {
	p0, p1 uint32
	a0, a1 float32
	reset  bool
}

// script makes a deterministic, game-like run of inputs: joystick moves and
// fire, the console switches (Reset to start games, Select, difficulty),
// paddle knob turns, and one power reset half way.
func script(seed uint64, frames int) []frameInput {
	s := seed | 1
	rnd := func(n int) int {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return int(s % uint64(n))
	}
	out := make([]frameInput, frames)
	var p0, p1 uint32
	var a0, a1 float32
	for f := range out {
		if f%20 == 0 {
			p0 = 0
			switch rnd(4) {
			case 0:
			case 1:
				p0 |= 1 << rnd(4) // a direction
			default:
				p0 |= 1<<rnd(4) | 1<<rnd(4)
			}
			if rnd(3) == 0 {
				p0 |= btnFire
			}
			p1 = uint32(rnd(32))
			if rnd(8) == 0 {
				a0 = float32(rnd(129)-64) / 64
			} else if rnd(4) == 0 {
				a0 = 0
			}
			if rnd(8) == 0 {
				a1 = float32(rnd(129)-64) / 64
			}
		}
		in := frameInput{p0: p0, p1: p1, a0: a0, a1: a1}
		// game reset switch now and then (starts most games), select rarely
		if f%300 >= 60 && f%300 < 66 {
			in.p0 |= btnReset
		}
		if f%700 >= 400 && f%700 < 404 {
			in.p0 |= btnSelect
		}
		if (f/500)%2 == 1 {
			in.p0 |= btnLDiff
		}
		if (f/900)%3 == 2 {
			in.p0 |= btnBW
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
	c.SetInput(0, in.p0)
	c.SetInput(1, in.p1)
	a := c.(core.Analog)
	a.SetAnalog(0, 0, in.a0)
	a.SetAnalog(1, 0, in.a1)
}

func writeScript(t *testing.T, ins []frameInput) string {
	b := make([]byte, 0, len(ins)*32)
	for _, in := range ins {
		b = binary.LittleEndian.AppendUint32(b, in.p0)
		b = binary.LittleEndian.AppendUint32(b, in.p1)
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(float64(in.a0)))
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(float64(in.a1)))
		r := uint32(0)
		if in.reset {
			r = 1
		}
		b = binary.LittleEndian.AppendUint32(b, r)
		b = binary.LittleEndian.AppendUint32(b, 0)
	}
	p := filepath.Join(t.TempDir(), "inputs.bin")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func romFiles(t *testing.T) []string {
	dir := os.Getenv("RP_A2600_ROMS")
	if dir == "" {
		t.Skip("RP_A2600_ROMS not set")
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
		if strings.HasSuffix(n, ".bin") || strings.HasSuffix(n, ".a26") {
			out = append(out, filepath.Join(dir, e.Name()))
		}
	}
	return out
}

type jsFrame struct {
	err    string
	kind   string // the first message: the cartridge type
	sram   []byte // the last message: the battery save
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

// runFrameSafe runs a frame, turning a core panic into an error message.
func runFrameSafe(c core.Core) (msg string) {
	defer func() {
		if r := recover(); r != nil {
			msg = fmt.Sprint(r)
		}
	}()
	c.RunFrame()
	return ""
}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	roms := romFiles(t)
	frames := 3000
	if s := os.Getenv("RP_A2600_FRAMES"); s != "" {
		frames, _ = strconv.Atoi(s)
	}
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/jsref.js")
	totalFrames, totalSamples := 0, 0
	for _, romPath := range roms {
		name := filepath.Base(romPath)
		t.Run(name, func(t *testing.T) {
			rom, err := os.ReadFile(romPath)
			if err != nil {
				t.Fatal(err)
			}
			h := fnv.New64a()
			h.Write([]byte(name))
			ins := script(h.Sum64(), frames)
			inPath := writeScript(t, ins)
			cmd := exec.Command(node, ref, emu, romPath, name, inPath, strconv.Itoa(frames), "48000")
			cmd.Stderr = os.Stderr
			stdout, _ := cmd.StdoutPipe()
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			defer cmd.Wait()
			defer stdout.Close()
			r := bufio.NewReaderSize(stdout, 1<<20)

			c, goErr := New(rom, core.Options{SampleRate: 48000, Name: name})
			if goErr != nil {
				f, err := readJSFrame(r)
				if err != nil || f.err == "" {
					t.Fatalf("Go core refused the game (%v) but the JS core accepted it", goErr)
				}
				if f.err != goErr.Error() {
					t.Errorf("error messages differ:\n go: %v\n js: %s", goErr, f.err)
				}
				t.Logf("both refuse: %s", f.err)
				return
			}
			{
				k := make([]byte, 8)
				io.ReadFull(r, k)
				kind := make([]byte, binary.LittleEndian.Uint32(k[4:]))
				io.ReadFull(r, kind)
				if binary.LittleEndian.Uint32(k) != 0 {
					t.Fatalf("the JS core refused the game (%s) but the Go core accepted it", kind)
				}
				if got := c.(*Atari).cart.kind(); got != string(kind) {
					t.Fatalf("scheme: go %s, js %s", got, kind)
				}
			}
			maxDiff := 0.0
			samples := 0
			distinct := map[uint64]bool{}
			for i := 0; i < frames; i++ {
				applyInput(c, ins[i])
				goMsg := runFrameSafe(c)
				f, err := readJSFrame(r)
				if err != nil {
					t.Fatalf("frame %d: reading JS output: %v", i, err)
				}
				if f.err != "" || goMsg != "" {
					if !strings.Contains(f.err, goMsg) || goMsg == "" {
						t.Fatalf("frame %d: errors differ: go %q, js %q", i, goMsg, f.err)
					}
					t.Logf("frame %d: both stop with %q", i, goMsg)
					return
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
					if d > maxDiff {
						maxDiff = d
					}
				}
				if maxDiff >= 1e-4 {
					t.Fatalf("frame %d: sound differs by %g", i, maxDiff)
				}
				samples += len(aud)
			}
			end, err := readJSFrame(r)
			if err != nil || !end.end {
				t.Fatalf("reading the JS battery save: %v", err)
			}
			if gs := c.SRAM(); string(gs) != string(end.sram) || (gs == nil) != (end.sram == nil) {
				t.Fatalf("battery save differs: go %d bytes, js %d bytes", len(gs), len(end.sram))
			}
			totalFrames += frames
			totalSamples += samples
			t.Logf("%s (%s): %d frames identical (%d different pictures), %d samples, max sample diff %g, battery save %d bytes",
				name, c.(*Atari).cart.kind(), frames, len(distinct), samples, maxDiff, len(c.SRAM()))
		})
	}
	t.Logf("total: %d frames, %d samples compared", totalFrames, totalSamples)
}
