// Package dec8 is Data East's "DEC8" board as used by The Real Ghostbusters
// (1987): an HD6309 main processor with banked program ROM, a DECO 222 (a
// 6502 with scrambled opcodes) running the sound on a YM2203 and a YM3812,
// a BAC06 scrolling playfield, sprites, a character layer and colour PROMs,
// and an i8751 microcontroller for the coins and protection. The user's set
// has no dump of the 8751, so the board answers for it (see mcu.go).
package dec8

import "retropixels/cores/arcade"

func opt(label string, v int) arcade.DipOption { return arcade.DipOption{Label: label, Value: v} }

// The switches, with the values the game reads (SW1 and SW2 on the board).
var dipsGhostb = []arcade.Dip{
	{ID: "lives", Label: "Lives", Default: 3, Options: []arcade.DipOption{opt("1", 1), opt("3", 3), opt("5", 2), opt("Invulnerable (cheat)", 0)}},
	{ID: "difficulty", Label: "Difficulty", Default: 3, Options: []arcade.DipOption{opt("Easy", 2), opt("Normal", 3), opt("Hard", 1), opt("Hardest", 0)}},
	{ID: "scenetime", Label: "Time for a scene", Default: 3, Options: []arcade.DipOption{opt("4:00", 0), opt("4:30", 1), opt("5:00", 3), opt("6:00", 2)}},
	{ID: "continue", Label: "Allow continue", Default: 0, Options: []arcade.DipOption{opt("No", 1), opt("Yes", 0)}},
	{ID: "energy", Label: "Energy bonus", Default: 1, Options: []arcade.DipOption{opt("None", 1), opt("+50%", 0)}},
	{ID: "demosound", Label: "Demo sounds", Default: 1, Options: []arcade.DipOption{opt("Off", 0), opt("On", 1)}},
	{ID: "flip", Label: "Flip screen", Default: 1, Options: []arcade.DipOption{opt("Off", 1), opt("On", 0)}},
}

func init() {
	arcade.Register(&arcade.BoardDef{
		ID: "dec8", Title: "Data East DEC8", Width: scrW, Height: scrH, FPS: fps,
		// a coin on a player's slot (Coin on that player's controller)
		// joins that player; Button 1 then starts
		Buttons: []string{"Up", "Down", "Left", "Right", "Button 1", "Button 2", "Coin", "Coin 2", "Service"},
		Dips:    dipsGhostb,
		Games: []arcade.Game{
			{ID: "ghostb", Title: "The Real Ghostbusters (US, 2 players)", Year: 1987, Maker: "Data East", Files: filesGhostb},
		},
		New: newGhostb,
	})
}
