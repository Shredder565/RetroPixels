//go:build linux

package main

import (
	"os/exec"

	"retropixels/internal/ffmpeg"
)

// Linux has no console windows to hide.
func hiddenCommand(name string, args ...string) *exec.Cmd { return exec.Command(name, args...) }

// lowPriority runs a command below normal priority (nice -n 10), so saving a
// recording doesn't slow down the next game.
func lowPriority(cmd *exec.Cmd) *exec.Cmd { return ffmpeg.LowPriority(cmd) }
