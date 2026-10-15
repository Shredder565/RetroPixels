package main

import (
	"archive/zip"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"retropixels/core"
)

func TestPacerFollowsTheClock(t *testing.T) {
	p := newPacer(60)
	frames := 0
	// one second in 1 ms steps, sound comfortably queued: 60 frames
	for i := 0; i < 1000; i++ {
		frames += p.step(0.001, 0.08, false)
	}
	if frames < 59 || frames > 61 {
		t.Fatalf("ran %d frames in a second at 60 fps", frames)
	}
	// starved sound: runs faster (half as fast again, like host.js)
	p = newPacer(60)
	frames = 0
	for i := 0; i < 1000; i++ {
		frames += p.step(0.001, 0.01, false)
	}
	if frames < 85 || frames > 95 {
		t.Fatalf("ran %d frames in a second with the sound running dry, want about 90", frames)
	}
	// too much queued: slower
	p = newPacer(60)
	frames = 0
	for i := 0; i < 1000; i++ {
		frames += p.step(0.001, 0.2, false)
	}
	if frames < 25 || frames > 35 {
		t.Fatalf("ran %d frames in a second with too much sound queued, want about 30", frames)
	}
	// no sound device: the clock alone
	p = newPacer(60.0988)
	frames = 0
	for i := 0; i < 10000; i++ {
		frames += p.step(0.001, -1, false)
	}
	if frames < 600 || frames > 602 {
		t.Fatalf("ran %d frames in 10 s at 60.0988 fps", frames)
	}
	// a stall counts as 0.1 s at most (as in host.js), run 4 frames at a time
	p = newPacer(60)
	if n := p.step(2, -1, false); n != 4 {
		t.Fatalf("after a stall ran %d frames, want 4", n)
	}
	if n := p.step(0, -1, false) + p.step(0, -1, false); n != 2 {
		t.Fatalf("caught up %d more frames after a stall, want 2", n)
	}
	if n := p.step(0.016, -1, true); n != fastFrames {
		t.Fatalf("fast forward ran %d frames", n)
	}
}

// The sound queue settles between the pacer's limits when the sound card
// plays at its own rate.
func TestPacerKeepsTheQueue(t *testing.T) {
	p := newPacer(60)
	queued := 0.0
	const card = 1.002 // the sound card's clock a bit fast
	var minQ, maxQ = 10.0, 0.0
	for i := 0; i < 60000; i++ { // 60 s
		dt := 0.001
		queued -= dt * card
		if queued < 0 {
			queued = 0
		}
		queued += float64(p.step(dt, queued, false)) / 60
		if i > 5000 {
			minQ, maxQ = min(minQ, queued), max(maxQ, queued)
		}
	}
	if minQ < 0.015 || maxQ > 0.17 {
		t.Fatalf("queue wandered to %.3f..%.3f s", minQ, maxQ)
	}
}

func TestToPCM(t *testing.T) {
	got := toPCM(nil, []float32{0, 0.5, -1, 2, -3}, false, 1)
	want := []int16{0, 0, 16383, 16383, -32767, -32767, 32767, 32767, -32768, -32768}
	if !equal16(got, want) {
		t.Fatalf("mono: %v want %v", got, want)
	}
	got = toPCM(got, []float32{1, -1}, true, 0.5)
	if !equal16(got, []int16{16383, -16383}) {
		t.Fatalf("stereo at half volume: %v", got)
	}
	if got := toPCM(nil, []float32{0.5}, false, 0); !equal16(got, []int16{0, 0}) {
		t.Fatalf("muted: %v", got)
	}
}

func equal16(a, b []int16) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func TestDefaultKeysMatchTheWebVersion(t *testing.T) {
	nes := loadMap("nes", nil)
	keys := map[string]bool{"x": true}
	in := newInputState()
	buttons := systemButtons["nes"]
	if b := in.bits(0, buttons, nes, keys, nil, false); b != 1 { // A
		t.Fatalf("X key: bits %b, want A", b)
	}
	keys = map[string]bool{"z": true, "enter": true, "arrowleft": true, "shift": true}
	if b := in.bits(0, buttons, nes, keys, nil, false); b != 2|4|8|64 {
		t.Fatalf("Z, Enter, Shift, Left: bits %b", b)
	}
	// keys only work player 1
	if b := in.bits(1, buttons, nes, keys, nil, false); b != 0 {
		t.Fatalf("player 2 got keyboard bits %b", b)
	}
	// controller: Xbox A = NES B, Xbox B = NES A, left stick = D-pad
	pad := &PadState{Connected: true}
	pad.Buttons[padA] = true
	pad.Axes[0] = 0.9
	pad.Axes[1] = -0.9
	if b := in.bits(1, buttons, nes, nil, pad, false); b != 2|16|128 {
		t.Fatalf("pad A + stick up-right: bits %b", b)
	}
	if b := in.bits(1, buttons, nes, nil, pad, true); b != 2 {
		t.Fatalf("a core reading the stick itself still got D-pad bits: %b", b)
	}
}

func TestAtariSwitchesToggle(t *testing.T) {
	m := loadMap("atari2600", nil)
	buttons := systemButtons["atari2600"]
	in := newInputState()
	ldiff := uint32(1) << 7
	press := func(down bool) uint32 { return in.bits(0, buttons, m, map[string]bool{"1": down}, nil, false) }
	if press(true)&ldiff == 0 {
		t.Fatal("first press didn't flip the left difficulty switch")
	}
	if press(true)&ldiff == 0 || press(false)&ldiff == 0 {
		t.Fatal("the switch didn't stay flipped")
	}
	if press(true)&ldiff != 0 {
		t.Fatal("second press didn't flip it back")
	}
	// Fire: Z, X or Space, Xbox A or B
	for _, k := range []string{"z", "x", " "} {
		if in.bits(0, buttons, m, map[string]bool{k: true}, nil, false)&(1<<4) == 0 {
			t.Fatalf("%q isn't Fire", k)
		}
	}
}

func TestControlMapSaving(t *testing.T) {
	var s Settings
	m := s.controlMap("nes")
	m["A"] = Binding{Pad: []int{3}, Keys: []string{"k"}}
	s.setControlMap("nes", m)
	if len(s.Maps["nes"]) != 1 {
		t.Fatalf("saved %v, want only A", s.Maps["nes"])
	}
	if got := s.controlMap("nes")["A"]; got.Keys[0] != "k" || got.Pad[0] != 3 {
		t.Fatalf("A is %+v", got)
	}
	if got := s.controlMap("nes")["B"]; got.Keys[0] != "z" {
		t.Fatalf("B lost its default: %+v", got)
	}
	// a saved map can't make a switch stop latching
	s.Maps["atari2600"] = ControlMap{"Left difficulty": {Pad: []int{}, Keys: []string{"q"}}}
	if bd := s.controlMap("atari2600")["Left difficulty"]; !bd.Toggle || bd.Keys[0] != "q" {
		t.Fatalf("left difficulty %+v", bd)
	}
	s.setControlMap("nes", loadMap("nes", nil))
	if _, ok := s.Maps["nes"]; ok {
		t.Fatal("defaults were saved as changes")
	}
}

func TestKeyNames(t *testing.T) {
	for vk, want := range map[int]string{'X': "x", '1': "1", 0x20: " ", 0x0d: "enter", 0x10: "shift", 0x26: "arrowup", 0x72: "f3", 0x63: "3"} {
		if got := keyName(vk); got != want {
			t.Errorf("vk %#x = %q, want %q", vk, got, want)
		}
	}
	if keyLabel(" ") != "Space" || keyLabel("arrowup") != "Up" || keyLabel("enter") != "Enter" {
		t.Error("labels")
	}
}

func TestSystemDetection(t *testing.T) {
	coreMakers["nes"] = newTestCore
	coreMakers["atari2600"] = newTestCore
	defer delete(coreMakers, "nes")
	defer delete(coreMakers, "atari2600")
	dir := t.TempDir()
	write := func(name string, data []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	nesROM := append([]byte("NES\x1a"), make([]byte, 16+16384)...)
	a26 := make([]byte, 4096)
	cases := []struct {
		path, sys, err string
	}{
		{write("Mario.nes", nesROM), "nes", ""},
		{write("Pitfall.a26", a26), "atari2600", ""},
		{write("Combat.bin", a26), "atari2600", ""},
		{write("game.sfc", make([]byte, 1024)), "snes", ""},
		{write("sonic.bin", append(append(make([]byte, 0x100), "SEGA"...), make([]byte, 70000)...)), "segaMD", ""},
		{write("zelda.iso", []byte{1}), "", "GameCube and Wii games are coming"},
		{write("seed.apsm", []byte{1}), "", "Archipelago patch file"},
		{write("notes.txt", []byte{1}), "", "couldn't tell which system"},
		{write("Zipped.zip", zipOf(t, map[string][]byte{"readme.txt": {1}, "Game (USA).nes": nesROM})), "nes", ""},
		{write("ZippedBin.zip", zipOf(t, map[string][]byte{"game.bin": a26})), "atari2600", ""},
		// arcade sets are recognised by their files' names (the core says what's missing)
		{write("pacman.zip", zipOf(t, map[string][]byte{"pacman.6e": {1}, "pacman.6f": {1}, "pacman.6h": {1}, "82s123.7f": {1}})), "arcade", ""},
		{write("Empty.zip", zipOf(t, map[string][]byte{"readme.txt": {1}})), "", "isn't an arcade game RetroPixels knows yet"},
	}
	for _, c := range cases {
		g, err := openGame(c.path)
		if c.err != "" {
			if err == nil || !strings.Contains(err.Error(), c.err) {
				t.Errorf("%s: error %v, want %q", filepath.Base(c.path), err, c.err)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: %v", filepath.Base(c.path), err)
			continue
		}
		if g.System != c.sys {
			t.Errorf("%s: system %q, want %q", filepath.Base(c.path), g.System, c.sys)
		}
		if g.Name != strings.TrimSuffix(filepath.Base(c.path), filepath.Ext(c.path)) {
			t.Errorf("%s: name %q", c.path, g.Name)
		}
	}
	if g, _ := openGame(filepath.Join(dir, "Zipped.zip")); g == nil || !bytes.Equal(g.ROM, nesROM) {
		t.Error("the ROM inside the zip wasn't read")
	}
}

// Test7z needs the 7z tool, or a python with py7zr (SEVENZIP_PYTHON, as the
// sevenzip package's tests use).
func Test7z(t *testing.T) {
	coreMakers["atari2600"] = newTestCore
	defer delete(coreMakers, "atari2600")
	dir := t.TempDir()
	rom := filepath.Join(dir, "Adventure.a26")
	os.WriteFile(rom, bytes.Repeat([]byte{0xea}, 4096), 0o644)
	arc := filepath.Join(dir, "Adventure.7z")
	var cmd *exec.Cmd
	if sz, err := exec.LookPath("7z"); err == nil {
		cmd = exec.Command(sz, "a", arc, rom)
	} else if py := os.Getenv("SEVENZIP_PYTHON"); py != "" {
		cmd = exec.Command(py, "-c", "import py7zr,sys; z=py7zr.SevenZipFile(sys.argv[1],'w'); z.write(sys.argv[2],'Adventure.a26'); z.close()", arc, rom)
	} else {
		t.Skip("no 7z tool or SEVENZIP_PYTHON to make a test archive")
	}
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("making the archive: %v %s", err, out)
	}
	g, err := openGame(arc)
	if err != nil || g.System != "atari2600" || len(g.ROM) != 4096 || g.Name != "Adventure" {
		t.Fatalf("7z: %+v %v", g, err)
	}
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

func TestGuessBin(t *testing.T) {
	if guessBin(make([]byte, 4096)) != "atari2600" || guessBin(make([]byte, 1<<20)) != "segaMD" {
		t.Fatal("size guess")
	}
	b := make([]byte, 0x8000)
	copy(b[0x7ff0:], "TMR SEGA")
	if guessBin(b) != "sms" {
		t.Fatal("TMR SEGA")
	}
	b = make([]byte, 200)
	copy(b[1:], "ATARI7800")
	if guessBin(b) != "atari7800" {
		t.Fatal("7800 header")
	}
}

func TestSavePathsAndStates(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", `C:\Users\me\AppData\Roaming\RetroPixels Native`)
	p := sramPath("nes", `Zelda: "Link" <USA>?`)
	if !strings.HasSuffix(p, filepath.Join("saves", "nes", "Zelda Link USA.sav")) {
		t.Fatalf("sram path %q", p)
	}
	if safeName("CON") != "_CON" || safeName("...") != "game" {
		t.Fatal("reserved names")
	}
	st := encodeState("nes", []byte{1, 2, 3})
	if b, err := decodeState("nes", st); err != nil || !bytes.Equal(b, []byte{1, 2, 3}) {
		t.Fatal("state round trip")
	}
	if _, err := decodeState("atari2600", st); err == nil {
		t.Fatal("loaded another system's state")
	}
	if _, err := decodeState("nes", []byte("junk")); err == nil {
		t.Fatal("loaded junk")
	}
}

func TestSettingsFile(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	s := loadSettings()
	if s.Volume != 80 || s.Scale != 3 || s.Smooth {
		t.Fatalf("defaults %+v", s)
	}
	for i := 0; i < 15; i++ {
		s.addRecent(filepath.Join("games", string(rune('a'+i))+".nes"))
	}
	s.addRecent(filepath.Join("games", "c.nes"))
	s.Scale = 5
	if err := saveSettings(s); err != nil {
		t.Fatal(err)
	}
	got := loadSettings()
	if len(got.Recent) != maxRecent || got.Recent[0] != filepath.Join("games", "c.nes") || got.Scale != 5 {
		t.Fatalf("reloaded %+v", got)
	}
	os.WriteFile(settingsPath(), []byte(`{"volume": 900, "windowScale": 0}`), 0o644)
	if got := loadSettings(); got.Volume != 80 || got.Scale != 3 {
		t.Fatalf("bad values kept: %+v", got)
	}
}

func TestLayout(t *testing.T) {
	if r := fitRect(1920, 1080, 4.0/3); r != (rect{240, 0, 1440, 1080}) {
		t.Fatalf("16:9 screen %+v", r)
	}
	if r := fitRect(960, 1000, 4.0/3); r != (rect{0, 140, 960, 720}) {
		t.Fatalf("tall window %+v", r)
	}
	if w, h := baseSize(240, 4.0/3); w != 320 || h != 240 {
		t.Fatal("NES 1x")
	}
	if w, h := baseSize(210, 4.0/3); w != 280 || h != 210 {
		t.Fatal("2600 1x")
	}
	if w, h := recordSize(240, 4.0/3); w != 960 || h != 720 {
		t.Fatalf("NES recording %dx%d", w, h)
	}
	if w, h := recordSize(210, 4.0/3); w != 840 || h != 630 {
		t.Fatalf("2600 recording %dx%d", w, h)
	}
	if pictureAspect(0, "nes", 256, 240) != 4.0/3 || pictureAspect(1.5, "nes", 256, 240) != 1.5 || pictureAspect(0, "gb", 160, 144) != 160.0/144 {
		t.Fatal("aspect")
	}
	src := []uint32{1, 2, 3, 4}
	dst := make([]uint32, 16)
	scaleTo(dst, 4, 4, src, 2, 2)
	if dst[0] != 1 || dst[3] != 2 || dst[12] != 3 || dst[15] != 4 {
		t.Fatalf("scale %v", dst)
	}
}

// ---------------- the session, with the test core ----------------

type fakeHost struct {
	mu      sync.Mutex
	frames  int
	lastPix []uint32
	events  []string
	padSt   [4]PadState
}

func (h *fakeHost) present(pix []uint32, w, hh int) {
	h.mu.Lock()
	h.frames++
	h.lastPix = append(h.lastPix[:0], pix...)
	h.mu.Unlock()
}
func (h *fakeHost) pads() [4]PadState {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.padSt
}
func (h *fakeHost) rumble(int, float32, float32) {}
func (h *fakeHost) event(name, detail string) {
	h.mu.Lock()
	h.events = append(h.events, name)
	h.mu.Unlock()
}

// fakeCard plays sound at real time.
type fakeCard struct {
	mu      sync.Mutex
	queued  float64
	written int
	at      time.Time
}

func (c *fakeCard) drain() {
	now := time.Now()
	if !c.at.IsZero() {
		c.queued -= now.Sub(c.at).Seconds()
		if c.queued < 0 {
			c.queued = 0
		}
	}
	c.at = now
}
func (c *fakeCard) Write(p []int16) {
	c.mu.Lock()
	c.drain()
	c.queued += float64(len(p)/2) / sampleRate
	c.written += len(p) / 2
	c.mu.Unlock()
}
func (c *fakeCard) Queued() float64 { c.mu.Lock(); defer c.mu.Unlock(); c.drain(); return c.queued }
func (c *fakeCard) Clear()          { c.mu.Lock(); c.queued = 0; c.mu.Unlock() }

func testSession(t *testing.T) (*session, *fakeHost, *fakeCard) {
	t.Helper()
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	coreMakers["fake"] = newTestCore
	systems = append(systems, system{"fake", "Test pattern", []string{"rptest"}, 4.0 / 3})
	t.Cleanup(func() {
		delete(coreMakers, "fake")
		systems = systems[:len(systems)-1]
	})
	h, card := &fakeHost{}, &fakeCard{}
	g := &loadedGame{System: "fake", ROM: []byte{1}, Name: "Test: game"}
	s, err := newSession(g, h, card, loadMap("fake", ControlMap{"A": {Keys: []string{"x"}}}))
	if err != nil {
		t.Fatal(err)
	}
	return s, h, card
}

func TestSessionRunsAndSaves(t *testing.T) {
	s, h, card := testSession(t)
	s.start()
	time.Sleep(1200 * time.Millisecond)
	h.mu.Lock()
	n := h.frames
	h.mu.Unlock()
	card.mu.Lock()
	q := card.queued
	card.mu.Unlock()
	if n < 50 || n > 100 {
		t.Fatalf("drew %d frames in 1.2 s", n)
	}
	if q < 0.02 || q > 0.2 {
		t.Fatalf("%.3f s of sound queued", q)
	}

	// tap A (the battery save counts presses): even a tap shorter than a
	// frame counts; the save is written at the end
	s.setKey("x", true)
	s.setKey("x", false)
	time.Sleep(100 * time.Millisecond)

	if err := s.loadState(); err == nil {
		t.Fatal("loaded a state that was never saved")
	}
	s.paused.Store(true) // so no frame runs between saving and reading the state back
	time.Sleep(20 * time.Millisecond)
	if err := s.saveState(); err != nil {
		t.Fatal(err)
	}
	var before []byte
	s.call(func() error { before, _ = s.core.SaveState(); return nil })
	s.paused.Store(false)
	time.Sleep(200 * time.Millisecond)
	s.paused.Store(true)
	time.Sleep(20 * time.Millisecond)
	if err := s.loadState(); err != nil {
		t.Fatal(err)
	}
	var after []byte
	s.call(func() error { after, _ = s.core.SaveState(); return nil })
	s.paused.Store(false)
	// the frame counter is in the first 4 bytes; loading put it back
	if !bytes.Equal(before[:4], after[:4]) {
		t.Fatalf("frame %v after loading, saved at %v", after[:4], before[:4])
	}
	if !s.canUndo() {
		t.Fatal("no undo after a load")
	}
	if err := s.undoLoad(); err != nil {
		t.Fatal(err)
	}
	if s.canUndo() {
		t.Fatal("undo stayed after undoing")
	}

	s.paused.Store(true)
	time.Sleep(50 * time.Millisecond)
	h.mu.Lock()
	n = h.frames
	h.mu.Unlock()
	time.Sleep(200 * time.Millisecond)
	h.mu.Lock()
	if h.frames != n {
		t.Fatal("frames ran while paused")
	}
	h.mu.Unlock()
	s.paused.Store(false)

	s.stop()
	sav, err := os.ReadFile(sramPath("fake", "Test: game"))
	if err != nil || sav[0] != 1 {
		t.Fatalf("battery save %v %v (want 1 press counted)", sav, err)
	}
	// it comes back next time
	s2, _, _ := testSessionAgain(t, s.game)
	if s2.core.SRAM()[0] != 1 {
		t.Fatal("battery save not loaded")
	}
}

func testSessionAgain(t *testing.T, g *loadedGame) (*session, *fakeHost, *fakeCard) {
	h, card := &fakeHost{}, &fakeCard{}
	s, err := newSession(g, h, card, loadMap("fake", nil))
	if err != nil {
		t.Fatal(err)
	}
	return s, h, card
}

func TestSessionPadHotkeys(t *testing.T) {
	s, h, _ := testSession(t)
	s.start()
	defer s.stop()
	h.mu.Lock()
	h.padSt[2].Connected = true
	h.padSt[2].Buttons[padR3] = true
	h.mu.Unlock()
	time.Sleep(100 * time.Millisecond)
	h.mu.Lock()
	h.padSt[2].Buttons[padR3] = false
	h.padSt[2].Buttons[padL3] = true
	h.mu.Unlock()
	time.Sleep(100 * time.Millisecond)
	h.mu.Lock()
	h.padSt[2].Buttons[padL3] = false
	h.padSt[2].Axes[3] = 1
	h.mu.Unlock()
	time.Sleep(1300 * time.Millisecond)
	h.mu.Lock()
	ev := strings.Join(h.events, ",")
	h.mu.Unlock()
	if ev != "pad-save,pad-load,pad-record" {
		t.Fatalf("events %q", ev)
	}
}

type crashCore struct{ core.Core }

func (crashCore) RunFrame() { panic("bad opcode") }

func TestSessionSurvivesACrashingCore(t *testing.T) {
	s, h, _ := testSession(t)
	s.core = crashCore{s.core}
	s.start()
	time.Sleep(150 * time.Millisecond)
	s.stop()
	h.mu.Lock()
	defer h.mu.Unlock()
	if len(h.events) == 0 || h.events[0] != "failed" {
		t.Fatalf("events %v", h.events)
	}
}

func TestRecording(t *testing.T) {
	ff, err := exec.LookPath("ffmpeg")
	if err != nil {
		t.Skip("no ffmpeg")
	}
	probe, err := exec.LookPath("ffprobe")
	if err != nil {
		t.Skip("no ffprobe")
	}
	dir := t.TempDir()
	c, _ := newTestCore([]byte{1}, core.Options{SampleRate: sampleRate})
	r, err := startRecorder(ff, "libx264", "balanced", dir, "Test: game", 256, 240, 4.0/3, 60.0988, sampleRate)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 180; i++ {
		c.RunFrame()
		w, h, pix := c.Width(), c.Height(), c.Screen()
		if i == 90 { // the game changes size mid-recording
			pix, w, h = pix[:128*240], 128, 240
		}
		r.frame(pix, w, h)
		r.audio(c.TakeAudio(), false)
	}
	out, err := r.stop()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(filepath.Base(out), "Test game ") || filepath.Ext(out) != ".mp4" {
		t.Fatalf("file %q", out)
	}
	left, _ := filepath.Glob(filepath.Join(dir, "*.part"))
	if len(left) != 0 {
		t.Fatalf("left behind %v", left)
	}
	info, err := exec.Command(probe, "-v", "error", "-show_entries", "stream=codec_type,width,height,nb_frames,duration", "-of", "compact", out).CombinedOutput()
	if err != nil {
		t.Fatalf("ffprobe: %v %s", err, info)
	}
	s := string(info)
	if !strings.Contains(s, "codec_type=video|width=960|height=720") || !strings.Contains(s, "nb_frames=180") || !strings.Contains(s, "codec_type=audio") {
		t.Fatalf("recording: %s", s)
	}
	t.Logf("recording: %s", strings.TrimSpace(s))
}

func TestSoundGuard(t *testing.T) {
	var g soundGuard
	now := time.Now()
	// normal speed for a minute: still trusted
	for i := 0; i < 3600; i++ {
		now = now.Add(time.Second / 60)
		if g.check(now, 1, false, 60) {
			t.Fatal("stopped trusting a device at normal speed")
		}
	}
	// 1.5x for 8 s: no longer trusted
	gave := false
	for i := 0; i < 480; i++ {
		now = now.Add(time.Second / 60)
		n := 1
		if i%2 == 0 {
			n = 2
		}
		gave = gave || g.check(now, n, false, 60)
	}
	if !gave || !g.untrusted {
		t.Fatal("kept trusting a device eating sound at 1.5x")
	}
}
