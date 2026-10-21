//go:build !windows

package main

import "errors"

// Docking Dolphin into the app window is Windows-only; elsewhere it keeps its own window.

func embedDolphin(pid int, exited <-chan struct{}) {}

func embedPixelU(pid int, exited <-chan struct{}) {}

func embedSupermodel(pid int, exited <-chan struct{}) {}

func hostWindowPresent() bool { return false }

func embeddedWindow(pid int) uintptr { return 0 }

func setEmbedTop(px int) {}

func stopEmbeddedGame() error { return errors.New("no game is playing inside RetroPixels") }

func pixeluScreen(mode int) (bool, error) {
	return false, errors.New("no Wii U game is playing inside RetroPixels")
}
