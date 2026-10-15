package main

// The Nintendo 64 (test build 3): RetroPixels' own N64 emulator (package
// n64, which RetroPixels.exe runs for the web player), as a core.

import "retropixels/cores/n64"

func init() {
	coreMakers["n64"] = n64.New
	// DEFAULT_MAPS.n64 in host.js: Xbox A / X = A / B (same spots as the N64's
	// big buttons), LT = Z. The left stick is the analog stick and the right
	// stick the C buttons; on the keyboard the arrows move the stick.
	defaultMaps["n64"] = ControlMap{
		"A": b([]int{padA}, "x"), "B": b([]int{padX}, "z"), "Z": b([]int{padLT}, "a"), "Start": b([]int{padMenu}, "enter"),
		"Up": b([]int{}, "t"), "Down": b([]int{}, "g"), "Left": b([]int{}, "f"), "Right": b([]int{}, "h"),
		"L": b([]int{padLB}, "q"), "R": b([]int{padRB}, "w"),
		"C-Up": b([]int{}, "i"), "C-Down": b([]int{}, "k"), "C-Left": b([]int{}, "j"), "C-Right": b([]int{}, "l"),
		"Stick Up": b([]int{}, "arrowup"), "Stick Down": b([]int{}, "arrowdown"),
		"Stick Left": b([]int{}, "arrowleft"), "Stick Right": b([]int{}, "arrowright"),
	}
	systemButtons["n64"] = n64.Buttons
	systemNotes["n64"] = n64.ControlsNote
}
