//go:build !windows

package rpcs3

import "os/exec"

func hideConsole(cmd *exec.Cmd) {}
