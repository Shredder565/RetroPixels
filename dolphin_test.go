//go:build !windows

package main

import (
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Real header bytes from the user's files (first bytes at the offsets that matter).
var realHeaders = map[string]map[int64]string{
	"- Mario Games/013 - Super Mario Sunshine (USA).ciso": {
		0:      "4349534f0000200001010100",
		0x8000: "474d5345303100000000000000000000000000000000000000000000c2339f3d5375706572204d6172696f2053756e7368696e6500000000000000000000000000000000",
	},
	"Sonic Games/Gamecube- Sonic Heroes .iso": {
		0: "47395345385000000100000000000000000000000000000000000000c2339f3d534f4e4943204845524f455300000000000000000000000000000000000000000000000000",
	},
	"Sonic Games/Gamecube- Sonic Adventure 2 - Battle .iso": {
		0: "47534e45385000000100000000000000000000000000000000000000c2339f3d534f4e494320414456454e5455524520322042415454424c450000000000000000000000",
	},
	"Wii/WII   - Wii Sports [RSPE01].wbfs": {
		0:     "57424653000a100009150000010000",
		0x200: "5253504530310001000000000000000000000000000000005d1c9ea30000000053504f525453205041434b20666f72205245564f4c5554494f4e000000000000000000",
	},
	"Wii/WII   - Super Smash Bros. BrawlRSBE01.wbfs": {
		0:     "5742465300e9800009150000010000",
		0x200: "5253424530310002000000000000000000000000000000005d1c9ea300000000537570657220536d6173682042726f732e20427261776c0000000000000000000000",
	},
	"Wii/WAD   - Mega Man 9 (USA) (WiiWare).wad": {
		0: "000000204973000000000a0000000000000002a40000037000f1b140",
	},
	"Wii/WAD   - Mega Man 9 (USA) (WiiWare) (DLC).wad": {
		0: "000000204973000000000a0000000000000002a40000037000f1b140",
	},
	"- Turtle Games/TMNT (USA) (En,Fr,Es)/TMNT (USA) (En,Fr,Es).iso": {
		0:      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
		0x8000: "0143443030310100504c415953544154494f4e",
	},
	"- Turtle Games/CD - TMNT_Win_ISO_EN/OGD_TMNT_ISO.iso": {0x8000: "014344303031010057494e3332"},
	"Wii U/WII  U -Hyrule Warriors (US).wua":               {0: "00"},
	"002 NES Complete/Super Mario Bros.nes":                {0: "4e45531a"},
}

func makeRoms(t *testing.T) string {
	root := filepath.Join(t.TempDir(), "Roms NES-switch")
	for name, parts := range realHeaders {
		p := filepath.Join(root, filepath.FromSlash(name))
		os.MkdirAll(filepath.Dir(p), 0o755)
		f, _ := os.Create(p)
		for off, h := range parts {
			b, _ := hex.DecodeString(h)
			f.WriteAt(b, off)
		}
		f.WriteAt([]byte{0}, 0x9000)
		f.Close()
	}
	os.MkdirAll(filepath.Join(root, "Wii", "- Wii", "Dolphin-x64"), 0o755)
	os.WriteFile(filepath.Join(root, "Wii", "- Wii", "Dolphin-x64", "Dolphin.exe"), []byte("#!/bin/sh\nsleep 30\n"), 0o755)
	return root
}

func TestDolphinLibrary(t *testing.T) {
	root := makeRoms(t)
	os.Setenv("XDG_CONFIG_HOME", t.TempDir())
	t.Setenv("HOME", t.TempDir())
	// The Dolphin the player used before is never started, but its games
	// folders are still found near it.
	old := filepath.Join(root, "Wii", "- Wii", "Dolphin-x64", "Dolphin.exe")
	cfgMu.Lock()
	cfg = config{DolphinExe: old}
	cfgMu.Unlock()
	if exe := dolphinExe(); exe != "" {
		t.Fatalf("dolphinExe = %q: a Dolphin.exe isn't PixelU's engine", exe)
	}
	if err := launchDolphinGame(filepath.Join(root, "Sonic Games", "Gamecube- Sonic Heroes .iso")); err != errNoWiiEngine {
		t.Fatalf("launch without the engine: %v", err)
	}
	folders := guessDolphinFolders()
	t.Logf("folders: %v", folders)
	if len(folders) != 3 {
		t.Fatalf("want 3 folders (Mario, Sonic, Wii), got %v", folders)
	}
	cfgMu.Lock()
	cfg.DolphinFolders = folders
	cfgMu.Unlock()
	got := map[string]string{}
	for _, g := range dolphinLibrary() {
		got[g.Name] = g.Platform + " " + g.ID
	}
	want := map[string]string{
		"Super Mario Sunshine":      "GameCube GMSE01",
		"Sonic Heroes":              "GameCube G9SE8P",
		"Sonic Adventure 2: Battle": "GameCube GSNE8P",
		"Wii Sports":                "Wii RSPE01",
		"Super Smash Bros. Brawl":   "Wii RSBE01",
		"Mega Man 9":                "WiiWare ",
	}
	for k, v := range got {
		t.Logf("  %-28s %s", k, v)
	}
	for k, v := range want {
		if g, ok := got[k]; !ok || !strings.HasPrefix(g, strings.TrimSpace(v)) {
			t.Errorf("missing/wrong %q: got %q want %q", k, g, v)
		}
	}
	if len(got) != len(want) {
		t.Errorf("got %d games, want %d (TMNT PC/PS2 discs, DLC and Wii U must be excluded)", len(got), len(want))
	}
}

func TestDolphinNames(t *testing.T) {
	for f, want := range map[string]string{"WII   - Punch-Out! [R7PE01].wbfs": "Punch-Out!", "hi-temunitu.iso": "Some Game", "GMSE01.iso": "Some Game", "Cuphead.iso": "Cuphead"} {
		if got := dolDisplayName(f, discInfo{ID: "R7PE01", Title: "SOME GAME"}); got != want {
			t.Errorf("%s -> %q want %q", f, got, want)
		}
	}
	if niceTitle("PUNCH-OUT!") != "Punch-Out!" {
		t.Error(niceTitle("PUNCH-OUT!"))
	}
}

func TestDolphinIniAndPads(t *testing.T) {
	dir := t.TempDir()
	os.Setenv("RETRO_DOLPHIN_USER", dir)
	defer os.Unsetenv("RETRO_DOLPHIN_USER")
	if err := setupXboxPads(false); err != nil {
		t.Fatal(err)
	}
	if padState("GCPadNew.ini", "GCPad1") != "xbox" || padState("WiimoteNew.ini", "Wiimote1") != "xbox" {
		t.Fatal("pads not set up")
	}
	b, _ := os.ReadFile(dolphinIni("GCPadNew.ini"))
	if !strings.Contains(string(b), "[GCPad1]\r\nDevice = XInput/0/Gamepad\r\nButtons/A = `Button A`") {
		t.Fatalf("GCPadNew.ini:\n%s", b)
	}
	os.WriteFile(dolphinIni("GCPadNew.ini"), []byte("[GCPad1]\r\nDevice = DInput/0/Keyboard Mouse\r\nButtons/A = X\r\n[GCPad2]\r\nDevice = XInput/1/Gamepad\r\n"), 0o644)
	setupXboxPads(false)
	if padState("GCPadNew.ini", "GCPad1") != "custom" {
		t.Fatal("overwrote a custom mapping")
	}
	setupXboxPads(true)
	b, _ = os.ReadFile(dolphinIni("GCPadNew.ini"))
	if padState("GCPadNew.ini", "GCPad1") != "xbox" || !strings.Contains(string(b), "[GCPad2]\r\nDevice = XInput/1/Gamepad") {
		t.Fatalf("forced set-up wrong:\n%s", b)
	}
	os.WriteFile(dolphinIni("GFX.ini"), []byte("[Settings]\r\nInternalResolution = 1\r\nShaderCompilationMode = 2\r\n[Hardware]\r\nVSync = False\r\n"), 0o644)
	if err := saveDolSettings(map[string]any{"res": float64(3), "aspect": float64(2), "vsync": true, "widehack": true, "backend": "D3D12"}); err != nil {
		t.Fatal(err)
	}
	g, _ := os.ReadFile(dolphinIni("GFX.ini"))
	d, _ := os.ReadFile(dolphinIni("Dolphin.ini"))
	for _, want := range []string{"InternalResolution = 3", "AspectRatio = 2", "wideScreenHack = True", "ShaderCompilationMode = 2", "VSync = True"} {
		if !strings.Contains(string(g), want) {
			t.Errorf("GFX.ini missing %q", want)
		}
	}
	if !strings.Contains(string(d), "GFXBackend = D3D12") {
		t.Error("backend not saved")
	}
	if err := saveDolSettings(map[string]any{"res": float64(7)}); err == nil {
		t.Error("accepted an invalid resolution")
	}
}

// fakeEngine makes a PixelU folder holding the Wii / GameCube engine (a
// shell script standing in for wii\PixelU-Wii.exe, with portable.txt next
// to it, as shipped) and chooses that folder's PixelU.exe.
func fakeEngine(t *testing.T, script string) string {
	t.Helper()
	dir := filepath.Join(t.TempDir(), "PixelU")
	os.MkdirAll(filepath.Join(dir, "wii"), 0o755)
	os.WriteFile(filepath.Join(dir, "PixelU.exe"), []byte("x"), 0o755)
	exe := filepath.Join(dir, "wii", "PixelU-Wii.exe")
	os.WriteFile(exe, []byte(script), 0o755)
	os.WriteFile(filepath.Join(dir, "wii", "portable.txt"), nil, 0o644)
	cfgMu.Lock()
	cfg.PixelUExe = filepath.Join(dir, "PixelU.exe")
	cfgMu.Unlock()
	return exe
}

// GameCube and Wii games start in PixelU's engine, with its own user folder,
// and the player's old Dolphin settings come over the first time.
func TestWiiEngineLaunchAndImport(t *testing.T) {
	root := makeRoms(t)
	t.Setenv("HOME", t.TempDir())
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	t.Setenv("RETRO_DOLPHIN_USER", "")
	old := t.TempDir() // the old Documents\Dolphin Emulator
	t.Setenv("RETRO_OLD_DOLPHIN_USER", old)
	os.MkdirAll(filepath.Join(old, "Config"), 0o755)
	os.WriteFile(filepath.Join(old, "Config", "GCPadNew.ini"), []byte("[GCPad1]\r\nDevice = DInput/0/Keyboard Mouse\r\n"), 0o644)
	os.WriteFile(filepath.Join(old, "Config", "GFX.ini"), []byte("[Settings]\r\nInternalResolution = 3\r\n"), 0o644)
	os.MkdirAll(filepath.Join(old, "GC"), 0o755)
	os.WriteFile(filepath.Join(old, "GC", "Card A.USA.raw"), []byte("card"), 0o644)
	args := filepath.Join(t.TempDir(), "args")
	t.Setenv("FAKE_ARGS", args)
	old2 := filepath.Join(root, "Wii", "- Wii", "Dolphin-x64", "Dolphin.exe")
	cfgMu.Lock()
	cfg = config{DolphinExe: old2, DolphinFolders: []string{filepath.Join(root, "- Mario Games")}}
	cfgMu.Unlock()
	exe := fakeEngine(t, "#!/bin/sh\necho \"$@\" > \"$FAKE_ARGS\"\n")
	if got := dolphinExe(); got != exe {
		t.Fatalf("dolphinExe = %q want %q", got, exe)
	}
	if dolphinExeName() != "PixelU-Wii.exe" {
		t.Fatalf("exe name %q", dolphinExeName())
	}
	user := filepath.Join(filepath.Dir(exe), "User")
	if dolphinUserDir() != user {
		t.Fatalf("user folder %q", dolphinUserDir())
	}
	off := false
	cfgMu.Lock()
	cfg.AutoRecord = &off
	cfgMu.Unlock()
	games := dolphinLibrary()
	if len(games) != 1 {
		t.Fatalf("library %+v", games)
	}
	if err := launchDolphinGame(games[0].Path); err != nil {
		t.Fatal(err)
	}
	waitIdle(t)
	a, _ := os.ReadFile(args)
	if !strings.HasPrefix(string(a), "-b -e "+games[0].Path+" -C Dolphin.DSP.Volume=100") {
		t.Fatalf("args %q", a)
	}
	// The old controller set-up came over and was kept (not replaced with
	// the Xbox layout); the Wii Remote, which had none, got the Xbox layout.
	if padState("GCPadNew.ini", "GCPad1") != "custom" || padState("WiimoteNew.ini", "Wiimote1") != "xbox" {
		t.Fatalf("pads: gc %s wii %s", padState("GCPadNew.ini", "GCPad1"), padState("WiimoteNew.ini", "Wiimote1"))
	}
	if v, _ := readIni(dolphinIni("GFX.ini")).Get("Settings", "InternalResolution"); v != "3" {
		t.Errorf("GFX.ini not brought over: %q", v)
	}
	if b, _ := os.ReadFile(filepath.Join(user, "GC", "Card A.USA.raw")); string(b) != "card" {
		t.Errorf("memory card: %q", b)
	}
	if _, err := os.Stat(filepath.Join(old, "Config", "WiimoteNew.ini")); err == nil {
		t.Error("wrote into the old Dolphin's folder")
	}
	// Only once: a later change in the old folder isn't copied again.
	os.WriteFile(filepath.Join(old, "Config", "Hotkeys.ini"), []byte("x"), 0o644)
	os.Remove(filepath.Join(user, "Config", "GFX.ini"))
	importOldDolphin()
	if _, err := os.Stat(filepath.Join(user, "Config", "GFX.ini")); err == nil {
		t.Error("brought settings over a second time")
	}
}
