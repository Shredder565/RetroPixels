package rpcs3

import (
	"io/fs"
	"syscall"
	"time"
)

// born is when a file was made.
func born(_ string, info fs.FileInfo) time.Time {
	if d, ok := info.Sys().(*syscall.Win32FileAttributeData); ok {
		return time.Unix(0, d.CreationTime.Nanoseconds())
	}
	return info.ModTime()
}
