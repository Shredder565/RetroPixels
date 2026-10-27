//go:build amd64 && windows

package ps3

import (
	"syscall"
	"unsafe"
)

var virtualAlloc = syscall.NewLazyDLL("kernel32.dll").NewProc("VirtualAlloc")

// jitAlloc reserves memory the code can run from.
func jitAlloc(size int) []byte {
	const memCommit, memReserve, pageExecuteReadWrite = 0x1000, 0x2000, 0x40
	p, _, _ := virtualAlloc.Call(0, uintptr(size), memCommit|memReserve, pageExecuteReadWrite)
	if p == 0 {
		return nil
	}
	// (memory outside Go's heap, so the uintptr can't go stale)
	ptr := *(*unsafe.Pointer)(unsafe.Pointer(&p))
	return unsafe.Slice((*byte)(ptr), size)
}

var virtualFree = syscall.NewLazyDLL("kernel32.dll").NewProc("VirtualFree")

func jitFree(b []byte) {
	const memRelease = 0x8000
	virtualFree.Call(uintptr(unsafe.Pointer(&b[0])), 0, memRelease)
}
