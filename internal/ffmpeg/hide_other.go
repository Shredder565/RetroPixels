//go:build !windows

package ffmpeg

import "os/exec"

func hide(cmd *exec.Cmd) {}

// LowPriority runs ffmpeg below normal priority (Windows only).
func LowPriority(cmd *exec.Cmd) *exec.Cmd { return cmd }
