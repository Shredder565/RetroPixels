//go:build amd64 && linux

package ps3

import "syscall"

// jitAlloc maps memory the code can run from.
func jitAlloc(size int) []byte {
	b, err := syscall.Mmap(-1, 0, size, syscall.PROT_READ|syscall.PROT_WRITE|syscall.PROT_EXEC,
		syscall.MAP_PRIVATE|syscall.MAP_ANON)
	if err != nil {
		return nil
	}
	return b
}

func jitFree(b []byte) { syscall.Munmap(b) }
