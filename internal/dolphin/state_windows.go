package dolphin

import (
	"errors"
	"syscall"
	"unsafe"
)

// Save states and pause: PixelU-Wii's game screen takes the registered window
// message "PixelU.HostCommand" (wParam 1 saves, 2 loads, lParam is the slot;
// 3 pauses or resumes) from RetroPixels, while a game plays.

var pRegisterWindowMessageW = modUser32.NewProc("RegisterWindowMessageW")

// StateSlots is how many save state slots PixelU-Wii has (1 to 10).
const StateSlots = 10

// ErrNoGameScreen: the engine has no game screen to ask (no game playing).
var ErrNoGameScreen = errors.New("no GameCube or Wii game is playing")

// SaveState asks the engine to save the game to a slot (1 to 10). image is
// the engine's .exe name (PixelU-Wii.exe), pid its process if known; docked
// is its game screen when it's docked as a child window (it isn't top-level
// then), else 0. The engine shows "Saved State to Slot N" on the game itself.
func SaveState(image string, pid int, docked uintptr, slot int) error {
	return stateCommand(image, pid, docked, 1, slot)
}

// LoadState asks the engine to load a slot (1 to 10).
func LoadState(image string, pid int, docked uintptr, slot int) error {
	return stateCommand(image, pid, docked, 2, slot)
}

// TogglePause pauses the game, or resumes it if it's paused.
func TogglePause(image string, pid int, docked uintptr) error {
	return stateCommand(image, pid, docked, 3, 0)
}

func stateCommand(image string, pid int, docked uintptr, op uintptr, slot int) error {
	var pids []int
	if pid > 0 {
		pids = append(pids, pid)
	}
	if image != "" {
		pids = append(pids, PIDsOf(image)...)
	}
	if op != 3 && (slot < 1 || slot > StateSlots) {
		return errors.New("save state slots go from 1 to 10")
	}
	msg, _, _ := pRegisterWindowMessageW.Call(uintptr(unsafe.Pointer(syscall.StringToUTF16Ptr("PixelU.HostCommand"))))
	if msg == 0 {
		return errors.New("couldn't reach PixelU (Wii/GameCube)")
	}
	// Every window of the engine gets it; only the game screen acts on it.
	seen := map[uintptr]bool{}
	if docked != 0 {
		if ok, _, _ := pIsWindow.Call(docked); ok != 0 {
			seen[docked] = true
		}
	}
	for _, pid := range pids {
		for _, h := range windowsOf(pid, true) {
			seen[h] = true
		}
	}
	if len(seen) == 0 {
		return ErrNoGameScreen
	}
	for h := range seen {
		pPostMessageW.Call(h, msg, op, uintptr(slot))
	}
	return nil
}
