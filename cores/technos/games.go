package technos

import "retropixels/cores/arcade"

// The sets: the user's file names, which are MAME 0.264's too.

var filesDDragon = []arcade.File{
	{Name: "21j-1-5.26", Size: 0x8000, Role: "cpu", Offset: 0},
	{Name: "21j-2-3.25", Size: 0x8000, Role: "banks", Offset: 0x00000},
	{Name: "21j-3.24", Size: 0x8000, Role: "banks", Offset: 0x08000},
	{Name: "21j-4-1.23", Size: 0x8000, Role: "banks", Offset: 0x10000},
	{Name: "21jm-0.ic55", Size: 0x4000, Role: "mcu", Offset: 0},
	{Name: "21j-0-1", Size: 0x8000, Role: "sound", Offset: 0},
	{Name: "21j-5", Size: 0x8000, Role: "chars", Offset: 0},
	{Name: "21j-a", Size: 0x10000, Role: "sprites", Offset: 0x00000},
	{Name: "21j-b", Size: 0x10000, Role: "sprites", Offset: 0x10000},
	{Name: "21j-c", Size: 0x10000, Role: "sprites", Offset: 0x20000},
	{Name: "21j-d", Size: 0x10000, Role: "sprites", Offset: 0x30000},
	{Name: "21j-e", Size: 0x10000, Role: "sprites", Offset: 0x40000},
	{Name: "21j-f", Size: 0x10000, Role: "sprites", Offset: 0x50000},
	{Name: "21j-g", Size: 0x10000, Role: "sprites", Offset: 0x60000},
	{Name: "21j-h", Size: 0x10000, Role: "sprites", Offset: 0x70000},
	{Name: "21j-8", Size: 0x10000, Role: "tiles", Offset: 0x00000},
	{Name: "21j-9", Size: 0x10000, Role: "tiles", Offset: 0x10000},
	{Name: "21j-i", Size: 0x10000, Role: "tiles", Offset: 0x20000},
	{Name: "21j-j", Size: 0x10000, Role: "tiles", Offset: 0x30000},
	{Name: "21j-6", Size: 0x10000, Role: "adpcm0", Offset: 0},
	{Name: "21j-7", Size: 0x10000, Role: "adpcm1", Offset: 0},
	// video timing and priority PROMs: built into the board's code
	{Name: "21j-k-0", Size: 0x100, Role: "prom", Offset: 0, Optional: true},
	{Name: "21j-l-0", Size: 0x200, Role: "prom2", Offset: 0, Optional: true},
}

var filesDDragon2 = []arcade.File{
	{Name: "26a9-04.bin", Size: 0x8000, Role: "cpu", Offset: 0},
	{Name: "26aa-03.bin", Size: 0x8000, Role: "banks", Offset: 0x00000},
	{Name: "26ab-0.bin", Size: 0x8000, Role: "banks", Offset: 0x08000},
	{Name: "26ac-0e.63", Size: 0x8000, Role: "banks", Offset: 0x10000},
	{Name: "26ae-0.bin", Size: 0x10000, Role: "sub", Offset: 0},
	{Name: "26ad-0.bin", Size: 0x8000, Role: "sound", Offset: 0},
	{Name: "26a8-0e.19", Size: 0x10000, Role: "chars", Offset: 0},
	{Name: "26j0-0.bin", Size: 0x20000, Role: "sprites", Offset: 0x00000},
	{Name: "26j1-0.bin", Size: 0x20000, Role: "sprites", Offset: 0x20000},
	{Name: "26af-0.bin", Size: 0x20000, Role: "sprites", Offset: 0x40000},
	{Name: "26j2-0.bin", Size: 0x20000, Role: "sprites", Offset: 0x60000},
	{Name: "26j3-0.bin", Size: 0x20000, Role: "sprites", Offset: 0x80000},
	{Name: "26a10-0.bin", Size: 0x20000, Role: "sprites", Offset: 0xa0000},
	{Name: "26j4-0.bin", Size: 0x20000, Role: "tiles", Offset: 0x00000},
	{Name: "26j5-0.bin", Size: 0x20000, Role: "tiles", Offset: 0x20000},
	{Name: "26j6-0.bin", Size: 0x20000, Role: "oki", Offset: 0x00000},
	{Name: "26j7-0.bin", Size: 0x20000, Role: "oki", Offset: 0x20000},
	{Name: "21j-k-0", Size: 0x100, Role: "prom", Offset: 0, Optional: true},
	{Name: "prom.16", Size: 0x200, Role: "prom2", Offset: 0, Optional: true},
}

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var offOn = []arcade.DipOption{opt("Off", 1), opt("On", 0)} // switches that are on when 0

var coins = []arcade.DipOption{opt("1 coin, 1 credit", 7), opt("1 coin, 2 credits", 6), opt("1 coin, 3 credits", 5),
	opt("1 coin, 4 credits", 4), opt("1 coin, 5 credits", 3), opt("2 coins, 1 credit", 2), opt("3 coins, 1 credit", 1),
	opt("4 coins, 1 credit", 0)}

var cabinet = []arcade.DipOption{opt("Upright", 1), opt("Cocktail table", 0)}

// Double Dragon's switches (SW1: coins, cabinet, flip; SW2: the rest).
var dipsDDragon = []arcade.Dip{
	{ID: "coina", Label: "Coin A", Default: 7, Options: coins},
	{ID: "coinb", Label: "Coin B", Default: 7, Options: coins},
	{ID: "cabinet", Label: "Cabinet", Default: 1, Options: cabinet},
	{ID: "flip", Label: "Flip screen", Default: 1, Options: offOn},
	{ID: "difficulty", Label: "Difficulty", Default: 3, Options: []arcade.DipOption{opt("Easy", 1), opt("Medium", 3), opt("Hard", 2), opt("Hardest", 0)}},
	{ID: "demosound", Label: "Demo sounds", Default: 1, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
	{ID: "bonus", Label: "Bonus life", Default: 3, Options: []arcade.DipOption{opt("20,000", 1), opt("40,000", 0),
		opt("30,000 and every 60,000", 3), opt("40,000 and every 80,000", 2)}},
	{ID: "lives", Label: "Lives", Default: 3, Options: []arcade.DipOption{opt("2", 3), opt("3", 2), opt("4", 1), opt("Infinite", 0)}},
}

var buttons = []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Coin", "Coin 2", "Start (1P)", "Start (2P)", "Service"}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "ddragon", Title: "Technos Double Dragon", Width: scrW, Height: scrH, FPS: fps,
		Buttons: buttons, Dips: dipsDDragon,
		Games: []arcade.Game{
			{ID: "ddragon", Title: "Double Dragon (Japan)", Year: 1987, Maker: "Technos Japan", Files: filesDDragon},
		},
		New: newDDragon,
	})
}
