package snes

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

// TestCompareJS runs the web SNES core (web/emu/snes.js and its chips, in
// Node) and this port on the same ROMs with the same scripted inputs (random
// buttons on both controllers, Start pressed early, a reset half way, the
// computer's clock jumping now and then for the clock chips) and compares
// every frame's picture, every audio sample and the battery save at the end.
// Games with a battery save then run again from that save.
//
// It needs ROMs, which are not in the repository: set SNES_COMPARE_LIST to a
// file listing one ROM path per line, optionally followed by a frame count
// and fw=<dir> (a folder of chip programs named by chip: "DSP-3", "ST018").
func TestCompareJS(t *testing.T) {
	list := os.Getenv("SNES_COMPARE_LIST")
	if list == "" {
		t.Skip("SNES_COMPARE_LIST not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not installed")
	}
	for _, r := range readList(t, list) {
		r := r
		t.Run(filepath.Base(r.path), func(t *testing.T) {
			t.Parallel()
			compareROM(t, node, r)
		})
	}
}

type listEntry struct {
	path   string
	frames int
	fw     string
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
		e := listEntry{frames: 3000}
		for {
			k := strings.LastIndexByte(line, ' ')
			if k <= 0 {
				break
			}
			tok := line[k+1:]
			if n, err := strconv.Atoi(tok); err == nil {
				e.frames = n
			} else if strings.HasPrefix(tok, "fw=") {
				e.fw = tok[3:]
			} else {
				break
			}
			line = strings.TrimSpace(line[:k])
		}
		e.path = line
		out = append(out, e)
	}
	return out
}

// frameIn is one frame of a scripted run.
type frameIn struct {
	bits  [2]uint32
	reset bool
	wall  float64 // the computer's clock, ms
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
	var bits [2]uint32
	for f := range out {
		r := rnd()
		if f%8 == 0 {
			bits[0] = r & 0xeff // not Start
			if (r>>12)&15 == 0 {
				bits[0] |= 0x100 // Start now and then
			}
			if (r>>16)&3 == 0 {
				bits[0] &^= 0x30 // not left and right together, mostly
			}
		}
		if f%24 == 5 {
			bits[1] = rnd() & 0xfff
		}
		in := frameIn{bits: bits}
		if f < 120 { // let the game start, then press Start
			in.bits = [2]uint32{}
			if f >= 60 && f < 70 {
				in.bits[0] = 0x100
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
	b := make([]byte, 0, len(ins)*24)
	for _, in := range ins {
		b = binary.LittleEndian.AppendUint32(b, in.bits[0])
		b = binary.LittleEndian.AppendUint32(b, in.bits[1])
		fl := uint32(0)
		if in.reset {
			fl = 1
		}
		b = binary.LittleEndian.AppendUint32(b, fl)
		b = binary.LittleEndian.AppendUint32(b, 0)
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

func loadFirmware(t *testing.T, dir string) map[string][]byte {
	if dir == "" {
		return nil
	}
	ents, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	m := map[string][]byte{}
	for _, e := range ents {
		b, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		m[e.Name()] = b
	}
	return m
}

func compareROM(t *testing.T, node string, e listEntry) {
	data, err := os.ReadFile(e.path)
	if err != nil {
		t.Fatal(err)
	}
	ins := makeScript(seedOf(e.path), e.frames, wallStart)
	sram, ok := comparePass(t, node, e, data, ins, nil, "")
	if !ok || sram == nil {
		return
	}
	n := min(e.frames, 1500)
	ins = makeScript(seedOf(e.path)+1, n, ins[len(ins)-1].wall+90061e3)
	comparePass(t, node, e, data, ins, sram, " (from its battery save)")
}

func runNode(node string, e listEntry, scriptPath, out, sramPath string, extra ...string) ([]byte, error) {
	emu, _ := filepath.Abs("../../web/emu")
	runner, _ := filepath.Abs("testdata/snesrun.js")
	fw := "-"
	if e.fw != "" {
		fw = e.fw
	}
	args := append([]string{"--max-old-space-size=2048", runner, emu, e.path, scriptPath, out, sramPath, fw}, extra...)
	return exec.Command(node, args...).CombinedOutput()
}

// comparePass runs both cores and compares them; it returns the battery save.
func comparePass(t *testing.T, node string, e listEntry, data []byte, ins []frameIn, sram []byte, what string) ([]byte, bool) {
	dir := t.TempDir()
	scriptPath, out, sramPath := filepath.Join(dir, "script.bin"), filepath.Join(dir, "out.bin"), "-"
	if err := writeScript(scriptPath, ins); err != nil {
		t.Fatal(err)
	}
	if sram != nil {
		sramPath = filepath.Join(dir, "sram.bin")
		os.WriteFile(sramPath, sram, 0o644)
	}
	if b, err := runNode(node, e, scriptPath, out, sramPath); err != nil {
		t.Fatalf("node: %v\n%s", err, b)
	}
	js, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	wall := ins[0].wall
	g, err := newSNES(data, core.Options{SampleRate: 48000, SRAM: sram, Firmware: loadFirmware(t, e.fw)}, func() float64 { return wall })
	if strings.HasPrefix(string(js), "ERR ") {
		msg := string(js[4:])
		if err == nil {
			t.Fatalf("JS refused the ROM (%s) but Go opened it", msg)
		}
		if fe, ok := err.(*FirmwareError); ok {
			if want := "FW " + fe.Chip + " " + fe.Msg; want != msg {
				t.Fatalf("error differs: js %q go %q", msg, want)
			}
		} else if err.Error() != msg {
			t.Fatalf("error differs: js %q go %q", msg, err)
		}
		t.Logf("both refuse: %v", err)
		return nil, false
	}
	if err != nil {
		t.Fatalf("Go refused the ROM: %v", err)
	}
	pos := 0
	maxDiff, samples, hires := 0.0, 0, 0
	for f, in := range ins {
		wall = in.wall
		if in.reset {
			g.Reset()
		}
		g.SetInput(0, in.bits[0])
		g.SetInput(1, in.bits[1])
		g.RunFrame()
		a := g.TakeAudio()
		wantHash := binary.LittleEndian.Uint32(js[pos:])
		cnt := int(binary.LittleEndian.Uint32(js[pos+4:]))
		w, h := int(binary.LittleEndian.Uint32(js[pos+8:])), int(binary.LittleEndian.Uint32(js[pos+12:]))
		pos += 16
		if w != g.Width() || h != g.Height() {
			t.Fatalf("frame %d%s: picture %dx%d, JS %dx%d", f, what, g.Width(), g.Height(), w, h)
		}
		if w == 512 {
			hires++
		}
		if got := screenHash(g.Screen()); got != wantHash {
			dump := filepath.Join(dir, "frame.bin")
			scr := filepath.Join(dir, "script2.bin")
			writeScript(scr, ins[:f+1])
			runNode(node, e, scr, out+".x", sramPath, strconv.Itoa(f), dump)
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
			t.Fatalf("frame %d%s: picture differs (%d pixels, first at x=%d y=%d)", f, what, diff, first%w, first/w)
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
		Chip     string `json:"chip"`
		Coproc   string `json:"coproc"`
		Map      string `json:"map"`
		PAL      bool   `json:"pal"`
		Title    string `json:"title"`
		SRAMSize int    `json:"sramSize"`
	}
	if err := json.Unmarshal(js[pos+4:pos+4+il], &info); err != nil {
		t.Fatal(err)
	}
	maps := []string{"lo", "hi", "exhi"}
	if info.Map != maps[g.cart.mapping] || info.PAL != g.pal || info.Title != g.cart.title || info.SRAMSize != g.cart.sramSize || info.Chip != g.chipName {
		t.Fatalf("cartridge info differs: js %+v, go %s %v %q %d %q", info, maps[g.cart.mapping], g.pal, g.cart.title, g.cart.sramSize, g.chipName)
	}
	chip := info.Chip
	if chip == "" {
		chip = info.Coproc
	}
	if chip == "" {
		chip = "no chip"
	}
	t.Logf("%d frames identical%s (%d hires), %d samples (max diff %.3g), %s %s, battery save %d bytes",
		len(ins), what, hires, samples, maxDiff, info.Map, chip, len(jsSram))
	return append([]byte(nil), jsSram...), true
}

var _ = fmt.Sprint
