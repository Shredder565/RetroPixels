package ctr

import (
	"encoding/binary"
	"os"
	"strings"
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
	checkProgram(t, m)
}

// TestGameFiles runs the same program packed as a game: a .cxi with
// compressed code, and that in a game card image and in a CIA.
func TestGameFiles(t *testing.T) {
	cxi, err := os.ReadFile("testdata/ctrtest.cxi")
	if err != nil {
		t.Fatal(err)
	}
	le := binary.LittleEndian
	// a game card image: the NCSD header, then the program at 0x4000
	ncsd := make([]byte, 0x4000+len(cxi))
	copy(ncsd[0x100:], "NCSD")
	le.PutUint32(ncsd[0x120:], 0x4000/0x200)
	le.PutUint32(ncsd[0x124:], uint32(len(cxi)/0x200))
	copy(ncsd[0x4000:], cxi)
	// a CIA: header, certificates, ticket, TMD (an unencrypted content), content
	cia := make([]byte, 0x2040)
	le.PutUint32(cia[0:], 0x2020)
	le.PutUint32(cia[0x08:], 0x40)
	le.PutUint32(cia[0x0c:], 0x40)
	tmd := make([]byte, 0x140+0xc4+0x900+0x30)
	binary.BigEndian.PutUint32(tmd, 0x10004) // RSA-2048 signature
	le.PutUint32(cia[0x10:], uint32(len(tmd)))
	le.PutUint64(cia[0x18:], uint64(len(cxi)))
	cia = append(cia, make([]byte, 0x80)...)
	cia = append(cia, tmd...)
	cia = append(cia, make([]byte, (64-len(cia)%64)%64)...)
	cia = append(cia, cxi...)
	for _, f := range []struct {
		name string
		data []byte
	}{{"cxi", cxi}, {"3ds", ncsd}, {"cia", cia}} {
		t.Run(f.name, func(t *testing.T) {
			m, err := Load3DS(f.data, Options{SDCard: t.TempDir(), SaveData: t.TempDir(), Log: func(s string) { t.Log(s) }})
			if err != nil {
				t.Fatal(err)
			}
			if m.productCode != "CTR-P-PXDS" || m.Title() != "ctrtest" {
				t.Errorf("product code %q, title %q", m.productCode, m.Title())
			}
			checkProgram(t, m)
		})
	}
	// an encrypted game is refused with a clear message
	enc := append([]byte(nil), cxi...)
	enc[0x18f] = 0
	if _, err := Load3DS(enc, Options{}); err == nil || !strings.Contains(err.Error(), "encrypted") {
		t.Errorf("encrypted game: %v", err)
	}
}

func checkProgram(t *testing.T, m *Machine) {
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

	if res(12) != 1 {
		t.Errorf("GPU: result %d, want 1", res(12))
	}
	// the GPU's triangle fills the bottom-right half of the top screen
	fr := m.Frame()
	px := func(x, y int) uint32 { return fr[y*TopW+x] & 0xffffff }
	if got := px(390, 230); got != 0x0080ff {
		t.Errorf("inside the GPU's triangle: %06x, want 0080ff", got)
	}
	if got := px(10, 10); got != 0 {
		t.Errorf("outside the GPU's triangle: %06x, want 000000", got)
	}
	if got := px(200, 360); got != 0x2040ff {
		t.Errorf("bottom screen: %06x, want 2040ff", got)
	}
}
