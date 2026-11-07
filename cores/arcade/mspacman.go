package arcade

// Ms. Pac-Man and Jr. Pac-Man: Pac-Man boards with extras, run by the Pac-Man
// board code (pacman.go) with b.kind set.
//
// Ms. Pac-Man is a Pac-Man board with General Computer's add-on board: three
// more ROMs (u5, u6, u7), scrambled, and a latch that swaps the Pac-Man ROMs
// for the patched, unscrambled Ms. Pac-Man code whenever the processor touches
// certain addresses ("traps"). After the swap the read comes from the new
// bank.
//
// Jr. Pac-Man has 24 KB of encrypted program ROM, 2 KB of video RAM with one
// colour per column instead of colour RAM, a playfield taller than the screen
// that scrolls (all but the 2 columns at each side), a second latch for
// palette, colour table, graphics bank and priority, and Pac-Man's sound.

const (
	pmPac = iota
	pmMs
	pmJr
)

// bitswap: out bit (n-1-i) = in bit b[i], like MAME's bitswap<n>.
func bitswap(v int, b ...int) int {
	out, n := 0, len(b)
	for i, s := range b {
		out |= (v >> s & 1) << (n - 1 - i)
	}
	return out
}

func msByte(v byte) byte { return byte(bitswap(int(v), 0, 4, 5, 7, 6, 3, 2, 1)) }

// msDecode builds the two banks from the 64 KB "cpu" region (Pac-Man ROMs at
// 0000-3fff, u5 at 8000, u6 at 9000, u7 at b000): bank 0 is Pac-Man (with its
// ROMs mirrored at 8000-bfff), bank 1 is Ms. Pac-Man.
func msDecode(rom []byte) (plain, dec []byte) {
	plain, dec = make([]byte, 0x10000), make([]byte, 0x10000)
	for i := 0; i < 0x1000; i++ {
		dec[i] = rom[i]
		dec[0x1000+i] = rom[0x1000+i]
		dec[0x2000+i] = rom[0x2000+i]
		dec[0x3000+i] = msByte(rom[0xb000+bitswap(i, 11, 3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0)])
	}
	for i := 0; i < 0x800; i++ {
		dec[0x8000+i] = msByte(rom[0x8000+bitswap(i, 8, 7, 5, 9, 10, 6, 3, 4, 2, 1, 0)])
		dec[0x8800+i] = msByte(rom[0x9800+bitswap(i, 3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0)])
		dec[0x9000+i] = msByte(rom[0x9000+bitswap(i, 3, 7, 9, 10, 8, 6, 5, 4, 2, 1, 0)])
		dec[0x9800+i] = rom[0x1800+i]
	}
	for i := 0; i < 0x1000; i++ {
		dec[0xa000+i] = rom[0x2000+i]
		dec[0xb000+i] = rom[0x3000+i]
	}
	// forty 8-byte patches into the Pac-Man code: {to, from}
	for _, p := range [][2]int{
		{0x0410, 0x8008}, {0x08e0, 0x81d8}, {0x0a30, 0x8118}, {0x0bd0, 0x80d8}, {0x0c20, 0x8120},
		{0x0e58, 0x8168}, {0x0ea8, 0x8198}, {0x1000, 0x8020}, {0x1008, 0x8010}, {0x1288, 0x8098},
		{0x1348, 0x8048}, {0x1688, 0x8088}, {0x16b0, 0x8188}, {0x16d8, 0x80c8}, {0x16f8, 0x81c8},
		{0x19a8, 0x80a8}, {0x19b8, 0x81a8}, {0x2060, 0x8148}, {0x2108, 0x8018}, {0x21a0, 0x81a0},
		{0x2298, 0x80a0}, {0x23e0, 0x80e8}, {0x2418, 0x8000}, {0x2448, 0x8058}, {0x2470, 0x8140},
		{0x2488, 0x8080}, {0x24b0, 0x8180}, {0x24d8, 0x80c0}, {0x24f8, 0x81c0}, {0x2748, 0x8050},
		{0x2780, 0x8090}, {0x27b8, 0x8190}, {0x2800, 0x8028}, {0x2b20, 0x8100}, {0x2b30, 0x8110},
		{0x2bf0, 0x81d0}, {0x2cc0, 0x80d0}, {0x2cd8, 0x80e0}, {0x2cf0, 0x81e0}, {0x2d60, 0x8160},
	} {
		copy(dec[p[0]:p[0]+8], dec[p[1]:p[1]+8])
	}
	for i := 0; i < 0x4000; i++ {
		plain[i] = rom[i]
		plain[0x8000+i] = rom[i]
	}
	return plain, dec
}

// msTrap: which bank touching address a selects (-1: none).
func msTrap(a int) int {
	switch {
	case a >= 0x0038 && a <= 0x003f, a >= 0x03b0 && a <= 0x03b7, a >= 0x1600 && a <= 0x1607,
		a >= 0x2120 && a <= 0x2127, a >= 0x3ff0 && a <= 0x3ff7, a >= 0x8000 && a <= 0x8007,
		a >= 0x97f0 && a <= 0x97f7:
		return 0
	case a >= 0x3ff8 && a <= 0x3fff:
		return 1
	}
	return -1
}

func (b *pacman) msRead(a int) uint8 {
	if a&0x4000 != 0 {
		return b.pmIO(a &^ 0xa000)
	}
	if t := msTrap(a); t >= 0 {
		b.bank = t
	}
	if b.bank == 1 {
		return b.drom[a]
	}
	return b.prom[a]
}

func (b *pacman) msWrite(a int, v uint8) {
	if a&0x4000 != 0 {
		b.pmWrite(a&^0xa000, v)
		return
	}
	if t := msTrap(a); t >= 0 {
		b.bank = t
	}
}

// jrDecrypt undoes the encryption PALs with David Caldwell's table of XOR
// values (run lengths from address 0).
func jrDecrypt(rom []byte) {
	table := [][2]int{
		{0x00C1, 0x00}, {0x0002, 0x80}, {0x0004, 0x00}, {0x0006, 0x80}, {0x0003, 0x00}, {0x0002, 0x80}, {0x0009, 0x00}, {0x0004, 0x80},
		{0x9968, 0x00}, {0x0001, 0x80}, {0x0002, 0x00}, {0x0001, 0x80}, {0x0009, 0x00}, {0x0002, 0x80}, {0x0009, 0x00}, {0x0001, 0x80},
		{0x00AF, 0x00}, {0x000E, 0x04}, {0x0002, 0x00}, {0x0004, 0x04}, {0x001E, 0x00}, {0x0001, 0x80}, {0x0002, 0x00}, {0x0001, 0x80},
		{0x0002, 0x00}, {0x0002, 0x80}, {0x0009, 0x00}, {0x0002, 0x80}, {0x0009, 0x00}, {0x0002, 0x80}, {0x0083, 0x00}, {0x0001, 0x04},
		{0x0001, 0x01}, {0x0001, 0x00}, {0x0002, 0x05}, {0x0001, 0x00}, {0x0003, 0x04}, {0x0003, 0x01}, {0x0002, 0x00}, {0x0001, 0x04},
		{0x0003, 0x01}, {0x0003, 0x00}, {0x0003, 0x04}, {0x0001, 0x01}, {0x002E, 0x00}, {0x0078, 0x01}, {0x0001, 0x04}, {0x0001, 0x05},
		{0x0001, 0x00}, {0x0001, 0x01}, {0x0001, 0x04}, {0x0002, 0x00}, {0x0001, 0x01}, {0x0001, 0x04}, {0x0002, 0x00}, {0x0001, 0x01},
		{0x0001, 0x04}, {0x0002, 0x00}, {0x0001, 0x01}, {0x0001, 0x04}, {0x0001, 0x05}, {0x0001, 0x00}, {0x0001, 0x01}, {0x0001, 0x04},
		{0x0002, 0x00}, {0x0001, 0x01}, {0x0001, 0x04}, {0x0002, 0x00}, {0x0001, 0x01}, {0x0001, 0x04}, {0x0001, 0x05}, {0x0001, 0x00},
		{0x01B0, 0x01}, {0x0001, 0x00}, {0x0002, 0x01}, {0x00AD, 0x00}, {0x0031, 0x01}, {0x005C, 0x00}, {0x0005, 0x01}, {0x604E, 0x00},
	}
	a := 0
	for _, t := range table {
		for j := 0; j < t[0] && a < len(rom); j++ {
			rom[a] ^= byte(t[1])
			a++
		}
	}
}

func (b *pacman) jrRead(a int) uint8 {
	switch {
	case a < 0x4000 || (a >= 0x8000 && a < 0xe000):
		return b.rom[a]
	case a < 0x4800:
		return b.jvram[a&0x7ff]
	case a < 0x5000:
		return b.jram[a&0x7ff]
	case a < 0x5100:
		switch a & 0xc0 {
		case 0x00:
			return b.readIn0()
		case 0x40:
			return b.readIn1()
		case 0x80:
			return b.readDsw()
		}
	}
	return 0xff
}

func (b *pacman) jrWrite(a int, v uint8) {
	switch {
	case a >= 0x4000 && a < 0x4800:
		b.jvram[a&0x7ff] = v
	case a >= 0x4800 && a < 0x5000:
		b.jram[a&0x7ff] = v
	case a >= 0x5070 && a < 0x5078:
		b.latch2[a&7] = v & 1
	case a == 0x5080:
		b.scroll = v
	case a >= 0x5000 && a < 0x5100:
		b.pmWrite(a, v)
	}
}

// jrColours: the palette is two PROMs of 4 bits (low and high nibble), only
// the first 32 entries used; pens 256-511 are the second palette bank.
func (b *pacman) jrColours(lo, hi []byte) {
	var colours [32]uint32
	for i := range colours {
		colours[i] = promColour(lo[i]&15 | hi[i]<<4)
	}
	for i := 0; i < 256; i++ {
		c := b.lookupProm[i] & 15
		b.jpens[i] = colours[c]
		b.jpens[i+256] = colours[16|c]
		b.clear[i] = c == 0
	}
}

// jrTile: the tile under screen pixel (x, y) (y already scrolled for the
// middle columns); its video RAM offset and its colour.
func (b *pacman) jrTile(col, row int) (int, int) {
	r, c := row+2, col-2
	var idx int
	if c < 0 || c >= 32 {
		if r&0x20 != 0 {
			idx = 0x77f
		} else {
			idx = r + ((c&3)|0x38)<<5
		}
	} else {
		idx = c + r<<5
	}
	ci := idx & 0x1f
	if idx >= 1792 {
		ci = idx + 0x80
	}
	return idx, int(b.jvram[ci&0x7ff] & 0x1f)
}

func (b *pacman) jrRender() {
	scr := b.screen
	charBank := int(b.latch2[4]) << 8
	bank := int(b.latch2[1])<<5 | int(b.latch2[0])<<6
	front := b.latch2[3] != 0 // the playfield over the sprites
	scroll := int(b.scroll)
	for y := 0; y < pmH; y++ {
		for col := 0; col < 36; col++ {
			ty := y
			if col >= 2 && col < 34 {
				ty = (y + scroll) % (54 * 8)
			}
			idx, colour := b.jrTile(col, ty>>3)
			code := (int(b.jvram[idx&0x7ff]) | charBank) & 0x1ff
			pal := (colour | bank) << 2
			row := b.tiles[code*64+(ty&7)*8:]
			d := y*pmW + col*8
			for x := 0; x < 8; x++ {
				if front {
					scr[d+x] = b.jpens[0]
				} else {
					scr[d+x] = b.jpens[pal|int(row[x])]
				}
			}
		}
	}
	sprBank := int(b.latch2[5]) << 6
	for n := 7; n >= 0; n-- {
		attr := int(b.jram[0x7f0+n*2])
		colour := (int(b.jram[0x7f1+n*2]&0x1f) | bank) << 2
		sx := 272 - int(b.spritePos[n*2+1])
		sy := int(b.spritePos[n*2]) - 31
		if n < 3 {
			sy++
		}
		base := ((attr >> 2) | sprBank) * 256
		fx, fy := 0, 0
		if attr&1 != 0 {
			fx = 15
		}
		if attr&2 != 0 {
			fy = 15
		}
		for _, ox := range []int{sx, sx - 256} {
			for y := 0; y < 16; y++ {
				yy := sy + y
				if yy < 0 || yy >= pmH {
					continue
				}
				for x := 0; x < 16; x++ {
					xx := ox + x
					if xx < 16 || xx >= 272 {
						continue
					}
					p := colour | int(b.sprites[base+((y^fy)<<4)+(x^fx)])
					if !b.clear[p&0xff] {
						scr[yy*pmW+xx] = b.jpens[p]
					}
				}
			}
		}
	}
	if front {
		for y := 0; y < pmH; y++ {
			for col := 0; col < 36; col++ {
				ty := y
				if col >= 2 && col < 34 {
					ty = (y + scroll) % (54 * 8)
				}
				idx, colour := b.jrTile(col, ty>>3)
				code := (int(b.jvram[idx&0x7ff]) | charBank) & 0x1ff
				pal := (colour | bank) << 2
				row := b.tiles[code*64+(ty&7)*8:]
				d := y*pmW + col*8
				for x := 0; x < 8; x++ {
					if row[x] != 0 {
						scr[d+x] = b.jpens[pal|int(row[x])]
					}
				}
			}
		}
	}
	if b.latch[3]&1 != 0 {
		for i, j := 0, len(scr)-1; i < j; i, j = i+1, j-1 {
			scr[i], scr[j] = scr[j], scr[i]
		}
	}
}

var pmDips = struct{ coins, lives, difficulty, cabinet, rack, test Dip }{
	coins:      Dip{ID: "coins", Label: "Coins", Default: 1, Options: []DipOption{{"1 coin, 1 game", 1}, {"1 coin, 2 games", 2}, {"2 coins, 1 game", 3}, {"Free play", 0}}},
	lives:      Dip{ID: "lives", Label: "Lives", Default: 2, Options: []DipOption{{"1", 0}, {"2", 1}, {"3", 2}, {"5", 3}}},
	difficulty: Dip{ID: "difficulty", Label: "Difficulty", Default: 1, Options: []DipOption{{"Normal", 1}, {"Hard", 0}}},
	cabinet:    Dip{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []DipOption{{"Upright", 0}, {"Cocktail table", 1}}},
	rack:       Dip{ID: "rack", Label: "Rack test (skip levels)", Default: 0, Options: offOpts},
	test:       Dip{ID: "test", Label: "Service mode (test screen)", Default: 0, Options: offOpts},
}

var pmButtons = []string{"Up", "Down", "Left", "Right", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"}

func init() {
	d := pmDips
	Register(&BoardDef{
		ID: "mspacman", Title: "Midway Ms. Pac-Man", Width: pmW, Height: pmH, Rotate: 90,
		FPS: float64(pmCPUHz) / (pmLineCyc * pmLines), Buttons: pmButtons,
		Dips: []Dip{d.coins, d.lives,
			{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"10,000 points", 0}, {"15,000 points", 1}, {"20,000 points", 2}, {"No extra life", 3}}},
			d.difficulty, d.cabinet, d.rack, d.test},
		Games: []Game{
			{ID: "mspacman", Title: "Ms. Pac-Man", Year: 1981, Maker: "Midway / General Computer Corporation", Files: files([]File{
				{Name: "pacman.6e", Size: 0x1000, Role: "cpu", Offset: 0x0000}, {Name: "pacman.6f", Size: 0x1000, Role: "cpu", Offset: 0x1000},
				{Name: "pacman.6h", Size: 0x1000, Role: "cpu", Offset: 0x2000}, {Name: "pacman.6j", Size: 0x1000, Role: "cpu", Offset: 0x3000},
				{Name: "u5", Size: 0x800, Role: "cpu", Offset: 0x8000}, {Name: "u6", Size: 0x1000, Role: "cpu", Offset: 0x9000},
				{Name: "u7", Size: 0x1000, Role: "cpu", Offset: 0xb000},
				{Name: "5e", Size: 0x1000, Role: "tiles", Offset: Next}, {Name: "5f", Size: 0x1000, Role: "sprites", Offset: Next},
			}, pacmanProms)},
		},
		New: func(g *Game, f Files, o Opts) (Board, error) { return newPacmanKind(g, f, o, pmMs) },
	})
	Register(&BoardDef{
		ID: "jrpacman", Title: "Bally Midway Jr. Pac-Man", Width: pmW, Height: pmH, Rotate: 90,
		FPS: float64(pmCPUHz) / (pmLineCyc * pmLines), Buttons: pmButtons,
		Dips: []Dip{d.coins, d.lives,
			{ID: "bonus", Label: "Extra life at", Default: 0, Options: []DipOption{{"10,000 points", 0}, {"15,000 points", 1}, {"20,000 points", 2}, {"30,000 points", 3}}},
			d.difficulty, d.cabinet, d.rack, d.test},
		Games: []Game{
			{ID: "jrpacman", Title: "Jr. Pac-Man", Year: 1983, Maker: "Bally Midway", Files: []File{
				{Name: "jr.pac-man_8d_11-9-83.8d", Alt: []string{"jrp8d.8d"}, Size: 0x2000, Role: "cpu", Offset: 0x0000},
				{Name: "jr.pac-man_8e_11-9-83.8e", Alt: []string{"jrp8e.8e"}, Size: 0x2000, Role: "cpu", Offset: 0x2000},
				{Name: "jr.pac-man_8h_11-9-83.8h", Alt: []string{"jrp8h.8h"}, Size: 0x2000, Role: "cpu", Offset: 0x8000},
				{Name: "jr.pac-man_8j_11-9-83.8j", Alt: []string{"jrp8j.8j"}, Size: 0x2000, Role: "cpu", Offset: 0xa000},
				{Name: "jr.pac-man_8k_11-9-83.8k", Alt: []string{"jrp8k.8k"}, Size: 0x2000, Role: "cpu", Offset: 0xc000},
				{Name: "jr.pac-man_2c_11-9-83.2c", Alt: []string{"jrp2c.2c"}, Size: 0x2000, Role: "tiles", Offset: 0},
				{Name: "jr.pac-man_2e_11-9-83.2e", Alt: []string{"jrp2e.2e"}, Size: 0x2000, Role: "sprites", Offset: 0},
				{Name: "a290-27axv-bxhd.9e", Size: 256, Role: "pal-lo", Offset: 0},
				{Name: "a290-27axv-cxhd.9f", Size: 256, Role: "pal-hi", Offset: 0},
				{Name: "a290-27axv-axhd.9p", Size: 256, Role: "lookup", Offset: 0},
				{Name: "a290-27axv-dxhd.7p", Size: 256, Role: "wave", Offset: 0},
				{Name: "a290-27axv-exhd.5s", Size: 256, Role: "timing", Offset: 0, Optional: true},
			}},
		},
		New: func(g *Game, f Files, o Opts) (Board, error) { return newPacmanKind(g, f, o, pmJr) },
	})
}
