package dkong3

import "retropixels/cores/arcade"

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var offOn = []arcade.DipOption{opt("Off", 0), opt("On", 1)}

// dips: the board's two banks of switches (7d00 and 7d80), as the game reads
// them.
var dips = []arcade.Dip{
	{ID: "coins", Label: "Coins", Default: 0, Options: []arcade.DipOption{
		opt("1 coin, 1 game", 0), opt("2 coins, 1 game", 4), opt("3 coins, 1 game", 2), opt("1 coin, 2 games", 6),
		opt("1 coin, 3 games", 1), opt("1 coin, 4 games", 3), opt("1 coin, 5 games", 5), opt("1 coin, 6 games", 7)}},
	{ID: "lives", Label: "Lives", Default: 0, Options: []arcade.DipOption{opt("3", 0), opt("4", 1), opt("5", 2), opt("6", 3)}},
	{ID: "bonus", Label: "First extra life at", Default: 0, Options: []arcade.DipOption{
		opt("30,000", 0), opt("40,000", 1), opt("50,000", 2), opt("None", 3)}},
	{ID: "bonus2", Label: "Second extra life at", Default: 0, Options: []arcade.DipOption{
		opt("30,000", 0), opt("40,000", 1), opt("50,000", 2), opt("None", 3)}},
	{ID: "difficulty", Label: "Difficulty", Default: 0, Options: []arcade.DipOption{
		opt("1 (easiest)", 0), opt("2", 1), opt("3", 2), opt("4 (hardest)", 3)}},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail table", 1)}},
	{ID: "service", Label: "Service mode (test screens)", Default: 0, Options: offOn},
	{ID: "sw4", Label: "Switch 2-4 (unused)", Default: 0, Options: offOn},
	{ID: "sw5", Label: "Switch 2-5 (unused)", Default: 0, Options: offOn},
	{ID: "sw6", Label: "Switch 2-6 (unused)", Default: 0, Options: offOn},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "dkong3", Title: "Nintendo Donkey Kong 3", Width: scrW, Height: scrH, Rotate: 270, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    dips,
		Games: []arcade.Game{
			{ID: "dkong3", Title: "Donkey Kong 3", Year: 1983, Maker: "Nintendo of America", Files: []arcade.File{
				{Name: "dk3c.7b", Size: 0x2000, Role: "cpu", Offset: 0x0000},
				{Name: "dk3c.7c", Size: 0x2000, Role: "cpu", Offset: 0x2000},
				{Name: "dk3c.7d", Size: 0x2000, Role: "cpu", Offset: 0x4000},
				{Name: "dk3c.7e", Size: 0x2000, Role: "cpu", Offset: 0x6000},
				{Name: "dk3c.5l", Size: 0x2000, Role: "sound1", Offset: 0},
				{Name: "dk3c.6h", Size: 0x2000, Role: "sound2", Offset: 0},
				{Name: "dk3v.3n", Size: 0x1000, Role: "tiles", Offset: 0},
				{Name: "dk3v.3p", Size: 0x1000, Role: "tiles", Offset: 0x1000},
				{Name: "dk3v.7c", Size: 0x1000, Role: "sprites", Offset: 0x0000},
				{Name: "dk3v.7d", Size: 0x1000, Role: "sprites", Offset: 0x1000},
				{Name: "dk3v.7e", Size: 0x1000, Role: "sprites", Offset: 0x2000},
				{Name: "dk3v.7f", Size: 0x1000, Role: "sprites", Offset: 0x3000},
				{Name: "dkc1-c.1d", Size: 0x200, Role: "palette", Offset: 0},
				{Name: "dkc1-c.1c", Size: 0x200, Role: "palette", Offset: 0x200},
				{Name: "dkc1-v.2n", Size: 0x100, Role: "tilecolour", Offset: 0},
				// the address decoder (the memory map is built in; see the package comment)
				{Name: "dkc1-v.5e", Size: 0x20, Role: "decoder", Offset: 0, Optional: true},
			}},
		},
		New: newBoard,
	})
}
