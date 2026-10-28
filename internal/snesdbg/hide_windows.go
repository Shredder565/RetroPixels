package snesdbg

import (
	"os/exec"
	"syscall"
)

// hideWindow: the debugger opens its own window; no console flashes up.
func hideWindow(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x08000000} // CREATE_NO_WINDOW
}
