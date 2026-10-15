package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"retropixels/core"
	"retropixels/internal/apatch"
	"retropixels/internal/apbridge"
)

func apTestData(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "archipelago", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestAPPatchFiles(t *testing.T) {
	dir := t.TempDir()
	write := func(name string, data []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	// a Game Boy patch (made by Archipelago's own patch classes, made-up data)
	gbPatch := write("AP_old_P2_Other.aprpold", apTestData(t, "AP_old_P2_Other.aprpold"))
	p, err := readAPPatchFile(gbPatch)
	if err != nil {
		t.Fatal(err)
	}
	if p.System != "gb" || p.PlayerName != "Other" {
		t.Fatalf("patch: %+v", p)
	}
	if !strings.Contains(apAskText(p, gbPatch), "AP_old_P2_Other.aprpold is an Archipelago patch for") ||
		!strings.Contains(apAskText(p, gbPatch), "(player Other)") {
		t.Fatal(apAskText(p, gbPatch))
	}
	// the right ROM makes the game; it's named after the patch (its own saves)
	g, err := apMakeGame(p, gbPatch, apTestData(t, "old.bin"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(g.ROM, apTestData(t, "new.bin")) || g.Name != "AP_old_P2_Other" || g.System != "gb" || g.AP != p || g.Path != gbPatch {
		t.Fatalf("game: %s %s %v", g.Name, g.System, g.AP)
	}
	if sramPath(g.System, g.Name) == sramPath("gb", "old") {
		t.Fatal("saves shared with the unpatched game")
	}
	// a zipped ROM works too
	var zb bytes.Buffer
	zw := zip.NewWriter(&zb)
	w, _ := zw.Create("Other (USA).gb")
	w.Write(apTestData(t, "old.bin"))
	zw.Close()
	if g, err := apMakeGame(p, gbPatch, zb.Bytes()); err != nil || !bytes.Equal(g.ROM, apTestData(t, "new.bin")) {
		t.Fatalf("zipped ROM: %v", err)
	}
	// a wrong ROM is refused as such (when the patch names its ROM's MD5)
	checked := *p
	checked.BaseMD5 = []string{apatch.MD5Hex(apTestData(t, "old.bin"))}
	bad := append([]byte(nil), apTestData(t, "old.bin")...)
	bad[0] ^= 1
	if _, err := apMakeGame(&checked, gbPatch, bad); !apWrongBase(err) {
		t.Fatalf("wrong ROM: %v", err)
	}

	// the original ROM is remembered per game
	var st apSettings
	st.setAPBase(p, `C:\roms\old.gb`)
	if st.apBase(p) != `C:\roms\old.gb` {
		t.Fatal("not remembered")
	}
	data, _ := json.Marshal(st)
	if !strings.Contains(string(data), "archipelagoBaseROMs") {
		t.Fatalf("settings: %s", data)
	}
	st.setAPBase(p, "")
	if st.apBase(p) != "" {
		t.Fatal("not forgotten")
	}

	// a SNES patch, with no SNES core in this build: the web page's message about later test builds
	snesMaker := coreMakers["snes"]
	delete(coreMakers, "snes")
	_, snesErr := readAPPatchFile(write("AP_test_P1_Tester.aprptest", apTestData(t, "AP_test_P1_Tester.aprptest")))
	if snesMaker != nil {
		coreMakers["snes"] = snesMaker
	}
	if snesErr == nil || !strings.Contains(snesErr.Error(), "Super Nintendo games are coming in a later test build") {
		t.Fatalf("SNES patch: %v", snesErr)
	}
	// a patch whose game needs its own code
	if _, err := readAPPatchFile(write("AP_custom.aprpcustom", apTestData(t, "AP_custom.aprpcustom"))); err == nil ||
		!strings.Contains(err.Error(), "Archipelago Launcher") {
		t.Fatalf("custom patch: %v", err)
	}
	if _, err := readAPPatchFile(write("broken.apsm", []byte("not a zip"))); err == nil {
		t.Fatal("read a broken patch")
	}
	// openGame leaves patch files to the window
	if _, err := openGame(gbPatch); err != errAPPatchFile {
		t.Fatalf("openGame on a patch: %v", err)
	}
	if exts := apBaseExts("gb"); strings.Join(exts, " ") != ".gb .gbc .zip" {
		t.Fatal(exts)
	}
}

func TestAPStatusText(t *testing.T) {
	g := &loadedGame{System: "gb", Name: "Tetris"}
	st := apbridge.Status{Clients: []string{}}
	if apShown(g, st) {
		t.Fatal("shown for a plain game with no client")
	}
	if !apShown(&loadedGame{System: "gb", Name: "AP_123_P1_Me"}, st) || !apShown(&loadedGame{AP: &apatch.Patch{}}, st) {
		t.Fatal("not shown for an Archipelago game")
	}
	if s := apStatusText("gb", st); s != "Archipelago: waiting for the BizHawk Client" {
		t.Fatal(s)
	}
	if s := apStatusText("snes", st); s != "Archipelago: waiting for the SNI Client" {
		t.Fatal(s)
	}
	st.BizHawkErr = "ports 43055-43059 are in use (is BizHawk running?)"
	if s := apStatusText("gb", st); s != "Archipelago: can't connect" {
		t.Fatal(s)
	}
	if h := apHelpText(g, st, true); !strings.Contains(h, "start the BizHawk Client") || !strings.Contains(h, "can't take the BizHawk Client's address") {
		t.Fatal(h)
	}
	st = apbridge.Status{Clients: []string{"BizHawk Client"}}
	if !apShown(g, st) || apStatusText("gb", st) != "Archipelago: BizHawk Client connected" {
		t.Fatal(apStatusText("gb", st))
	}
	ap := &loadedGame{System: "snes", AP: &apatch.Patch{Game: "Super Metroid", PlayerName: "Sam", Server: "archipelago.gg:38281"}}
	if h := apHelpText(ap, apbridge.Status{}, true); !strings.Contains(h, "Super Metroid · player Sam · server archipelago.gg:38281") ||
		!strings.Contains(h, "start the SNI Client") || !strings.Contains(h, "don't need SNI") {
		t.Fatal(h)
	}
	if h := apHelpText(nil, apbridge.Status{}, false); !strings.Contains(h, "Open your patch file") {
		t.Fatal(h)
	}
}

func TestROMHash(t *testing.T) {
	// SHA-1 of "abc"
	if h := romHash("gb", []byte("abc")); h != "A9993E364706816ABA3E25717850C26C9CD0D89D" {
		t.Fatal(h)
	}
	nes := append([]byte("NES\x1a\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"), "abc"...)
	if h := romHash("nes", nes); h != "A9993E364706816ABA3E25717850C26C9CD0D89D" {
		t.Fatal("iNES header counted:", h)
	}
	trained := append([]byte("NES\x1a\x01\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00"), make([]byte, 512)...)
	if h := romHash("nes", append(trained, "abc"...)); h != "A9993E364706816ABA3E25717850C26C9CD0D89D" {
		t.Fatal("trainer counted:", h)
	}
}

// gbAPROM: a Game Boy Color cartridge that counts frames in WRAM ($C000), so
// the test can see whether frames run.
func gbAPROM() []byte {
	rom := make([]byte, 0x8000)
	copy(rom[0x100:], []byte{0x00, 0xc3, 0x50, 0x01}) // nop; jp $150
	copy(rom[0x150:], []byte{
		0x3e, 0x01, 0xe0, 0xff, // ld a,1; ldh ($ff),a   (the VBlank interrupt)
		0xfb,       // ei
		0x76, 0x00, // halt; nop
		0x18, 0xfc, // jr -4 (back to halt)
	})
	copy(rom[0x40:], []byte{0xfa, 0x00, 0xc0, 0x3c, 0xea, 0x00, 0xc0, 0xd9}) // ld a,($c000); inc a; ld ($c000),a; reti
	rom[0x143] = 0x80                                                        // Game Boy Color
	rom[0x147], rom[0x149] = 0x03, 0x02
	return rom
}

func TestAPSessionBridge(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	h, card := &fakeHost{}, &fakeCard{}
	g := &loadedGame{System: "gb", ROM: gbAPROM(), Name: "AP_1_P1_Tester", AP: &apatch.Patch{Game: "Test Game"}}
	s, err := newSession(g, h, card, loadMap("gb", nil))
	if err != nil {
		t.Fatal(err)
	}
	game := apGameFor(g, s.core)
	if game == nil || game.System != "GBC" || game.Core != "gb" || game.Title != "Test Game" || game.Hash != romHash("gb", g.ROM) {
		t.Fatalf("game: %+v", game)
	}
	if apGameFor(&loadedGame{System: "atari2600"}, s.core) != nil {
		t.Fatal("a system with no Archipelago client")
	}
	if apGameFor(g, &testCore{}) != nil {
		t.Fatal("a core without core.Memory")
	}
	s.start()
	defer s.stop()

	hub := apbridge.NewHub(0, 0)
	tg := &apTarget{s: s}
	hub.SetGame(game, tg)
	a, b := net.Pipe()
	defer a.Close()
	go hub.ServeBizHawk(b)
	_ = a.SetDeadline(time.Now().Add(20 * time.Second))
	rw := bufio.NewReadWriter(bufio.NewReader(a), bufio.NewWriter(a))
	ask := func(reqs ...map[string]any) []map[string]any {
		t.Helper()
		line, _ := json.Marshal(reqs)
		rw.Write(append(line, '\n'))
		rw.Flush()
		s, err := rw.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		var res []map[string]any
		if err := json.Unmarshal([]byte(s), &res); err != nil || len(res) != len(reqs) {
			t.Fatalf("%q: %v", s, err)
		}
		return res
	}
	frames := func() int {
		r := ask(map[string]any{"type": "READ", "address": 0, "size": 1, "domain": "WRAM"})
		v, _ := base64.StdEncoding.DecodeString(r[0]["value"].(string))
		return int(v[0])
	}
	r := ask(map[string]any{"type": "SYSTEM"}, map[string]any{"type": "HASH"}, map[string]any{"type": "MEMORY_SIZE", "domain": "WRAM"},
		map[string]any{"type": "READ", "address": 0x143, "size": 1, "domain": "ROM"})
	if r[0]["value"] != "GBC" || r[1]["value"] != game.Hash || r[2]["value"] != float64(0x8000) || r[3]["value"] != "gA==" {
		t.Fatalf("handshake: %v", r)
	}
	// the game runs: its frame counter moves
	f0 := frames()
	time.Sleep(300 * time.Millisecond)
	if f1 := frames(); f1 == f0 {
		t.Fatalf("frames don't run (%d)", f1)
	}
	// LOCK holds it still; UNLOCK lets it go
	ask(map[string]any{"type": "LOCK"})
	time.Sleep(50 * time.Millisecond)
	f0 = frames()
	time.Sleep(300 * time.Millisecond)
	if f1 := frames(); f1 != f0 {
		t.Fatalf("frames ran while locked (%d -> %d)", f0, f1)
	}
	ask(map[string]any{"type": "UNLOCK"})
	time.Sleep(300 * time.Millisecond)
	if f1 := frames(); f1 == f0 {
		t.Fatal("frames don't run after UNLOCK")
	}
	// a lock that is never released ends by itself after 2 s, like BizHawk's
	ask(map[string]any{"type": "LOCK"})
	time.Sleep(2300 * time.Millisecond)
	f0 = frames()
	time.Sleep(300 * time.Millisecond)
	if f1 := frames(); f1 == f0 {
		t.Fatal("still held after 2 s")
	}
	// guarded write through the System Bus lands in WRAM bank 1 and the battery RAM
	b64 := func(b ...byte) string { return base64.StdEncoding.EncodeToString(b) }
	r = ask(map[string]any{"type": "GUARD", "address": 0x1000, "expected_data": b64(0), "domain": "WRAM"},
		map[string]any{"type": "WRITE", "address": 0xd000, "value": b64(0x42, 0x43), "domain": "System Bus"},
		map[string]any{"type": "WRITE", "address": 0x10, "value": b64(0x99), "domain": "CartRAM"},
		map[string]any{"type": "READ", "address": 0x1000, "size": 2, "domain": "WRAM"},
		map[string]any{"type": "DISPLAY_MESSAGE", "message": "Got the Moon Stone"})
	if r[0]["value"] != true || r[3]["value"] != b64(0x42, 0x43) || r[4]["type"] != "DISPLAY_MESSAGE_RESPONSE" {
		t.Fatalf("guarded write: %v", r)
	}
	var sram []byte
	s.call(func() error { sram = s.core.SRAM(); return nil })
	if len(sram) < 0x11 || sram[0x10] != 0x99 {
		t.Fatal("the battery RAM write didn't land")
	}
	h.mu.Lock()
	ev := strings.Join(h.events, ",")
	h.mu.Unlock()
	if !strings.Contains(ev, "ap-msg") {
		t.Fatalf("events: %s", ev)
	}
	// the game stops: the clients hear there's no game
	hub.ClearGame(tg)
	if r := ask(map[string]any{"type": "SYSTEM"}); r[0]["value"] != "NULL" {
		t.Fatal(r)
	}
	// a stopped session answers with an error, not a hang
	s.stop()
	if _, err := tg.Run([]apbridge.Op{{T: "size", D: "WRAM"}}); err == nil {
		t.Fatal("ran on a stopped game")
	}
	var _ core.Memory = s.core.(core.Memory)
}

// snesStub: what the SNES core needs to provide for the SNI Client: core.Memory
// with the usb2snes address space's domains.
type snesStub struct {
	*testCore
	mem core.Domains
}

func TestAPSNESHook(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	tc, _ := newTestCore([]byte{1}, core.Options{})
	wram, sram := make([]byte, 0x20000), make([]byte, 0x2000)
	rom := make([]byte, 0x80000)
	copy(rom[0x7fc0:], "SUPER METROID")
	stub := &snesStub{testCore: tc.(*testCore), mem: core.Domains{
		{Name: "CARTROM", Mem: rom}, {Name: "CARTRAM", Mem: sram}, {Name: "WRAM", Mem: wram},
		{Name: "VRAM", Mem: make([]byte, 0x10000)}, {Name: "CGRAM", Mem: make([]byte, 0x200)},
		{Name: "OAM", Mem: make([]byte, 0x220)}, {Name: "APURAM", Mem: make([]byte, 0x10000)},
	}}
	snesMaker := coreMakers["snes"]
	coreMakers["snes"] = func([]byte, core.Options) (core.Core, error) { return stub, nil }
	defer func() { coreMakers["snes"] = snesMaker }()
	g := &loadedGame{System: "snes", ROM: rom, Name: "AP_1_P3_Sam", AP: &apatch.Patch{Game: "Super Metroid"}}
	s, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	if err != nil {
		t.Fatal(err)
	}
	s.start()
	defer s.stop()
	game := apGameFor(g, s.core)
	if game == nil || game.System != "SNES" || game.Core != "snes" || game.Title != "Super Metroid" {
		t.Fatalf("game: %+v", game)
	}
	hub := apbridge.NewHub(0, 0)
	hub.SetGame(game, &apTarget{s: s})
	// what the SNI Client's GetAddress / PutAddress turn into
	wram[0x10] = 0x77
	res, err := hub.Run(append(apbridge.SNISplit(0xF50010, 1), apbridge.SNISplit(0x7fc0, 5)...))
	if err != nil || len(res) != 2 || !bytes.Equal(res[0].V, []byte{0x77}) || string(res[1].V) != "SUPER" {
		t.Fatalf("reads: %+v %v", res, err)
	}
	if _, err := hub.Run([]apbridge.Op{{T: "write", D: "CARTRAM", A: 0x1fff, V: []byte{1, 2}, Pad: true}}); err != nil || sram[0x1fff] != 1 {
		t.Fatalf("SRAM write: %v %x", err, sram[0x1fff])
	}
}

func (s *snesStub) MemoryDomains() []string                        { return s.mem.MemoryDomains() }
func (s *snesStub) MemorySize(d string) int                        { return s.mem.MemorySize(d) }
func (s *snesStub) ReadMemory(d string, a uint32, b []byte) error  { return s.mem.ReadMemory(d, a, b) }
func (s *snesStub) WriteMemory(d string, a uint32, b []byte) error { return s.mem.WriteMemory(d, a, b) }

// With the real Super Nintendo core: a SNES patch makes the game, and the
// SNI Client's reads and writes reach its memory.
func TestAPSNESRealCore(t *testing.T) {
	if coreMakers["snes"] == nil {
		t.Skip("no Super Nintendo core in this build")
	}
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	dir := t.TempDir()
	path := filepath.Join(dir, "AP_test_P1_Tester.aprptest")
	os.WriteFile(path, apTestData(t, "AP_test_P1_Tester.aprptest"), 0o644)
	p, err := readAPPatchFile(path)
	if err != nil {
		t.Fatal(err)
	}
	g, err := apMakeGame(p, path, apTestData(t, "procedure_base.bin"))
	if err != nil {
		t.Fatal(err)
	}
	s, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	if err != nil {
		t.Fatal(err)
	}
	s.start()
	defer s.stop()
	game := apGameFor(g, s.core)
	if game == nil || game.System != "SNES" || game.Title != "RetroPixels Test" {
		t.Fatalf("game: %+v", game)
	}
	hub := apbridge.NewHub(0, 0)
	hub.SetGame(game, &apTarget{s: s})
	res, err := hub.Run(apbridge.SNISplit(0x100, 16)) // what the patch wrote into the ROM
	if err != nil || string(res[0].V) != "RETROPIXELS TEST" {
		t.Fatalf("ROM read: %+v %v", res, err)
	}
	ops := apbridge.SNISplit(0xF51F00, 4)
	ops[0].T, ops[0].V = "write", []byte{1, 2, 3, 4}
	if _, err := hub.Run(ops); err != nil {
		t.Fatal(err)
	}
	if res, err := hub.Run(apbridge.SNISplit(0xF51F00, 4)); err != nil || !bytes.Equal(res[0].V, []byte{1, 2, 3, 4}) {
		t.Fatalf("WRAM: %+v %v", res, err)
	}
}
