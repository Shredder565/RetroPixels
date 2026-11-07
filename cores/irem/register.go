package irem

import "retropixels/cores/arcade"

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

var dips = []arcade.Dip{
	{ID: "coins", Label: "Coins", Default: 0xf0, Options: []arcade.DipOption{
		opt("1 coin, 1 game", 0xf0), opt("1 coin, 2 games", 0x70), opt("1 coin, 3 games", 0x60),
		opt("2 coins, 1 game", 0xe0), opt("3 coins, 1 game", 0xd0), opt("4 coins, 1 game", 0xc0), opt("Free play", 0)}},
	{ID: "lives", Label: "Lives", Default: 3, Options: []arcade.DipOption{opt("2", 2), opt("3", 3), opt("4", 1), opt("5", 0)}},
	{ID: "difficulty", Label: "Difficulty", Default: 1, Options: []arcade.DipOption{opt("Easy", 1), opt("Hard", 0)}},
	{ID: "energy", Label: "Energy loss", Default: 1, Options: []arcade.DipOption{opt("Slow", 1), opt("Fast", 0)}},
	{ID: "cabinet", Label: "Cabinet", Default: 0, Options: []arcade.DipOption{opt("Upright", 0), opt("Cocktail table", 1)}},
	{ID: "slowmo", Label: "Slow motion (cheat)", Default: 1, Options: []arcade.DipOption{opt("Off", 1), opt("On", 0)}},
	{ID: "freeze", Label: "Freeze (cheat)", Default: 1, Options: []arcade.DipOption{opt("Off", 1), opt("On", 0)}},
	{ID: "levelselect", Label: "Level select (cheat)", Default: 1, Options: []arcade.DipOption{opt("Off", 1), opt("On", 0)}},
	{ID: "invulnerable", Label: "Invulnerability (cheat)", Default: 1, Options: []arcade.DipOption{opt("Off", 1), opt("On", 0)}},
}

func f(name, alt string, size int, role string, offset int) arcade.File {
	return arcade.File{Name: name, Alt: []string{alt}, Size: size, Role: role, Offset: offset}
}

func init() {
	common := []arcade.File{
		f("g-4c-a.bin", "g-4c-a", 0x2000, "tiles", 0x0000),
		f("g-4d-a.bin", "g-4d-a", 0x2000, "tiles", 0x2000),
		f("g-4e-a.bin", "g-4e-a", 0x2000, "tiles", 0x4000),
		f("b-4k-.bin", "b-4k-", 0x2000, "sprites", 0x00000),
		f("b-4f-.bin", "b-4f-", 0x2000, "sprites", 0x02000),
		f("b-4l-.bin", "b-4l-", 0x2000, "sprites", 0x04000),
		f("b-4h-.bin", "b-4h-", 0x2000, "sprites", 0x06000),
		f("b-3n-.bin", "b-3n-", 0x2000, "sprites", 0x08000),
		f("b-4n-.bin", "b-4n-", 0x2000, "sprites", 0x0a000),
		f("b-4m-.bin", "b-4m-", 0x2000, "sprites", 0x0c000),
		f("b-3m-.bin", "b-3m-", 0x2000, "sprites", 0x0e000),
		f("b-4c-.bin", "b-4c-", 0x2000, "sprites", 0x10000),
		f("b-4e-.bin", "b-4e-", 0x2000, "sprites", 0x12000),
		f("b-4d-.bin", "b-4d-", 0x2000, "sprites", 0x14000),
		f("b-4a-.bin", "b-4a-", 0x2000, "sprites", 0x16000),
		f("b-5f-.bin", "b-5f-", 0x20, "height", 0),
		f("b-1m-.bin", "b-1m-", 0x100, "sprpal", 0x000),
		f("b-1n-.bin", "b-1n-", 0x100, "sprpal", 0x100),
		f("b-1l-.bin", "b-1l-", 0x100, "sprpal", 0x200),
		f("g-1j-.bin", "g-1j-", 0x100, "chrpal", 0x000),
		f("g-1f-.bin", "g-1f-", 0x100, "chrpal", 0x100),
		f("g-1h-.bin", "g-1h-", 0x100, "chrpal", 0x200),
		{Name: "b-6f-.bin", Alt: []string{"b-6f-"}, Size: 0x100, Role: "timing", Optional: true},
	}
	sound := []arcade.File{
		f("a-3e-.bin", "a-3e-", 0x2000, "sound", 0x0000),
		f("a-3f-.bin", "a-3f-", 0x2000, "sound", 0x2000),
		f("a-3h-.bin", "a-3h-", 0x2000, "sound", 0x4000),
	}
	set := func(cpu0, cpu1 string) []arcade.File {
		fs := []arcade.File{f(cpu0+".bin", cpu0, 0x4000, "cpu", 0), f(cpu1+".bin", cpu1, 0x4000, "cpu", 0x4000)}
		fs = append(fs, sound...)
		return append(fs, common...)
	}
	arcade.Register(&arcade.BoardDef{
		ID: "m62", Title: "Irem M62", Width: scrW, Height: scrH, FPS: FPS,
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
		Dips:    dips,
		Games: []arcade.Game{
			{ID: "kungfum", Title: "Kung-Fu Master", Year: 1984, Maker: "Irem", Files: set("a-4e-c", "a-4d-c")},
			{ID: "spartanx", Title: "Spartan X (Japan)", Year: 1984, Maker: "Irem", Files: set("a-4e-c-j", "a-4d-c-j")},
		},
		New: newBoard,
	})
}
