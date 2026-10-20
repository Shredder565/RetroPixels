package dolphin

import (
	"os"
	"path/filepath"
	"syscall"
	"unsafe"
)

var (
	pGetLogicalDrives = modKernel32.NewProc("GetLogicalDrives")
	pGetDriveTypeW    = modKernel32.NewProc("GetDriveTypeW")
)

const (
	driveRemovable = 2 // an SD card, where BootMii puts its backup
	driveFixed     = 3
)

// WiiKeysScanRoots: the player's own folders first (where a download or a
// copy from the SD card usually lands), then every fixed drive and memory
// card. Floppy drives (A:, B:), network and optical drives are left out.
func WiiKeysScanRoots() []string {
	var roots []string
	if home, err := os.UserHomeDir(); err == nil {
		for _, sub := range []string{"Downloads", "Desktop", "Documents"} {
			roots = append(roots, filepath.Join(home, sub))
		}
	}
	mask, _, _ := pGetLogicalDrives.Call()
	for i := 2; i < 26; i++ { // C: to Z:
		if mask&(1<<uint(i)) == 0 {
			continue
		}
		root := string(rune('A'+i)) + `:\`
		p, err := syscall.UTF16PtrFromString(root)
		if err != nil {
			continue
		}
		t, _, _ := pGetDriveTypeW.Call(uintptr(unsafe.Pointer(p)))
		if t == driveFixed || t == driveRemovable {
			roots = append(roots, root)
		}
	}
	return roots
}
