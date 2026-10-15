package snes

// The memory domains Archipelago's SNI Client reads and writes (core.Memory).

import (
	"bytes"
	"testing"

	"retropixels/core"
)

func fuzzVariant(t *testing.T, name string) []byte {
	t.Helper()
	for _, v := range fuzzVariants {
		if v.name == name {
			v.seed = 7
			return buildFuzzSNES(v)
		}
	}
	t.Fatalf("no fuzz variant %s", name)
	return nil
}

func TestMemoryDomains(t *testing.T) {
	for _, tc := range []struct {
		variant string
		cartRAM int  // CARTRAM's size
		battery bool // CARTRAM is the battery save
		sa1     bool
	}{
		{"lorom-sram8-fast", 0x2000, true, false},
		{"hirom", 0, false, false},
		{"sa1-bwram32", 0x8000, true, true},
		{"sa1", 0x2000, false, true},
		{"gsu2-battery", 0x8000, true, false},
	} {
		t.Run(tc.variant, func(t *testing.T) {
			c, err := New(fuzzVariant(t, tc.variant), core.Options{SampleRate: 48000})
			if err != nil {
				t.Fatal(err)
			}
			m, ok := c.(core.Memory)
			if !ok {
				t.Fatal("the core doesn't implement core.Memory")
			}
			c.RunFrame()
			s := c.(*SNES)
			want := map[string]int{"WRAM": 0x20000, "CARTROM": s.cart.size, "CARTRAM": tc.cartRAM, "VRAM": 0x10000,
				"CGRAM": 0x200, "OAM": 0x220, "APURAM": 0x10000, "System Bus": 0x1000000}
			if tc.sa1 {
				want["SA1 IRAM"] = 0x800
			}
			if len(m.MemoryDomains()) != len(want) {
				t.Errorf("domains %v", m.MemoryDomains())
			}
			for d, n := range want {
				if m.MemorySize(d) != n {
					t.Errorf("%s: size %d, want %d", d, m.MemorySize(d), n)
				}
			}
			// every domain reads whole
			for _, d := range m.MemoryDomains() {
				n := min(m.MemorySize(d), 1<<20)
				if err := m.ReadMemory(d, 0, make([]byte, n)); err != nil {
					t.Errorf("%s: %v", d, err)
				}
			}
			// WRAM and the System Bus: banks 7E/7F and the low mirror
			buf := make([]byte, 4)
			m.WriteMemory("WRAM", 0x1f00, []byte{0xde, 0xad, 0xbe, 0xef})
			if m.ReadMemory("System Bus", 0x7e1f00, buf); !bytes.Equal(buf, []byte{0xde, 0xad, 0xbe, 0xef}) {
				t.Fatalf("bus 7E:1F00 %x", buf)
			}
			if m.ReadMemory("System Bus", 0x001f00, buf); !bytes.Equal(buf, []byte{0xde, 0xad, 0xbe, 0xef}) {
				t.Fatalf("bus 00:1F00 %x", buf)
			}
			m.WriteMemory("System Bus", 0x7f0010, []byte{0x42})
			m.WriteMemory("System Bus", 0x000020, []byte{0x43})
			m.WriteMemory("System Bus", 0xc00030, []byte{0x44}) // ROM: dropped
			if s.wram[0x10010] != 0x42 || s.wram[0x20] != 0x43 {
				t.Fatal("bus writes to WRAM")
			}
			// the ROM header through CARTROM
			if m.ReadMemory("CARTROM", 0, buf); !bytes.Equal(buf, s.cart.rom[:4]) {
				t.Fatal("CARTROM")
			}
			// VRAM and CGRAM are words seen as little-endian bytes
			m.WriteMemory("VRAM", 0x100, []byte{0x34, 0x12})
			m.WriteMemory("CGRAM", 0x3, []byte{0x7f})
			if s.ppu.vram[0x80] != 0x1234 || s.ppu.cgram[1]>>8 != 0x7f {
				t.Fatalf("VRAM %04x CGRAM %04x", s.ppu.vram[0x80], s.ppu.cgram[1])
			}
			if m.ReadMemory("VRAM", 0x100, buf[:2]); buf[0] != 0x34 || buf[1] != 0x12 {
				t.Fatal("VRAM read")
			}
			// cartridge RAM writes reach the battery save
			if tc.cartRAM > 0 {
				end := uint32(tc.cartRAM - 2)
				if err := m.WriteMemory("CARTRAM", end, []byte{0x5a, 0xa5}); err != nil {
					t.Fatal(err)
				}
				if sr := c.SRAM(); tc.battery && (len(sr) < tc.cartRAM || sr[end] != 0x5a || sr[end+1] != 0xa5) {
					t.Fatalf("CARTRAM write not in the battery save (%d bytes)", len(sr))
				}
			}
			if err := m.ReadMemory("WRAM", 0x1fffe, buf); err == nil {
				t.Fatal("read past the end of WRAM")
			}
			if err := m.WriteMemory("No Such RAM", 0, buf); err == nil {
				t.Fatal("wrote an unknown domain")
			}
			c.RunFrame()
		})
	}
}
