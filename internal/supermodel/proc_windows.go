package supermodel

import (
	"os/exec"
	"syscall"
	"unicode/utf16"
	"unsafe"
)

const createNoWindow = 0x08000000

// hideConsole: the game shows Supermodel's own window but no console window
// (supermodel.exe is a console program). The window itself must not be
// hidden, so HideWindow isn't set.
func hideConsole(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
}

var procGetShortPathNameW = syscall.NewLazyDLL("kernel32.dll").NewProc("GetShortPathNameW")

// shortPath gives a path with letters outside plain ASCII in its short 8.3
// form, which Supermodel (reading its command line in the Windows code
// page) can open. Paths that are plain ASCII, or that have no short form,
// stay as they are.
func shortPath(p string) string {
	ascii := true
	for _, r := range p {
		if r > 127 {
			ascii = false
			break
		}
	}
	if ascii {
		return p
	}
	w, err := syscall.UTF16FromString(p)
	if err != nil {
		return p
	}
	buf := make([]uint16, 1024)
	n, _, _ := procGetShortPathNameW.Call(uintptr(unsafe.Pointer(&w[0])), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if n == 0 || int(n) >= len(buf) {
		return p
	}
	return string(utf16.Decode(buf[:n]))
}
