package nds

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"os"
	"testing"
)

// testdata/selftest.nds is built from tools/pixelds/selftest (both
// processors check the machine and report through memory at 0x02380000).
func TestSelfTest(t *testing.T) {
	rom, err := os.ReadFile("testdata/selftest.nds")
	if err != nil {
		t.Skip(err)
	}
	m, err := New(rom, Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	var audio int
	var loud float32
	for f := 0; f < 120; f++ {
		m.RunFrame()
		for _, s := range m.TakeAudio() {
			audio++
			if s > loud {
				loud = s
			}
		}
	}
	res := func(i int) uint32 { return binary.LittleEndian.Uint32(m.mainRAM[0x380000+4*i:]) }
	names := []string{"ARM9 runs", "VBlank IRQ", "timer IRQ", "IPC sync", "IPC FIFO", "divider/sqrt", "DMA", "card read", "LZ77", "save chip", "ARM7 report"}
	for i, n := range names {
		if res(i) != 1 {
			t.Errorf("%s: result %d", n, res(i))
		}
	}
	t.Logf("ARM7 report %08x, done %x, arm9 pc %08x arm7 pc %08x", res(31), res(30), m.arm9.PC(), m.arm7.PC())
	if audio < 48000*2 || loud < 0.05 {
		t.Errorf("sound: %d samples, peak %.3f", audio, loud)
	}
	if p := os.Getenv("PIXELDS_SHOT"); p != "" {
		img := image.NewRGBA(image.Rect(0, 0, 256, 384))
		for i, c := range m.Frame() {
			img.Set(i%256, i/256, color.RGBA{uint8(c >> 16), uint8(c >> 8), uint8(c), 255})
		}
		f, _ := os.Create(p)
		png.Encode(f, img)
		f.Close()
	}
}

func TestStateRoundTrip(t *testing.T) {
	rom, err := os.ReadFile("testdata/selftest.nds")
	if err != nil {
		t.Skip(err)
	}
	m, _ := New(rom, Options{SampleRate: 48000})
	for f := 0; f < 10; f++ {
		m.RunFrame()
	}
	st, err := m.SaveState()
	if err != nil {
		t.Fatal(err)
	}
	for f := 0; f < 30; f++ {
		m.RunFrame()
	}
	want := append([]uint32(nil), m.Frame()...)
	wantRes := append([]byte(nil), m.mainRAM[0x380000:0x380080]...)
	m2, _ := New(rom, Options{SampleRate: 48000})
	if err := m2.LoadState(st); err != nil {
		t.Fatal(err)
	}
	for f := 0; f < 30; f++ {
		m2.RunFrame()
	}
	if !bytes.Equal(wantRes, m2.mainRAM[0x380000:0x380080]) {
		t.Error("results differ after loading the state")
	}
	for i := range want {
		if want[i] != m2.Frame()[i] {
			t.Errorf("picture differs at %d", i)
			break
		}
	}
	t.Logf("state is %d bytes", len(st))
}
