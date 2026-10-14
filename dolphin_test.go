//go:build !windows

package main

import (
	"encoding/hex"
	"os"
	"os/exec"
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
	cfgMu.Lock()
	cfg = config{DolphinFolders: []string{filepath.Join(root, "Sonic Games")}}
	cfgMu.Unlock()
	exe := findDolphin()
	if !strings.HasSuffix(exe, filepath.Join("Dolphin-x64", "Dolphin.exe")) {
		t.Fatalf("findDolphin = %q", exe)
	}
	cfgMu.Lock()
	cfg.DolphinExe = exe
	cfg.DolphinFolders = nil
	cfgMu.Unlock()
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
	if o := dolphinAspect(); o.Aspect != 4.0/3 || !o.PostCrop {
		t.Errorf("aspect %+v", o)
	}
	if err := saveDolSettings(map[string]any{"res": float64(7)}); err == nil {
		t.Error("accepted an invalid resolution")
	}
}

// A 4:3 game recorded inside a 16:9 frame gets its black side bars trimmed.
func TestTrimBlackBars(t *testing.T) {
	ff := os.Getenv("RETRO_FFMPEG")
	if ff == "" {
		t.Skip("set RETRO_FFMPEG")
	}
	f := filepath.Join(t.TempDir(), "game.mp4")
	err := exec.Command(ff, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=960x720:rate=30:d=4",
		"-vf", "pad=1280:720:160:0:black,format=yuv420p", "-c:v", "libx264", "-g", "15", f).Run()
	if err != nil {
		t.Fatal(err)
	}
	trimBlackBars(f, "libx264")
	out, _ := exec.Command(ff, "-hide_banner", "-i", f).CombinedOutput()
	if !strings.Contains(string(out), "960x720") {
		t.Fatalf("not trimmed:\n%s", out)
	}
	g := filepath.Join(t.TempDir(), "full.mp4")
	exec.Command(ff, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:d=2", "-pix_fmt", "yuv420p", "-c:v", "libx264", g).Run()
	st1, _ := os.Stat(g)
	trimBlackBars(g, "libx264")
	st2, _ := os.Stat(g)
	if st1.ModTime() != st2.ModTime() {
		t.Error("touched a video with no bars")
	}
}
