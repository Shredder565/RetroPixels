package gb

// The memory domains Archipelago's BizHawk Client reads and writes (core.Memory).

import (
	"bytes"
	"testing"

	"retropixels/core"
)

func TestMemoryDomains(t *testing.T) {
	c, err := New(gbMemROM(), core.Options{SampleRate: 48000})
	if err != nil {
		t.Fatal(err)
	}
	m, ok := c.(core.Memory)
	if !ok {
		t.Fatal("the core doesn't implement core.Memory")
	}
	c.RunFrame()
	want := map[string]int{"ROM": 0x8000, "WRAM": 0x2000, "VRAM": 0x2000, "OAM": 0xa0, "HRAM": 0x7f, "System Bus": 0x10000, "CartRAM": 0x2000}
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
	ram, ramAt, bus, busAt := "WRAM", uint32(0x123), "System Bus", uint32(0xc123)
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

// gbMemROM: a 32 KB cartridge with 8 KB of battery RAM (MBC1).
func gbMemROM() []byte {
	rom := make([]byte, 0x8000)
	rom[0x100], rom[0x101], rom[0x102], rom[0x103] = 0x00, 0xc3, 0x50, 0x01 // nop; jp $150
	rom[0x150], rom[0x151], rom[0x152] = 0xc3, 0x50, 0x01                   // jp $150
	rom[0x147], rom[0x149] = 0x03, 0x02
	return rom
}
