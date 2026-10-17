package cps1

import "retropixels/cores/arcade"

// dipMask: which switch bank (0 = A, 1 = B, 2 = C) and bits a setting is.
type dipMask struct {
	bank int
	mask int
}

// dipMasks: the bits of every setting id the games use. A setting's option
// values are the bits as the switches read them (on = 0).
var dipMasks = map[string]dipMask{
	"coin_a":     {0, 0x07},
	"coin_b":     {0, 0x38},
	"two_coins":  {0, 0x40},
	"difficulty": {1, 0x07},
	"level_up":   {1, 0x18},
	"bullets":    {1, 0x60},
	"vitality":   {1, 0x80},
	"throttle":   {2, 0x01},
	"free_play":  {2, 0x04},
	"freeze":     {2, 0x08},
	"flip":       {2, 0x10},
	"demo_sound": {2, 0x20},
	"continue":   {2, 0x40},
	"test":       {2, 0x80},
}

func opts(pairs ...any) []arcade.DipOption {
	var o []arcade.DipOption
	for i := 0; i+1 < len(pairs); i += 2 {
		o = append(o, arcade.DipOption{Label: pairs[i].(string), Value: pairs[i+1].(int)})
	}
	return o
}

// coinage: the usual CPS-1 coin settings for 3 switches at bit shift sh.
func coinage(id, label string, sh int) arcade.Dip {
	return arcade.Dip{ID: id, Label: label, Default: 7 << sh, Options: opts(
		"4 coins, 1 credit", 0<<sh, "3 coins, 1 credit", 1<<sh, "2 coins, 1 credit", 2<<sh,
		"1 coin, 1 credit", 7<<sh, "1 coin, 2 credits", 6<<sh, "1 coin, 3 credits", 5<<sh,
		"1 coin, 4 credits", 4<<sh, "1 coin, 6 credits", 3<<sh)}
}

var dips1941 = []arcade.Dip{
	coinage("coin_a", "Coin A", 0),
	coinage("coin_b", "Coin B", 3),
	{ID: "two_coins", Label: "2 coins to start, 1 to continue", Default: 0x40, Options: opts("Off", 0x40, "On", 0)},
	{ID: "difficulty", Label: "Difficulty", Default: 4, Options: opts("0 (easiest)", 7, "1", 6, "2", 5,
		"3 (normal)", 4, "4", 3, "5", 2, "6", 1, "7 (hardest)", 0)},
	{ID: "level_up", Label: "Level up timer", Default: 0x18, Options: opts("More slowly", 0x18, "Slowly", 0x10, "Quickly", 0x08, "More quickly", 0)},
	{ID: "bullets", Label: "Bullet speed", Default: 0x60, Options: opts("Very slow", 0x60, "Slow", 0x40, "Fast", 0x20, "Very fast", 0)},
	{ID: "vitality", Label: "Initial vitality", Default: 0x80, Options: opts("3 bars", 0x80, "4 bars", 0)},
	{ID: "throttle", Label: "Throttle game speed", Default: 1, Options: opts("On", 1, "Off", 0)},
	{ID: "free_play", Label: "Free play", Default: 4, Options: opts("Off", 4, "On", 0)},
	{ID: "freeze", Label: "Freeze", Default: 8, Options: opts("Off", 8, "On", 0)},
	{ID: "flip", Label: "Flip screen", Default: 0x10, Options: opts("Off", 0x10, "On", 0)},
	{ID: "demo_sound", Label: "Demo sounds", Default: 0, Options: opts("On", 0, "Off", 0x20)},
	{ID: "continue", Label: "Allow continue", Default: 0, Options: opts("Yes", 0, "No", 0x40)},
	{ID: "test", Label: "Game mode", Default: 0x80, Options: opts("Game", 0x80, "Test", 0)},
}

func games() []arcade.Game {
	return []arcade.Game{
		{ID: "1941", Title: "1941: Counter Attack (World)", Year: 1990, Maker: "Capcom", Rotate: 270, Dips: dips1941, Players: 2,
			Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Start (1P)", "Start (2P)", "Coin 2", "Service"},
			Files: []arcade.File{
				{Name: "41e_30.rom", Alt: []string{"41e_30.11f"}, Size: 0x20000, Role: "cpu_even", Offset: 0x00000},
				{Name: "41e_35.rom", Alt: []string{"41e_35.11h"}, Size: 0x20000, Role: "cpu_odd", Offset: 0x00000},
				{Name: "41e_31.rom", Alt: []string{"41e_31.12f"}, Size: 0x20000, Role: "cpu_even", Offset: 0x40000},
				{Name: "41e_36.rom", Alt: []string{"41e_36.12h"}, Size: 0x20000, Role: "cpu_odd", Offset: 0x40000},
				{Name: "41_32.rom", Alt: []string{"41-32m.8h"}, Size: 0x80000, Role: "cpu_word", Offset: 0x80000},
				{Name: "41_gfx5.rom", Alt: []string{"41-5m.7a"}, Size: 0x80000, Role: "gfx", Offset: 0, Lane: 0},
				{Name: "41_gfx7.rom", Alt: []string{"41-7m.9a"}, Size: 0x80000, Role: "gfx", Offset: 0, Lane: 1},
				{Name: "41_gfx1.rom", Alt: []string{"41-1m.3a"}, Size: 0x80000, Role: "gfx", Offset: 0, Lane: 2},
				{Name: "41_gfx3.rom", Alt: []string{"41-3m.5a"}, Size: 0x80000, Role: "gfx", Offset: 0, Lane: 3},
				{Name: "41_09.rom", Alt: []string{"41_9.12b"}, Size: 0x10000, Role: "audio", Offset: 0},
				{Name: "41_18.rom", Alt: []string{"41_18.11c"}, Size: 0x20000, Role: "oki", Offset: 0},
				{Name: "41_19.rom", Alt: []string{"41_19.12c"}, Size: 0x20000, Role: "oki", Offset: 0x20000},
				// the board's PALs: not needed to play
				{Name: "buf1", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "ioa1", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "prg1", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "rom1", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "sou1", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "yi24b.1a", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
				{Name: "iob1.11e", Size: 279, Role: "pld", Offset: arcade.Next, Optional: true},
			}},
	}
}
