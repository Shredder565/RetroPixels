package model3

import "testing"

// The Real3D's DMA reads the program ROM through the bus: addresses from
// 0xFF800000 up are the fixed ROM, those below it the banked ROM. Star Wars
// Trilogy copies C-3PO's mesh this way, and reading the banked ROM instead
// left him invisible.
func TestBusReadsFixedAndBankedROM(t *testing.T) {
	m := &Machine{crom: make([]byte, 0x800000), bankedCROM: make([]byte, 0x2000000)}
	m.crom[0x272060] = 0xAB
	m.bankedCROM[0x272060] = 0x11
	m.bankedCROM[0x1000000+0x10] = 0x22
	m.cromBank = 0x1000000

	if got := m.Read32(0xFFA72060); got>>24 != 0xAB {
		t.Errorf("fixed ROM read gave %08X, want AB......", got)
	}
	if got := m.Read8(0xFF000010); got != 0x22 {
		t.Errorf("banked ROM read gave %02X, want 22", got)
	}
}
