package dolphin

import (
	"strings"
	"syscall"
	"unsafe"
)

// Running tells whether a program (by file name, e.g. PixelU-Wii.exe) is running.
func Running(image string) bool {
	snap, err := syscall.CreateToolhelp32Snapshot(syscall.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return false
	}
	defer syscall.CloseHandle(snap)
	var e syscall.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = syscall.Process32First(snap, &e); err == nil; err = syscall.Process32Next(snap, &e) {
		if strings.EqualFold(syscall.UTF16ToString(e.ExeFile[:]), image) {
			return true
		}
	}
	return false
}

// PIDsOf lists the processes running image (an .exe name, any case).
func PIDsOf(image string) []int {
	snap, err := syscall.CreateToolhelp32Snapshot(syscall.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return nil
	}
	defer syscall.CloseHandle(snap)
	var out []int
	var e syscall.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = syscall.Process32First(snap, &e); err == nil; err = syscall.Process32Next(snap, &e) {
		if strings.EqualFold(syscall.UTF16ToString(e.ExeFile[:]), image) {
			out = append(out, int(e.ProcessID))
		}
	}
	return out
}
