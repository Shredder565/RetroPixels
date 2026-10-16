package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"

	"retropixels/core"
	"retropixels/internal/games"
)

// The engine is built for this computer and driven over its standard input
// and output, exactly as the Android app drives it.

var (
	buildOnce  sync.Once
	enginePath string
	buildErr   error
)

func engineBinary(t *testing.T) string {
	t.Helper()
	buildOnce.Do(func() {
		dir, err := os.MkdirTemp("", "rpengine-test-")
		if err != nil {
			buildErr = err
			return
		}
		enginePath = filepath.Join(dir, "rpengine")
		if runtime.GOOS == "windows" {
			enginePath += ".exe"
		}
		cmd := exec.Command("go", "build", "-o", enginePath, ".")
		cmd.Env = append(os.Environ(), "CGO_ENABLED=0")
		if out, err := cmd.CombinedOutput(); err != nil {
			buildErr = &buildError{err, string(out)}
		}
	})
	if buildErr != nil {
		t.Fatal(buildErr)
	}
	return enginePath
}

type buildError struct {
	err error
	out string
}

func (e *buildError) Error() string { return e.err.Error() + "\n" + e.out }

func TestMain(m *testing.M) {
	code := m.Run()
	if enginePath != "" {
		os.RemoveAll(filepath.Dir(enginePath))
	}
	os.Exit(code)
}

type client struct {
	t   *testing.T
	cmd *exec.Cmd
	in  *bufio.Writer
	out *bufio.Reader
	dir string
}

func startEngine(t *testing.T) *client {
	t.Helper()
	dir := t.TempDir()
	cmd := exec.Command(engineBinary(t), "-dir", dir)
	cmd.Env = append(os.Environ(), "TMPDIR="+t.TempDir())
	cmd.Stderr = os.Stderr
	stdin, _ := cmd.StdinPipe()
	stdout, _ := cmd.StdoutPipe()
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	c := &client{t: t, cmd: cmd, in: bufio.NewWriter(stdin), out: bufio.NewReader(stdout), dir: dir}
	t.Cleanup(func() {
		stdin.Close()
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			cmd.Process.Kill()
			t.Error("the engine didn't stop when its input closed")
		}
	})
	hello := c.readJSON()
	if hello["hello"] != "RetroPixels engine" || hello["protocol"] != float64(protocolVersion) {
		t.Fatalf("hello: %v", hello)
	}
	return c
}

func (c *client) readJSON() map[string]any {
	c.t.Helper()
	typ, body, err := readMessage(c.out, nil)
	if err != nil || typ != msgJSON {
		c.t.Fatalf("reply: %q %v", typ, err)
	}
	var m map[string]any
	if err := json.Unmarshal(body, &m); err != nil {
		c.t.Fatal(err)
	}
	return m
}

func (c *client) call(req map[string]any) map[string]any {
	c.t.Helper()
	b, _ := json.Marshal(req)
	if err := writeMessage(c.in, msgJSON, b); err != nil {
		c.t.Fatal(err)
	}
	return c.readJSON()
}

func (c *client) must(req map[string]any) map[string]any {
	c.t.Helper()
	r := c.call(req)
	if r["ok"] != true {
		c.t.Fatalf("%v: %v", req["cmd"], r["error"])
	}
	return r
}

type frame struct {
	failed  bool
	w, h    int
	rumble  float32
	message string
	pcm     []int16
	rgba    []byte
}

func (c *client) run(q runRequest) frame {
	c.t.Helper()
	if q.keys == nil {
		q.keys = map[string]bool{}
	}
	if err := writeMessage(c.in, msgRun, encodeRun(q)); err != nil {
		c.t.Fatal(err)
	}
	typ, body, err := readMessage(c.out, nil)
	if err != nil || typ != msgFrame {
		c.t.Fatalf("frame: %q %v", typ, err)
	}
	r := &reader{b: body}
	var f frame
	f.failed = r.u8() == 1
	f.w, f.h = int(r.u16()), int(r.u16())
	f.rumble = math.Float32frombits(r.u32())
	flags := r.u8()
	f.message = string(r.take(int(r.u16())))
	n := int(r.u32())
	for i := 0; i < n; i++ {
		f.pcm = append(f.pcm, int16(r.u16()))
	}
	if flags&1 != 0 {
		f.rgba = r.take(f.w * f.h * 4)
	}
	if r.err != nil || len(r.b) != 0 {
		c.t.Fatalf("frame body: %v, %d bytes left", r.err, len(r.b))
	}
	return f
}

func writeFile(t *testing.T, dir, name string, data []byte) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, data, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

// smsLoop: a Master System program that turns the display on with a
// coloured backdrop and loops (no ROM from anyone else needed).
func smsLoop() []byte {
	rom := make([]byte, 0x8000)
	prog := []byte{
		0xf3,                                           // di
		0x3e, 0x04, 0xd3, 0xbf, 0x3e, 0x80, 0xd3, 0xbf, // VDP reg 0 = $04: mode 4
		0x3e, 0x60, 0xd3, 0xbf, 0x3e, 0x81, 0xd3, 0xbf, // VDP reg 1 = $60: display on
		0x3e, 0x00, 0xd3, 0xbf, 0x3e, 0xc0, 0xd3, 0xbf, // CRAM address 0
		0x3e, 0x2a, 0xd3, 0xbe, // colour 0 = $2a
		0x3e, 0x30, 0xd3, 0x7f, // PSG: something to hear
		0x18, 0xfe, // jr $
	}
	copy(rom, prog)
	copy(rom[0x7ff0:], "TMR SEGA")
	return rom
}

func sha(b []byte) string { h := sha1.Sum(b); return hex.EncodeToString(h[:]) }

func pcmBytes(p []int16) []byte {
	var out []byte
	for _, s := range p {
		out = binary.LittleEndian.AppendUint16(out, uint16(s))
	}
	return out
}

func TestEngineProtocol(t *testing.T) {
	c := startEngine(t)
	sys := c.must(map[string]any{"cmd": "systems"})
	if n := len(sys["systems"].([]any)); n != 10 {
		t.Fatalf("%d systems: %v", n, sys["systems"])
	}
	if !strings.Contains(sys["labels"].(string), "Nintendo 64") {
		t.Fatal(sys["labels"])
	}
	ctl := c.must(map[string]any{"cmd": "controls", "system": "snes", "saved": map[string]any{"A": map[string]any{"pad": []int{0}, "keys": []string{"k"}}}})
	if m := ctl["map"].(map[string]any)["A"].(map[string]any); m["keys"].([]any)[0] != "k" {
		t.Fatalf("controls: %v", ctl["map"])
	}
	if d := ctl["defaults"].(map[string]any)["A"].(map[string]any); d["keys"].([]any)[0] != "x" {
		t.Fatalf("defaults: %v", ctl["defaults"])
	}

	dir := t.TempDir()
	rom := writeFile(t, dir, "Loop (Homebrew).sms", smsLoop())
	g := c.must(map[string]any{"cmd": "open", "path": rom, "sampleRate": 44100})
	if g["system"] != "sms" || g["name"] != "Loop (Homebrew)" || g["aspect"] != 4.0/3 || g["fps"].(float64) < 59.9 || g["sampleRate"] != 44100.0 {
		t.Fatalf("open: %v", g)
	}
	if b := g["buttons"].([]any); len(b) != 8 || b[4] != "1" {
		t.Fatalf("buttons %v", b)
	}

	// frames and sound match the core run on its own
	ref, err := runReference(rom, 90, 44100)
	if err != nil {
		t.Fatal(err)
	}
	var pcm []int16
	var last frame
	for i := 0; i < 90; i++ {
		last = c.run(runRequest{frames: 1, volume: 1})
		if last.failed || last.rgba == nil {
			t.Fatalf("frame %d: %+v", i, last.message)
		}
		pcm = append(pcm, last.pcm...)
	}
	if last.w != ref.Width || last.h != ref.Height || sha(last.rgba) != ref.Pixels {
		t.Fatalf("picture %dx%d %s, core %dx%d %s", last.w, last.h, sha(last.rgba), ref.Width, ref.Height, ref.Pixels)
	}
	if len(pcm) != ref.Samples || sha(pcmBytes(pcm)) != ref.Audio {
		t.Fatalf("sound: %d samples, core %d", len(pcm), ref.Samples)
	}
	if len(pcm) < 90*44100/60*2*95/100 {
		t.Fatalf("only %d samples for 90 frames", len(pcm))
	}
	if last.rgba[3] != 255 || last.rgba[0]|last.rgba[1]|last.rgba[2] == 0 {
		t.Fatalf("backdrop %v", last.rgba[:4])
	}

	// several frames in one go (fast-forward: the sound of the first only)
	f := c.run(runRequest{frames: 4, fast: true, volume: 1})
	if f.failed || len(f.pcm) == 0 || len(f.pcm) > 44100/60*2+8 {
		t.Fatalf("fast: %d samples", len(f.pcm))
	}
	// no frames: input only, no picture
	if f := c.run(runRequest{}); f.rgba != nil || len(f.pcm) != 0 {
		t.Fatal("zero frames made something")
	}
	// volume
	half := c.run(runRequest{frames: 1, volume: 0})
	for _, s := range half.pcm {
		if s != 0 {
			t.Fatal("volume 0 isn't silent")
		}
	}

	// save states: four slots, load, undo
	if r := c.call(map[string]any{"cmd": "load", "slot": 2}); r["ok"] == true || !strings.Contains(r["error"].(string), "empty") {
		t.Fatalf("empty slot: %v", r)
	}
	r := c.must(map[string]any{"cmd": "save", "slot": 2})
	slots := r["slots"].([]any)
	if s := slots[2].(map[string]any); s["used"] != true || s["thumb"] == "" {
		t.Fatalf("slots %v", slots)
	}
	if _, err := os.Stat(filepath.Join(c.dir, "saves", "sms", "Loop (Homebrew).slot3.state")); err != nil {
		t.Fatal(err)
	}
	c.run(runRequest{frames: 10, volume: 1})
	if r := c.must(map[string]any{"cmd": "load", "slot": 2}); r["canUndo"] != true {
		t.Fatal(r)
	}
	c.must(map[string]any{"cmd": "undo"})
	if r := c.call(map[string]any{"cmd": "undo"}); r["ok"] == true {
		t.Fatal("undid twice")
	}
	c.must(map[string]any{"cmd": "deleteSlot", "slot": 2})
	c.must(map[string]any{"cmd": "reset"})
	c.must(map[string]any{"cmd": "flush"})
	if r := c.call(map[string]any{"cmd": "exportSRAM", "path": filepath.Join(dir, "x.sav")}); r["ok"] == true {
		t.Fatal("exported a battery save the game hasn't got")
	}
	if r := c.call(map[string]any{"cmd": "nonsense"}); r["ok"] == true {
		t.Fatal("unknown command")
	}
	c.must(map[string]any{"cmd": "close"})
	if f := c.run(runRequest{frames: 1}); !f.failed {
		t.Fatal("ran with no game")
	}

	// files that aren't games
	for name, want := range map[string]string{"zelda.iso": "Dolphin has its own Android app", "notes.txt": "couldn't tell which system",
		"Empty.zip": "isn't an arcade game RetroPixels knows yet"} {
		data := []byte{1}
		if strings.HasSuffix(name, ".zip") {
			data = zipOf(t, map[string][]byte{"readme.txt": {1}})
		}
		r := c.call(map[string]any{"cmd": "open", "path": writeFile(t, dir, name, data)})
		if r["ok"] == true || !strings.Contains(r["error"].(string), want) {
			t.Errorf("%s: %v", name, r["error"])
		}
	}
	if r := c.call(map[string]any{"cmd": "open", "path": filepath.Join(dir, "missing.nes")}); r["ok"] == true {
		t.Error("opened a missing file")
	}
	// a zipped game
	r = c.must(map[string]any{"cmd": "open", "path": writeFile(t, dir, "Zipped.zip", zipOf(t, map[string][]byte{"a.txt": {1}, "Loop.sms": smsLoop()}))})
	if r["system"] != "sms" || r["name"] != "Zipped" {
		t.Fatal(r)
	}
	c.must(map[string]any{"cmd": "quit"})
}

func zipOf(t *testing.T, files map[string][]byte) []byte {
	var buf bytes.Buffer
	w := zip.NewWriter(&buf)
	for n, d := range files {
		f, err := w.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		f.Write(d)
	}
	w.Close()
	return buf.Bytes()
}

// snesChipROM: a do-nothing 32 KB LoROM game whose header names a DSP-3
// cartridge (a chip whose program the player supplies).
func snesChipROM() []byte {
	r := make([]byte, 0x8000)
	r[0], r[1] = 0x80, 0xfe // BRA -2
	h := 0x7fc0
	copy(r[h:], []byte("SD RP GX TEST        "))
	r[h+0x15], r[h+0x16], r[h+0x17], r[h+0x19] = 0x20, 0x03, 0x05, 1
	r[0x7ffc], r[0x7ffd] = 0x00, 0x80
	r[h+0x1c], r[h+0x1d] = 0xff, 0xff
	return r
}

func TestEngineFirmware(t *testing.T) {
	c := startEngine(t)
	dir := t.TempDir()
	rom := writeFile(t, dir, "chip game.sfc", snesChipROM())
	r := c.call(map[string]any{"cmd": "open", "path": rom})
	if r["ok"] == true || r["need"] != "firmware" || r["chip"] != "DSP-3" || r["file"] != "dsp3.rom" || !strings.Contains(r["error"].(string), "Choose the file") {
		t.Fatalf("want a DSP-3 question: %v", r)
	}
	bad := writeFile(t, dir, "small.rom", make([]byte, 100))
	r = c.call(map[string]any{"cmd": "open", "path": rom, "firmware": map[string]string{"DSP-3": bad}})
	if r["need"] != "firmware" || !strings.Contains(r["error"].(string), "100 bytes") {
		t.Fatalf("wrong size: %v", r)
	}
	// my own stand-in program, the right size (2048 x 24-bit words, 1024 x 16-bit)
	fw := make([]byte, 2048*3+1024*2)
	for i := range fw {
		fw[i] = byte(i * 7)
	}
	c.must(map[string]any{"cmd": "open", "path": rom, "firmware": map[string]string{"DSP-3": writeFile(t, dir, "mine.rom", fw)}})
	if b, _ := os.ReadFile(filepath.Join(c.dir, "firmware", "DSP-3")); !bytes.Equal(b, fw) {
		t.Fatal("chip program not kept")
	}
	if f := c.run(runRequest{frames: 2, volume: 1}); f.failed {
		t.Fatal(f.message)
	}
	// kept: asked no more
	c.must(map[string]any{"cmd": "open", "path": rom})
}

// invadersZip: a Space Invaders-shaped set whose program fills the screen.
func invadersZip(t *testing.T) []byte {
	prog := make([]byte, 0x2000)
	copy(prog, []byte{0x21, 0x00, 0x24, 0x36, 0x55, 0x23, 0x7c, 0xfe, 0x40, 0xc2, 0x03, 0x00, 0x76})
	files := map[string][]byte{}
	for i, n := range []string{"invaders.h", "invaders.g", "invaders.f", "invaders.e"} {
		files[n] = prog[i*0x800 : (i+1)*0x800]
	}
	return zipOf(t, files)
}

func TestEngineArcade(t *testing.T) {
	c := startEngine(t)
	dir := t.TempDir()
	set := writeFile(t, dir, "Space Invaders (1978).zip", invadersZip(t))
	r := c.must(map[string]any{"cmd": "open", "path": set, "dips": map[string]int{"overlay": 0}})
	if r["system"] != "arcade" || r["width"] != 224.0 || r["height"] != 256.0 || r["aspect"] != 0.75 {
		t.Fatalf("open: %v", r)
	}
	dips := r["dips"].([]any)
	if len(dips) == 0 {
		t.Fatal("no machine settings")
	}
	d := dips[0].(map[string]any)
	opts := d["options"].([]any)
	last := opts[len(opts)-1].(map[string]any)["value"]
	r = c.must(map[string]any{"cmd": "dip", "id": d["id"], "value": last})
	if r["dips"].([]any)[0].(map[string]any)["value"] != last {
		t.Fatal("setting not changed")
	}
	if r := c.call(map[string]any{"cmd": "dip", "id": "no such", "value": 1}); r["ok"] == true {
		t.Fatal("set a setting the game hasn't got")
	}
	var f frame
	for i := 0; i < 12; i++ {
		f = c.run(runRequest{frames: 1, volume: 1})
	}
	lit := 0
	for i := 0; i < len(f.rgba); i += 4 {
		if f.rgba[i] == 255 && f.rgba[i+1] == 255 && f.rgba[i+2] == 255 {
			lit++
		}
	}
	if f.w != 224 || f.h != 256 || lit == 0 {
		t.Fatalf("%dx%d, %d lit", f.w, f.h, lit)
	}
}

func TestEngineArchipelago(t *testing.T) {
	data := func(n string) []byte {
		b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "archipelago", n))
		if err != nil {
			t.Fatal(err)
		}
		return b
	}
	c := startEngine(t)
	dir := t.TempDir()
	patch := writeFile(t, dir, "AP_old_P2_Other.aprpold", data("AP_old_P2_Other.aprpold"))
	r := c.call(map[string]any{"cmd": "open", "path": patch})
	if r["need"] != "apbase" || !strings.Contains(r["error"].(string), "(player Other)") {
		t.Fatalf("want the original ROM asked for: %v", r)
	}
	base := writeFile(t, dir, "old.gb", data("old.bin"))
	r = c.call(map[string]any{"cmd": "open", "path": patch, "apBase": base})
	// (the made-up ROM may not run as a game; it must be made and named after the patch)
	if r["ok"] != true && (r["need"] != nil || !strings.Contains(r["error"].(string), "Game Boy game")) {
		t.Fatalf("with the ROM: %v", r)
	}
	if r["ok"] == true && (r["name"] != "AP_old_P2_Other" || r["system"] != "gb") {
		t.Fatal(r)
	}
	// remembered for the next patch
	if r2 := c.call(map[string]any{"cmd": "open", "path": patch}); r2["need"] == "apbase" {
		t.Fatalf("not remembered: %v", r2)
	}
}

// fakeCore records what the engine hands it.
type fakeCore struct {
	input  [4]uint32
	analog [4][4]float32
	sram   []byte
	frames int
}

func (c *fakeCore) Info() core.Info {
	return core.Info{System: "fake", Buttons: []string{"A", "B", "Select", "Start", "Up", "Down", "Left", "Right", "Switch"}, Players: 4}
}
func (c *fakeCore) FPS() float64               { return 60 }
func (c *fakeCore) Width() int                 { return 2 }
func (c *fakeCore) Height() int                { return 1 }
func (c *fakeCore) Screen() []uint32           { return []uint32{0x112233, uint32(c.frames)} }
func (c *fakeCore) SetInput(p int, b uint32)   { c.input[p] = b }
func (c *fakeCore) RunFrame()                  { c.frames++; c.sram[0] = byte(c.frames) }
func (c *fakeCore) TakeAudio() []float32       { return []float32{0.5, -0.5} }
func (c *fakeCore) Reset()                     {}
func (c *fakeCore) SaveState() ([]byte, error) { return []byte{byte(c.frames)}, nil }
func (c *fakeCore) LoadState(b []byte) error   { c.frames = int(b[0]); return nil }
func (c *fakeCore) SRAM() []byte               { return c.sram }
func (c *fakeCore) SetAnalog(p, a int, v float32) {
	c.analog[p][a] = v
}
func (c *fakeCore) Rumble() float32 { return 0.5 }

func TestGameInput(t *testing.T) {
	fc := &fakeCore{sram: make([]byte, 8)}
	games.Makers["fake"] = func([]byte, core.Options) (core.Core, error) { return fc, nil }
	games.DefaultMaps["fake"] = games.ControlMap{"A": games.B([]int{games.PadB}, "x"), "Switch": games.TG([]int{games.PadY}, "1")}
	games.SystemButtons["fake"] = fc.Info().Buttons
	defer func() {
		delete(games.Makers, "fake")
		delete(games.DefaultMaps, "fake")
		delete(games.SystemButtons, "fake")
	}()
	st := games.Store{Dir: t.TempDir()}
	g, err := newGame(&games.Game{System: "fake", Name: "Fake"}, st, openOptions{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	pad := func(buttons uint16, axes ...float32) padIn {
		p := padIn{buttons: buttons}
		copy(p.axes[:], axes)
		return p
	}
	keys := map[string]bool{"x": true, "arrowleft": true}
	// player 1: keys and the touch controls (B); controller 1 = player 1, controller 2 = player 2
	f := g.run(runRequest{frames: 1, volume: 1, touch: 1 << 1, keys: keys,
		pads: []padIn{pad(1 << games.PadUp), pad(1<<games.PadB, 0.9, 0)}})
	if fc.input[0] != 1|2|16|64 || fc.input[1] != 1|128 || fc.input[2] != 0 {
		t.Fatalf("input %b %b %b", fc.input[0], fc.input[1], fc.input[2])
	}
	if len(f.pcm) != 4 || f.pcm[0] != f.pcm[1] || f.pcm[0] <= 0 || f.rumble != 0.5 || f.w != 2 || len(f.pix) != 2 {
		t.Fatalf("frame %+v", f)
	}
	// the touch stick moves player 1's left stick
	g.run(runRequest{frames: 1, touchAxes: 3, axis: [2]float32{-1, 0.5}, pads: []padIn{pad(0, 0.2, 0.2, 0.7)}})
	if fc.analog[0] != [4]float32{-1, 0.5, 0.7, 0} {
		t.Fatalf("analog %v", fc.analog[0])
	}
	// a touch on a switch flips it, as a key does
	g.run(runRequest{frames: 1, touch: 1 << 8})
	g.run(runRequest{frames: 1})
	if fc.input[0]&(1<<8) == 0 {
		t.Fatal("the switch didn't stay on")
	}
	// the battery save is written when it changes (and on close)
	if err := g.checkSRAM(true); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(st.SRAMPath("fake", "Fake")); len(b) != 8 || int(b[0]) != fc.frames {
		t.Fatalf("battery save %v", b)
	}
	// fast-forward: no rumble
	if f := g.run(runRequest{frames: 3, fast: true}); f.rumble != 0 || len(f.pcm) != 4 {
		t.Fatalf("fast %+v", f)
	}
}

// TestFrameTimes runs the free test ROMs (homebrew and test programs) found
// in RP_TEST_ROMS and says how long a frame takes on this computer, and
// checks the engine makes the same frames and sound as the core does.
func TestFrameTimes(t *testing.T) {
	root := os.Getenv("RP_TEST_ROMS")
	if root == "" {
		t.Skip("RP_TEST_ROMS not set (a folder of free test ROMs)")
	}
	roms := []string{
		"nes/nes-test-roms/other/nestest.nes",
		"chips/SNES/CHIP/GSU/GSUTest/CMP/GSUCMP.sfc",
		"gbgba/retrio_gb-test-roms/cpu_instrs/cpu_instrs.gb",
		"gbgba/gtr/cgb-acid2/cgb-acid2.gbc",
		"gbgba/alyosha-tas_gba-tests/arm/arm.gba",
		"segaatari/rb-md-games/shatteringjaws.md",
		"segaatari/rb-sms-games/shootingstars.sms",
		"segaatari/rb-atari2600-games/hauntedbakery.bin",
		"../rel/bas_adventurer.a78",
		"../roms/pl/CPUTest/CPU/ADD/CPUADD.N64",
	}
	c := startEngine(t)
	for _, r := range roms {
		p := filepath.Join(root, r)
		if _, err := os.Stat(p); err != nil {
			t.Logf("%s: not here", r)
			continue
		}
		const frames = 300
		start := time.Now()
		ref, err := runReference(p, frames, 48000)
		if err != nil {
			t.Errorf("%s: %v", r, err)
			continue
		}
		per := time.Since(start) / frames
		g := c.must(map[string]any{"cmd": "open", "path": p, "sampleRate": 48000})
		var last frame
		samples := 0
		h := sha1.New()
		for i := 0; i < frames; i++ {
			last = c.run(runRequest{frames: 1, volume: 1})
			samples += len(last.pcm)
			h.Write(pcmBytes(last.pcm))
		}
		same := sha(last.rgba) == ref.Pixels && hex.EncodeToString(h.Sum(nil)) == ref.Audio && samples == ref.Samples
		t.Logf("%-10s %-24s %3dx%-3d %6.2f ms/frame on the core; engine frames and sound same as the core: %v",
			g["system"], filepath.Base(r), last.w, last.h, float64(per.Microseconds())/1000, same)
		if !same {
			t.Errorf("%s: the engine's frames or sound differ from the core's", r)
		}
	}
}
