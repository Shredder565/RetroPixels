package nes

// The memory domains Archipelago's BizHawk Client reads and writes (core.Memory).

import (
	"bytes"
	"testing"

	"retropixels/core"
)

func TestMemoryDomains(t *testing.T) {
	c, err := New(exerciserROM(0, 32, 8, 1), core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	m, ok := c.(core.Memory)
	if !ok {
		t.Fatal("the core doesn't implement core.Memory")
	}
	c.RunFrame()
	want := map[string]int{"RAM": 0x800, "PRG ROM": 0x8000, "WRAM": 0x2000, "CIRAM (nametables)": 0x800, "OAM": 0x100, "PALRAM": 0x20, "System Bus": 0x10000, "CHR VROM": 0x2000, "Battery RAM": 0x2000}
	got := map[string]bool{}
	for _, d := range m.MemoryDomains() {
		got[d] = true
	}
	for d, n := range want {
		if !got[d] {
			t.Errorf("no %q domain (has %v)", d, m.MemoryDomains())
		} else if n >= 0 && m.MemorySize(d) != n {
			t.Errorf("%s: size %d, want %d", d, m.MemorySize(d), n)
		}
	}
	// a write to RAM shows through the bus (when there is one), and back
	ram, ramAt, bus, busAt := "RAM", uint32(0x345), "System Bus", uint32(0x1345)
	if err := m.WriteMemory(ram, ramAt, []byte{0xde, 0xad, 0xbe, 0xef}); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 4)
	if err := m.ReadMemory(ram, ramAt, buf); err != nil || !bytes.Equal(buf, []byte{0xde, 0xad, 0xbe, 0xef}) {
		t.Fatalf("%s read back: %x %v", ram, buf, err)
	}
	if bus != "" {
		if err := m.ReadMemory(bus, busAt, buf); err != nil || !bytes.Equal(buf, []byte{0xde, 0xad, 0xbe, 0xef}) {
			t.Fatalf("%s at %x: %x %v", bus, busAt, buf, err)
		}
		if err := m.WriteMemory(bus, busAt+1, []byte{0x42}); err != nil {
			t.Fatal(err)
		}
		if err := m.ReadMemory(ram, ramAt, buf); err != nil || buf[1] != 0x42 {
			t.Fatalf("%s write through %s: %x %v", ram, bus, buf, err)
		}
	}
	// out of range and unknown domains are refused, not a crash
	if err := m.ReadMemory(ram, uint32(m.MemorySize(ram)-2), buf); err == nil {
		t.Fatal("read past the end of " + ram)
	}
	if err := m.WriteMemory("No Such RAM", 0, buf); err == nil {
		t.Fatal("wrote an unknown domain")
	}
	// every domain reads whole, and the game runs on
	for _, d := range m.MemoryDomains() {
		n := m.MemorySize(d)
		if n > 1<<20 {
			n = 1 << 20
		}
		if err := m.ReadMemory(d, 0, make([]byte, n)); err != nil {
			t.Errorf("%s: %v", d, err)
		}
	}
	c.RunFrame()
}
