package arcade

// Compares the Go arcade core with the web one (web/emu/arcade.js and its
// boards, run in Node) frame by frame: every pixel of every frame and every
// sound sample, on generated ROM sets for every board (see testsets_test.go),
// with scripted controls (random buttons, a coin and Start early, machine
// settings changed and a reset part way). Sets the loader must refuse are
// checked to give the same message. Skipped without node.
//
//	RP_ARCADE_FRAMES=n sets the frames per set (default 600; the full run is
//	RP_ARCADE_FRAMES=3000).

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
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
	p0, p1, action, arg uint32
}

func script(seed uint64, frames, buttons, dips int) []frameInput {
	r := &rng{seed | 1}
	rnd := func(n int) int { return int(r.next() % uint64(n)) }
	out := make([]frameInput, frames)
	var p0, p1 uint32
	for f := range out {
		if f%15 == 0 {
			p0 = 0
			for k := 0; k < 3; k++ {
				if rnd(2) == 0 {
					p0 |= 1 << rnd(buttons)
				}
			}
			p1 = uint32(rnd(1 << buttons))
		}
		in := frameInput{p0: p0, p1: p1}
		if f >= 30 && f < 36 || f >= 60 && f < 66 { // coin, then start
			in.p0 = 0xffffffff
		}
		if f == frames/2 && frames >= 100 {
			in.action |= 1
		}
		if dips > 0 && (f == frames/3 || f == frames*2/3 || f == 10) {
			in.action |= 2
			in.arg = uint32(rnd(dips)<<8 | rnd(8))
		}
		out[f] = in
	}
	return out
}

func writeScript(t *testing.T, ins []frameInput) string {
	b := make([]byte, 0, len(ins)*16)
	for _, in := range ins {
		for _, v := range []uint32{in.p0, in.p1, in.action, in.arg} {
			b = binary.LittleEndian.AppendUint32(b, v)
		}
	}
	p := filepath.Join(t.TempDir(), "inputs.bin")
	if err := os.WriteFile(p, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func (ts *testSet) writeDir(t *testing.T) string {
	dir := t.TempDir()
	for _, n := range ts.order {
		os.WriteFile(filepath.Join(dir, strings.NewReplacer("/", "__", `\`, "__").Replace(n)), ts.files[n], 0o644)
	}
	os.WriteFile(filepath.Join(dir, "order.txt"), []byte(strings.Join(ts.order, "\n")+"\n"), 0o644)
	return dir
}

func (ts *testSet) open(dips map[string]int) (*Core, error) {
	var sizes []int
	var data [][]byte
	for _, n := range ts.order {
		sizes = append(sizes, len(ts.files[n]))
		data = append(data, ts.files[n])
	}
	return NewFromFiles(ts.order, sizes, data, ts.zipName, core.Options{SampleRate: 48000, Dips: dips})
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

type jsReader struct{ r *bufio.Reader }

func (j jsReader) u32() uint32 {
	var b [4]byte
	io.ReadFull(j.r, b[:])
	return binary.LittleEndian.Uint32(b[:])
}
func (j jsReader) str() string {
	b := make([]byte, j.u32())
	io.ReadFull(j.r, b)
	return string(b)
}

type result struct {
	frames, samples, distinct int
	maxDiff, energy           float64
	refused                   string
}

func compareRun(t *testing.T, node string, ts *testSet, frames int, seed uint64, dips map[string]int) *result {
	emu, _ := filepath.Abs("../../web/emu")
	ref, _ := filepath.Abs("testdata/jsref.js")
	goCore, goErr := ts.open(dips)
	nb, nd := 8, 0
	if goCore != nil {
		nb, nd = len(goCore.buttons), len(goCore.dipList)
	}
	ins := script(seed, frames, nb, nd)
	dj := ""
	if dips != nil {
		b, _ := json.Marshal(dips)
		dj = string(b)
	}
	cmd := exec.Command(node, ref, emu, ts.writeDir(t), ts.zipName, writeScript(t, ins), strconv.Itoa(frames), "48000", dj)
	cmd.Stderr = os.Stderr
	stdout, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Wait()
	defer stdout.Close()
	j := jsReader{bufio.NewReaderSize(stdout, 1<<20)}
	res := &result{}
	if j.u32() != 0 {
		msg := j.str()
		if goErr == nil {
			t.Fatalf("the JS core refused the set (%s) but the Go core took it", msg)
		}
		if goErr.Error() != msg {
			t.Fatalf("messages differ:\n go: %s\n js: %s", goErr, msg)
		}
		res.refused = msg
		return res
	}
	if goErr != nil {
		t.Fatalf("the Go core refused the set: %v", goErr)
	}
	c := goCore
	if head, want := j.str(), fmt.Sprintf("%s %d %d", c.GameID(), c.Width(), c.Height()); head != want {
		t.Fatalf("game: go %q, js %q", want, head)
	}
	distinct := map[uint64]bool{}
	for i := 0; i < frames; i++ {
		in := ins[i]
		if in.action&1 != 0 {
			c.Reset()
		}
		if in.action&2 != 0 {
			if d := c.Dips(); int(in.arg>>8) < len(d) {
				dd := d[in.arg>>8]
				c.SetDip(dd.ID, dd.Options[int(in.arg&255)%len(dd.Options)].Value)
			}
		}
		c.SetInput(0, in.p0)
		c.SetInput(1, in.p1)
		c.RunFrame()
		if st := j.u32(); st != 0 {
			msg := ""
			if st == 1 {
				msg = j.str()
			}
			t.Fatalf("frame %d: JS stopped (%d %s)", i, st, msg)
		}
		w, h := int(j.u32()), int(j.u32())
		if w != c.Width() || h != c.Height() {
			t.Fatalf("frame %d: size %dx%d, JS %dx%d", i, c.Width(), c.Height(), w, h)
		}
		buf := make([]byte, w*h*4)
		io.ReadFull(j.r, buf)
		scr := c.Screen()
		px := make([]uint32, w*h)
		for k := range px {
			px[k] = binary.LittleEndian.Uint32(buf[k*4:])
		}
		hp := hashPixels(scr)
		distinct[hp] = true
		if hp != hashPixels(px) {
			n, first := 0, -1
			for k := range scr {
				if scr[k] != px[k] {
					if first < 0 {
						first = k
					}
					n++
				}
			}
			t.Fatalf("frame %d: %d pixels differ, first at x=%d y=%d: go %06x js %06x", i, n, first%w, first/w, scr[first], px[first])
		}
		n := int(j.u32())
		ab := make([]byte, n*4)
		io.ReadFull(j.r, ab)
		aud := c.TakeAudio()
		if len(aud) != n {
			t.Fatalf("frame %d: %d samples, JS %d", i, len(aud), n)
		}
		for k := range aud {
			d := math.Abs(float64(aud[k]) - float64(math.Float32frombits(binary.LittleEndian.Uint32(ab[k*4:]))))
			res.maxDiff = math.Max(res.maxDiff, d)
			res.energy += float64(aud[k]) * float64(aud[k])
		}
		if res.maxDiff >= 1e-4 {
			t.Fatalf("frame %d: sound differs by %g", i, res.maxDiff)
		}
		res.samples += n
	}
	if j.u32() != 2 {
		t.Fatal("no end record")
	}
	if n := j.u32(); n != 0xffffffff || c.SRAM() != nil {
		t.Fatalf("battery save: go %d bytes, js %d", len(c.SRAM()), n)
	}
	res.frames, res.distinct = frames, len(distinct)
	return res
}

func allTestSets() []*testSet {
	var sets []*testSet
	for nth := 0; nth < 3; nth++ {
		sets = append(sets, pacmanSet(nth, uint64(11+nth), false, map[int]string{0: "puckman.zip", 1: "Puck Man (Japan).zip", 2: "pacman (1).zip"}[nth]))
	}
	sets = append(sets, pacmanSet(2, 20, true, "pacman.zip"))
	sets = append(sets, galaxianSet(0, 31, false, "galaxian.zip"), galaxianSet(1, 32, false, "galmidw.zip"), galaxianSet(0, 33, true, "galaxian.zip"))
	sets = append(sets, invadersSet(41, false, "invaders.zip"), invadersSet(42, true, "Space Invaders.zip"))
	sets = append(sets, dkongSet(51, false, "dkong.zip"), dkongSet(52, true, "dkong.zip"))
	// zips made by other tools: other file orders, files in folders
	s := galaxianSet(0, 34, false, "galaxian-2.zip")
	s.shuffle(&rng{5})
	s.order[0], s.files["dir/"+s.order[0]] = "dir/"+s.order[0], s.files[s.order[0]]
	s.name += "-shuffled"
	sets = append(sets, s)
	return sets
}

// badSets are sets the loader must refuse, with the same message as the web page.
func badSets() []*testSet {
	var out []*testSet
	s := pacmanSet(2, 61, false, "pacman.zip")
	delete(s.files, "pacman.6f")
	s.order = without(s.order, "pacman.6f")
	s.name = "missing file"
	out = append(out, s)
	s = dkongSet(62, false, "dkong.zip")
	s.files["c-2k.bpr"] = make([]byte, 300)
	s.files["v_3pt.bin"] = make([]byte, 0x1000)
	s.name = "wrong sizes"
	out = append(out, s)
	s = invadersSet(63, false, "galaxian.zip")
	s.name = "named after another game"
	out = append(out, s)
	s = &testSet{name: "unknown", zipName: "mystery.zip", frames: 1}
	s.add("a.bin", make([]byte, 100))
	out = append(out, s)
	s = galaxianSet(1, 64, false, "galmidw.zip")
	delete(s.files, "galmidw.u")
	s.order = without(s.order, "galmidw.u")
	s.name = "clone missing a file"
	out = append(out, s)
	return out
}

func without(l []string, x string) []string {
	var o []string
	for _, v := range l {
		if v != x {
			o = append(o, v)
		}
	}
	return o
}

func TestMatchesJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not found")
	}
	// the framework test's own board isn't in the web version
	regMu.Lock()
	saved := boards
	boards = nil
	for _, b := range saved {
		if b != testDef {
			boards = append(boards, b)
		}
	}
	regMu.Unlock()
	defer func() { regMu.Lock(); boards = saved; regMu.Unlock() }()

	total, totalSamples := 0, 0
	for i, ts := range allTestSets() {
		ts := ts
		t.Run(ts.name, func(t *testing.T) {
			frames := min(ts.frames, 600)
			if testing.Short() {
				frames = 100
			}
			if s := os.Getenv("RP_ARCADE_FRAMES"); s != "" {
				frames, _ = strconv.Atoi(s)
			}
			var dips map[string]int
			if i%3 == 1 {
				dips = map[string]int{"lives": 3, "test": 1, "overlay": 0, "bogus": 5}
			}
			res := compareRun(t, node, ts, frames, uint64(i)*977+3, dips)
			if res.refused != "" {
				t.Fatalf("refused: %s", res.refused)
			}
			total += res.frames
			totalSamples += res.samples
			t.Logf("%s (%s): %d frames identical (%d different pictures), %d samples (rms %.3f), max sample diff %g",
				ts.name, ts.zipName, res.frames, res.distinct, res.samples, math.Sqrt(res.energy/float64(max(1, res.samples))), res.maxDiff)
		})
	}
	for _, ts := range badSets() {
		t.Run("refuse "+ts.name, func(t *testing.T) {
			res := compareRun(t, node, ts, 1, 1, nil)
			if res.refused == "" {
				t.Fatal("the set was taken")
			}
			t.Logf("same message: %s", res.refused)
		})
	}
	t.Logf("total: %d frames, %d samples compared", total, totalSamples)
}
