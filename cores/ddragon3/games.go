package ddragon3

import "retropixels/cores/arcade"

// The DIP switches are one 16-bit word at 100004: switch bank 1 in the low
// byte, bank 2 in the high byte. A setting's value is its bits as the
// switches read them (on = 0); switches no setting uses read 1.

func opts(pairs ...any) []arcade.DipOption {
	var o []arcade.DipOption
	for i := 0; i+1 < len(pairs); i += 2 {
		o = append(o, arcade.DipOption{Label: pairs[i].(string), Value: pairs[i+1].(int)})
	}
	return o
}

// dipMasks: the bits of the DIP word each setting is.
var dipMasks = map[string]int{
	"coinage":    0x0003,
	"continue":   0x0010,
	"demo_sound": 0x0020,
	"flip":       0x0040,
	"difficulty": 0x0300,
	"pvp_damage": 0x0400,
	"service":    0x1000,
	"stage_life": 0x2000,
	"start_life": 0x4000,
	"players":    0x8000,
}

var dd3Dips = []arcade.Dip{
	{ID: "coinage", Label: "Coinage", Default: 0x0003, Options: opts(
		"3 coins, 1 credit", 0x0000, "2 coins, 1 credit", 0x0001, "1 coin, 1 credit", 0x0003, "1 coin, 2 credits", 0x0002)},
	{ID: "continue", Label: "Continue discount", Default: 0x0010, Options: opts("Off", 0x0010, "On", 0)},
	{ID: "demo_sound", Label: "Demo sounds", Default: 0x0020, Options: opts("Off", 0, "On", 0x0020)},
	{ID: "flip", Label: "Flip screen", Default: 0x0040, Options: opts("Off", 0x0040, "On", 0)},
	{ID: "difficulty", Label: "Difficulty", Default: 0x0300, Options: opts(
		"Easy", 0x0200, "Normal", 0x0300, "Hard", 0x0100, "Hardest", 0)},
	{ID: "pvp_damage", Label: "Player vs. player damage", Default: 0x0400, Options: opts("Off", 0x0400, "On", 0)},
	{ID: "service", Label: "Service mode", Default: 0x1000, Options: opts("Off", 0x1000, "On", 0)},
	{ID: "stage_life", Label: "Stage clear energy", Default: 0x2000, Options: opts("None", 0, "50", 0x2000)},
	{ID: "start_life", Label: "Starting energy", Default: 0x4000, Options: opts("200", 0, "230", 0x4000)},
	{ID: "players", Label: "Players", Default: 0x8000, Options: opts("2", 0x8000, "3", 0)},
}

var dd3Buttons = []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"}

func games() []arcade.Game {
	return []arcade.Game{
		{ID: "ddragon3", Title: "Double Dragon 3: The Rosetta Stone (US)", Year: 1990, Maker: "East Technology / Technos Japan",
			Dips: dd3Dips, Players: 3, Characters: []string{"Billy", "Jimmy", "Sonny"},
			Files: []arcade.File{
				// 68000 program: even and odd bytes
				{Name: "30a15-0.ic79", Size: 0x20000, Role: "cpu_even"},
				{Name: "30a14-0.ic78", Size: 0x40000, Role: "cpu_odd"},
				// Z80 sound program
				{Name: "30a13-0.ic43", Size: 0x10000, Role: "audio"},
				// background tiles: one bit plane per chip
				{Name: "30j-4.ic7", Size: 0x40000, Role: "tiles", Lane: 0},
				{Name: "30j-5.ic6", Size: 0x40000, Role: "tiles", Lane: 1},
				{Name: "30j-6.ic5", Size: 0x40000, Role: "tiles", Lane: 2},
				{Name: "30j-7.ic4", Size: 0x40000, Role: "tiles", Lane: 3},
				// sprites: one bit plane per pair of chips (a 512 KB chip then a 64 KB one)
				{Name: "30j-0.ic15", Size: 0x80000, Role: "sprites", Lane: 0, Offset: 0},
				{Name: "30a9-0.ic14", Size: 0x10000, Role: "sprites", Lane: 0, Offset: 0x80000},
				{Name: "30j-1.ic13", Size: 0x80000, Role: "sprites", Lane: 1, Offset: 0},
				{Name: "30a10-0.ic12", Size: 0x10000, Role: "sprites", Lane: 1, Offset: 0x80000},
				{Name: "30j-2.ic11", Size: 0x80000, Role: "sprites", Lane: 2, Offset: 0},
				{Name: "30a11-0.ic10", Size: 0x10000, Role: "sprites", Lane: 2, Offset: 0x80000},
				{Name: "30j-3.ic9", Size: 0x80000, Role: "sprites", Lane: 3, Offset: 0},
				{Name: "30a12-0.ic8", Size: 0x10000, Role: "sprites", Lane: 3, Offset: 0x80000},
				// MSM6295 samples (two 256 KB banks)
				{Name: "30j-8.ic73", Size: 0x80000, Role: "oki"},
				// a timing PROM: not needed
				{Name: "mb7114h.ic38", Size: 0x100, Role: "prom", Optional: true},
			}},
	}
}
