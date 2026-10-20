//go:build !windows

package dolphin

import "os"

// WiiKeysScanRoots: the player's home folder (the app itself is Windows only).
func WiiKeysScanRoots() []string {
	if home, err := os.UserHomeDir(); err == nil {
		return []string{home}
	}
	return nil
}
