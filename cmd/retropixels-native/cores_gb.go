package main

// The Game Boy / Game Boy Color emulator (test build 2).

import "retropixels/cores/gb"

func init() {
	coreMakers["gb"] = gb.New
	// DEFAULT_MAPS.gb in host.js: Xbox B (right) = A, Xbox A (bottom) = B, like
	// the original pad. The tilt keys only show for tilt cartridges (their
	// game lists them); the right stick tilts too.
	defaultMaps["gb"] = ControlMap{"A": b([]int{1}, "x"), "B": b([]int{0}, "z"), "Select": b([]int{8}, "shift"), "Start": b([]int{9}, "enter"),
		"Tilt Up": b([]int{}, "i"), "Tilt Down": b([]int{}, "k"), "Tilt Left": b([]int{}, "j"), "Tilt Right": b([]int{}, "l")}
	systemButtons["gb"] = gb.Buttons
}
