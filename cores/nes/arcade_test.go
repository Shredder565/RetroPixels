package nes

import "testing"

// The arcade hooks: a program reads $4016 and shows the
// backdrop colour through the board's own palette.
func TestArcadeHooks(t *testing.T) {
	prg := make([]byte, 0x8000)
	code := []byte{
		0xad, 0x16, 0x40, 0x85, 0x10, // LDA $4016; STA $10
		0x4c, 0x05, 0x80, // JMP $8005
	}
	copy(prg, code)
	prg[0x7ffc], prg[0x7ffd] = 0x00, 0x80
	rom := append([]byte{'N', 'E', 'S', 0x1a, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, prg...)
	rom = append(rom, make([]byte, 0x2000)...)
	var pal [512]uint32
	for i := range pal {
		pal[i] = 0x123456
	}
	n, err := NewArcade(rom, 48000, ArcadeHooks{
		Palette: &pal,
		Port:    func(a, serial int) int { return 0xa0 | serial },
		RGBPPU:  true,
	})
	if err != nil {
		t.Fatal(err)
	}
	n.RunFrame()
	n.RunFrame()
	if n.ram[0x10] != 0xa0 {
		t.Errorf("$4016 read %02x", n.ram[0x10])
	}
	if p := n.Screen()[100*256+100]; p != 0x123456 {
		t.Errorf("pixel %06x", p)
	}
}
