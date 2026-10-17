package vsnes

import "retropixels/cores/arcade"

// opt is one setting of a switch.
func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

// dips: Vs. Castlevania's switches (switch 1 is the lowest bit).
var dips = []arcade.Dip{
	{ID: "coinage", Label: "Coinage", Default: 0, Options: []arcade.DipOption{
		opt("1 coin, 1 credit", 0), opt("1 coin, 2 credits", 4), opt("1 coin, 3 credits", 2), opt("2 coins, 1 credit", 6),
		opt("3 coins, 1 credit", 1), opt("4 coins, 1 credit", 5), opt("5 coins, 1 credit", 3), opt("Free play", 7)}},
	{ID: "lives", Label: "Lives", Default: 0, Options: []arcade.DipOption{opt("3", 0), opt("2", 1)}},
	{ID: "bonus", Label: "Bonus points", Default: 0, Options: []arcade.DipOption{
		opt("Timer x 4, hearts x 40", 0), opt("Timer x 6, hearts x 60", 2), opt("Timer x 8, hearts x 80", 1), opt("Timer x 9, hearts x 90", 3)}},
	{ID: "difficulty", Label: "Difficulty", Default: 0, Options: []arcade.DipOption{opt("Easy", 0), opt("Hard", 1)}},
	{ID: "unused", Label: "Switch 8 (unused)", Default: 0, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "vsnes", Title: "Nintendo Vs. UniSystem", Width: scrW, Height: scrH, Rotate: 0, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Fire", "Jump", "Coin", "Start (1P)", "Start (2P)", "Service"},
		Dips:    dips,
		Games: []arcade.Game{
			{ID: "cstlevna", Title: "Vs. Castlevania", Year: 1987, Maker: "Konami (Nintendo license)", Files: []arcade.File{
				{Name: "mds-cv.prg", Alt: []string{"mds-cv.u7"}, Size: 0x20000, Role: "prg", Offset: 0},
				// the PPU's colour PROM (the built-in table is used without it)
				{Name: "rp2c04-0002.pal", Size: 192, Role: "palette", Offset: 0, Optional: true},
			}},
		},
		New: newBoard,
	})
}
