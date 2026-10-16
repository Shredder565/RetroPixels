package mame

import (
	"os/exec"
	"syscall"
)

const createNoWindow = 0x08000000

// hideProbe: a quick question to MAME (-version, -listfull) shows no window at all.
func hideProbe(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}

// hideConsole: a game shows MAME's own window but no console window
// (official mame.exe is a console program). The window itself must not be
// hidden, so HideWindow isn't set.
func hideConsole(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
}
