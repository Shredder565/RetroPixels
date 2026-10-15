package dolphin

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

const createNoWindow = 0x08000000

// systemUnpack7z unpacks a .7z with Windows' own tar.exe (Windows 11 can read .7z).
func systemUnpack7z(archive, dest string) error {
	tar := filepath.Join(os.Getenv("SystemRoot"), "System32", "tar.exe")
	if _, err := os.Stat(tar); err != nil {
		return err
	}
	cmd := exec.Command(tar, "-x", "-f", archive, "-C", dest)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("tar: %v %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}
