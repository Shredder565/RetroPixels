package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSNESControls(t *testing.T) {
	m := loadMap("snes", nil)
	for name, key := range map[string]string{"A": "x", "B": "z", "X": "s", "Y": "a", "L": "q", "R": "w", "Select": "shift", "Start": "enter"} {
		if bd := m[name]; len(bd.Keys) != 1 || bd.Keys[0] != key {
			t.Errorf("%s: keys %v", name, bd.Keys)
		}
	}
	if m["A"].Pad[0] != padB || m["B"].Pad[0] != padA {
		t.Error("Xbox B should be A and Xbox A should be B, as on the original pad")
	}
}

// snesChipROM: a do-nothing 32 KB LoROM game whose header names a DSP-3
// cartridge (a chip whose program the player supplies).
func snesChipROM() []byte {
	r := make([]byte, 0x8000)
	r[0] = 0x80 // BRA -2
	r[1] = 0xfe
	h := 0x7fc0
	copy(r[h:], []byte("SD RP GX TEST        "))
	r[h+0x15], r[h+0x16], r[h+0x17], r[h+0x19] = 0x20, 0x03, 0x05, 1
	r[0x7ffc], r[0x7ffd] = 0x00, 0x80 // reset vector $8000
	r[h+0x1c], r[h+0x1d] = 0xff, 0xff // checksum complement / checksum
	return r
}

// A game that needs a chip program is refused with a question for the
// window; the program chosen is kept and found next time.
func TestSNESFirmwareKept(t *testing.T) {
	t.Setenv("RETRO_NATIVE_DIR", t.TempDir())
	g := &loadedGame{System: "snes", ROM: snesChipROM(), Name: "chip game"}
	_, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	chip, file, msg, ok := needsFirmware(err)
	if !ok || chip != "DSP-3" || file != "dsp3.rom" || !strings.Contains(msg, "Choose the file") {
		t.Fatalf("want a DSP-3 program question, got %v (%q %q %q)", err, chip, file, msg)
	}
	// a file of the wrong size: asked again, with the reason
	g.Firmware = map[string][]byte{chip: make([]byte, 100)}
	_, err = newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	if _, _, msg, ok := needsFirmware(err); !ok || !strings.Contains(msg, "100 bytes") {
		t.Fatalf("wrong size: %v", err)
	}
	// my own stand-in program (the right size: 2048 x 24-bit words, 1024 x 16-bit)
	fw := make([]byte, 2048*3+1024*2)
	for i := range fw {
		fw[i] = byte(i * 7)
	}
	g.Firmware = map[string][]byte{chip: fw}
	s, err := newSession(g, &fakeHost{}, &fakeCard{}, loadMap("snes", nil))
	if err != nil {
		t.Fatal(err)
	}
	s.core.RunFrame()
	if err := saveFirmware(chip, fw); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(firmwareDir(), "DSP-3")); !bytes.Equal(b, fw) {
		t.Fatal("program not kept")
	}
	g2 := &loadedGame{System: "snes", ROM: snesChipROM(), Name: "chip game"}
	if _, err := newSession(g2, &fakeHost{}, &fakeCard{}, loadMap("snes", nil)); err != nil {
		t.Fatalf("kept program not used: %v", err)
	}
	if _, _, _, ok := needsFirmware(os.ErrNotExist); ok {
		t.Fatal("other errors aren't firmware questions")
	}
}
