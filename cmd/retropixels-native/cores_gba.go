package main

// The Game Boy Advance emulator.

import "retropixels/cores/gba"

func init() {
	coreMakers["gba"] = gba.New
	// DEFAULT_MAPS.gba in host.js: Xbox B (right) = A, Xbox A (bottom) = B, like
	// the original pad; LB / RB = L / R. The sensor keys only show for the carts
	// that have them (their game lists them); the right stick works them too.
	defaultMaps["gba"] = ControlMap{"A": b([]int{1}, "x"), "B": b([]int{0}, "z"), "L": b([]int{4}, "q"), "R": b([]int{5}, "w"),
		"Select": b([]int{8}, "shift"), "Start": b([]int{9}, "enter"),
		"Tilt Up": b([]int{}, "i"), "Tilt Down": b([]int{}, "k"), "Tilt Left": b([]int{}, "j"), "Tilt Right": b([]int{}, "l"),
		"Turn Left": b([]int{}, "j"), "Turn Right": b([]int{}, "l"), "More Sun": b([]int{}, "i"), "Less Sun": b([]int{}, "k")}
	systemButtons["gba"] = gba.Buttons
}
