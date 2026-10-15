//go:build !windows

package ffmpeg

import "os/exec"

func hide(cmd *exec.Cmd) {}
