package vsnes

import "retropixels/cores/arcade"

// The Vs. System's PPUs make RGB video straight from a colour PROM inside
// the chip instead of the NES's composite signal. The RP2C03's colours are
// the NES's colour numbers in order; each RP2C04 variant has the same 64
// colours shuffled into its own order (to stop games being moved between
// boards). Both tables are from the NESdev wiki: each entry is red, green,
// blue on a 0-7 scale.

// rgb2C03 is the RGB PPU's colour for each NES colour number.
var rgb2C03 = [64][3]uint8{
	{3, 3, 3}, {0, 1, 4}, {0, 0, 6}, {3, 2, 6}, {4, 0, 3}, {5, 0, 3}, {5, 1, 0}, {4, 2, 0},
	{3, 2, 0}, {1, 2, 0}, {0, 3, 1}, {0, 4, 0}, {0, 2, 2}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{5, 5, 5}, {0, 3, 6}, {0, 2, 7}, {4, 0, 7}, {5, 0, 7}, {7, 0, 4}, {7, 0, 0}, {6, 3, 0},
	{4, 3, 0}, {1, 4, 0}, {0, 4, 0}, {0, 5, 3}, {0, 4, 4}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{7, 7, 7}, {3, 5, 7}, {4, 4, 7}, {6, 3, 7}, {7, 0, 7}, {7, 3, 7}, {7, 4, 0}, {7, 5, 0},
	{6, 6, 0}, {3, 6, 0}, {0, 7, 0}, {2, 7, 6}, {0, 7, 7}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
	{7, 7, 7}, {5, 6, 7}, {6, 5, 7}, {7, 5, 7}, {7, 4, 7}, {7, 5, 5}, {7, 6, 4}, {7, 7, 2},
	{7, 7, 3}, {5, 7, 2}, {4, 7, 3}, {2, 7, 6}, {4, 6, 7}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
}

// order2C04_0002 is the RP2C04-0002's order: the NES colour number its
// colour n shows.
var order2C04_0002 = [64]uint8{
	0x2e, 0x27, 0x18, 0x39, 0x3a, 0x25, 0x1c, 0x31, 0x16, 0x13, 0x38, 0x34, 0x20, 0x23, 0x3c, 0x0b,
	0x0f, 0x21, 0x06, 0x3d, 0x1b, 0x29, 0x1e, 0x22, 0x1d, 0x24, 0x0e, 0x2b, 0x32, 0x08, 0x2e, 0x03,
	0x04, 0x36, 0x26, 0x33, 0x11, 0x1f, 0x10, 0x02, 0x14, 0x3f, 0x00, 0x09, 0x12, 0x2e, 0x28, 0x20,
	0x3e, 0x0d, 0x2a, 0x17, 0x0c, 0x01, 0x15, 0x19, 0x2e, 0x2c, 0x07, 0x37, 0x35, 0x05, 0x0a, 0x2d,
}

// level is a 0-7 level as 0-255.
func level(v uint8) int { return (int(v&7)*255 + 3) / 7 }

// buildPalette makes the PPU's 512 colours (64 colours x the 8 settings of
// the emphasis bits) from 64 RGB triples. On the RGB PPUs an emphasis bit
// turns its channel fully on: bit 0 red, 1 green, 2 blue.
func buildPalette(c [64][3]uint8) *[512]uint32 {
	var p [512]uint32
	for e := 0; e < 8; e++ {
		for i := 0; i < 64; i++ {
			r, g, b := level(c[i][0]), level(c[i][1]), level(c[i][2])
			if e&1 != 0 {
				r = 255
			}
			if e&2 != 0 {
				g = 255
			}
			if e&4 != 0 {
				b = 255
			}
			p[e*64+i] = arcade.RGB(r, g, b)
		}
	}
	return &p
}

// palette2C04_0002 is the RP2C04-0002's colours from the tables above.
func palette2C04_0002() [64][3]uint8 {
	var c [64][3]uint8
	for i, n := range order2C04_0002 {
		c[i] = rgb2C03[n]
	}
	return c
}

// palettePROM reads a dump of the PPU's colour PROM (192 bytes: red,
// green, blue for each colour, 0-7 each, as MAME's sets have it).
func palettePROM(d []byte) [64][3]uint8 {
	var c [64][3]uint8
	for i := range c {
		c[i] = [3]uint8{d[i*3] & 7, d[i*3+1] & 7, d[i*3+2] & 7}
	}
	return c
}
