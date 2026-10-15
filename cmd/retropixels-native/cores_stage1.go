package main

// The emulators of test build 1.

import (
	"retropixels/cores/atari2600"
	"retropixels/cores/nes"
)

func init() {
	coreMakers["nes"] = nes.New
	coreMakers["atari2600"] = atari2600.New
}
