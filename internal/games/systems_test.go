package games

import (
	"os"
	"path/filepath"
	"testing"
)

// A Wii U archive bigger than any ROM still opens: only its start is read.
func TestOpenBigWUA(t *testing.T) {
	p := filepath.Join(t.TempDir(), "Big Game.wua")
	f, err := os.Create(p)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(MaxROM * 3); err != nil { // sparse: no disk used
		t.Fatal(err)
	}
	f.Close()
	g, err := Open(p, OpenOptions{})
	if _, notHere := err.(NotYetError); notHere {
		return // the Wii U isn't registered in this test: the size was fine
	}
	if err != nil {
		t.Fatal(err)
	}
	if len(g.ROM) > 64 {
		t.Fatalf("%d bytes read", len(g.ROM))
	}
}
