package rpcs3

import (
	"os/exec"
	"syscall"
)

const createNoWindow = 0x08000000

// hideConsole: RPCS3 shows its own game window but no console window. The
// window itself must not be hidden, so HideWindow isn't set.
func hideConsole(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
}
