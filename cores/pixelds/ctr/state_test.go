package ctr

import (
	"bytes"
	"hash/crc32"
	"os"
	"testing"
)

// A save state taken at any frame plays on exactly as the machine it was
// taken from: the picture, memory, processor and sound stay the same.
func TestSaveState(t *testing.T) {
	file, err := os.ReadFile("testdata/ctrtest.cxi")
	if err != nil {
		t.Fatal(err)
	}
	sd, save := t.TempDir(), t.TempDir()
	load := func() *Machine {
		m, err := Load3DS(file, Options{SDCard: sd, SaveData: save})
		if err != nil {
			t.Fatal(err)
		}
		return m
	}
	keys := func(f int) uint32 {
		if f%7 < 3 {
			return KeyA
		}
		return 0
	}
	for _, split := range []int{1, 2, 4, 8, 12, 20} {
		m1 := load()
		for f := 0; f < split; f++ {
			m1.SetKeys(keys(f), 0, 0)
			m1.RunFrame()
			if f == 0 { // the DSP's audio frames run too
				d := m1.svcs.byName["dsp::DSP"].(*dspService)
				d.running = true
				d.startFrames()
			}
		}
		st, err := m1.SaveState()
		if err != nil {
			t.Fatalf("frame %d: %v", split, err)
		}
		m2 := load()
		if err := m2.LoadState(st); err != nil {
			t.Fatalf("frame %d: loading: %v", split, err)
		}
		if split == 20 {
			t.Logf("state: %d KB", len(st)>>10)
		}
		for f := split; f < split+12; f++ {
			for _, m := range []*Machine{m1, m2} {
				m.SetKeys(keys(f), 0, 0)
				m.RunFrame()
			}
			a1, a2 := m1.TakeAudio(), m2.TakeAudio()
			if len(a1) != len(a2) {
				t.Fatalf("split %d frame %d: sound %d vs %d samples", split, f, len(a1), len(a2))
			}
			if crc32.ChecksumIEEE(f32bytes(a1)) != crc32.ChecksumIEEE(f32bytes(a2)) {
				t.Fatalf("split %d frame %d: the sound differs", split, f)
			}
			if !equal32(m1.Frame(), m2.Frame()) {
				t.Fatalf("split %d frame %d: the picture differs", split, f)
			}
			if m1.cpu.R != m2.cpu.R || m1.cpu.Cycles != m2.cpu.Cycles || m1.cpu.CPSR != m2.cpu.CPSR {
				t.Fatalf("split %d frame %d: the processor differs", split, f)
			}
			if !bytes.Equal(m1.mem.fcram, m2.mem.fcram) || !bytes.Equal(m1.mem.vram, m2.mem.vram) {
				t.Fatalf("split %d frame %d: memory differs", split, f)
			}
			if (m1.Err() == nil) != (m2.Err() == nil) {
				t.Fatalf("split %d frame %d: %v vs %v", split, f, m1.Err(), m2.Err())
			}
		}
	}
	// a state from another game is refused
	m1 := load()
	m1.RunFrame()
	st, _ := m1.SaveState()
	other, err := os.ReadFile("testdata/ctrtest.3dsx")
	if err != nil {
		t.Fatal(err)
	}
	m3, err := Load3DSX(other, Options{SDCard: sd, SaveData: save})
	if err != nil {
		t.Fatal(err)
	}
	if err := m3.LoadState(st); err == nil {
		t.Error("a state from another game loaded")
	}
}

func equal32(a, b []uint32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func f32bytes(a []float32) []byte {
	b := make([]byte, 0, len(a)*4)
	for _, v := range a {
		u := int32(v * 1e6)
		b = append(b, byte(u), byte(u>>8), byte(u>>16), byte(u>>24))
	}
	return b
}
