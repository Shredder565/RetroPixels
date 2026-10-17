package berzerk

import "retropixels/cores/arcade"

// opt is one setting of a switch.
func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

// coinOptions: the coin switches (the same table for all three slots).
var coinOptions = []arcade.DipOption{
	opt("1 coin, 1 credit", 0), opt("1 coin, 2 credits", 1), opt("1 coin, 3 credits", 2), opt("1 coin, 4 credits", 3),
	opt("1 coin, 5 credits", 4), opt("1 coin, 6 credits", 5), opt("1 coin, 7 credits", 6), opt("1 coin, 10 credits", 7),
	opt("1 coin, 14 credits", 8), opt("2 coins, 1 credit", 9), opt("2 coins, 3 credits", 10), opt("2 coins, 5 credits", 11),
	opt("2 coins, 7 credits", 12), opt("4 coins, 3 credits", 13), opt("4 coins, 5 credits", 14), opt("4 coins, 7 credits", 15),
}

var dips = []arcade.Dip{
	{ID: "bonus", Label: "Bonus life", Default: 3, Options: []arcade.DipOption{
		opt("5,000 and 10,000 points", 3), opt("5,000 points", 1), opt("10,000 points", 2), opt("None", 0)}},
	{ID: "language", Label: "Language", Default: 0, Options: []arcade.DipOption{
		opt("English", 0), opt("German", 1), opt("French", 2), opt("Spanish", 3)}},
	{ID: "coin1", Label: "Coin slot 1", Default: 0, Options: coinOptions},
	{ID: "coin2", Label: "Coin slot 2", Default: 0, Options: coinOptions},
	{ID: "coin3", Label: "Coin slot 3", Default: 0, Options: coinOptions},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail", 1)}},
	{ID: "inputTest", Label: "Input test mode", Default: 0, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
	{ID: "crosshair", Label: "Crosshair pattern", Default: 0, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
	{ID: "colourTest", Label: "Colour test", Default: 0, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "berzerk", Title: "Stern Berzerk", Width: scrW, Height: scrH, Rotate: 0, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Coin", "Start (1P)", "Start (2P)"},
		Dips:    dips,
		Games: []arcade.Game{
			{ID: "berzerk", Title: "Berzerk", Year: 1980, Maker: "Stern Electronics", Files: []arcade.File{
				// the user's names first, then MAME's (5c: revision RC31, or RC31A as in MAME's parent set)
				{Name: "1c-0", Alt: []string{"berzerk_rc31_1c.rom0.1c"}, Size: 0x800, Role: "cpu", Offset: 0x0000},
				{Name: "1d-1", Alt: []string{"berzerk_rc31_1d.rom1.1d"}, Size: 0x800, Role: "cpu", Offset: 0x1000},
				{Name: "3d-2", Alt: []string{"berzerk_rc31_3d.rom2.3d"}, Size: 0x800, Role: "cpu", Offset: 0x1800},
				{Name: "5d-3", Alt: []string{"berzerk_rc31_5d.rom3.5d"}, Size: 0x800, Role: "cpu", Offset: 0x2000},
				{Name: "6d-4", Alt: []string{"berzerk_rc31_6d.rom4.6d"}, Size: 0x800, Role: "cpu", Offset: 0x2800},
				{Name: "5c-5", Alt: []string{"berzerk_rc31_5c.rom5.5c", "berzerk_rc31a_5c.rom5.5c"}, Size: 0x800, Role: "cpu", Offset: 0x3000},
				// the speech chip's ROMs: the robots talk when the set has them (see speech.go)
				{Name: "berzerk_r_vo_1c.1c", Alt: []string{"1c", "r_vo_1c.1c", "vo_1c.1c", "voice_1c.1c", "1c.bin", "berzerk_r_vo_1c.bin"}, Size: 0x800, Role: "speech", Offset: 0x0000, Optional: true},
				{Name: "berzerk_r_vo_2c.2c", Alt: []string{"2c", "r_vo_2c.2c", "vo_2c.2c", "voice_2c.2c", "2c.bin", "berzerk_r_vo_2c.bin"}, Size: 0x800, Role: "speech", Offset: 0x0800, Optional: true},
			}},
		},
		New: newBoard,
	})
}
