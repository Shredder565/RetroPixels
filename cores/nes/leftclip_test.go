package nes

import "testing"

// With the background's left 8 pixels hidden (PPUMASK bit 1 clear), the PPU
// fills that column with the backdrop colour. Super Mario Bros. 3 does this,
// and the sky blue backdrop showed as a bright line down the left edge. That
// column is now black; sprites drawn there still show, and the rest of the
// line and games that show all 256 pixels are unchanged.
func TestLeftColumnClipIsBlack(t *testing.T) {
	initPalette()
	p := newPPU(nil)
	p.palRam[0] = 0x31 // sky blue backdrop, as in SMB3
	p.palRam[0x11] = 0x16
	sky := p.pal[0x31]
	if sky == 0 {
		t.Fatal("palette not built")
	}

	p.mask = 0x18 // show background and sprites, both hidden in the left 8
	for x := 0; x < 16; x++ {
		p.pixel(x, 10)
	}
	for x := 0; x < 8; x++ {
		if got := p.screen[10*256+x]; got != 0 {
			t.Fatalf("x=%d: got %06x, want black", x, got)
		}
	}
	if got := p.screen[10*256+8]; got != sky {
		t.Fatalf("x=8: got %06x, want the backdrop %06x", got, sky)
	}

	// A sprite the game draws in the column (sprites shown there) still shows.
	p.mask = 0x1c
	p.lineSp[3] = 1 // sprite pixel, palette 0, colour 1
	p.pixel(3, 11)
	if got, want := p.screen[11*256+3], p.pal[0x16]; got != want {
		t.Fatalf("sprite in left column: got %06x, want %06x", got, want)
	}

	// Left column shown: the backdrop is drawn as before.
	p.mask = 0x1e
	p.lineSp = [256]uint16{}
	p.pixel(0, 12)
	if got := p.screen[12*256]; got != sky {
		t.Fatalf("unclipped: got %06x, want %06x", got, sky)
	}
}
