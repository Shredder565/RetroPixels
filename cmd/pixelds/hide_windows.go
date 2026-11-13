package main

import (
	"os/exec"
	"syscall"
)

// hideConsole keeps a console program (ffmpeg) from opening a window.
func hideConsole(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
}
