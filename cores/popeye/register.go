package popeye

import "retropixels/cores/arcade"

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var dips = []arcade.Dip{
	{ID: "coins", Label: "Coins", Default: 0x0f, Options: []arcade.DipOption{
		opt("1 coin, 1 game", 0x0f), opt("1 coin, 2 games", 0x0e), opt("1 coin, 3 games", 0x03),
		opt("2 coins, 1 game", 0x0d), opt("3 coins, 1 game", 0x0a), opt("4 coins, 1 game", 0x09),
		opt("5 coins, 1 game", 0x05), opt("6 coins, 1 game", 0x08), opt("Free play", 0)}},
	{ID: "lives", Label: "Lives", Default: 1, Options: []arcade.DipOption{opt("1", 3), opt("2", 2), opt("3", 1), opt("4", 0)}},
	{ID: "difficulty", Label: "Difficulty", Default: 3, Options: []arcade.DipOption{
		opt("Easy", 3), opt("Medium", 2), opt("Hard", 1), opt("Hardest", 0)}},
	{ID: "bonus", Label: "Extra life at", Default: 3, Options: []arcade.DipOption{
		opt("40,000 points", 3), opt("60,000 points", 2), opt("80,000 points", 1), opt("No extra life", 0)}},
	{ID: "demosound", Label: "Sound in demo", Default: 0, Options: []arcade.DipOption{opt("On", 0), opt("Off", 1)}},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail table", 1)}},
	{ID: "copyright", Label: "Copyright notice", Default: 2, Options: []arcade.DipOption{
		opt("Nintendo", 2), opt("Nintendo Co.,Ltd", 1), opt("Nintendo of America", 3)}},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "popeye", Title: "Nintendo Popeye", Width: scrW, Height: scrH, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Punch", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    dips,
		Games: []arcade.Game{
			{ID: "popeye", Title: "Popeye", Year: 1982, Maker: "Nintendo", Files: []arcade.File{
				{Name: "tpp2-c.7a", Size: 0x2000, Role: "cpu", Offset: 0x0000},
				{Name: "tpp2-c.7b", Size: 0x2000, Role: "cpu", Offset: 0x2000},
				{Name: "tpp2-c.7c", Size: 0x2000, Role: "cpu", Offset: 0x4000},
				{Name: "tpp2-c.7e", Size: 0x2000, Role: "cpu", Offset: 0x6000},
				{Name: "tpp2-v.5n", Size: 0x1000, Role: "chars", Offset: 0},
				{Name: "tpp2-v.1e", Size: 0x2000, Role: "sprites", Offset: 0x0000},
				{Name: "tpp2-v.1f", Size: 0x2000, Role: "sprites", Offset: 0x2000},
				{Name: "tpp2-v.1j", Size: 0x2000, Role: "sprites", Offset: 0x4000},
				{Name: "tpp2-v.1k", Size: 0x2000, Role: "sprites", Offset: 0x6000},
				{Name: "tpp2-c.4a", Size: 0x20, Role: "bgpal", Offset: 0},
				{Name: "tpp2-c.3a", Size: 0x20, Role: "charpal", Offset: 0},
				{Name: "tpp2-c.5b", Size: 0x100, Role: "sprpal-lo", Offset: 0},
				{Name: "tpp2-c.5a", Size: 0x100, Role: "sprpal-hi", Offset: 0},
				{Name: "tpp2-v.7j", Size: 0x100, Role: "timing", Offset: 0, Optional: true},
			}},
		},
		New: newBoard,
	})
}
