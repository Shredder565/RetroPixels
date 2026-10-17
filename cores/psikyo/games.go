package psikyo

import (
	"fmt"

	"retropixels/cores/arcade"
)

// Graphics ROMs come in pairs: "gfx" files with Lane 0 hold the low 16 bits
// of each 32-bit word, Lane 1 the high 16 bits; Offset is the pair's place in
// the video chip's address space.
func gfxPair(n int, lo, hi string) []arcade.File {
	at := n * 0x400000
	return []arcade.File{
		{Name: fmt.Sprintf("%dl.%s", n, lo), Size: 0x200000, Role: "gfx", Offset: at, Lane: 0},
		{Name: fmt.Sprintf("%dh.%s", n, hi), Size: 0x200000, Role: "gfx", Offset: at, Lane: 1},
	}
}

func dragnblzFiles() []arcade.File {
	f := []arcade.File{
		// the program: two 16-bit ROMs side by side, high half first
		{Name: "2prog_h.u21", Size: 0x80000, Role: "cpu", Lane: 0},
		{Name: "1prog_l.u22", Size: 0x80000, Role: "cpu", Lane: 1},
	}
	for i, u := range [][2]string{{"u4", "u12"}, {"u5", "u13"}, {"u6", "u14"}, {"u7", "u15"}, {"u8", "u16"},
		{"u1", "u2"}, {"u19", "u20"}, {"u28", "u29"}, {"u41", "u42"}, {"u58", "u59"}} {
		f = append(f, gfxPair(i+1, u[0], u[1])...)
	}
	return append(f,
		arcade.File{Name: "snd0.u52", Size: 0x200000, Role: "pcm"},
		// the EEPROM's contents (settings, high scores and a check of the
		// program); without it the board starts with one it builds itself
		arcade.File{Name: "eeprom-dragnblz.u44", Size: 0x100, Role: "eeprom", Optional: true},
	)
}

// The region jumper (JP4) and the debug switch.
var dragnblzDips = []arcade.Dip{
	{ID: "region", Label: "Region", Default: 1, Options: []arcade.DipOption{
		{Label: "Japan", Value: 0}, {Label: "International Ver. A", Value: 2}, {Label: "International Ver. B", Value: 1}}},
	{ID: "debug", Label: "Tilt (debug mode)", Default: 0, Options: []arcade.DipOption{
		{Label: "Off", Value: 0}, {Label: "On", Value: 1}}},
	{ID: "test", Label: "Service mode", Default: 0, Options: []arcade.DipOption{
		{Label: "Off", Value: 0}, {Label: "On", Value: 1}}},
}

func games() []arcade.Game {
	return []arcade.Game{
		{ID: "dragnblz", Title: "Dragon Blaze", Year: 2000, Maker: "Psikyo", Files: dragnblzFiles(), Dips: dragnblzDips},
	}
}
