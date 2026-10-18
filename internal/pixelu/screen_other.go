//go:build !windows

package pixelu

import "errors"

// SetScreen: only PixelU on Windows takes this.
func SetScreen(hwnd uintptr, mode int) (bool, error) {
	return false, errors.New("only on Windows")
}
