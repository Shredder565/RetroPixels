package main

import (
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"retropixels/internal/supermodel"
)

// Only Star Wars Trilogy Arcade's two ROM set names go to Supermodel; every
// other file, zipped or not, keeps going where it went.
func TestIsSupermodelGame(t *testing.T) {
	for _, p := range []string{`C:\ROMs\swtrilgy.zip`, `D:\MAME\roms\SWTRILGYA.ZIP`, "swtrilgya.zip", "/home/x/roms/SwTrilgy.Zip"} {
		if !isSupermodelGame(p) {
			t.Errorf("%s: not sent to Supermodel", p)
		}
	}
	for _, p := range []string{`C:\ROMs\sf2.zip`, `C:\ROMs\swtrilgy.7z`, `C:\ROMs\swtrilgyp.zip`, `C:\swtrilgy.zip\pacman.zip`,
		`C:\ROMs\swtrilgy`, `C:\ROMs\star wars trilogy.zip`, `C:\Games\zelda.sfc`, ""} {
		if isSupermodelGame(p) {
			t.Errorf("%s: sent to Supermodel", p)
		}
	}
}

func TestSupermodelSettingsDefaults(t *testing.T) {
	var s SupermodelSettings
	if !s.fullscreen() || s.embed() || !s.flight() || s.smooth() != "medium" || s.InvertY || s.Wide {
		t.Fatalf("defaults differ from RetroPixels.exe's: %+v", s)
	}
	if len(smoothOrder) != len(supermodel.Smoothness) {
		t.Fatal("the menu doesn't list every smoothing")
	}
	for _, n := range smoothOrder {
		if _, ok := supermodel.Smoothness[n]; !ok {
			t.Fatalf("unknown smoothing %q in the menu", n)
		}
	}

	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	st := loadSettings()
	off := false
	st.Supermodel = SupermodelSettings{ROM: `D:\ROMs\swtrilgy.zip`, FlightStick: &off, Smooth: "heavy"}
	if err := saveSettings(st); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(settingsPath())
	if !strings.Contains(string(b), `"supermodel": {`) || !strings.Contains(string(b), `"rom": "D:\\ROMs\\swtrilgy.zip"`) {
		t.Fatalf("settings.json:\n%s", b)
	}
	got := loadSettings().Supermodel
	if got.ROM != st.Supermodel.ROM || got.flight() || got.smooth() != "heavy" || !got.fullscreen() {
		t.Fatalf("read back %+v", got)
	}
}

// The default start is RetroPixels.exe's: fullscreen in Supermodel's own
// window at the desktop's resolution, the Xbox controller, the flight stick
// feel at medium smoothing.
func TestSupermodelPlan(t *testing.T) {
	rom := `C:\ROMs\swtrilgy.zip`
	var s SupermodelSettings
	p := s.plan(rom, supermodel.Check{OK: true}, true, "", "")
	want := []string{rom, "-fullscreen", "-desktop-res", "-no-wide-screen", "-input-system=" + supermodel.InputSystem}
	if !slices.Equal(p.Args, want) || p.Embed {
		t.Fatalf("default args = %q, want %q", p.Args, want)
	}
	if !p.Controls.FlightStick || p.Controls.InvertY || p.Env != supermodel.FlightStickEnv(true, "medium") {
		t.Fatalf("default controls %+v env %q", p.Controls, p.Env)
	}

	// Force feedback when the drive board's ROM is there; recorded only
	// with both a folder and ffmpeg.
	p = s.plan(rom, supermodel.Check{OK: true, DriveBoard: true}, true, `C:\rec`, `C:\ff\ffmpeg.exe`)
	if !slices.Contains(p.Args, "-force-feedback") || !slices.Contains(p.Args, `-rp-record=C:\rec`) || !slices.Contains(p.Args, `-rp-ffmpeg=C:\ff\ffmpeg.exe`) {
		t.Fatalf("args %q", p.Args)
	}
	if p = s.plan(rom, supermodel.Check{OK: true}, true, `C:\rec`, ""); slices.ContainsFunc(p.Args, func(a string) bool { return strings.HasPrefix(a, "-rp-") }) {
		t.Fatalf("recorded without ffmpeg: %q", p.Args)
	}

	// Docked: a borderless window off-screen until it's docked, never fullscreen itself.
	on := true
	s.Embed = &on
	p = s.plan(rom, supermodel.Check{OK: true}, true, "", "")
	if !p.Embed || !slices.Contains(p.Args, "-borderless") || !slices.Contains(p.Args, "-window-pos=-20000,-20000") || slices.Contains(p.Args, "-fullscreen") {
		t.Fatalf("docked args %q", p.Args)
	}

	// Another Supermodel build (the official download) has no docking,
	// flight stick or recording: fullscreen at 1080p, the plain stick.
	p = s.plan(rom, supermodel.Check{OK: true}, false, `C:\rec`, `C:\ff\ffmpeg.exe`)
	if p.Embed || !slices.Contains(p.Args, "-res=1920,1080") || p.Controls.FlightStick || p.Env != "RP_FLIGHTSTICK=off" ||
		slices.ContainsFunc(p.Args, func(a string) bool { return strings.HasPrefix(a, "-rp-") }) {
		t.Fatalf("official build: %+v", p)
	}

	// The player's switches.
	off := false
	s = SupermodelSettings{Fullscreen: &off, FlightStick: &off, InvertY: true, Wide: true}
	p = s.plan(rom, supermodel.Check{OK: true}, true, "", "")
	if !slices.Contains(p.Args, "-res=992,768") || !slices.Contains(p.Args, "-wide-screen") || !p.Controls.InvertY ||
		p.Controls.FlightStick || p.Env != "RP_FLIGHTSTICK=off" {
		t.Fatalf("switches: %+v", p)
	}
}

func TestWebSupermodelROM(t *testing.T) {
	dir := t.TempDir()
	cfg := filepath.Join(dir, "config.json")
	if webSupermodelROM(cfg) != "" {
		t.Fatal("no config.json, but a ROM")
	}
	os.WriteFile(cfg, []byte(`{"supermodelRom":"D:\\Arcade\\swtrilgya.zip","pixeluFolders":["x"]}`), 0o644)
	if got := webSupermodelROM(cfg); got != `D:\Arcade\swtrilgya.zip` {
		t.Fatalf("webSupermodelROM = %q", got)
	}
}
