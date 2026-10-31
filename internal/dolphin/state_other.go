//go:build !windows

package dolphin

import "errors"

// StateSlots is how many save state slots PixelU-Wii has (1 to 10).
const StateSlots = 10

// ErrNoGameScreen: the engine has no game screen to ask (no game playing).
var ErrNoGameScreen = errors.New("no GameCube or Wii game is playing")

var errStatesHere = errors.New("save and load state for GameCube and Wii are in the game itself here: Shift+F1 saves, F1 loads")

// SaveState: on Windows only for now.
func SaveState(image string, pid int, docked uintptr, slot int) error { return errStatesHere }

// LoadState: on Windows only for now.
func LoadState(image string, pid int, docked uintptr, slot int) error { return errStatesHere }
