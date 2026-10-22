//go:build !windows

package dolphin

import "os"

// mountRoots: where Linux desktops mount memory cards and USB drives
// (/media/<user>/<card>, /run/media/<user>/<card>) and other disks (/mnt).
var mountRoots = []string{"/media", "/run/media", "/mnt"}

// WiiKeysScanRoots: the player's own folders first (where a download or a
// copy from the SD card usually lands: the desktop's Downloads, Desktop and
// Documents, whatever their names), then the rest of the home folder, then
// mounted memory cards and drives, the way the Windows version looks on
// every drive and SD card.
func WiiKeysScanRoots() []string {
	home, _ := os.UserHomeDir()
	return wiiKeysRoots(home, mountRoots)
}

func wiiKeysRoots(home string, mounts []string) []string {
	var roots []string
	if home != "" {
		for _, sub := range []string{"Downloads", "Desktop", "Documents"} {
			roots = append(roots, UserFolder(home, sub))
		}
		roots = append(roots, home)
	}
	for _, m := range mounts {
		if isDir(m) {
			roots = append(roots, m)
		}
	}
	return roots
}
