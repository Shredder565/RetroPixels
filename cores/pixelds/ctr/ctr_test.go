package ctr

import (
	"os"
	"testing"
)

// TestProgram runs the test program in testdata (built by
// tools/pixelds/ctrtest/build.sh), which checks the kernel and the
// services with raw system calls and writes its results at 0x00100100.
func TestProgram(t *testing.T) {
	file, err := os.ReadFile("testdata/ctrtest.3dsx")
	if err != nil {
		t.Fatal(err)
	}
	m, err := Load3DSX(file, Options{SDCard: t.TempDir(), SaveData: t.TempDir(), Log: func(s string) { t.Log(s) }})
	if err != nil {
		t.Fatal(err)
	}
	res := func(i int) uint32 { return m.mem.Read32(0x100100 + uint32(i)*4) }
	for f := 0; f < 30 && res(31) != 0xd04e; f++ {
		if err := m.RunFrame(); err != nil {
			t.Fatalf("frame %d: %v", f, err)
		}
	}
	if res(31) != 0xd04e {
		t.Fatalf("the program did not finish: results %x", res(31))
	}
	names := []string{"srv", "GSP", "HID", "linear memory", "VBlanks", "thread", "address arbiter", "timer", "timeout", "floating point", "WaitSynchronizationN"}
	want := []uint32{1, 1, 1, 1, 3, 1, 1, 1, 1, 1414, 1}
	for i, n := range names {
		if got := res(i); got != want[i] {
			t.Errorf("%s: result %d, want %d", n, got, want[i])
		}
	}

	m.SetKeys(KeyA|KeyStart, 0, 0)
	for f := 0; f < 3; f++ {
		if err := m.RunFrame(); err != nil {
			t.Fatal(err)
		}
	}
	if got := res(11) & 0xfff; got != KeyA|KeyStart {
		t.Errorf("buttons: %x, want %x", got, KeyA|KeyStart)
	}

	fr := m.Frame()
	px := func(x, y int) uint32 { return fr[y*TopW+x] & 0xffffff }
	if got := px(0, 0); got != 0x000000 {
		t.Errorf("top-left of the top screen: %06x, want 000000", got)
	}
	if got := px(399, 239); got != 0xffff00 {
		t.Errorf("bottom-right of the top screen: %06x, want ffff00", got)
	}
	if got := px(200, 360); got != 0x2040ff {
		t.Errorf("bottom screen: %06x, want 2040ff", got)
	}
}
