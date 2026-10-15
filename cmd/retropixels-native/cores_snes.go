package main

// The Super Nintendo emulator (test build 4).

import (
	"errors"

	"retropixels/cores/snes"
)

func init() {
	coreMakers["snes"] = snes.New
	// DEFAULT_MAPS.snes in host.js: the same button positions as the Xbox
	// pad's diamond (Xbox B = A, A = B, Y = X, X = Y), LB / RB = L / R.
	defaultMaps["snes"] = ControlMap{"A": b([]int{1}, "x"), "B": b([]int{0}, "z"), "X": b([]int{3}, "s"), "Y": b([]int{2}, "a"),
		"L": b([]int{4}, "q"), "R": b([]int{5}, "w"), "Select": b([]int{8}, "shift"), "Start": b([]int{9}, "enter")}
	systemButtons["snes"] = snes.Buttons
	// games whose cartridge chip runs a program the player supplies
	needsFirmware = func(err error) (chip, file, msg string, ok bool) {
		var fe *snes.FirmwareError
		if errors.As(err, &fe) {
			return fe.Chip, fe.File, fe.Msg, true
		}
		return "", "", "", false
	}
}
