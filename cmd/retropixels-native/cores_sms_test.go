package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"

	"retropixels/core"
)

func TestMasterSystemWiring(t *testing.T) {
	if !available("sms") {
		t.Fatal("no Master System core")
	}
	dir := t.TempDir()
	rom := make([]byte, 0x8000)
	rom[0], rom[1], rom[2] = 0xf3, 0x18, 0xfe // di; jr $
	copy(rom[0x7ff0:], "TMR SEGA")
	for name, data := range map[string][]byte{
		"Game.sms": rom,
		"Game.bin": rom,
		"Game.zip": zipOf(t, map[string][]byte{"readme.txt": {1}, "Game (Homebrew).sms": rom}),
	} {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
		g, err := openGame(p)
		if err != nil || g.System != "sms" {
			t.Fatalf("%s: %v, system %v", name, err, g)
		}
	}
	if !slices.Contains(availableExts(), ".sms") {
		t.Error(".sms isn't listed")
	}
	c, err := coreMakers["sms"](rom, core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	c.RunFrame()
	if !slices.Equal(c.Info().Buttons, systemButtons["sms"]) {
		t.Errorf("buttons %v, controls window lists %v", c.Info().Buttons, systemButtons["sms"])
	}
	for name := range defaultMaps["sms"] {
		if !slices.Contains(systemButtons["sms"], name) {
			t.Errorf("default control for %q, which the core hasn't got", name)
		}
	}
	m := loadMap("sms", nil)
	if len(m["Pause"].Pad) != 1 || m["Pause"].Pad[0] != padMenu || m["Up"].Keys[0] != "arrowup" {
		t.Errorf("map %+v", m)
	}
}
