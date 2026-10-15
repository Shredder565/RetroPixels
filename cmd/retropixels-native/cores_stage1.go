//go:build stage1cores

package main

// The emulators of test build 1. They live on their own branches until they
// are merged; once cores/nes and cores/atari2600 are in, remove the build
// line above so every build has them.

import (
	"retropixels/cores/atari2600"
	"retropixels/cores/nes"
)

func init() {
	coreMakers["nes"] = nes.New
	coreMakers["atari2600"] = atari2600.New
}
