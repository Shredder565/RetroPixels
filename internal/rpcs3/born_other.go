//go:build !windows

package rpcs3

import (
	"io/fs"
	"time"
)

// born is when a file was made (its last change, where that isn't kept).
func born(_ string, info fs.FileInfo) time.Time { return info.ModTime() }
