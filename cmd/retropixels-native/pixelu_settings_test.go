package main

import (
	"os"
	"slices"
	"strings"
	"testing"

	"retropixels/internal/pixelu"
)

func TestPixelUSettingsDefaults(t *testing.T) {
	var p PixelUSettings
	if !p.fullscreen() || !p.embed() {
		t.Fatal("defaults differ from RetroPixels.exe's (fullscreen, in the window)")
	}
	off := false
	p.Fullscreen, p.Embed = &off, &off
	if p.fullscreen() || p.embed() {
		t.Fatal("switches")
	}

	// kept in settings.json and read back
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	s := loadSettings()
	s.PixelU = p
	s.PixelU.Exe = `C:\PixelU\PixelU.exe`
	if err := saveSettings(s); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(settingsPath())
	if !strings.Contains(string(b), `"pixelu": {`) || !strings.Contains(string(b), `"playInWindow": false`) {
		t.Fatalf("settings.json:\n%s", b)
	}
	got := loadSettings().PixelU
	if got.Exe != s.PixelU.Exe || got.embed() || got.fullscreen() {
		t.Fatalf("read back %+v", got)
	}
}

// Docked, PixelU shows only the game picture (--rp-embed) and isn't
// fullscreen itself (this window is); in its own window it takes -f.
func TestPixelULaunchArgs(t *testing.T) {
	on, off := true, false
	game := `D:\Wii U\Mario Kart 8.wua`
	for _, c := range []struct {
		name        string
		full, embed *bool
		want        []string
	}{
		{"defaults", nil, nil, []string{"-g", game, "--rp-embed"}},
		{"docked, not fullscreen", &off, &on, []string{"-g", game, "--rp-embed"}},
		{"own window, fullscreen", &on, &off, []string{"-g", game, "-f"}},
		{"own window", &off, &off, []string{"-g", game}},
	} {
		p := PixelUSettings{Fullscreen: c.full, Embed: c.embed}
		o := p.launchOptions()
		if got := pixelu.GameArgs(game, o); !slices.Equal(got, c.want) {
			t.Errorf("%s: %q, want %q", c.name, got, c.want)
		}
		if o.Embed && o.Fullscreen {
			t.Errorf("%s: docked and PixelU fullscreen at once", c.name)
		}
	}
}
