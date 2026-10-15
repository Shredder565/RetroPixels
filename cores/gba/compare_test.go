package gba

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
	"time"

	"retropixels/core"
)

// TestCompareJS runs the web Game Boy Advance core (web/emu/arm7.js,
// gba_gpio.js and gba.js in Node) and this port on the same ROMs with the
// same scripted inputs (random buttons and sensor keys, the right stick, Start
// pressed early, a reset half way, the computer's clock jumping now and then)
// and compares every frame's picture, every audio sample, the rumble motor and
// the battery save at the end. Three quarters of the way through, the Go side
// carries on in a new machine loaded from a save state. Games with a battery
// save then run again from that save, a day later.
//
// It needs ROMs, which are not in the repository: set GBA_COMPARE_LIST to a
// file listing one ROM path per line (optionally followed by a frame count).
func TestCompareJS(t *testing.T) {
	list := os.Getenv("GBA_COMPARE_LIST")
	if list == "" {
		t.Skip("GBA_COMPARE_LIST not set")
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

// TestCompareTestROMsJS does the same with the test ROMs built from
// testdata/roms (when node and the LLVM tools are installed), for
// GBA_TESTROM_FRAMES frames each (300 if not set).
func TestCompareTestROMsJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	frames := 300
	if n, err := strconv.Atoi(os.Getenv("GBA_TESTROM_FRAMES")); err == nil && n > 0 {
		frames = n
	}
	roms := testROMs(t)
	for _, s := range romSpecs {
		path := roms[s.name]
		t.Run(s.name, func(t *testing.T) {
			t.Parallel()
			compareROM(t, node, path, frames)
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
	sx, sy float64 // right stick (sensors)
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
			bits = r & 0x3f7
			if (r>>10)&15 == 0 {
				bits |= 8 // Start now and then
			}
			if (r>>14)&3 == 0 {
				bits |= ((r >> 16) & 15) << 10 // sensor keys
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
			wall += 4000
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

var saveNames = map[int]string{saveNone: "none", saveEEPROM: "eeprom", saveFlash128: "flash128", saveFlash64: "flash64", saveSRAM: "sram"}

// comparePass runs both cores and compares them; it returns the battery save.
func comparePass(t *testing.T, node, rom string, data []byte, ins []frameIn, sram []byte, what string) ([]byte, bool) {
	emu, _ := filepath.Abs("../../web/emu")
	runner, _ := filepath.Abs("testdata/gbarun.js")
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
	cmd.Env = append(os.Environ(), "TZ=UTC")
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("node: %v\n%s", err, b)
	}
	js, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	wall := ins[0].wall
	clock := func() time.Time { return time.UnixMilli(int64(wall)).UTC() }
	opts := core.Options{SampleRate: 48000, SRAM: sram}
	g, err := newGBA(data, opts, clock)
	if strings.HasPrefix(string(js), "ERR ") {
		t.Fatalf("JS refused the ROM (%s)", js[4:])
	}
	if err != nil {
		t.Fatalf("Go refused the ROM: %v", err)
	}
	pos := 0
	maxDiff, samples, rumbled, frames, peak := 0.0, 0, 0, 0, 0.0
	swapAt := len(ins) * 3 / 4
	for f, in := range ins {
		if binary.LittleEndian.Uint32(js[pos:]) == 0xffffffff && binary.LittleEndian.Uint32(js[pos+4:]) == 0xffffffff {
			break // the JS core stopped (its error is in the info below)
		}
		wall = in.wall
		if f == swapAt {
			st, err := g.SaveState()
			if err != nil {
				t.Fatal(err)
			}
			g2, _ := newGBA(data, opts, clock)
			if err := g2.LoadState(st); err != nil {
				t.Fatalf("frame %d: loading the save state: %v", f, err)
			}
			g = g2
		}
		if in.reset {
			g.Reset()
		}
		g.SetInput(0, in.bits)
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
			c := exec.Command(node, runner, emu, rom, scr, out+".x", sramPath, strconv.Itoa(f), dump)
			c.Env = append(os.Environ(), "TZ=UTC")
			c.Run()
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
			t.Fatalf("frame %d%s: picture differs (%d pixels, first at x=%d y=%d)", f, what, diff, first%240, first/240)
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
			peak = max(peak, math.Abs(float64(w)))
			if d >= 1e-4 {
				t.Fatalf("frame %d%s sample %d: js %v go %v", f, what, i, w, a[i])
			}
		}
		if math.IsNaN(rum) {
			if g.HasRumble() {
				t.Fatalf("Go has a rumble motor, JS hasn't")
			}
		} else if got := g.Rumble(); float32(rum) != got || !g.HasRumble() {
			t.Fatalf("frame %d%s: rumble %v, JS %v", f, what, got, rum)
		}
		if rum != 0 && !math.IsNaN(rum) {
			rumbled++
		}
		samples += cnt
		pos += 4 * cnt
		frames++
	}
	pos += 16
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
		Save    string   `json:"save"`
		RTC     bool     `json:"rtc"`
		Kind    *string  `json:"kind"`
		Buttons []string `json:"buttons"`
		Stick   bool     `json:"stick"`
		Note    string   `json:"note"`
		Error   string   `json:"error"`
	}
	if err := json.Unmarshal(js[pos+4:pos+4+il], &info); err != nil {
		t.Fatal(err)
	}
	kind := ""
	if info.Kind != nil {
		kind = *info.Kind
	}
	goKind, goRTC := "", false
	if g.gpio != nil {
		goKind, goRTC = g.gpio.kind, g.gpio.rtc != nil
	}
	if info.Save != saveNames[g.save.typ] || info.RTC != goRTC || kind != goKind || fmt.Sprint(info.Buttons) != fmt.Sprint(g.Info().Buttons) ||
		info.Stick != g.UsesRightStick() || info.Note != g.ControlsNote() {
		t.Fatalf("info differs: js %+v, go %s %v %q %v %v", info, saveNames[g.save.typ], goRTC, goKind, g.Info().Buttons, g.UsesRightStick())
	}
	if info.Error != "" {
		t.Logf("the JS core stopped at %s; the %d frames before it are identical", info.Error, frames)
		return nil, false
	}
	t.Logf("%d frames identical%s, %d samples (peak %.3f, max diff %.3g), save %s, gpio %q, battery save %d bytes, rumble on %d frames",
		frames, what, samples, peak, maxDiff, info.Save, kind, len(jsSram), rumbled)
	return append([]byte(nil), jsSram...), true
}
