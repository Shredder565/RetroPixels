package gb

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"retropixels/core"
)

// TestCompareJS runs the web Game Boy core (web/emu/gb.js and gb_carts.js in
// Node) and this port on the same ROMs with the same scripted inputs (random
// buttons and tilt, Start pressed early, a reset half way, the computer's
// clock jumping now and then) and compares every frame's picture, every audio
// sample, the rumble motor and the battery save at the end. Games with a
// battery save then run again from that save, a day later.
//
// It needs ROMs, which are not in the repository: set GB_COMPARE_LIST to a
// file listing one ROM path per line (optionally followed by a frame count).
func TestCompareJS(t *testing.T) {
	list := os.Getenv("GB_COMPARE_LIST")
	if list == "" {
		t.Skip("GB_COMPARE_LIST not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	for _, r := range readList(t, list) {
		r := r
		t.Run(filepath.Base(r.path), func(t *testing.T) {
			t.Parallel()
			compareROM(t, node, r.path, r.frames)
		})
	}
}

type listEntry struct {
	path   string
	frames int
}

func readList(t *testing.T, list string) []listEntry {
	f, err := os.Open(list)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []listEntry
	sc := bufio.NewScanner(f)
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
		out = append(out, listEntry{line, frames})
	}
	return out
}

// frameIn is one frame of a scripted run.
type frameIn struct {
	bits   uint32
	reset  bool
	sx, sy float64 // right stick (tilt)
	wall   float64 // the computer's clock, ms
}

const wallStart = 1.7e12

// makeScript: deterministic, game-like input.
func makeScript(seed uint64, frames int, wall float64) []frameIn {
	s := seed | 1
	rnd := func() uint32 {
		s ^= s << 13
		s ^= s >> 7
		s ^= s << 17
		return uint32(s >> 16)
	}
	out := make([]frameIn, frames)
	var bits uint32
	var sx, sy float64
	for f := range out {
		r := rnd()
		if f%8 == 0 {
			bits = r & 0xf7
			if (r>>8)&15 == 0 {
				bits |= 8 // Start now and then
			}
			if (r>>12)&3 == 0 {
				bits |= ((r >> 16) & 15) << 8 // tilt keys
			}
		}
		if f%30 == 0 {
			sx = float64(int(rnd()%129)-64) / 64
			sy = float64(int(rnd()%129)-64) / 64
			if rnd()%3 == 0 {
				sx, sy = 0, 0
			}
		}
		in := frameIn{bits: bits, sx: sx, sy: sy}
		if f < 120 { // let the game start, then press Start
			in.bits = 0
			if f >= 60 && f < 70 {
				in.bits = 8
			}
		}
		in.reset = f == frames/2 && frames >= 200
		wall += 17
		switch {
		case f%1000 == 500:
			wall += 4000 // a pause: the cartridge clocks catch up
		case f%2500 == 1234:
			wall += 3600e3 + 61e3
		}
		in.wall = wall
		out[f] = in
	}
	return out
}

func writeScript(path string, ins []frameIn) error {
	b := make([]byte, 0, len(ins)*32)
	for _, in := range ins {
		b = binary.LittleEndian.AppendUint32(b, in.bits)
		fl := uint32(0)
		if in.reset {
			fl = 1
		}
		b = binary.LittleEndian.AppendUint32(b, fl)
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(in.sx))
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(in.sy))
		b = binary.LittleEndian.AppendUint64(b, math.Float64bits(in.wall))
	}
	return os.WriteFile(path, b, 0o644)
}

func screenHash(s []uint32) uint32 {
	h := uint32(2166136261)
	for _, c := range s {
		h = (h ^ c) * 16777619
	}
	return h
}

func seedOf(name string) uint64 {
	h := fnv.New64a()
	h.Write([]byte(filepath.Base(name)))
	return h.Sum64()
}

func compareROM(t *testing.T, node, rom string, frames int) {
	data, err := os.ReadFile(rom)
	if err != nil {
		t.Fatal(err)
	}
	ins := makeScript(seedOf(rom), frames, wallStart)
	sram, ok := comparePass(t, node, rom, data, ins, nil, "")
	if !ok || sram == nil {
		return
	}
	// again from the battery save, a day (and an hour, a minute, a second) later
	n := min(frames, 1500)
	ins = makeScript(seedOf(rom)+1, n, ins[len(ins)-1].wall+90061e3)
	comparePass(t, node, rom, data, ins, sram, " (from its battery save)")
}

// comparePass runs both cores and compares them; it returns the battery save.
func comparePass(t *testing.T, node, rom string, data []byte, ins []frameIn, sram []byte, what string) ([]byte, bool) {
	emu, _ := filepath.Abs("../../web/emu")
	runner, _ := filepath.Abs("testdata/gbrun.js")
	dir := t.TempDir()
	scriptPath, out, sramPath := filepath.Join(dir, "script.bin"), filepath.Join(dir, "out.bin"), "-"
	if err := writeScript(scriptPath, ins); err != nil {
		t.Fatal(err)
	}
	if sram != nil {
		sramPath = filepath.Join(dir, "sram.bin")
		os.WriteFile(sramPath, sram, 0o644)
	}
	cmd := exec.Command(node, "--max-old-space-size=2048", runner, emu, rom, scriptPath, out, sramPath)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("node: %v\n%s", err, b)
	}
	js, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	wall := ins[0].wall
	g, err := newGB(data, core.Options{SampleRate: 48000, SRAM: sram}, func() float64 { return wall })
	if strings.HasPrefix(string(js), "ERR ") {
		if err == nil {
			t.Fatalf("JS refused the ROM (%s) but Go opened it", js[4:])
		}
		if err.Error() != string(js[4:]) {
			t.Fatalf("error differs: js %q go %q", js[4:], err)
		}
		t.Logf("both refuse: %v", err)
		return nil, false
	}
	if err != nil {
		t.Fatalf("Go refused the ROM: %v", err)
	}
	pos := 0
	maxDiff, samples, rumbled := 0.0, 0, 0
	for f, in := range ins {
		wall = in.wall
		if in.reset {
			g.Reset()
		}
		g.SetInput(0, in.bits)
		g.SetAnalog(0, 0, 0)
		g.SetAnalog(0, 1, 0)
		g.SetAnalog(0, 2, float32(in.sx))
		g.SetAnalog(0, 3, float32(in.sy))
		g.RunFrame()
		a := g.TakeAudio()
		wantHash := binary.LittleEndian.Uint32(js[pos:])
		cnt := int(binary.LittleEndian.Uint32(js[pos+4:]))
		rum := math.Float64frombits(binary.LittleEndian.Uint64(js[pos+8:]))
		pos += 16
		if got := screenHash(g.Screen()); got != wantHash {
			dump := filepath.Join(dir, "frame.bin")
			scr := filepath.Join(dir, "script2.bin")
			writeScript(scr, ins[:f+1])
			exec.Command(node, runner, emu, rom, scr, out+".x", sramPath, strconv.Itoa(f), dump).Run()
			jsPix, _ := os.ReadFile(dump)
			diff, first := 0, -1
			for i, c := range g.Screen() {
				if 4*i+4 <= len(jsPix) && binary.LittleEndian.Uint32(jsPix[4*i:]) != c {
					if first < 0 {
						first = i
					}
					diff++
				}
			}
			t.Fatalf("frame %d%s: picture differs (%d pixels, first at x=%d y=%d)", f, what, diff, first%160, first/160)
		}
		if cnt != len(a) {
			t.Fatalf("frame %d%s: %d audio samples, JS made %d", f, what, len(a), cnt)
		}
		for i := 0; i < cnt; i++ {
			w := math.Float32frombits(binary.LittleEndian.Uint32(js[pos+4*i:]))
			d := math.Abs(float64(w) - float64(a[i]))
			if d > maxDiff {
				maxDiff = d
			}
			if d >= 1e-4 {
				t.Fatalf("frame %d%s sample %d: js %v go %v", f, what, i, w, a[i])
			}
		}
		if got := g.Rumble(); float32(rum) != got {
			t.Fatalf("frame %d%s: rumble %v, JS %v", f, what, got, rum)
		}
		if rum > 0 {
			rumbled++
		}
		samples += cnt
		pos += 4 * cnt
	}
	sl := binary.LittleEndian.Uint32(js[pos:])
	pos += 4
	got := g.SRAM()
	var jsSram []byte
	if sl == 0xffffffff {
		if got != nil {
			t.Fatalf("Go has a battery save, JS has none")
		}
	} else {
		jsSram = js[pos : pos+int(sl)]
		pos += int(sl)
		if string(got) != string(jsSram) {
			first := -1
			for i := range got {
				if i < len(jsSram) && got[i] != jsSram[i] {
					first = i
					break
				}
			}
			t.Fatalf("battery save differs%s (go %d bytes, js %d; first difference at %d)", what, len(got), len(jsSram), first)
		}
	}
	il := int(binary.LittleEndian.Uint32(js[pos:]))
	var info struct {
		Kind    string   `json:"kind"`
		CGB     bool     `json:"cgb"`
		Buttons []string `json:"buttons"`
	}
	if err := json.Unmarshal(js[pos+4:pos+4+il], &info); err != nil {
		t.Fatal(err)
	}
	if info.Kind != g.cb.kind || info.CGB != g.cgb || fmt.Sprint(info.Buttons) != fmt.Sprint(g.Info().Buttons) {
		t.Fatalf("info differs: js %+v, go %s %v %v", info, g.cb.kind, g.cgb, g.Info().Buttons)
	}
	t.Logf("%d frames identical%s, %d samples (max diff %.3g), %s%s, battery save %d bytes, rumble on %d frames",
		len(ins), what, samples, maxDiff, info.Kind, map[bool]string{true: " (Color)", false: ""}[info.CGB], len(jsSram), rumbled)
	return append([]byte(nil), jsSram...), true
}
