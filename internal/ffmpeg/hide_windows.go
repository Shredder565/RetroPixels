package ffmpeg

import (
	"os/exec"
	"syscall"
)

const (
	createNoWindow = 0x08000000
	belowNormal    = 0x00004000 // BELOW_NORMAL_PRIORITY_CLASS
)

func hide(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}

// LowPriority runs ffmpeg below normal priority, so encoding never slows the game.
func LowPriority(cmd *exec.Cmd) *exec.Cmd {
	if cmd.SysProcAttr == nil {
		cmd.SysProcAttr = &syscall.SysProcAttr{}
	}
	cmd.SysProcAttr.CreationFlags |= belowNormal
	return cmd
}
