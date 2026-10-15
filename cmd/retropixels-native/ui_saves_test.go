package main

import (
	"bytes"
	"encoding/json"
	"image/png"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSlotPaths(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	if slotPath("nes", "Zelda", 0) != statePath("nes", "Zelda") {
		t.Fatal("slot 1 isn't the old save state file")
	}
	if got := filepath.Base(slotPath("nes", "Zelda: x", 3)); got != "Zelda x.slot4.state" {
		t.Fatalf("slot 4 file %q", got)
	}
	if !strings.HasSuffix(slotThumbPath("nes", "Zelda", 1), ".slot2.state.png") {
		t.Fatal("thumbnail path")
	}
	now := time.Date(2026, 10, 15, 14, 3, 0, 0, time.Local)
	if slotTime(now.Add(-time.Hour), now) != "13:03" || slotTime(now.AddDate(0, 0, -3), now) != "12 Oct 14:03" || slotTime(time.Time{}, now) != "" {
		t.Fatal("slot times")
	}
	if slotLabel(1, slotInfo{}, now) != "Slot 2 (empty)" || slotLabel(0, slotInfo{true, now}, now) != "Slot 1 - 14:03" {
		t.Fatal("slot labels")
	}
}

func TestSlotsSaveLoadImport(t *testing.T) {
	s, _, _ := testSession(t)
	s.start()
	defer s.stop()
	time.Sleep(100 * time.Millisecond)
	s.paused.Store(true)
	time.Sleep(20 * time.Millisecond)
	frame := func() []byte {
		var st []byte
		s.call(func() error { st, _ = s.core.SaveState(); return nil })
		return st[:4]
	}
	run := func() {
		s.paused.Store(false)
		time.Sleep(120 * time.Millisecond)
		s.paused.Store(true)
		time.Sleep(20 * time.Millisecond)
	}
	if err := s.saveSlot(2); err != nil {
		t.Fatal(err)
	}
	at2 := frame()
	run()
	if err := s.saveSlot(0); err != nil {
		t.Fatal(err)
	}
	at0 := frame()
	if bytes.Equal(at0, at2) {
		t.Fatal("the game didn't move on between saves")
	}
	slots := readSlots("fake", s.game.Name)
	if !slots[0].Used || slots[1].Used || !slots[2].Used || slots[3].Used {
		t.Fatalf("slots %+v", slots)
	}
	// the picture: the game's shape (4:3), 120 lines
	pix, w, h := readThumb(slotThumbPath("fake", s.game.Name, 2))
	if pix == nil || w != 160 || h != thumbHeight {
		t.Fatalf("thumbnail %dx%d", w, h)
	}
	if err := s.loadSlot(2); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(frame(), at2) {
		t.Fatal("slot 3 didn't load")
	}
	if err := s.loadState(); err != nil { // F4 with slot 1: the old single save state
		t.Fatal(err)
	}
	if !bytes.Equal(frame(), at0) {
		t.Fatal("slot 1 didn't load")
	}
	if err := s.loadSlot(1); err == nil || !strings.Contains(err.Error(), "slot 2 is empty") {
		t.Fatalf("empty slot: %v", err)
	}
	// import a file exported from slot 3, then undo it
	file, _ := os.ReadFile(slotPath("fake", s.game.Name, 2))
	if err := s.importState(file); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(frame(), at2) {
		t.Fatal("imported state didn't load")
	}
	if err := s.undoLoad(); err != nil || !bytes.Equal(frame(), at0) {
		t.Fatalf("undo after import: %v", err)
	}
	// raw emulator states go to the core as they are; another system's are refused
	if err := s.importState(file[5+len("fake"):]); err != nil {
		t.Fatal(err)
	}
	if err := s.importState(encodeState("nes", []byte{1, 2, 3})); err == nil {
		t.Fatal("loaded another system's state")
	}
	if err := deleteSlot("fake", s.game.Name, 2); err != nil {
		t.Fatal(err)
	}
	if readSlots("fake", s.game.Name)[2].Used {
		t.Fatal("slot not deleted")
	}
	if _, err := os.Stat(slotThumbPath("fake", s.game.Name, 2)); err == nil {
		t.Fatal("picture not deleted")
	}
	if err := deleteSlot("fake", s.game.Name, 3); err != nil {
		t.Fatal("deleting an empty slot:", err)
	}
	if sr := s.sram(); len(sr) != 16 || !s.hasSRAM() {
		t.Fatalf("battery save %v", sr)
	}
}

func TestImportSRAM(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	p := sramPath("gb", "Tobu")
	if err := importSRAM("gb", "Tobu", []byte{1, 2}); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); !bytes.Equal(b, []byte{1, 2}) {
		t.Fatal("not written")
	}
	if _, err := os.Stat(p + ".bak"); err == nil {
		t.Fatal("a backup of nothing")
	}
	if err := importSRAM("gb", "Tobu", []byte{3}); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p + ".bak"); !bytes.Equal(b, []byte{1, 2}) {
		t.Fatal("old battery save not kept")
	}
	if importSRAM("gb", "Tobu", nil) == nil || importSRAM("gb", "Tobu", make([]byte, maxSRAMImport+1)) == nil {
		t.Fatal("took an empty or huge file")
	}
	if b, _ := os.ReadFile(p); !bytes.Equal(b, []byte{3}) {
		t.Fatal("a refused file changed the battery save")
	}
	// the session reads it back as the game's battery save
	coreMakers["fake"] = newTestCore
	defer delete(coreMakers, "fake")
	sav := make([]byte, 16)
	sav[0] = 7
	importSRAM("fake", "Test", sav)
	s, err := newSession(&loadedGame{System: "fake", ROM: []byte{1}, Name: "Test"}, &fakeHost{}, &fakeCard{}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if s.core.SRAM()[0] != 7 {
		t.Fatal("imported battery save not used")
	}
}

func TestScreenshot(t *testing.T) {
	dir := t.TempDir()
	pix := make([]uint32, 256*224)
	pix[0] = 0x00ff8000
	now := time.Date(2026, 10, 15, 9, 30, 5, 0, time.Local)
	p, err := saveScreenshot(dir, "Mario: World", pix, 256, 224, 4.0/3, now)
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Base(p) != "Mario World 2026-10-15 09-30-05.png" {
		t.Fatalf("name %q", p)
	}
	p2, _ := saveScreenshot(dir, "Mario: World", pix, 256, 224, 4.0/3, now)
	if filepath.Base(p2) != "Mario World 2026-10-15 09-30-05 (2).png" {
		t.Fatalf("second name %q", p2)
	}
	f, _ := os.Open(p)
	img, err := png.Decode(f)
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	// 224 lines x3 = 672, at 4:3
	if b := img.Bounds(); b.Dx() != 896 || b.Dy() != 672 {
		t.Fatalf("size %v", b)
	}
	r, g, bb, _ := img.At(0, 0).RGBA()
	if r>>8 != 0xff || g>>8 != 0x80 || bb != 0 {
		t.Fatal("colours")
	}
	if _, err := saveScreenshot(dir, "x", nil, 0, 0, 1, now); err == nil {
		t.Fatal("no picture")
	}
}

func TestRecentRecordings(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	for i, n := range []string{"a.mp4", "b.MP4", "c.mp4", "d.txt"} {
		p := filepath.Join(dir, n)
		os.WriteFile(p, []byte("x"), 0o644)
		os.Chtimes(p, now, now.Add(time.Duration(i)*time.Minute))
	}
	got := recentRecordings(dir, 2)
	if len(got) != 2 || filepath.Base(got[0]) != "c.mp4" || filepath.Base(got[1]) != "b.MP4" {
		t.Fatalf("%v", got)
	}
	if recentRecordings(filepath.Join(dir, "nope"), 5) != nil {
		t.Fatal("missing folder")
	}
}

func TestUISettings(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	t.Setenv("RETRO_REC_DIR", "")
	s := loadSettings()
	if s.RumbleOff || s.recQuality() != "balanced" || s.recDir() != recordingsDir() {
		t.Fatalf("defaults %+v", s.uiSettings)
	}
	s.RumbleOff, s.RecQuality, s.RecDir = true, "small", `D:\Videos`
	saveSettings(s)
	data, _ := os.ReadFile(settingsPath())
	var raw map[string]any
	json.Unmarshal(data, &raw)
	if raw["rumbleOff"] != true || raw["recordQuality"] != "small" || raw["recordDir"] != `D:\Videos` {
		t.Fatalf("file %s", data)
	}
	got := loadSettings()
	if !got.RumbleOff || got.recQuality() != "small" || got.recDir() != `D:\Videos` {
		t.Fatalf("reloaded %+v", got.uiSettings)
	}
	got.RecQuality = "silly"
	if got.recQuality() != "balanced" {
		t.Fatal("unknown quality")
	}
	t.Setenv("RETRO_REC_DIR", "/tmp/x")
	if got.recDir() != "/tmp/x" {
		t.Fatal("RETRO_REC_DIR")
	}
}

func TestBinChoice(t *testing.T) {
	defer func() { binSystem = "" }()
	coreMakers["atari7800"] = newTestCore
	coreMakers["atari2600"] = newTestCore
	defer delete(coreMakers, "atari7800")
	defer delete(coreMakers, "atari2600")
	small := make([]byte, 4096)
	if systemForBin(small) != "atari2600" {
		t.Fatal("guess")
	}
	binSystem = "atari7800"
	if systemForBin(small) != "atari7800" {
		t.Fatal("choice not used")
	}
	binSystem = "nosuch" // a system that isn't in this build: guessed instead
	if systemForBin(small) != "atari2600" {
		t.Fatal("a system that isn't there")
	}
}

func TestPadSlotHotkeys(t *testing.T) {
	s, h, _ := testSession(t)
	p := PadState{Connected: true}
	p.Axes[2] = 1
	s.padHotkeys(p)
	s.padHotkeys(p) // held: once
	p.Axes[2] = 0
	s.padHotkeys(p)
	p.Axes[2] = -0.9
	s.padHotkeys(p)
	// the N64: View + D-pad left / right
	s.game.System = "n64"
	p = PadState{Connected: true}
	p.Buttons[padView], p.Buttons[padRight] = true, true
	s.padHotkeys(p)
	s.padHotkeys(p)
	p.Axes[2] = 1 // (the right stick is the C buttons there)
	p.Buttons[padRight] = false
	s.padHotkeys(p)
	h.mu.Lock()
	defer h.mu.Unlock()
	if strings.Join(h.events, ",") != "pad-slot,pad-slot,pad-slot" {
		t.Fatalf("events %v", h.events)
	}
	// tilt games use the right stick themselves
	s2, h2, _ := testSession(t)
	s2.rightStick = true
	p = PadState{Connected: true}
	p.Axes[2] = 1
	s2.padHotkeys(p)
	if len(h2.events) != 0 {
		t.Fatalf("tilt game: %v", h2.events)
	}
}
