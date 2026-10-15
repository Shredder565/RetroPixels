package main

// The Sega Genesis / Mega Drive (test build 3).

import "retropixels/cores/genesis"

func init() {
	coreMakers["segaMD"] = genesis.New
	// DEFAULT_MAPS.segaMD in host.js: Xbox X / A / B = A / B / C (the same row
	// as on the Genesis pad), LB / Y / RB = X / Y / Z, View = Mode. Players 3
	// and 4 are the J-Cart's pads (the third and fourth controllers).
	defaultMaps["segaMD"] = ControlMap{"A": b([]int{2}, "a"), "B": b([]int{0}, "s"), "C": b([]int{1}, "d"),
		"X": b([]int{4}, "q"), "Y": b([]int{3}, "w"), "Z": b([]int{5}, "e"),
		"Start": b([]int{9}, "enter"), "Mode": b([]int{8}, "shift")}
	systemButtons["segaMD"] = genesis.Buttons
}
