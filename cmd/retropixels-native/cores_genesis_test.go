package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"

	"retropixels/core"
)

func TestGenesisWiring(t *testing.T) {
	if !available("segaMD") {
		t.Fatal("no Genesis core")
	}
	dir := t.TempDir()
	// a 64K cartridge: vectors, the header, and a loop at $200
	rom := make([]byte, 0x10000)
	copy(rom[0:], []byte{0x00, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x02, 0x00})
	copy(rom[0x100:], "SEGA MEGA DRIVE ")
	copy(rom[0x1f0:], "JUE")
	copy(rom[0x200:], []byte{0x60, 0xfe}) // bra.s *
	smd := make([]byte, 512+len(rom))
	smd[8], smd[9] = 0xaa, 0xbb
	for b := 0; b < len(rom); b += 16384 {
		for i := 0; i < 8192; i++ {
			smd[512+b+i] = rom[b+i*2+1]
			smd[512+b+8192+i] = rom[b+i*2]
		}
	}
	for name, data := range map[string][]byte{
		"Game.md":  rom,
		"Game.gen": rom,
		"Game.smd": smd,
		"Game.bin": rom,
		"Game.zip": zipOf(t, map[string][]byte{"readme.txt": {1}, "Game (Homebrew).md": rom}),
		"Bin.zip":  zipOf(t, map[string][]byte{"game.bin": rom}),
	} {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, data, 0o644); err != nil {
			t.Fatal(err)
		}
		g, err := openGame(p)
		if err != nil || g.System != "segaMD" {
			t.Fatalf("%s: %v, system %v", name, err, g)
		}
	}
	for _, x := range []string{".md", ".gen", ".smd"} {
		if !slices.Contains(availableExts(), x) {
			t.Errorf("%s isn't listed", x)
		}
	}
	c, err := coreMakers["segaMD"](rom, core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	c.RunFrame()
	if in := c.Info(); !slices.Equal(in.Buttons, systemButtons["segaMD"]) || in.Players != 4 || !in.Stereo {
		t.Errorf("info %+v, controls window lists %v", in, systemButtons["segaMD"])
	}
	for name := range defaultMaps["segaMD"] {
		if !slices.Contains(systemButtons["segaMD"], name) {
			t.Errorf("default control for %q, which the core hasn't got", name)
		}
	}
	// the keys and controller buttons the web version uses
	m := loadMap("segaMD", nil)
	in := newInputState()
	bits := in.bits(0, systemButtons["segaMD"], m, map[string]bool{"a": true, "s": true, "d": true, "enter": true, "arrowup": true}, nil, false)
	if bits != 1|2|4|8|1<<8 {
		t.Errorf("A S D Enter Up: bits %b", bits)
	}
	pad := &PadState{Connected: true}
	pad.Buttons[padX], pad.Buttons[padLB], pad.Buttons[padRB], pad.Buttons[padY], pad.Buttons[padView] = true, true, true, true, true
	if bits := in.bits(3, systemButtons["segaMD"], m, nil, pad, false); bits != 1|16|32|64|128 {
		t.Errorf("controller 4 X LB Y RB View: bits %b", bits)
	}
}
