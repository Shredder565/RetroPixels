package sega16

import (
	"strconv"

	"retropixels/cores/arcade"
)

// gameDef is what differs between System 16B games: the ROM files (by role:
// cpu_hi/cpu_lo the 68000's even/odd bytes, tiles0-2 the tile bitplanes,
// spr_hi/spr_lo the sprite ROMs' high/low bytes, sound the Z80 program,
// samples the uPD7759 data), how sprite banks and the sound board's sample
// window map onto the ROMs, the inputs and the DIP switches.
type gameDef struct {
	game       arcade.Game
	dips       []arcade.Dip
	buttons    []string
	spriteBank func(n int) int   // sprite bank number -> 64K-word block of sprite ROM, or -1
	soundBank  func(v uint8) int // port 0x40 value -> sample window offset
	input      func(b *board, port int) uint8
	dsw        func(dips map[string]int, n int) uint8
}

// linearSprites: bank n is the n'th 128 KB of sprite data.
func linearSprites(n int) int { return n }

// linearSamples: the window's bank is port 0x40's low 6 bits, 16 KB each.
func linearSamples(v uint8) int { return int(v&0x3f) * 0x4000 }

// standardInput: the usual 16B inputs, active low.
// Port 0 (service): coin 1, coin 2, test, service, start 1, start 2.
// Ports 1 and 3 (players 1 and 2): button 3, button 1, button 2, -, down, up, right, left.
func standardInput(b *board, port int) uint8 {
	v := uint8(0)
	bit := func(on bool, m uint8) {
		if on {
			v |= m
		}
	}
	switch port {
	case 0:
		p1, p2 := b.pad(0), b.pad(1)
		bit(p1["Coin"], 0x01)
		bit(p2["Coin"] || p1["Coin 2"], 0x02)
		bit(b.dips["test"] == 1, 0x04)
		bit(p1["Service"], 0x08)
		bit(p1["Start (1P)"], 0x10)
		bit(p1["Start (2P)"] || p2["Start (1P)"], 0x20)
	case 1, 3:
		p := b.pad(port >> 1)
		bit(p["Button 3"], 0x01)
		bit(p["Button 1"], 0x02)
		bit(p["Button 2"], 0x04)
		bit(p["Down"], 0x10)
		bit(p["Up"], 0x20)
		bit(p["Right"], 0x40)
		bit(p["Left"], 0x80)
	}
	return ^v
}

// coinage: the 16 settings of Sega's standard coin switches (4 bits).
func coinage() []arcade.DipOption {
	return []arcade.DipOption{
		{Label: "1 coin, 1 credit", Value: 15}, {Label: "1 coin, 2 credits", Value: 14},
		{Label: "1 coin, 3 credits", Value: 13}, {Label: "1 coin, 4 credits", Value: 12},
		{Label: "1 coin, 5 credits", Value: 11}, {Label: "1 coin, 6 credits", Value: 10},
		{Label: "2 coins, 1 credit", Value: 9}, {Label: "3 coins, 1 credit", Value: 8},
		{Label: "4 coins, 1 credit", Value: 7}, {Label: "2 coins, 3 credits", Value: 6},
		{Label: "2 coins, 1 credit (5 for 3, 6 for 4)", Value: 5}, {Label: "2 coins, 1 credit (4 for 3)", Value: 4},
		{Label: "1 coin, 1 credit (5 for 6)", Value: 3}, {Label: "1 coin, 1 credit (4 for 5)", Value: 2},
		{Label: "1 coin, 1 credit (2 for 3)", Value: 1}, {Label: "Free play (when both are set to it)", Value: 0},
	}
}

var testDip = arcade.Dip{ID: "test", Label: "Service mode (test screen)", Default: 0,
	Options: []arcade.DipOption{{Label: "Off", Value: 0}, {Label: "On", Value: 1}}}

// ---- Altered Beast (1988) ----

var altbeastDips = []arcade.Dip{
	{ID: "coin_a", Label: "Coin A", Default: 15, Options: coinage()},
	{ID: "coin_b", Label: "Coin B", Default: 15, Options: coinage()},
	{ID: "credits", Label: "Credits needed", Default: 1, Options: []arcade.DipOption{
		{Label: "1 to start", Value: 1}, {Label: "2 to start", Value: 0}}},
	{ID: "demo", Label: "Sound in attract mode", Default: 1, Options: []arcade.DipOption{
		{Label: "On", Value: 1}, {Label: "Off", Value: 0}}},
	{ID: "lives", Label: "Lives", Default: 3, Options: []arcade.DipOption{
		{Label: "2", Value: 2}, {Label: "3", Value: 3}, {Label: "4", Value: 1}, {Label: "240 (free play)", Value: 0}}},
	{ID: "meter", Label: "Player energy meter", Default: 3, Options: []arcade.DipOption{
		{Label: "2", Value: 2}, {Label: "3", Value: 3}, {Label: "4", Value: 1}, {Label: "5", Value: 0}}},
	{ID: "difficulty", Label: "Difficulty", Default: 3, Options: []arcade.DipOption{
		{Label: "Easy", Value: 2}, {Label: "Normal", Value: 3}, {Label: "Hard", Value: 1}, {Label: "Hardest", Value: 0}}},
	testDip,
}

// altbeastDSW: switch bank A (n = 0) is the coinage, B the game settings.
func altbeastDSW(d map[string]int, n int) uint8 {
	if n == 0 {
		return uint8(d["coin_a"]&15 | (d["coin_b"]&15)<<4)
	}
	v := d["credits"]&1 | (d["lives"]&3)<<2 | (d["meter"]&3)<<4 | (d["difficulty"]&3)<<6
	if d["demo"] == 0 {
		v |= 2
	}
	return uint8(v)
}

func altbeastFiles() []arcade.File {
	f := []arcade.File{
		// 68000 program: MAME's altbeast2 (no MCU, no encryption)
		{Name: "11705", Alt: []string{"epr-11705.a7"}, Size: 0x20000, Role: "cpu_hi", Offset: 0},
		{Name: "11704", Alt: []string{"epr-11704.a5"}, Size: 0x20000, Role: "cpu_lo", Offset: 0},
		// the unencrypted sound program (MAME's altbeast2 has the MC-8123
		// encrypted epr-11686 instead, which this board can't run)
		{Name: "11671", Alt: []string{"epr-11671.a10"}, Size: 0x8000, Role: "sound", Offset: 0},
		{Name: "opr11672", Alt: []string{"opr-11672.a11"}, Size: 0x20000, Role: "samples", Offset: 0},
		{Name: "opr11673", Alt: []string{"opr-11673.a12"}, Size: 0x20000, Role: "samples", Offset: 0x20000},
		{Name: "11674", Alt: []string{"opr-11674.a14"}, Size: 0x20000, Role: "tiles0", Offset: 0},
		{Name: "11675", Alt: []string{"opr-11675.a15"}, Size: 0x20000, Role: "tiles1", Offset: 0},
		{Name: "11676", Alt: []string{"opr-11676.a16"}, Size: 0x20000, Role: "tiles2", Offset: 0},
		{Name: "315-5298.b9", Size: 235, Role: "pld", Optional: true},
	}
	// sprites: b5-b8 are the high bytes and b1-b4 the low bytes of banks 0-7
	// (each ROM pair holds two 64K-word banks)
	for i := 0; i < 4; i++ {
		lo, hi := 11677+i, 11681+i
		f = append(f,
			arcade.File{Name: "epr" + strconv.Itoa(hi) + ".b" + strconv.Itoa(i+5), Alt: []string{"epr-" + strconv.Itoa(hi) + ".b" + strconv.Itoa(i+5)},
				Size: 0x20000, Role: "spr_hi", Offset: i * 0x20000},
			arcade.File{Name: "epr" + strconv.Itoa(lo) + ".b" + strconv.Itoa(i+1), Alt: []string{"epr-" + strconv.Itoa(lo) + ".b" + strconv.Itoa(i+1)},
				Size: 0x20000, Role: "spr_lo", Offset: i * 0x20000})
	}
	return f
}

var games = []*gameDef{
	{
		game: arcade.Game{ID: "altbeast", Title: "Altered Beast", Year: 1988, Maker: "Sega", Files: altbeastFiles()},
		dips: altbeastDips, buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3",
			"Coin", "Coin 2", "Start (1P)", "Start (2P)", "Service"},
		spriteBank: linearSprites, soundBank: linearSamples, input: standardInput, dsw: altbeastDSW,
	},
}

func init() {
	def := &arcade.BoardDef{
		ID: "sega16b", Title: "Sega System 16B", Width: scrW, Height: scrH, FPS: fps, Players: 2,
		Buttons: games[0].buttons, Dips: games[0].dips,
	}
	for _, gd := range games {
		g := gd.game
		g.Dips, g.Buttons = gd.dips, gd.buttons
		def.Games = append(def.Games, g)
	}
	def.New = func(g *arcade.Game, files arcade.Files, opts arcade.Opts) (arcade.Board, error) {
		for _, gd := range games {
			if gd.game.ID == g.ID {
				return newBoard(gd, g, files, opts)
			}
		}
		return newBoard(games[0], g, files, opts)
	}
	arcade.Register(def)
}
